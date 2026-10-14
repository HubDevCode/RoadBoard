#include "core/Params.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>

namespace road {

const char* const kBusNames[kBuses] = {"A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"};

const ReverbPreset kReverbPresets[] = {
    {"Small Room", 0.45f, 0.35f, 0.45f, 0.8f, 0.6f},   {"Medium Room", 0.8f, 0.5f, 0.4f, 0.7f, 0.65f},
    {"Large Room", 1.2f, 0.65f, 0.35f, 0.6f, 0.7f},    {"Vocal Plate", 1.6f, 0.55f, 0.25f, 0.3f, 0.85f},
    {"Bright Plate", 2.0f, 0.6f, 0.12f, 0.25f, 0.9f},  {"Chamber", 1.4f, 0.6f, 0.4f, 0.5f, 0.75f},
    {"Small Hall", 1.8f, 0.75f, 0.35f, 0.45f, 0.8f},   {"Concert Hall", 2.6f, 0.9f, 0.3f, 0.4f, 0.85f},
    {"Large Hall", 3.5f, 1.0f, 0.3f, 0.35f, 0.85f},    {"Church", 4.5f, 1.0f, 0.45f, 0.3f, 0.8f},
    {"Cathedral", 7.0f, 1.0f, 0.5f, 0.25f, 0.85f},     {"Ambience", 0.3f, 0.3f, 0.5f, 1.0f, 0.5f},
};
const int kReverbPresetCount = int(sizeof(kReverbPresets) / sizeof(kReverbPresets[0]));

const char* const kBusModeNames[] = {
    "Normal mode", "Mix Down A", "Mix Down B", "Stereo Repeat", "Composite", "Up Mix TV",
    "Up Mix 2.1", "Up Mix 4.1", "Up Mix 6.1", "Left Only", "Right Only", "Center Only",
    "LFE Only", "Rear Only"};
const char* const kBusModeKeys[] = {
    "normal", "amix", "bmix", "repeat", "composite", "tvmix", "upmix21", "upmix41",
    "upmix61", "leftonly", "rightonly", "centeronly", "lfeonly", "rearonly"};

const char* const kStripModeNames[] = {
    "Normal", "Mono", "Mix Down A", "Mix Down B", "Repeat Mono", "Repeat Stereo", "Up Mix TV",
    "Up Mix 2.1", "Up Mix 4.1", "Up Mix 6.1", "Left Only", "Right Only", "Center Only",
    "LFE Only", "Rear Only"};

const char* const kEqTypeNames[] = {"Peak", "Notch", "Band Pass", "Low Pass", "High Pass",
                                    "Low Shelf", "High Shelf"};

void Param::set(float x) {
    if (!std::isfinite(x)) return;
    x = std::clamp(x, min, max);
    if (integer) x = std::round(x);
    float old = v.exchange(x, std::memory_order_relaxed);
    if (old != x) {
        if (group) group->fetch_add(1, std::memory_order_relaxed);
        ParamStore::instance().bump();
    }
}

ParamStore& ParamStore::instance() {
    static ParamStore store;
    return store;
}

std::string ParamStore::normalise(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == '(') c = '[';
        else if (c == ')') c = ']';
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

Param* ParamStore::add(const std::string& key, float min, float max, float def, bool integer,
                       std::atomic<uint32_t>* group) {
    auto p = std::make_unique<Param>();
    p->key = normalise(key);
    p->min = min;
    p->max = max;
    p->def = def;
    p->integer = integer;
    p->v.store(def);
    p->group = group;
    Param* raw = p.get();
    index_[raw->key] = raw;
    params_.push_back(std::move(p));
    return raw;
}

void ParamStore::addStr(const std::string& key, const std::string& def) {
    strings_[normalise(key)] = def;
}

ParamStore::ParamStore() {
    for (auto& r : eqRevision) r.store(0);
    for (auto& r : stripEqRevision) r.store(0);
    auto idx = [](const char* base, int i) { return std::string(base) + "[" + std::to_string(i) + "]"; };
    std::atomic<uint32_t>* fx = &fxRevision;

    for (int s = 0; s < kStrips; ++s) {
        const std::string b = idx("strip", s) + ".";
        const bool hw = isHwStrip(s);
        Strip& st = strip[s];
        st.mono = add(b + "mono", 0, 1, 0, true);
        st.mute = add(b + "mute", 0, 1, 0, true);
        st.solo = add(b + "solo", 0, 1, 0, true);
        st.mc = add(b + "mc", 0, 1, 0, true);
        st.gain = add(b + "gain", -60, 12, 0);
        st.panX = add(b + "pan_x", -0.5f, 0.5f, 0);
        st.panY = hw ? add(b + "pan_y", 0, 1, 0.5f) : add(b + "pan_y", -0.5f, 0.5f, 0);
        st.colorX = add(b + "color_x", -0.5f, 0.5f, 0);
        st.colorY = add(b + "color_y", 0, 1, 0.5f);
        st.fxX = add(b + "fx_x", -0.5f, 0.5f, 0);
        st.fxY = add(b + "fx_y", 0, 1, 0.5f);
        st.comp = add(b + "comp", 0, 10, 0);
        st.gate = add(b + "gate", 0, 10, 0);
        st.denoiser = add(b + "denoiser", 0, 10, 0);
        st.denoiseThreshold = add(b + "denoiser.threshold", 0, 10, 0);
        st.denoiseBypass = add(b + "denoiser.bypass", 0, 1, 0, true);
        st.karaoke = add(b + "karaoke", 0, 4, 0, true);
        st.limit = add(b + "limit", -40, 12, 12);
        st.eq3[0] = add(b + "eqgain1", -12, 12, 0);
        st.eq3[1] = add(b + "eqgain2", -12, 12, 0);
        st.eq3[2] = add(b + "eqgain3", -12, 12, 0);
        for (int k = 0; k < kBuses; ++k) {
            st.bus[k] = add(b + std::string(kBusNames[k]), 0, 1, (k == 0) ? 1 : 0, true);
            st.gainLayer[k] = add(b + "gainlayer[" + std::to_string(k) + "]", -60, 12, 0);
        }
        st.mode = add(b + "mode", 0, int(StripMode::Count) - 1, 0, true);
        st.limType = add(idx("option.limtypein", s), 0, 1, 0, true);
        st.raio = add(b + "raio", 0, 1, 1, true);
        static const char* const sends[4] = {"reverb", "delay", "fx1", "fx2"};
        for (int k = 0; k < 4; ++k) {
            st.send[k] = add(b + sends[k], 0, 10, 0);
            st.post[k] = add(b + "post" + sends[k], 0, 1, 0, true);
        }
        Comp& c = st.comp_;
        c.gainIn = add(b + "comp.gainin", -24, 24, 0);
        c.ratio = add(b + "comp.ratio", 1, 8, 1);
        c.threshold = add(b + "comp.threshold", -40, -3, -20);
        c.attack = add(b + "comp.attack", 0, 200, 10);
        c.release = add(b + "comp.release", 0, 5000, 150);
        c.knee = add(b + "comp.knee", 0, 1, 0.5f);
        c.gainOut = add(b + "comp.gainout", -24, 24, 0);
        c.makeUp = add(b + "comp.makeup", 0, 1, 0, true);
        c.prg = add(b + "comp.prg", 0, 1, 0, true);
        Gate& g = st.gate_;
        g.threshold = add(b + "gate.threshold", -60, -10, -60);
        g.damping = add(b + "gate.damping", -60, -10, -60);
        g.bpSidechain = add(b + "gate.bpsidechain", 0, 4000, 0);
        g.attack = add(b + "gate.attack", 0, 1000, 1);
        g.hold = add(b + "gate.hold", 0, 5000, 80);
        g.release = add(b + "gate.release", 0, 5000, 150);
        g.prg = add(b + "gate.prg", 0, 1, 0, true);
        Pitch& pt = st.pitch;
        pt.on = add(b + "pitch.on", 0, 1, 0, true);
        pt.dryWet = add(b + "pitch.drywet", -100, 100, 100);
        pt.value = add(b + "pitch.pitchvalue", -12, 12, 0);
        pt.lo = add(b + "pitch.loformant", -12, 12, 0);
        pt.med = add(b + "pitch.medformant", -12, 12, 0);
        pt.hi = add(b + "pitch.hiformant", -12, 12, 0);
        std::atomic<uint32_t>* eqg = hw ? &stripEqRevision[s] : nullptr;
        st.eqOn = add(b + "eq.on", 0, 1, 0, true, eqg);
        st.eqAB = add(b + "eq.ab", 0, 1, 0, true, eqg);
        if (hw)
            for (int m = 0; m < 2; ++m)
                for (int c2 = 0; c2 < kHwStripCh; ++c2)
                    addEqChan(st.eq[m][c2], b + "eq.ab[" + std::to_string(m) + "].channel[" + std::to_string(c2) + "].", eqg);
        addStr(b + "label", "");
        if (hw) addStr(b + "device", "");
    }

    for (int b = 0; b < kBuses; ++b) {
        const std::string pfx = idx("bus", b) + ".";
        Bus& bu = bus[b];
        bu.mono = add(pfx + "mono", 0, 2, 0, true);
        bu.mute = add(pfx + "mute", 0, 1, 0, true);
        bu.eqOn = add(pfx + "eq.on", 0, 1, 0, true, &eqRevision[b]);
        bu.eqAB = add(pfx + "eq.ab", 0, 1, 0, true, &eqRevision[b]);
        bu.gain = add(pfx + "gain", -60, 12, 0);
        bu.mode = add(pfx + "mode", 0, int(BusMode::Count) - 1, 0, true);
        bu.limit = add(pfx + "limit", -20, 12, 0);
        bu.limType = add(idx("option.limtypeout", b), 0, 1, 0, true);
        bu.delay = add(idx("option.delay", b), 0, 500, 0);
        bu.sel = add(pfx + "sel", 0, 1, 0, true);
        bu.monitor = add(pfx + "monitor", 0, 1, 0, true);
        bu.raio = add(pfx + "raio", 0, 1, 1, true);
        static const char* const rets[4] = {"returnreverb", "returndelay", "returnfx1", "returnfx2"};
        for (int k = 0; k < 4; ++k) bu.ret[k] = add(pfx + rets[k], 0, 10, 0);
        for (int m = 0; m < 2; ++m)
            for (int c = 0; c < kBusCh; ++c)
                addEqChan(bu.eq[m][c], pfx + "eq.ab[" + std::to_string(m) + "].channel[" + std::to_string(c) + "].", &eqRevision[b]);
        addStr(pfx + "label", "");
        if (b < kHwBuses) addStr(pfx + "device", "");
    }

    opt.sr = add("option.sr", 0, 192000, 0, true);              // 0 = follow PipeWire graph
    opt.buffer = add("option.buffer", 0, 8192, 512, true);       // 0 = PipeWire default
    opt.outLimiter = add("option.outlimiter", 0, 1, 1, true);
    for (int v = 0; v < kVirtStrips; ++v) opt.vinCh[v] = add(idx("option.raio.inch", v), 1, 8, 2, true);
    for (int v = 0; v < kVirtBuses; ++v) opt.voutCh[v] = add(idx("option.raio.outch", v), 1, 8, 2, true);
    for (int c = 0; c < kBusCh; ++c) opt.composite[c] = add(idx("patch.composite", c), 0, kInCh, 0, true);
    opt.postFaderComposite = add("patch.postfadercomposite", 0, 1, 0, true);
    opt.meterPost = add("option.meterpostfader", 0, 1, 0, true);
    opt.roadPanPage = add("option.roadpanpage", 0, 2, 0, true);
    opt.monitorOnSel = add("option.monitoronsel", 0, 1, 0, true);
    opt.sliderMode = add("option.slidermode", 0, 1, 0, true);
    opt.fxType = add("option.fxtype", 0, 1, 0, true);           // 0 = MultiTap delay, 1 = 5-band compressor
    opt.externalFx = add("option.externalfx", 0, 1, 0, true);
    opt.raioExt = add("option.raioextension", 0, 1, 1, true);

    // ---- internal FX
    reverb.on = add("fx.reverb.on", 0, 1, 1, true, fx);
    reverb.ab = add("fx.reverb.ab", 0, 1, 0, true, fx);
    for (int m = 0; m < 2; ++m) {
        const std::string r = "fx.reverb.ab[" + std::to_string(m) + "].";
        auto& M = reverb.m[m];
        M.preset = add(r + "preset", 0, float(kReverbPresetCount - 1), 3, true, fx);
        M.dry = add(r + "dry", -60, 0, -60, false, fx);
        M.wet = add(r + "wet", -60, 12, 0, false, fx);
        M.preDelay = add(r + "predelay", 0, 1000, 10, false, fx);
        M.er = add(r + "er", 0, 100, 50, false, fx);
        M.decay = add(r + "decay", 50, 200, 100, false, fx);
        static const float fdef[3] = {200, 1500, 6000};
        static const char* const bn[3] = {"lo", "mid", "hi"};
        for (int k = 0; k < 3; ++k) {
            M.eqGain[k] = add(r + "eq" + bn[k] + "gain", -12, 12, 0, false, fx);
            M.eqFreq[k] = add(r + "eq" + bn[k] + "freq", 20, 20000, fdef[k], false, fx);
        }
        M.bypass = add(r + "bypass", 0, 2, 0, true, fx);           // 0 none, 1 all, 2 EQ only
    }
    delay.on = add("fx.delay.on", 0, 1, 1, true, fx);
    delay.ab = add("fx.delay.ab", 0, 1, 0, true, fx);
    for (int m = 0; m < 2; ++m) {
        const std::string d = "fx.delay.ab[" + std::to_string(m) + "].";
        auto& M = delay.m[m];
        M.tempo = add(d + "tempo", 30, 300, 120, false, fx);
        M.ratio = add(d + "ratio", 0.25f, 4, 1, false, fx);
        M.feedback = add(d + "feedback", 0, 95, 25, false, fx);
        M.dry = add(d + "dry", -60, 0, -60, false, fx);
        M.wet = add(d + "wet", -60, 12, 0, false, fx);
        M.preDelay = add(d + "predelay", 0, 1000, 0, false, fx);
        M.balL = add(d + "balancel", -1, 1, -1, false, fx);
        M.balR = add(d + "balancer", -1, 1, 1, false, fx);
        M.bypass = add(d + "bypass", 0, 1, 0, true, fx);
        static const float tdef[kDelayTaps] = {250, 500, 750, 1000, 1250, 1500, 1750, 2000};
        static const float pdef[kDelayTaps] = {-0.6f, 0.6f, -0.3f, 0.3f, -0.8f, 0.8f, 0, 0};
        for (int t = 0; t < kDelayTaps; ++t) {
            const std::string tp = d + "tap[" + std::to_string(t) + "].";
            auto& T = M.tap[t];
            T.on = add(tp + "on", 0, 1, t < 4 ? 1 : 0, true, fx);
            T.time = add(tp + "time", 1, 4000, tdef[t], false, fx);
            T.gain = add(tp + "gain", -60, 6, -3.0f * float(t + 1), false, fx);
            T.pan = add(tp + "pan", -1, 1, pdef[t], false, fx);
            T.lpf = add(tp + "lpf", 200, 20000, 12000, false, fx);
            T.hpf = add(tp + "hpf", 20, 5000, 80, false, fx);
        }
    }
    static const float xdef[kC5Bands - 1] = {120, 500, 2000, 6000};
    for (int k = 0; k < kC5Bands - 1; ++k) c5.xover[k] = add(idx("fx.c5.xover", k), 30, 16000, xdef[k], false, fx);
    for (int bnd = 0; bnd < kC5Bands; ++bnd) {
        const std::string c = idx("fx.c5.band", bnd) + ".";
        auto& B = c5.band[bnd];
        B.on = add(c + "on", 0, 1, 1, true, fx);
        B.gain = add(c + "gain", -24, 24, 0, false, fx);
        B.threshold = add(c + "threshold", -60, 0, -20, false, fx);
        B.ratio = add(c + "ratio", 1, 20, 2.5f, false, fx);
        B.attack = add(c + "attack", 0.1f, 200, 10, false, fx);
        B.release = add(c + "release", 5, 5000, 150, false, fx);
        B.knee = add(c + "knee", 0, 1, 0.5f, false, fx);
        B.outGain = add(c + "outgain", -24, 24, 0, false, fx);
        B.limit = add(c + "limit", -30, 0, 0, false, fx);
        B.dist = add(c + "distortion", 0, 1, 0.3f, false, fx);
        B.solo = add(c + "solo", 0, 1, 0, true, fx);
        B.mute = add(c + "mute", 0, 1, 0, true, fx);
    }
    c5.outGain = add("fx.c5.outgain", -24, 24, 0, false, fx);
    c5.makeUp = add("fx.c5.makeup", 0, 1, 0, true, fx);
    c5.bypass = add("fx.c5.bypass", 0, 1, 0, true, fx);
    c5.link = add("fx.c5.link", 0, 2, 0, true, fx);

    rec.play = add("recorder.play", 0, 1, 0, true);
    rec.stop = add("recorder.stop", 0, 1, 0, true);
    rec.record = add("recorder.record", 0, 1, 0, true);
    rec.pause = add("recorder.pause", 0, 1, 0, true);
    rec.ff = add("recorder.ff", 0, 1, 0, true);
    rec.rew = add("recorder.rew", 0, 1, 0, true);
    rec.loop = add("recorder.mode.loop", 0, 1, 0, true);
    rec.playOnLoad = add("recorder.mode.playonload", 0, 1, 1, true);
    rec.multiTrack = add("recorder.mode.multitrack", 0, 1, 0, true);
    rec.gain = add("recorder.gain", -60, 12, 0);
    for (int k = 0; k < kBuses; ++k) rec.bus[k] = add("recorder." + std::string(kBusNames[k]), 0, 1, k == 0 ? 1 : 0, true);
    rec.recBus = add("recorder.mode.recbus", 0, 1, 0, true);
    for (int s = 0; s < kStrips; ++s) rec.armStrip[s] = add(idx("recorder.armstrip", s), 0, 1, 1, true);
    for (int b = 0; b < kBuses; ++b) rec.armBus[b] = add(idx("recorder.armbus", b), 0, 1, b == 0 ? 1 : 0, true);
    rec.bitResolution = add("recorder.bitresolution", 8, 32, 24, true);
    rec.channels = add("recorder.channel", 1, 8, 2, true);
    rec.kbps = add("recorder.kbps", 32, 320, 192, true);
    rec.fileType = add("recorder.filetype", 1, 100, 1, true);  // 1 WAV 2 AIFF 3 BWF 4 FLAC 5 OGG 100 MP3
    rec.preRec = add("recorder.prerecord", 0, 20, 0, true);
    rec.stopAfter = add("recorder.stopafter", 0, 86400, 0, true);
    rec.sampleRate = add("recorder.samplerate", 0, 192000, 0, true);
    addStr("recorder.prefix", "Record");
    addStr("recorder.folder", "");
    addStr("recorder.file", "");

    link.enable = add("roadlink.enable", 0, 1, 0, true);
    for (int i = 0; i < kLinkStreams; ++i) {
        const std::string in = idx("roadlink.instream", i) + ".";
        link.in[i].on = add(in + "on", 0, 1, 0, true);
        link.in[i].port = add(in + "port", 1, 65535, 6980, true);
        link.in[i].quality = add(in + "quality", 0, 4, 1, true);
        link.in[i].route = add(in + "route", 0, kStrips - 1, std::min(i, kStrips - 1), true);
        link.in[i].sr = add(in + "sr", 0, 768000, 0, true);
        link.in[i].ch = add(in + "channel", 0, 8, 0, true);
        link.in[i].bit = add(in + "bit", 0, 7, 0, true);
        addStr(in + "name", "Stream" + std::to_string(i + 1));
        addStr(in + "ip", "0.0.0.0");
        const std::string out = idx("roadlink.outstream", i) + ".";
        link.out[i].on = add(out + "on", 0, 1, 0, true);
        link.out[i].port = add(out + "port", 1, 65535, 6980, true);
        link.out[i].quality = add(out + "quality", 0, 4, 0, true);
        link.out[i].route = add(out + "route", 0, kBuses - 1, std::min(i, kBuses - 1), true);
        link.out[i].sr = add(out + "sr", 0, 96000, 0, true);
        link.out[i].ch = add(out + "channel", 1, 8, 2, true);
        link.out[i].bit = add(out + "bit", 1, 4, 1, true);
        addStr(out + "name", "Stream" + std::to_string(i + 1));
        addStr(out + "ip", "127.0.0.1");
    }
    link.textOn = add("roadlink.instream.text.on", 0, 1, 0, true);
    link.textPort = add("roadlink.instream.text.port", 1, 65535, 6980, true);
    link.compat = add("roadlink.compatible", 0, 1, 1, true);
    addStr("roadlink.instream.text.name", "Command1");
    addStr("roadlink.name", "RoadBoard");
}

void ParamStore::addEqChan(EqChan& ch, const std::string& e, std::atomic<uint32_t>* group) {
    static const float kDefF[kEqCells] = {63, 180, 500, 1500, 4000, 10000};
    for (int k = 0; k < kEqCells; ++k) {
        const std::string cell = e + "cell[" + std::to_string(k) + "].";
        ch.cell[k].on = add(cell + "on", 0, 1, 0, true, group);
        ch.cell[k].type = add(cell + "type", 0, int(EqType::Count) - 1, 0, true, group);
        ch.cell[k].f = add(cell + "f", 20, 20000, kDefF[k], false, group);
        ch.cell[k].gain = add(cell + "gain", -36, 18, 0, false, group);
        ch.cell[k].q = add(cell + "q", 0.3f, 100, 2.0f, false, group);
    }
    ch.trim = add(e + "trim", -24, 24, 0, false, group);
    ch.delay = add(e + "delay", 0, 500, 0, false, group);
}

void ParamStore::applyCompKnob(int s) {
    const float k = strip[s].comp->get();
    Comp& c = strip[s].comp_;
    c.gainIn->set(0);
    c.ratio->set(1.0f + 0.5f * k);            // 1:1 .. 6:1
    c.threshold->set(-3.0f - 2.4f * k);       // -3 .. -27 dB
    c.attack->set(10);
    c.release->set(150);
    c.knee->set(0.6f);
    c.gainOut->set(0);
    c.makeUp->set(k > 0 ? 1 : 0);
    c.prg->set(0);
}

void ParamStore::applyGateKnob(int s) {
    const float k = strip[s].gate->get();
    Gate& g = strip[s].gate_;
    g.threshold->set(k <= 0 ? -60.0f : -60.0f + 5.0f * k);   // off .. -10 dB
    g.damping->set(-60);
    g.bpSidechain->set(0);
    g.attack->set(1);
    g.hold->set(80);
    g.release->set(150);
    g.prg->set(0);
}

void ParamStore::syncLinked() {
    struct Snap { float gain, layer[kBuses], comp, gate, den, denT, compD[8], gateD[6]; };
    static Snap last[kStrips];
    static bool init = false;
    auto compVals = [](const Comp& c, float* v) {
        v[0] = c.gainIn->get(); v[1] = c.ratio->get(); v[2] = c.threshold->get(); v[3] = c.attack->get();
        v[4] = c.release->get(); v[5] = c.knee->get(); v[6] = c.gainOut->get(); v[7] = c.makeUp->get();
    };
    auto gateVals = [](const Gate& g, float* v) {
        v[0] = g.threshold->get(); v[1] = g.damping->get(); v[2] = g.bpSidechain->get();
        v[3] = g.attack->get(); v[4] = g.hold->get(); v[5] = g.release->get();
    };
    auto snap = [&](int s) {
        Snap& n = last[s];
        n.gain = strip[s].gain->get();
        for (int b = 0; b < kBuses; ++b) n.layer[b] = strip[s].gainLayer[b]->get();
        n.comp = strip[s].comp->get(); n.gate = strip[s].gate->get();
        n.den = strip[s].denoiser->get(); n.denT = strip[s].denoiseThreshold->get();
        compVals(strip[s].comp_, n.compD); gateVals(strip[s].gate_, n.gateD);
    };
    if (!init) { for (int s = 0; s < kStrips; ++s) snap(s); init = true; return; }
    const bool rel = opt.sliderMode->on();
    for (int s = 0; s < kStrips; ++s) {
        Strip& st = strip[s];
        Snap& l = last[s];
        // fader <-> layers
        const float g = st.gain->get();
        if (g != l.gain) {
            for (int b = 0; b < kBuses; ++b) st.gainLayer[b]->set(rel ? st.gainLayer[b]->get() + (g - l.gain) : g);
        } else {
            bool changed = false;
            float mx = -60;
            for (int b = 0; b < kBuses; ++b) {
                changed |= st.gainLayer[b]->get() != l.layer[b];
                mx = std::max(mx, st.gainLayer[b]->get());
            }
            if (changed) st.gain->set(mx);
        }
        // knobs <-> detailed parameters
        if (st.comp->get() != l.comp) {
            if (st.comp_.prg->on()) { std::copy(l.compD, l.compD + 8, compBackup_[s]); hasCompBackup_[s] = true; }
            applyCompKnob(s);
        }
        else {
            float v[8]; compVals(st.comp_, v);
            if (!std::equal(v, v + 8, l.compD)) st.comp_.prg->set(1);
        }
        if (st.gate->get() != l.gate) {
            if (st.gate_.prg->on()) { std::copy(l.gateD, l.gateD + 6, gateBackup_[s]); hasGateBackup_[s] = true; }
            applyGateKnob(s);
        }
        else {
            float v[6]; gateVals(st.gate_, v);
            if (!std::equal(v, v + 6, l.gateD)) st.gate_.prg->set(1);
        }
        if (st.denoiser->get() != l.den) { st.denoiseThreshold->set(st.denoiser->get()); st.pitch.on->set(0); }
        else if (st.denoiseThreshold->get() != l.denT) st.denoiser->set(st.denoiseThreshold->get());
        snap(s);
    }
}

bool ParamStore::undoComp(int s) {
    if (!hasCompBackup_[s]) return false;
    Comp& c = strip[s].comp_;
    Param* ps[8] = {c.gainIn, c.ratio, c.threshold, c.attack, c.release, c.knee, c.gainOut, c.makeUp};
    for (int k = 0; k < 8; ++k) ps[k]->set(compBackup_[s][k]);
    c.prg->set(1);
    return true;
}

bool ParamStore::undoGate(int s) {
    if (!hasGateBackup_[s]) return false;
    Gate& g = strip[s].gate_;
    Param* ps[6] = {g.threshold, g.damping, g.bpSidechain, g.attack, g.hold, g.release};
    for (int k = 0; k < 6; ++k) ps[k]->set(gateBackup_[s][k]);
    g.prg->set(1);
    return true;
}

Param* ParamStore::find(const std::string& key) const {
    auto it = index_.find(normalise(key));
    return it == index_.end() ? nullptr : it->second;
}

Param& ParamStore::p(const std::string& key) const {
    Param* x = find(key);
    assert(x && "unknown parameter");
    return *x;
}

std::string ParamStore::str(const std::string& key) const {
    std::lock_guard<std::mutex> lk(strMutex_);
    auto it = strings_.find(normalise(key));
    return it == strings_.end() ? std::string() : it->second;
}

bool ParamStore::hasStr(const std::string& key) const {
    std::lock_guard<std::mutex> lk(strMutex_);
    return strings_.count(normalise(key)) != 0;
}

std::vector<std::string> ParamStore::strKeys() const {
    std::lock_guard<std::mutex> lk(strMutex_);
    std::vector<std::string> out;
    for (auto& kv : strings_) out.push_back(kv.first);
    return out;
}

void ParamStore::setStr(const std::string& key, const std::string& value) {
    const std::string k = normalise(key);
    std::vector<StrListener> listeners;
    {
        std::lock_guard<std::mutex> lk(strMutex_);
        auto it = strings_.find(k);
        if (it == strings_.end() || it->second == value) return;
        it->second = value;
        listeners = strListeners_;
    }
    strRevision_.fetch_add(1, std::memory_order_relaxed);
    bump();
    for (auto& l : listeners) l(k, value);
}

void ParamStore::addStrListener(StrListener l) {
    std::lock_guard<std::mutex> lk(strMutex_);
    strListeners_.push_back(std::move(l));
}

} // namespace road
