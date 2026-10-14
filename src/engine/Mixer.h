// The mixing console: 8 input strips -> 8 multichannel buses with a full
// 8x8 gain matrix (one gain layer per bus), internal FX send / return and
// external FX insert ports. process() runs on the PipeWire realtime thread.
#pragma once

#include <atomic>
#include <memory>

#include "core/Params.h"
#include "dsp/Dsp.h"

namespace road {

class TapeDeck;
class RoadLinkEngine;

// engine port layout
constexpr int kRaioInBase = kInCh;                               // 5 x stereo RAIO inputs of hw strips
constexpr int kFxReturnBase = kRaioInBase + kHwStrips * 2;        // 2 x stereo external FX returns
constexpr int kEngineIn = kFxReturnBase + kExtFx * 2;
constexpr int kFxSendBase = kBuses * kBusCh;                      // 2 x stereo external FX sends
constexpr int kEngineOut = kFxSendBase + kExtFx * 2;

struct Meters {
    std::atomic<float> stripPre[kStrips][kBusCh];
    std::atomic<float> stripPost[kStrips][kBusCh];
    std::atomic<float> bus[kBuses][kBusCh];
    std::atomic<float> stripLimGain[kStrips];
    std::atomic<float> busLimGain[kBuses];
    std::atomic<float> stripGate[kStrips];
    std::atomic<float> stripComp[kStrips];      // gain reduction dB
    std::atomic<float> stripCompIn[kStrips];    // detector input peak
    std::atomic<float> fx[4];                   // reverb, delay, fx1, fx2 output peaks
    std::atomic<uint64_t> cycles{0};
    std::atomic<float> dspLoad{0};
    Meters();
    // read and reset a peak meter (GUI side)
    static float take(std::atomic<float>& m) { return m.exchange(0.0f, std::memory_order_relaxed); }
};

class Mixer {
public:
    Mixer();
    ~Mixer();

    // realtime: in = kEngineIn channel pointers (nullptr = silence)
    //           out = kEngineOut pointers (nullptr = discard)
    void process(const float* const* in, float* const* out, int n, float sampleRate);

    // non-realtime housekeeping: rebuild DSP state for a new sample rate, free retired state
    void maintenance();
    float sampleRate() const { return stateRate_.load(); }

    void setTapeDeck(TapeDeck* t) { tape_ = t; }
    void setLink(RoadLinkEngine* v) { link_ = v; }
    Meters& meters() { return meters_; }

    static float sendGain(float level) { return level <= 0.01f ? 0.0f : dsp::dbToLin(-40.0f + 4.0f * level); }

    struct State;

private:
    void processChunk(State& s, const float* const* in, float* const* out, int n);

    std::atomic<State*> pending_{nullptr};
    std::atomic<State*> retired_{nullptr};
    State* state_ = nullptr;           // owned by the RT thread once published
    std::atomic<float> requestedRate_{48000};
    std::atomic<float> stateRate_{48000};
    Meters meters_;
    TapeDeck* tape_ = nullptr;
    RoadLinkEngine* link_ = nullptr;
};

} // namespace road
