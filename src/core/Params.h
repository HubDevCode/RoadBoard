// Central parameter store shared by the GUI, the audio thread and every
// remote-control path (CLI socket, RoadLink text, MIDI, macro buttons).
//
// Numeric parameters are lock-free atomics so the realtime thread can read
// them at any time. String parameters (labels, device names, stream names)
// live behind a mutex and are never touched from the realtime thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace road {

constexpr int kHwStrips = 5;
constexpr int kVirtStrips = 3;
constexpr int kStrips = kHwStrips + kVirtStrips;
constexpr int kHwBuses = 5;
constexpr int kVirtBuses = 3;
constexpr int kBuses = kHwBuses + kVirtBuses;
constexpr int kBusCh = 8;
constexpr int kHwStripCh = 2;
constexpr int kVirtStripCh = 8;
constexpr int kInCh = kHwStrips * kHwStripCh + kVirtStrips * kVirtStripCh; // 34
constexpr int kEqCells = 6;
constexpr int kLinkStreams = 8;
constexpr int kDelayTaps = 8;
constexpr int kC5Bands = 5;
constexpr int kExtFx = 2;
constexpr int kMaxFrames = 8192;

inline bool isHwStrip(int s) { return s < kHwStrips; }
inline int stripChannels(int s) { return isHwStrip(s) ? kHwStripCh : kVirtStripCh; }
// first input-channel index (0..33) of a strip; used by the composite patch
inline int stripFirstInCh(int s) {
    return isHwStrip(s) ? s * kHwStripCh : kHwStrips * kHwStripCh + (s - kHwStrips) * kVirtStripCh;
}

extern const char* const kBusNames[kBuses]; // "A1".."B3"

enum class BusMode : int {
    Normal = 0, MixDownA, MixDownB, Repeat, Composite, TVMix, UpMix21, UpMix41,
    UpMix61, LeftOnly, RightOnly, CenterOnly, LFEOnly, RearOnly, Count
};
extern const char* const kBusModeNames[];      // display names
extern const char* const kBusModeKeys[];       // remote names (mode.normal, mode.amix ...)

// Virtual strip channel modes (Strip[i].Mode)
enum class StripMode : int {
    Normal = 0, Mono, MixDownA, MixDownB, RepeatMono, RepeatStereo, TVMix, UpMix21,
    UpMix41, UpMix61, LeftOnly, RightOnly, CenterOnly, LFEOnly, RearOnly, Count
};
extern const char* const kStripModeNames[];

enum class EqType : int { Peak = 0, Notch, BandPass, LowPass, HighPass, LowShelf, HighShelf, Count };
extern const char* const kEqTypeNames[];

struct ReverbPreset { const char* name; float decay, size, damping, er, diffusion; };
extern const ReverbPreset kReverbPresets[];
extern const int kReverbPresetCount;

struct Param {
    std::string key;     // normalised: lower case, [] indices, e.g. "strip[0].gain"
    float min = 0, max = 1, def = 0;
    bool integer = false;
    std::atomic<float> v{0};
    std::atomic<uint32_t>* group = nullptr; // bumped on change (e.g. EQ coefficient refresh)

    float get() const { return v.load(std::memory_order_relaxed); }
    bool on() const { return get() >= 0.5f; }
    int i() const { return int(get() + (get() >= 0 ? 0.5f : -0.5f)); }
    void set(float x);   // clamps, rounds integers, bumps counters
    void reset() { set(def); }
    bool isDefault() const { return get() == def; }
};

class ParamStore {
public:
    ParamStore();
    static ParamStore& instance();

    Param* find(const std::string& key) const;      // accepts any casing / () or []
    Param& p(const std::string& key) const;          // asserts existence
    static std::string normalise(const std::string& key);

    // string values
    std::string str(const std::string& key) const;
    void setStr(const std::string& key, const std::string& value);
    bool hasStr(const std::string& key) const;
    std::vector<std::string> strKeys() const;

    const std::vector<std::unique_ptr<Param>>& all() const { return params_; }

    // Monotonic counters: any numeric / string change bumps these.
    uint32_t revision() const { return revision_.load(std::memory_order_relaxed); }
    uint32_t strRevision() const { return strRevision_.load(std::memory_order_relaxed); }
    void bump() { revision_.fetch_add(1, std::memory_order_relaxed); }

    // EQ groups bumped whenever an EQ param changes (buses, then hw strips)
    std::atomic<uint32_t> eqRevision[kBuses];
    std::atomic<uint32_t> stripEqRevision[kHwStrips];
    std::atomic<uint32_t> fxRevision{0};
    // structural config (device routing, RAIO channel count...) changed
    std::atomic<uint32_t> routingRevision{0};

    // listeners for string changes that need action (device selection...)
    using StrListener = std::function<void(const std::string& key, const std::string& value)>;
    void addStrListener(StrListener l);

    // Fast accessors used by DSP and GUI (no hashing at runtime)
    struct EqCell { Param *on, *type, *f, *gain, *q; };
    struct EqChan { EqCell cell[kEqCells]; Param *trim, *delay; };
    struct Comp { Param *gainIn, *ratio, *threshold, *attack, *release, *knee, *gainOut, *makeUp, *prg; };
    struct Gate { Param *threshold, *damping, *bpSidechain, *attack, *hold, *release, *prg; };
    struct Pitch { Param *on, *dryWet, *value, *lo, *med, *hi; };
    struct Strip {
        Param *mono, *mute, *solo, *mc, *gain, *panX, *panY, *colorX, *colorY, *fxX, *fxY,
              *comp, *gate, *denoiser, *karaoke, *limit, *eq3[3], *bus[kBuses], *gainLayer[kBuses],
              *mode, *limType, *raio;
        Param *send[4], *post[4];          // reverb, delay, fx1, fx2
        Param *denoiseThreshold, *denoiseBypass;
        Comp comp_; Gate gate_; Pitch pitch;
        Param *eqOn, *eqAB;
        EqChan eq[2][kHwStripCh];          // hardware strips only
    } strip[kStrips];
    struct Bus {
        Param *mono, *mute, *eqOn, *eqAB, *gain, *mode, *limit, *limType, *delay, *sel, *monitor, *raio;
        Param *ret[4];                     // reverb, delay, fx1, fx2 returns
        EqChan eq[2][kBusCh];
    } bus[kBuses];
    struct Opt {
        Param *sr, *buffer, *outLimiter, *vinCh[kVirtStrips], *voutCh[kVirtBuses];
        Param *composite[kBusCh], *postFaderComposite, *meterPost, *roadPanPage;
        Param *monitorOnSel, *sliderMode, *fxType, *externalFx, *raioExt;
    } opt;
    struct Reverb {
        Param *on, *ab;
        struct Mem { Param *preset, *dry, *wet, *preDelay, *er, *decay, *eqGain[3], *eqFreq[3], *bypass; } m[2];
    } reverb;
    struct Delay {
        Param *on, *ab;
        struct Tap { Param *on, *time, *gain, *pan, *lpf, *hpf; };
        struct Mem { Param *tempo, *ratio, *feedback, *dry, *wet, *preDelay, *balL, *balR, *bypass; Tap tap[kDelayTaps]; } m[2];
    } delay;
    struct C5 {
        Param *xover[kC5Bands - 1];
        struct Band { Param *on, *gain, *threshold, *ratio, *attack, *release, *knee, *outGain, *limit, *dist, *solo, *mute; } band[kC5Bands];
        Param *outGain, *makeUp, *bypass, *link;
    } c5;
    struct Rec {
        Param *play, *stop, *record, *pause, *ff, *rew, *loop, *playOnLoad, *multiTrack, *gain,
              *bus[kBuses], *recBus, *armStrip[kStrips], *armBus[kBuses], *bitResolution,
              *channels, *kbps, *fileType, *preRec, *stopAfter, *sampleRate;
    } rec;
    struct LinkIn { Param *on, *port, *quality, *route, *sr, *ch, *bit; };
    struct LinkOut { Param *on, *port, *quality, *route, *sr, *ch, *bit; };
    struct Link { Param* enable; LinkIn in[kLinkStreams]; LinkOut out[kLinkStreams]; Param* textOn; Param* textPort; Param* compat; } link;

    // knob -> detailed parameter mapping (comp / gate / denoiser knobs)
    void applyCompKnob(int strip);
    void applyGateKnob(int strip);
    // keeps linked parameters coherent (call periodically from the GUI thread):
    //  - strip fader <-> per bus gain layers (ABS / REL slider mode)
    //  - Comp / Gate / Denoiser knobs <-> detailed parameters ("PRG" state)
    void syncLinked();
    // CTRL + right click on the Comp / Gate knob: restore the detailed settings
    // that were active before the knob was moved
    bool undoComp(int strip);
    bool undoGate(int strip);

private:
    float compBackup_[kStrips][8] = {};
    float gateBackup_[kStrips][6] = {};
    bool hasCompBackup_[kStrips] = {}, hasGateBackup_[kStrips] = {};
    Param* add(const std::string& key, float min, float max, float def, bool integer = false,
               std::atomic<uint32_t>* group = nullptr);
    void addStr(const std::string& key, const std::string& def);
    void addEqChan(EqChan& ch, const std::string& prefix, std::atomic<uint32_t>* group);

    std::vector<std::unique_ptr<Param>> params_;
    std::unordered_map<std::string, Param*> index_;
    mutable std::mutex strMutex_;
    std::map<std::string, std::string> strings_;
    std::vector<StrListener> strListeners_;
    std::atomic<uint32_t> revision_{0};
    std::atomic<uint32_t> strRevision_{0};
    friend struct Param;
};

inline ParamStore& params() { return ParamStore::instance(); }

} // namespace road
