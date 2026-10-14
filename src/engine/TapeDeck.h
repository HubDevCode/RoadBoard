// Integrated cassette player / recorder.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/Params.h"
#include "engine/RingBuffer.h"

namespace road {

class TapeDeck {
public:
    TapeDeck();
    ~TapeDeck();

    // ---- control (any non realtime thread)
    bool load(const std::string& path, std::string* error = nullptr);
    void eject();
    void play();
    void replay();             // play from 0
    void stop();
    void pause();
    void setRecord(bool on);
    void seek(double seconds);
    void skip(double seconds) { seek(position() + seconds); }

    bool loaded() const { return loaded_.load(); }
    bool playing() const { return playing_.load(); }
    bool paused() const { return paused_.load(); }
    bool recording() const { return recording_.load(); }
    double position() const;          // seconds
    double duration() const { return duration_.load(); }
    double recordSeconds() const;
    std::string fileName() const;
    std::string lastRecordFile() const;
    std::string lastError() const;
    int fileChannels() const { return playCh_.load(); }
    int fileRate() const { return fileRate_.load(); }

    static std::string defaultFolder();
    static std::string fileExtension(int fileType);

    // events for macro buttons: "play", "stop", "record", "eof"
    std::function<void(const std::string&)> onEvent;
    // call periodically from a non realtime thread (GUI timer): dispatches events,
    // applies "stop record after" and keeps the recorder.* status params in sync
    void poll();

    // ---- realtime
    void rtPlay(float* const* bus, int n, float fs);
    void rtRecord(const float* const* raw, float* const* bus, int n, float fs);

private:
    void decoderLoop();
    void writerLoop();
    void stopDecoder();
    void emitEvent(const std::string& e) { if (onEvent) onEvent(e); }
    bool openRecordFiles(int channels, int rate, std::string* err);
    void closeRecordFiles();

    mutable std::mutex mutex_;
    std::string path_, lastRecord_, error_;

    // playback
    std::thread decoder_;
    std::atomic<bool> decoderRun_{false};
    RingBuffer playRing_;
    std::atomic<int> playCh_{2};
    std::atomic<int> fileRate_{0};
    std::atomic<bool> loaded_{false}, playing_{false}, paused_{false};
    std::atomic<double> duration_{0};
    std::atomic<int64_t> playedFrames_{0};      // at engine rate since seek origin
    std::atomic<double> seekOrigin_{0};
    std::atomic<double> seekRequest_{-1};
    std::atomic<bool> flushRequest_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> eofPending_{false};
    std::atomic<bool> recStartedPending_{false};
    int ffmpegPid_ = -1;
    std::atomic<float> engineRate_{48000};
    float playGain_ = 1.0f;

    // recording
    std::thread writer_;
    std::atomic<bool> writerRun_{true};
    RingBuffer recRing_;
    std::atomic<int> recCh_{2};
    std::atomic<bool> recording_{false};
    std::atomic<bool> recRequest_{false};
    std::atomic<int64_t> recFrames_{0};
    std::atomic<int> recRate_{48000};
    struct Files;
    Files* files_ = nullptr;
};

} // namespace road
