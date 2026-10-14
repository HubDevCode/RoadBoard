// RoadLink: audio over the local network (UDP). 8 incoming + 8 outgoing audio
// streams, incoming text command stream and discovery ping replies. By default
// it uses the common packet format of existing network-audio apps so phones and
// other computers can talk to RoadBoard.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Params.h"
#include "engine/RingBuffer.h"

namespace road {

struct RoadLinkDetected {
    std::string name, ip;
    int sampleRate = 0, channels = 0, bits = 0;
    double lastSeen = 0;
};

struct RoadLinkStats {
    std::atomic<uint32_t> overload{0}, corrupt{0}, disorder{0}, missing{0}, underrun{0};
    std::atomic<bool> active{false};
    std::atomic<float> level{0};
};

class RoadLinkEngine {
public:
    RoadLinkEngine();
    ~RoadLinkEngine();

    void start();
    void stop();
    bool running() const { return running_.load(); }

    std::vector<RoadLinkDetected> detected() const;
    RoadLinkStats& inStats(int i) { return inStats_[i]; }
    RoadLinkStats& outStats(int i) { return outStats_[i]; }

    // incoming RoadLink text requests (called from the network thread)
    std::function<void(const std::string& text, const std::string& fromIp)> onText;

    // realtime
    void rtInput(int strip, float* const* ch, int nch, int n);
    void rtOutput(float* const* bus, int n, float fs);

    static int srIndex(int rate);
    static int srFromIndex(int idx);

private:
    void netLoop();
    void handlePacket(const uint8_t* data, size_t len, uint32_t fromAddr, uint16_t fromPort, int sock);
    void sendOutgoing();
    void reopenSockets();

    struct InStream {
        RingBuffer ring;
        std::atomic<int> rate{0}, ch{0};
        std::atomic<bool> primed{false};
        uint32_t lastFrame = 0;
        bool haveFrame = false;
        double pos = 0;          // fractional read position (RT)
        double ratioAdj = 1.0;   // drift correction (RT)
        std::atomic<uint32_t> generation{0};
        uint32_t rtGeneration = 0;
    } in_[kLinkStreams];

    struct OutStream {
        RingBuffer ring;
        std::atomic<int> ch{0};
        std::atomic<int> rate{0};
        uint32_t frame = 0;
        void* src = nullptr;     // SRC_STATE
        int srcRate = 0, srcCh = 0;
        std::vector<float> pending;
    } out_[kLinkStreams];

    RoadLinkStats inStats_[kLinkStreams];
    RoadLinkStats outStats_[kLinkStreams];

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::vector<std::pair<int, int>> sockets_; // (fd, port)
    int sendSock_ = -1;
    mutable std::mutex detMutex_;
    std::vector<RoadLinkDetected> detected_;
    std::atomic<float> engineRate_{48000};
};

} // namespace road
