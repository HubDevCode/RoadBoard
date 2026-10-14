#include "engine/RoadLink.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <samplerate.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <strings.h>

namespace road {

namespace {

constexpr int kHeader = 28;
// packet signatures: the widespread 4 byte signature used by existing network-audio
// apps (interoperability, default) and RoadBoard's own one
const uint8_t kSigCompat[4] = {0x56, 0x42, 0x41, 0x4E};
const uint8_t kSigOwn[4] = {'R', 'D', 'L', 'K'};
constexpr int kMaxPayload = 1436;
constexpr uint8_t kProtoAudio = 0x00, kProtoText = 0x40, kProtoService = 0x60;
const int kSrList[21] = {6000, 12000, 24000, 48000, 96000, 192000, 384000, 8000, 16000, 32000, 64000,
                         128000, 256000, 512000, 11025, 22050, 44100, 88200, 176400, 352800, 705600};
const int kBytesPerType[8] = {1, 2, 3, 4, 4, 8, 0, 0};
const float kQualityMs[5] = {12, 24, 48, 96, 192};

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool sameName(const char* a16, const std::string& b) {
    char tmp[17] = {};
    std::memcpy(tmp, a16, 16);
    return strncasecmp(tmp, b.c_str(), 16) == 0;
}

uint32_t resolve(const std::string& host) {
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) return a.s_addr;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        uint32_t ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        return ip;
    }
    return 0;
}

std::string ipString(uint32_t addr) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a{addr};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

} // namespace

int RoadLinkEngine::srIndex(int rate) {
    for (int i = 0; i < 21; ++i) if (kSrList[i] == rate) return i;
    return 3;
}
int RoadLinkEngine::srFromIndex(int idx) { return idx >= 0 && idx < 21 ? kSrList[idx] : 0; }

RoadLinkEngine::RoadLinkEngine() {
    for (auto& s : in_) s.ring.resize(size_t(96000) * 8 / 2);
    for (auto& s : out_) s.ring.resize(size_t(96000) * 8 / 2);
}

RoadLinkEngine::~RoadLinkEngine() { stop(); }

void RoadLinkEngine::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { netLoop(); });
}

void RoadLinkEngine::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    for (auto& [fd, port] : sockets_) close(fd);
    sockets_.clear();
    if (sendSock_ >= 0) { close(sendSock_); sendSock_ = -1; }
    for (auto& o : out_) if (o.src) { src_delete(static_cast<SRC_STATE*>(o.src)); o.src = nullptr; }
}

std::vector<RoadLinkDetected> RoadLinkEngine::detected() const {
    std::lock_guard<std::mutex> lk(detMutex_);
    std::vector<RoadLinkDetected> v;
    const double t = nowSec();
    for (auto& d : detected_) if (t - d.lastSeen < 5.0) v.push_back(d);
    return v;
}

void RoadLinkEngine::reopenSockets() {
    ParamStore& P = params();
    std::set<int> ports;
    if (P.link.enable->on()) {
        for (int i = 0; i < kLinkStreams; ++i) if (P.link.in[i].on->on()) ports.insert(P.link.in[i].port->i());
        if (P.link.textOn->on()) ports.insert(P.link.textPort->i());
        ports.insert(6980); // always answer pings / show detected streams on the default port
    }
    std::set<int> have;
    for (auto& s : sockets_) have.insert(s.second);
    if (have == ports) return;
    for (auto& [fd, port] : sockets_) close(fd);
    sockets_.clear();
    for (int port : ports) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(uint16_t(port));
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { close(fd); continue; }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        sockets_.push_back({fd, port});
    }
}

void RoadLinkEngine::netLoop() {
    sendSock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sendSock_ >= 0) {
        int one = 1;
        setsockopt(sendSock_, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        fcntl(sendSock_, F_SETFL, O_NONBLOCK);
    }
    double lastCfg = 0;
    uint8_t buf[2048];
    while (running_) {
        const double t = nowSec();
        if (t - lastCfg > 0.5) { reopenSockets(); lastCfg = t; }
        std::vector<pollfd> pfd;
        for (auto& s : sockets_) pfd.push_back({s.first, POLLIN, 0});
        if (pfd.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        else if (poll(pfd.data(), nfds_t(pfd.size()), 2) > 0) {
            for (size_t k = 0; k < pfd.size(); ++k) {
                if (!(pfd[k].revents & POLLIN)) continue;
                for (int burst = 0; burst < 64; ++burst) {
                    sockaddr_in from{};
                    socklen_t fl = sizeof from;
                    ssize_t len = recvfrom(pfd[k].fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                    if (len <= 0) break;
                    handlePacket(buf, size_t(len), from.sin_addr.s_addr, ntohs(from.sin_port), pfd[k].fd);
                }
            }
        }
        sendOutgoing();
    }
}

void RoadLinkEngine::handlePacket(const uint8_t* d, size_t len, uint32_t fromAddr, uint16_t fromPort, int sock) {
    if (len < size_t(kHeader) || (std::memcmp(d, kSigCompat, 4) != 0 && std::memcmp(d, kSigOwn, 4) != 0)) return;
    ParamStore& P = params();
    if (!P.link.enable->on()) return;
    const uint8_t sr = d[4] & 0x1F, proto = d[4] & 0xE0;
    const int nbs = d[5] + 1, nbc = d[6] + 1;
    const int type = d[7] & 0x07, codec = d[7] & 0xF0;
    const char* name = reinterpret_cast<const char*>(d + 8);
    uint32_t frame;
    std::memcpy(&frame, d + 24, 4);
    const uint8_t* payload = d + kHeader;
    const size_t plen = len - size_t(kHeader);
    const std::string ip = ipString(fromAddr);
    int localPort = 6980;
    for (auto& s : sockets_) if (s.first == sock) localPort = s.second;

    if (proto == kProtoService) {
        // service identification: answer PING0 requests
        if (d[6] == 0 && d[5] == 0 && sendSock_ >= 0) {
            std::vector<uint8_t> pkt(size_t(kHeader) + 676, 0);
            std::memcpy(pkt.data(), d, kHeader);
            pkt[5] = 0x80; // reply
            uint8_t* p = pkt.data() + kHeader;
            const uint32_t bitType = 0x00000001 | 0x00000010; // receptor + transmitter
            const uint32_t features = 0x00000001 | 0x00000008; // audio + text
            std::memcpy(p, &bitType, 4); std::memcpy(p + 4, &features, 4);
            const uint32_t pref = uint32_t(engineRate_.load()), minR = 8000, maxR = 192000, color = 0x00E6C832;
            std::memcpy(p + 12, &pref, 4); std::memcpy(p + 16, &minR, 4); std::memcpy(p + 20, &maxR, 4); std::memcpy(p + 24, &color, 4);
            p[28] = 1; p[29] = 0; p[30] = 0; p[31] = 0;
            char host[64] = {};
            gethostname(host, sizeof host - 1);
            const std::string dev = P.str("roadlink.name");
            std::snprintf(reinterpret_cast<char*>(p + 164), 64, "%s", dev.c_str());
            std::snprintf(reinterpret_cast<char*>(p + 228), 64, "%s", "RoadBoard");
            std::snprintf(reinterpret_cast<char*>(p + 292), 64, "%s", "RoadBoard Virtual Mixer (Linux)");
            std::snprintf(reinterpret_cast<char*>(p + 356), 64, "%s", host);
            const char* user = std::getenv("USER");
            std::snprintf(reinterpret_cast<char*>(p + 420), 128, "%s", user ? user : "");
            sockaddr_in to{};
            to.sin_family = AF_INET; to.sin_port = htons(fromPort); to.sin_addr.s_addr = fromAddr;
            sendto(sendSock_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        }
        return;
    }

    if (proto == kProtoText) {
        if (!P.link.textOn->on() || P.link.textPort->i() != localPort) return;
        const std::string want = P.str("roadlink.instream.text.name");
        if (!want.empty() && !sameName(name, want)) return;
        std::string text(reinterpret_cast<const char*>(payload), plen);
        while (!text.empty() && text.back() == '\0') text.pop_back();
        if (onText && !text.empty()) onText(text, ip);
        return;
    }

    if (proto != kProtoAudio || codec != 0 || kBytesPerType[type] == 0) return;
    const int rate = srFromIndex(sr);
    const size_t need = size_t(nbs) * size_t(nbc) * size_t(kBytesPerType[type]);

    // detected streams list
    {
        std::lock_guard<std::mutex> lk(detMutex_);
        char nm[17] = {};
        std::memcpy(nm, name, 16);
        auto it = std::find_if(detected_.begin(), detected_.end(), [&](const RoadLinkDetected& x) { return x.name == nm && x.ip == ip; });
        if (it == detected_.end()) { detected_.push_back({nm, ip, rate, nbc, kBytesPerType[type] * 8, nowSec()}); }
        else { it->sampleRate = rate; it->channels = nbc; it->bits = kBytesPerType[type] * 8; it->lastSeen = nowSec(); }
    }

    for (int i = 0; i < kLinkStreams; ++i) {
        auto& cfg = P.link.in[i];
        if (!cfg.on->on() || cfg.port->i() != localPort) continue;
        if (!sameName(name, P.str("roadlink.instream[" + std::to_string(i) + "].name"))) continue;
        const std::string wantIp = P.str("roadlink.instream[" + std::to_string(i) + "].ip");
        if (!wantIp.empty() && wantIp != "0.0.0.0" && resolve(wantIp) != fromAddr) continue;
        InStream& s = in_[i];
        RoadLinkStats& st = inStats_[i];
        if (plen < need) { st.corrupt++; continue; }
        if (s.rate != rate || s.ch != std::min(nbc, 8)) {
            s.rate = rate; s.ch = std::min(nbc, 8);
            s.generation++;
            s.haveFrame = false;
            cfg.sr->set(float(rate)); cfg.ch->set(float(nbc)); cfg.bit->set(float(type));
        }
        if (s.haveFrame) {
            const uint32_t expect = s.lastFrame + 1;
            if (frame != expect) {
                if (int32_t(frame - expect) < 0) { st.disorder++; continue; }
                st.missing += frame - expect;
            }
        }
        s.lastFrame = frame; s.haveFrame = true;
        st.active = true;

        float tmp[256 * 8];
        const int ch = s.ch;
        float peak = 0;
        for (int f = 0; f < nbs; ++f) {
            for (int c = 0; c < ch; ++c) {
                const uint8_t* q = payload + (size_t(f) * size_t(nbc) + size_t(c)) * size_t(kBytesPerType[type]);
                float v = 0;
                switch (type) {
                case 0: v = (float(*q) - 128.0f) / 128.0f; break;
                case 1: { int16_t x; std::memcpy(&x, q, 2); v = x / 32768.0f; break; }
                case 2: { int32_t x = int32_t(uint32_t(q[0]) << 8 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 24) >> 8; v = x / 8388608.0f; break; }
                case 3: { int32_t x; std::memcpy(&x, q, 4); v = float(x / 2147483648.0); break; }
                case 4: std::memcpy(&v, q, 4); break;
                case 5: { double x; std::memcpy(&x, q, 8); v = float(x); break; }
                }
                tmp[f * ch + c] = v;
                peak = std::max(peak, std::fabs(v));
            }
        }
        st.level = std::max(st.level.load() * 0.9f, peak);
        if (s.ring.write(tmp, size_t(nbs) * size_t(ch)) < size_t(nbs) * size_t(ch)) st.overload++;
    }
}

void RoadLinkEngine::sendOutgoing() {
    ParamStore& P = params();
    if (sendSock_ < 0) return;
    for (int i = 0; i < kLinkStreams; ++i) {
        auto& cfg = P.link.out[i];
        OutStream& o = out_[i];
        if (!P.link.enable->on() || !cfg.on->on()) { o.ring.clear(); o.pending.clear(); outStats_[i].active = false; continue; }
        const int ch = o.ch.load();
        const int engineRate = o.rate.load();
        if (ch <= 0 || engineRate <= 0) continue;
        const size_t avail = o.ring.readable() / size_t(ch);
        if (avail == 0) continue;
        std::vector<float> in(avail * size_t(ch));
        o.ring.read(in.data(), in.size());
        int outRate = cfg.sr->i();
        if (outRate <= 0) outRate = engineRate;
        if (outRate != engineRate) {
            if (!o.src || o.srcRate != outRate || o.srcCh != ch) {
                if (o.src) src_delete(static_cast<SRC_STATE*>(o.src));
                int err = 0;
                o.src = src_new(SRC_SINC_FASTEST, ch, &err);
                o.srcRate = outRate; o.srcCh = ch;
            }
            std::vector<float> res(size_t(double(avail) * outRate / engineRate + 64) * size_t(ch));
            SRC_DATA d{};
            d.data_in = in.data(); d.input_frames = long(avail);
            d.data_out = res.data(); d.output_frames = long(res.size() / size_t(ch));
            d.src_ratio = double(outRate) / engineRate;
            if (src_process(static_cast<SRC_STATE*>(o.src), &d) == 0)
                o.pending.insert(o.pending.end(), res.begin(), res.begin() + d.output_frames_gen * ch);
        } else {
            o.pending.insert(o.pending.end(), in.begin(), in.end());
        }
        const int bitSel = cfg.bit->i();
        const int type = bitSel == 2 ? 2 : (bitSel == 4 ? 4 : 1);
        const int bps = kBytesPerType[type];
        const int nbs = std::min(256, kMaxPayload / (ch * bps));
        const std::string ipStr = P.str("roadlink.outstream[" + std::to_string(i) + "].ip");
        const uint32_t ip = resolve(ipStr);
        if (ip == 0) { o.pending.clear(); continue; }
        const std::string sname = P.str("roadlink.outstream[" + std::to_string(i) + "].name");
        sockaddr_in to{};
        to.sin_family = AF_INET; to.sin_port = htons(uint16_t(cfg.port->i())); to.sin_addr.s_addr = ip;
        size_t off = 0;
        uint8_t pkt[kHeader + kMaxPayload];
        while (o.pending.size() - off >= size_t(nbs) * size_t(ch)) {
            std::memcpy(pkt, P.link.compat->on() ? kSigCompat : kSigOwn, 4);
            pkt[4] = uint8_t(srIndex(outRate) & 0x1F) | kProtoAudio;
            pkt[5] = uint8_t(nbs - 1);
            pkt[6] = uint8_t(ch - 1);
            pkt[7] = uint8_t(type);
            std::memset(pkt + 8, 0, 16);
            std::memcpy(pkt + 8, sname.c_str(), std::min<size_t>(16, sname.size()));
            std::memcpy(pkt + 24, &o.frame, 4);
            uint8_t* p = pkt + kHeader;
            for (int k = 0; k < nbs * ch; ++k) {
                const float v = std::clamp(o.pending[off + size_t(k)], -1.0f, 1.0f);
                if (type == 1) { int16_t x = int16_t(std::lrint(v * 32767.0f)); std::memcpy(p, &x, 2); }
                else if (type == 2) { int32_t x = int32_t(std::lrint(v * 8388607.0f)); p[0] = uint8_t(x); p[1] = uint8_t(x >> 8); p[2] = uint8_t(x >> 16); }
                else std::memcpy(p, &v, 4);
                p += bps;
            }
            sendto(sendSock_, pkt, size_t(kHeader + nbs * ch * bps), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
            o.frame++;
            off += size_t(nbs) * size_t(ch);
        }
        o.pending.erase(o.pending.begin(), o.pending.begin() + long(off));
        outStats_[i].active = true;
    }
}

void RoadLinkEngine::rtInput(int strip, float* const* chOut, int nch, int n) {
    ParamStore& P = params();
    if (!P.link.enable->on()) return;
    const float fs = engineRate_.load(std::memory_order_relaxed);
    for (int i = 0; i < kLinkStreams; ++i) {
        auto& cfg = P.link.in[i];
        if (!cfg.on->on() || cfg.route->i() != strip) continue;
        InStream& s = in_[i];
        const uint32_t gen = s.generation.load(std::memory_order_acquire);
        if (gen != s.rtGeneration) { s.ring.clear(); s.rtGeneration = gen; s.pos = 0; s.primed = false; s.ratioAdj = 1; }
        const int ch = s.ch.load(std::memory_order_relaxed), rate = s.rate.load(std::memory_order_relaxed);
        if (ch <= 0 || rate <= 0) continue;
        const double step0 = double(rate) / fs;
        const double target = kQualityMs[std::clamp(cfg.quality->i(), 0, 4)] * 0.001 * rate + n * step0;
        double fill = double(s.ring.readable() / size_t(ch));
        if (!s.primed) { if (fill >= target) s.primed = true; else continue; }
        if (fill > target * 4) { // way too much buffered: drop the excess
            s.ring.skip(size_t(fill - target) * size_t(ch));
            inStats_[i].overload++;
            fill = target;
        }
        const double err = std::clamp((fill - target) / std::max(target, 1.0), -1.0, 1.0);
        s.ratioAdj = s.ratioAdj * 0.995 + (1.0 + err * 0.004) * 0.005;
        const double step = step0 * s.ratioAdj;
        if (fill < s.pos + n * step + 2) { inStats_[i].underrun++; s.primed = false; continue; }
        double pos = s.pos;
        for (int k = 0; k < n; ++k) {
            const size_t i0 = size_t(pos);
            const float fr = float(pos - double(i0));
            for (int c = 0; c < nch; ++c) {
                const int sc = ch == 1 ? 0 : c;
                if (sc >= ch) continue;
                const float a = s.ring.peek(i0 * size_t(ch) + size_t(sc));
                const float b = s.ring.peek((i0 + 1) * size_t(ch) + size_t(sc));
                chOut[c][k] += a + (b - a) * fr;
            }
            pos += step;
        }
        const size_t used = size_t(pos);
        s.ring.skip(used * size_t(ch));
        s.pos = pos - double(used);
    }
}

void RoadLinkEngine::rtOutput(float* const* bus, int n, float fs) {
    engineRate_.store(fs, std::memory_order_relaxed);
    ParamStore& P = params();
    if (!P.link.enable->on()) return;
    float tmp[1024 * 8];
    for (int i = 0; i < kLinkStreams; ++i) {
        auto& cfg = P.link.out[i];
        if (!cfg.on->on()) continue;
        OutStream& o = out_[i];
        const int ch = cfg.ch->i();
        const int b = cfg.route->i();
        o.ch.store(ch, std::memory_order_relaxed);
        o.rate.store(int(fs), std::memory_order_relaxed);
        float peak = 0;
        for (int start = 0; start < n; start += 1024) {
            const int len = std::min(1024, n - start);
            for (int k = 0; k < len; ++k)
                for (int c = 0; c < ch; ++c) {
                    const float v = bus[b * kBusCh + c][start + k];
                    tmp[k * ch + c] = v;
                    peak = std::max(peak, std::fabs(v));
                }
            o.ring.write(tmp, size_t(len) * size_t(ch));
        }
        outStats_[i].level.store(peak, std::memory_order_relaxed);
    }
}

} // namespace road
