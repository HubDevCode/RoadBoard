#include "engine/TapeDeck.h"

#include <samplerate.h>
#include <sndfile.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "dsp/Dsp.h"

namespace road {

namespace {

// Spawn a process and return a FILE* reading its stdout (no shell involved).
FILE* spawnReader(const std::vector<std::string>& args, int* pidOut) {
    int fds[2];
    if (pipe(fds) != 0) return nullptr;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return nullptr; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    if (pidOut) *pidOut = pid;
    return fdopen(fds[0], "rb");
}

void reap(FILE*& f, int& pid) {
    if (f) { fclose(f); f = nullptr; }
    if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); pid = -1; }
}

std::string runCapture(const std::vector<std::string>& args) {
    int pid = -1;
    FILE* f = spawnReader(args, &pid);
    if (!f) return {};
    std::string out;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    if (pid > 0) waitpid(pid, nullptr, 0);
    return out;
}

bool hasFfmpeg() {
    static int cached = -1;
    if (cached < 0) cached = runCapture({"ffmpeg", "-hide_banner", "-version"}).find("ffmpeg") != std::string::npos ? 1 : 0;
    return cached == 1;
}

void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) mkdir(cur.c_str(), 0755);
    }
}

} // namespace

struct TapeDeck::Files {
    SNDFILE* main = nullptr;
    std::vector<SNDFILE*> tracks;
    SRC_STATE* src = nullptr;
    double ratio = 1.0;
    int channels = 2;
};

TapeDeck::TapeDeck() {
    playRing_.resize(size_t(48000) * 8 * 2);
    recRing_.resize(size_t(192000) * 8 * 2);
    writer_ = std::thread([this] { writerLoop(); });
}

TapeDeck::~TapeDeck() {
    stopDecoder();
    writerRun_ = false;
    if (writer_.joinable()) writer_.join();
    closeRecordFiles();
}

std::string TapeDeck::defaultFolder() {
    const char* xdg = std::getenv("XDG_MUSIC_DIR");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else {
        const char* home = std::getenv("HOME");
        base = std::string(home ? home : "/tmp") + "/Music";
    }
    return base + "/RoadBoard";
}

std::string TapeDeck::fileExtension(int t) {
    switch (t) {
    case 2: return ".aiff";
    case 3: return ".wav";   // BWF
    case 4: return ".flac";
    case 5: return ".ogg";
    case 100: return ".mp3";
    default: return ".wav";
    }
}

// ------------------------------------------------------------------------ playback

void TapeDeck::stopDecoder() {
    decoderRun_ = false;
    if (decoder_.joinable()) decoder_.join();
}

bool TapeDeck::load(const std::string& path, std::string* error) {
    stop();
    stopDecoder();
    SF_INFO info{};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    int ch = 2;
    double dur = 0;
    int rate = 0;
    if (sf) {
        ch = std::clamp(info.channels, 1, 8);
        rate = info.samplerate;
        dur = info.samplerate > 0 ? double(info.frames) / info.samplerate : 0;
        sf_close(sf);
    } else if (hasFfmpeg()) {
        const std::string d = runCapture({"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path});
        dur = std::atof(d.c_str());
        if (dur <= 0) {
            if (error) *error = "unsupported or unreadable file";
            std::lock_guard<std::mutex> lk(mutex_); error_ = "unsupported file";
            return false;
        }
        ch = 2;
        rate = 0;
    } else {
        if (error) *error = std::string("cannot open file: ") + sf_strerror(nullptr) + " (install ffmpeg for more formats)";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        path_ = path; error_.clear();
    }
    params().setStr("recorder.file", path);
    playCh_ = ch;
    fileRate_ = rate;
    duration_ = dur;
    playRing_.clear();
    playedFrames_ = 0;
    seekOrigin_ = 0;
    seekRequest_ = -1;
    eof_ = false;
    loaded_ = true;
    decoderRun_ = true;
    decoder_ = std::thread([this] { decoderLoop(); });
    if (params().rec.playOnLoad->on()) play();
    return true;
}

void TapeDeck::eject() {
    stop();
    stopDecoder();
    loaded_ = false;
    duration_ = 0;
    std::lock_guard<std::mutex> lk(mutex_);
    path_.clear();
}

void TapeDeck::play() {
    if (!loaded_) return;
    if (eof_ && playRing_.readable() == 0) seek(0);
    paused_ = false;
    playing_ = true;
    params().rec.play->set(1); params().rec.pause->set(0); params().rec.stop->set(0);
    emitEvent("play");
}

void TapeDeck::replay() { seek(0); play(); }

void TapeDeck::stop() {
    const bool was = playing_ || recording_ || recRequest_;
    playing_ = false;
    paused_ = false;
    if (recRequest_) setRecord(false);
    params().rec.play->set(0); params().rec.pause->set(0);
    if (was) emitEvent("stop");
}

void TapeDeck::pause() {
    if (!playing_) return;
    paused_ = !paused_;
    params().rec.pause->set(paused_ ? 1 : 0);
}

void TapeDeck::seek(double seconds) {
    if (!loaded_) return;
    seconds = std::clamp(seconds, 0.0, std::max(0.0, duration_.load()));
    seekRequest_ = seconds;
}

double TapeDeck::position() const {
    const double fs = engineRate_.load();
    double pos = seekOrigin_.load() + double(playedFrames_.load()) / (fs > 0 ? fs : 48000);
    const double dur = duration_.load();
    if (dur > 0 && params().rec.loop->on()) pos = std::fmod(pos, dur);
    return std::min(pos, dur > 0 ? dur : pos);
}

std::string TapeDeck::fileName() const { std::lock_guard<std::mutex> lk(mutex_); return path_; }
std::string TapeDeck::lastRecordFile() const { std::lock_guard<std::mutex> lk(mutex_); return lastRecord_; }
std::string TapeDeck::lastError() const { std::lock_guard<std::mutex> lk(mutex_); return error_; }

void TapeDeck::decoderLoop() {
    std::string path;
    { std::lock_guard<std::mutex> lk(mutex_); path = path_; }
    const int ch = playCh_.load();
    SF_INFO info{};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    FILE* ff = nullptr;
    int ffPid = -1;
    float ffRate = 0;
    SRC_STATE* src = nullptr;
    float srcRate = 0;
    int err = 0;
    std::vector<float> inBuf(4096 * size_t(std::max(info.channels, 8))), outBuf(4096 * 8 * 8), chBuf(4096 * 8);

    auto openFf = [&](double at) {
        reap(ff, ffPid);
        ffRate = engineRate_.load();
        char ss[32]; std::snprintf(ss, sizeof ss, "%.3f", at);
        ff = spawnReader({"ffmpeg", "-v", "error", "-nostdin", "-ss", ss, "-i", path, "-vn", "-f", "f32le",
                          "-ac", "2", "-ar", std::to_string(int(ffRate)), "-"}, &ffPid);
    };
    if (!sf) openFf(0);

    while (decoderRun_) {
        const double sk = seekRequest_.exchange(-1);
        if (sk >= 0) {
            flushRequest_ = true;
            for (int i = 0; i < 50 && flushRequest_ && playing_ && !paused_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (flushRequest_) { playRing_.clear(); flushRequest_ = false; }
            if (sf) sf_seek(sf, sf_count_t(sk * info.samplerate), SEEK_SET);
            else openFf(sk);
            if (src) src_reset(src);
            seekOrigin_ = sk;
            playedFrames_ = 0;
            eof_ = false;
        }
        const float fs = engineRate_.load();
        if (!sf && ff && fs != ffRate) { // engine rate changed: restart ffmpeg at the new rate
            seekRequest_ = position();
            continue;
        }
        const size_t chunk = 1024;
        if (eof_ || playRing_.writable() < chunk * 8 * size_t(ch) * 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        size_t frames = 0;
        if (sf) {
            const sf_count_t got = sf_readf_float(sf, inBuf.data(), sf_count_t(chunk));
            frames = size_t(std::max<sf_count_t>(got, 0));
            // keep up to 8 channels
            if (info.channels != ch) {
                for (size_t i = 0; i < frames; ++i)
                    for (int c = 0; c < ch; ++c) chBuf[i * size_t(ch) + size_t(c)] = inBuf[i * size_t(info.channels) + size_t(c)];
            } else {
                std::memcpy(chBuf.data(), inBuf.data(), frames * size_t(ch) * sizeof(float));
            }
            if (frames > 0 && info.samplerate != int(fs)) {
                if (!src || srcRate != fs) {
                    if (src) src_delete(src);
                    src = src_new(SRC_SINC_MEDIUM_QUALITY, ch, &err);
                    srcRate = fs;
                }
                SRC_DATA d{};
                d.data_in = chBuf.data(); d.input_frames = long(frames);
                d.data_out = outBuf.data(); d.output_frames = long(outBuf.size() / size_t(ch));
                d.src_ratio = double(fs) / info.samplerate;
                d.end_of_input = 0;
                if (src && src_process(src, &d) == 0) {
                    playRing_.write(outBuf.data(), size_t(d.output_frames_gen) * size_t(ch));
                }
            } else if (frames > 0) {
                playRing_.write(chBuf.data(), frames * size_t(ch));
            }
        } else if (ff) {
            const size_t got = fread(chBuf.data(), sizeof(float) * 2, chunk, ff);
            frames = got;
            if (frames > 0) playRing_.write(chBuf.data(), frames * 2);
        }
        if (frames == 0) {
            if (params().rec.loop->on() && duration_ > 0) {
                if (sf) sf_seek(sf, 0, SEEK_SET);
                else openFf(0);
            } else {
                eof_ = true;
            }
        }
    }
    if (sf) sf_close(sf);
    reap(ff, ffPid);
    if (src) src_delete(src);
}

void TapeDeck::rtPlay(float* const* bus, int n, float fs) {
    engineRate_.store(fs, std::memory_order_relaxed);
    if (flushRequest_.load(std::memory_order_acquire)) { playRing_.clear(); flushRequest_.store(false); }
    if (!playing_.load(std::memory_order_relaxed) || paused_.load(std::memory_order_relaxed)) return;
    static thread_local std::vector<float> tmp(size_t(kMaxFrames) * 8);
    const int ch = playCh_.load(std::memory_order_relaxed);
    const size_t got = playRing_.read(tmp.data(), size_t(n) * size_t(ch)) / size_t(ch);
    ParamStore& P = params();
    const float target = dsp::dbToLin(P.rec.gain->get());
    const float g0 = playGain_;
    const float step = (target - g0) / float(std::max(n, 1));
    for (int b = 0; b < kBuses; ++b) {
        if (!P.rec.bus[b]->on()) continue;
        float g = g0;
        for (size_t i = 0; i < got; ++i) {
            g += step;
            const float* fr = &tmp[i * size_t(ch)];
            if (ch == 1) { bus[b * kBusCh][i] += fr[0] * g; bus[b * kBusCh + 1][i] += fr[0] * g; }
            else for (int c = 0; c < std::min(ch, kBusCh); ++c) bus[b * kBusCh + c][i] += fr[c] * g;
        }
    }
    playGain_ = target;
    playedFrames_.fetch_add(int64_t(got), std::memory_order_relaxed);
    if (got < size_t(n) && eof_.load(std::memory_order_relaxed) && playRing_.readable() == 0) {
        playing_.store(false);
        eofPending_.store(true);
    }
}

// ------------------------------------------------------------------------ recording

void TapeDeck::rtRecord(const float* const* raw, float* const* bus, int n, float fs) {
    ParamStore& P = params();
    recRate_.store(int(fs), std::memory_order_relaxed);
    if (!recRequest_.load(std::memory_order_relaxed) && P.rec.preRec->i() <= 0) return;
    static thread_local std::vector<float> tmp(size_t(kMaxFrames) * 8);
    int ch;
    if (P.rec.recBus->on()) {
        int b = 0;
        for (int k = 0; k < kBuses; ++k) if (P.rec.armBus[k]->on()) { b = k; break; }
        ch = P.rec.fileType->i() == 100 ? 2 : P.rec.channels->i();
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c) tmp[size_t(i * ch + c)] = bus[b * kBusCh + c][i];
    } else {
        ch = 2;
        std::fill(tmp.begin(), tmp.begin() + n * 2, 0.0f);
        for (int s = 0; s < kStrips; ++s) {
            if (!P.rec.armStrip[s]->on()) continue;
            const float* L = raw[stripFirstInCh(s)];
            const float* R = raw[stripFirstInCh(s) + 1];
            for (int i = 0; i < n; ++i) { tmp[size_t(2 * i)] += L[i]; tmp[size_t(2 * i + 1)] += R[i]; }
        }
    }
    if (ch != recCh_.load(std::memory_order_relaxed)) {
        if (recording_.load(std::memory_order_relaxed)) return; // channel layout locked while recording
        recCh_.store(ch);
    }
    recRing_.write(tmp.data(), size_t(n) * size_t(ch));
}

void TapeDeck::setRecord(bool on) {
    if (on == recRequest_.load()) return;
    recRequest_ = on;
    params().rec.record->set(on ? 1 : 0);
    if (!on) emitEvent("stop");
}

double TapeDeck::recordSeconds() const {
    const int r = recRate_.load();
    return r > 0 ? double(recFrames_.load()) / r : 0;
}

bool TapeDeck::openRecordFiles(int channels, int rate, std::string* err) {
    ParamStore& P = params();
    const int type = P.rec.fileType->i();
    const int bits = P.rec.bitResolution->i();
    int outRate = P.rec.sampleRate->i();
    if (outRate <= 0) outRate = rate;

    std::string folder = P.str("recorder.folder");
    if (folder.empty()) folder = defaultFolder();
    mkdirs(folder);
    std::string prefix = P.str("recorder.prefix");
    if (prefix.empty()) prefix = "Record";
    char stamp[64];
    std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d at %Hh%Mm%Ss", std::localtime(&t));
    const std::string ext = fileExtension(type);
    const std::string base = folder + "/" + prefix + " " + stamp;

    int fmt = 0;
    switch (type) {
    case 2: fmt = SF_FORMAT_AIFF; break;
    case 4: fmt = SF_FORMAT_FLAC; break;
    case 5: fmt = SF_FORMAT_OGG | SF_FORMAT_VORBIS; break;
#ifdef SF_FORMAT_MPEG
    case 100: fmt = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III; channels = std::min(channels, 2); break;
#else
    case 100:   // libsndfile < 1.1 cannot write MP3
        if (err) *err = "MP3 recording needs libsndfile 1.1 or newer";
        return false;
#endif
    default: fmt = SF_FORMAT_WAV; break;
    }
    if (type != 5 && type != 100) {
        int sub = SF_FORMAT_PCM_24;
        if (bits <= 8) sub = type == 2 ? SF_FORMAT_PCM_S8 : SF_FORMAT_PCM_U8;
        else if (bits <= 16) sub = SF_FORMAT_PCM_16;
        else if (bits >= 32) sub = type == 4 ? SF_FORMAT_PCM_24 : SF_FORMAT_FLOAT;
        if (type == 4 && sub == SF_FORMAT_PCM_U8) sub = SF_FORMAT_PCM_S8;
        fmt |= sub;
    }
    auto open = [&](const std::string& path, int nch) -> SNDFILE* {
        SF_INFO info{};
        info.samplerate = outRate;
        info.channels = nch;
        info.format = fmt;
        if (!sf_format_check(&info)) return nullptr;
        SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
        if (!f) return nullptr;
        if (type == 100) {
#ifdef SF_FORMAT_MPEG
            int mode = SF_BITRATE_MODE_CONSTANT;
            sf_command(f, SFC_SET_BITRATE_MODE, &mode, sizeof mode);
#endif
            double level = std::clamp((320.0 - P.rec.kbps->get()) / (320.0 - 32.0), 0.0, 1.0);
            sf_command(f, SFC_SET_COMPRESSION_LEVEL, &level, sizeof level);
        }
        if (type == 3) {
            SF_BROADCAST_INFO bi{};
            std::snprintf(bi.description, sizeof bi.description, "RoadBoard recording");
            std::snprintf(bi.originator, sizeof bi.originator, "RoadBoard");
            std::strftime(bi.origination_date, sizeof bi.origination_date, "%Y-%m-%d", std::localtime(&t));
            std::strftime(bi.origination_time, sizeof bi.origination_time, "%H:%M:%S", std::localtime(&t));
            sf_command(f, SFC_SET_BROADCAST_INFO, &bi, sizeof bi);
        }
        return f;
    };
    auto* files = new Files;
    files->channels = channels;
    files->main = open(base + ext, channels);
    if (!files->main) {
        if (err) *err = std::string("cannot create record file: ") + sf_strerror(nullptr);
        delete files;
        return false;
    }
    if (P.rec.multiTrack->on() && channels > 1)
        for (int c = 0; c < channels; ++c)
            files->tracks.push_back(open(base + "_Track" + std::to_string(c + 1) + ext, 1));
    if (outRate != rate) {
        int e = 0;
        files->src = src_new(SRC_SINC_MEDIUM_QUALITY, channels, &e);
        files->ratio = double(outRate) / rate;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        lastRecord_ = base + ext;
    }
    files_ = files;
    return true;
}

void TapeDeck::closeRecordFiles() {
    if (!files_) return;
    if (files_->main) sf_close(files_->main);
    for (SNDFILE* f : files_->tracks) if (f) sf_close(f);
    if (files_->src) src_delete(files_->src);
    delete files_;
    files_ = nullptr;
}

void TapeDeck::writerLoop() {
    std::vector<float> chunk(4096 * 8), res(4096 * 8 * 8 + 64), mono(4096 * 8);
    std::vector<float> pre;      // circular pre-record buffer (interleaved)
    size_t prePos = 0, preFill = 0;
    int preCh = 0, preRate = 0;

    auto writeFrames = [&](const float* data, size_t frames, int ch) {
        if (!files_) return;
        const float* out = data;
        size_t outFrames = frames;
        if (files_->src) {
            SRC_DATA d{};
            d.data_in = data; d.input_frames = long(frames);
            d.data_out = res.data(); d.output_frames = long(res.size() / size_t(ch));
            d.src_ratio = files_->ratio;
            if (src_process(files_->src, &d) != 0) return;
            out = res.data(); outFrames = size_t(d.output_frames_gen);
        }
        sf_writef_float(files_->main, out, sf_count_t(outFrames));
        for (size_t c = 0; c < files_->tracks.size(); ++c) {
            if (!files_->tracks[c]) continue;
            for (size_t i = 0; i < outFrames; ++i) mono[i] = out[i * size_t(ch) + c];
            sf_writef_float(files_->tracks[c], mono.data(), sf_count_t(outFrames));
        }
    };

    while (writerRun_) {
        const int ch = std::max(1, recCh_.load());
        const int rate = recRate_.load();
        const bool want = recRequest_.load();

        if (want && !recording_) {
            std::string err;
            if (openRecordFiles(ch, rate, &err)) {
                recFrames_ = 0;
                recording_ = true;
                recStartedPending_ = true;
                // flush the pre-record buffer first
                if (preFill > 0 && preCh == ch && preRate == rate) {
                    const size_t frames = preFill / size_t(ch);
                    const size_t cap = pre.size();
                    size_t start = (prePos + cap - preFill) % cap;
                    std::vector<float> lin(preFill);
                    for (size_t i = 0; i < preFill; ++i) lin[i] = pre[(start + i) % cap];
                    writeFrames(lin.data(), frames, ch);
                    recFrames_ += int64_t(frames);
                }
                preFill = 0;
            } else {
                std::lock_guard<std::mutex> lk(mutex_);
                error_ = err;
                recRequest_ = false;
                params().rec.record->set(0);
            }
        } else if (!want && recording_) {
            recording_ = false;
            closeRecordFiles();
        }

        const size_t avail = recRing_.readable();
        const size_t frames = std::min(avail / size_t(ch), size_t(4096));
        if (frames == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        recRing_.read(chunk.data(), frames * size_t(ch));

        if (recording_) {
            writeFrames(chunk.data(), frames, ch);
            recFrames_ += int64_t(frames);
        } else {
            const int preSec = params().rec.preRec->i();
            if (preSec <= 0) { preFill = 0; continue; }
            const size_t cap = size_t(preSec) * size_t(rate) * size_t(ch);
            if (pre.size() != cap || preCh != ch || preRate != rate) {
                pre.assign(cap, 0.0f); prePos = 0; preFill = 0; preCh = ch; preRate = rate;
            }
            for (size_t i = 0; i < frames * size_t(ch); ++i) {
                pre[prePos] = chunk[i];
                prePos = (prePos + 1) % cap;
            }
            preFill = std::min(cap, preFill + frames * size_t(ch));
        }
    }
}

void TapeDeck::poll() {
    ParamStore& P = params();
    if (eofPending_.exchange(false)) {
        P.rec.play->set(0);
        emitEvent("eof");
        emitEvent("stop");
    }
    if (recStartedPending_.exchange(false)) emitEvent("record");
    const int stopAfter = P.rec.stopAfter->i();
    if (recording_ && stopAfter > 0 && recordSeconds() >= stopAfter) setRecord(false);
    P.rec.record->set(recRequest_ ? 1 : 0);
}

} // namespace road
