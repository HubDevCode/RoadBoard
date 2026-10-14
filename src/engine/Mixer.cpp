#include "engine/Mixer.h"

#include <chrono>
#include <cstring>

#include "dsp/Fx.h"
#include "engine/RoadLink.h"
#include "engine/TapeDeck.h"

namespace road {

using namespace dsp;

Meters::Meters() {
    for (int s = 0; s < kStrips; ++s) {
        for (int c = 0; c < kBusCh; ++c) { stripPre[s][c] = 0; stripPost[s][c] = 0; }
        stripLimGain[s] = 1; stripGate[s] = 1; stripComp[s] = 0; stripCompIn[s] = 0;
    }
    for (int b = 0; b < kBuses; ++b) {
        for (int c = 0; c < kBusCh; ++c) bus[b][c] = 0;
        busLimGain[b] = 1;
    }
    for (auto& f : fx) f = 0;
}

static inline void meterMax(std::atomic<float>& m, float v) {
    float cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

static inline void addRamp(float* dst, const float* src, int n, float g0, float g1) {
    if (g0 == g1) {
        if (g1 == 0.0f) return;
        if (g1 == 1.0f) { for (int i = 0; i < n; ++i) dst[i] += src[i]; return; }
        for (int i = 0; i < n; ++i) dst[i] += src[i] * g1;
        return;
    }
    const float step = (g1 - g0) / float(n);
    float g = g0;
    for (int i = 0; i < n; ++i) { g += step; dst[i] += src[i] * g; }
}

// All DSP state for a given sample rate. Built off the RT thread, swapped in atomically.
struct Mixer::State {
    float fs;
    std::vector<float> raw[kInCh];                 // pre-fader inputs (recorder, composite PRE)
    std::vector<float> strip[kStrips][kBusCh];     // processed strip (pre-fader)
    std::vector<float> post[kStrips][kBusCh];      // post-fader (composite POST)
    std::vector<float> bus[kBuses][kBusCh];
    std::vector<float> scratch[kBusCh];
    std::vector<float> fxIn[4][2], fxOut[4][2];

    struct StripDsp {
        Gate gate; Compressor comp; GatePro gatePro; CompressorPro compPro; Denoiser denoise; VoiceModeler voice;
        ColorPanel color; Modulation mod; Binaural bin;
        Karaoke kar; ThreeBandEq eq3; PanPot51 pan; Limiter lim;
        Biquad peq[kHwStripCh][kEqCells]; bool peqOn[kEqCells] = {}; uint32_t peqRev = ~0u; int peqAB = -1;
        float layer[kBuses] = {}; bool layerInit = false;
        float audible = 1, postGain = 1, mc = 1;
        float sendG[4] = {};
    } sd[kStrips];

    struct BusDsp {
        Biquad eq[kBusCh][kEqCells];
        bool cellOn[kBusCh][kEqCells] = {};
        float trim[kBusCh] = {1, 1, 1, 1, 1, 1, 1, 1};
        int eqDelay[kBusCh] = {};
        bool chActive[kBusCh] = {};
        uint32_t eqRev = ~0u;
        int eqAB = -1;
        DelayLine delay[kBusCh];
        Limiter lim;
        GainRamp fader;
        float ret[4] = {};
    } bd[kBuses];

    ReverbFx reverb;
    MultiTapDelay delay;
    MultibandComp c5;

    explicit State(float rate) : fs(rate) {
        auto alloc = [](std::vector<float>& v) { v.assign(kMaxFrames, 0.0f); };
        for (auto& v : raw) alloc(v);
        for (auto& s : strip) for (auto& v : s) alloc(v);
        for (auto& s : post) for (auto& v : s) alloc(v);
        for (auto& b : bus) for (auto& v : b) alloc(v);
        for (auto& v : scratch) alloc(v);
        for (auto& f : fxIn) for (auto& v : f) alloc(v);
        for (auto& f : fxOut) for (auto& v : f) alloc(v);
        for (int s = 0; s < kStrips; ++s) {
            StripDsp& d = sd[s];
            d.gate.prepare(fs); d.comp.prepare(fs); d.gatePro.prepare(fs); d.compPro.prepare(fs);
            if (isHwStrip(s)) { d.denoise.prepare(fs); d.voice.prepare(fs); }
            d.color.prepare(fs); d.mod.prepare(fs); d.bin.prepare(fs); d.kar.prepare(fs); d.eq3.prepare(fs);
            d.lim.prepare(fs, stripChannels(s));
        }
        const int maxDelay = int(fs * 1.0f) + 16;
        for (int b = 0; b < kBuses; ++b) {
            for (int c = 0; c < kBusCh; ++c) bd[b].delay[c].prepare(maxDelay);
            bd[b].lim.prepare(fs, kBusCh);
        }
        reverb.prepare(fs);
        delay.prepare(fs);
        c5.prepare(fs);
    }
};

Mixer::Mixer() { state_ = new State(48000); }

Mixer::~Mixer() {
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete state_;
}

void Mixer::maintenance() {
    if (State* r = retired_.exchange(nullptr)) delete r;
    const float want = requestedRate_.load();
    if (want > 0 && want != stateRate_.load() && pending_.load() == nullptr) {
        State* s = new State(want);
        State* expected = nullptr;
        if (!pending_.compare_exchange_strong(expected, s)) delete s;
    }
}

void Mixer::process(const float* const* in, float* const* out, int n, float sampleRate) {
    const auto t0 = std::chrono::steady_clock::now();
    if (State* p = pending_.exchange(nullptr)) {
        State* old = state_;
        state_ = p;
        stateRate_.store(p->fs);
        State* expected = nullptr;
        if (!retired_.compare_exchange_strong(expected, old)) delete old;
    }
    if (sampleRate > 0 && sampleRate != requestedRate_.load(std::memory_order_relaxed))
        requestedRate_.store(sampleRate);

    State& s = *state_;
    int done = 0;
    while (done < n) {
        const int chunk = std::min(n - done, kMaxFrames);
        const float* inC[kEngineIn];
        float* outC[kEngineOut];
        for (int i = 0; i < kEngineIn; ++i) inC[i] = in && in[i] ? in[i] + done : nullptr;
        for (int i = 0; i < kEngineOut; ++i) outC[i] = out && out[i] ? out[i] + done : nullptr;
        processChunk(s, inC, outC, chunk);
        done += chunk;
    }
    meters_.cycles.fetch_add(1, std::memory_order_relaxed);
    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double avail = n / double(sampleRate > 0 ? sampleRate : 48000);
    const float load = float(used / std::max(avail, 1e-6));
    const float prev = meters_.dspLoad.load(std::memory_order_relaxed);
    meters_.dspLoad.store(prev * 0.9f + load * 0.1f, std::memory_order_relaxed);
}

void Mixer::processChunk(State& s, const float* const* in, float* const* out, int n) {
    ParamStore& P = params();
    const size_t bytes = sizeof(float) * size_t(n);

    // ---------------------------------------------------------- inputs (+ RAIO inputs of hw strips)
    for (int i = 0; i < kInCh; ++i) {
        if (in[i]) std::memcpy(s.raw[i].data(), in[i], bytes);
        else std::memset(s.raw[i].data(), 0, bytes);
    }
    for (int st = 0; st < kHwStrips; ++st) {
        if (!P.strip[st].raio->on()) continue;
        for (int c = 0; c < 2; ++c)
            if (const float* r = in[kRaioInBase + st * 2 + c]) {
                float* dst = s.raw[stripFirstInCh(st) + c].data();
                for (int i = 0; i < n; ++i) dst[i] += r[i];
            }
    }
    if (link_) {
        for (int st = 0; st < kStrips; ++st) {
            float* ch[kBusCh];
            const int nch = stripChannels(st);
            for (int c = 0; c < nch; ++c) ch[c] = s.raw[stripFirstInCh(st) + c].data();
            link_->rtInput(st, ch, nch, n);
        }
    }

    for (int b = 0; b < kBuses; ++b)
        for (int c = 0; c < kBusCh; ++c) std::memset(s.bus[b][c].data(), 0, bytes);
    for (auto& f : s.fxIn) for (auto& v : f) std::memset(v.data(), 0, bytes);

    bool anySolo = false;
    for (int st = 0; st < kStrips; ++st) anySolo |= P.strip[st].solo->on();

    // ---------------------------------------------------------- strips
    for (int st = 0; st < kStrips; ++st) {
        const ParamStore::Strip& sp = P.strip[st];
        auto& d = s.sd[st];
        const int nch = stripChannels(st);
        float* ch[kBusCh];
        for (int c = 0; c < kBusCh; ++c) ch[c] = s.strip[st][c].data();
        for (int c = 0; c < nch; ++c) std::memcpy(ch[c], s.raw[stripFirstInCh(st) + c].data(), bytes);

        if (isHwStrip(st)) {
            if (sp.mono->on())
                for (int i = 0; i < n; ++i) ch[0][i] = ch[1][i] = 0.5f * (ch[0][i] + ch[1][i]);
            // 6-cell parametric EQ (stereo)
            if (sp.eqOn->on()) {
                const int ab = sp.eqAB->i();
                const uint32_t rev = P.stripEqRevision[st].load(std::memory_order_relaxed);
                if (rev != d.peqRev || ab != d.peqAB) {
                    for (int k = 0; k < kEqCells; ++k) {
                        d.peqOn[k] = false;
                        for (int c = 0; c < 2; ++c) {
                            const auto& cell = sp.eq[ab][c].cell[k];
                            const bool on = cell.on->on();
                            if (on) d.peq[c][k].c = makeBiquad(EqType(cell.type->i()), s.fs, cell.f->get(), cell.q->get(), cell.gain->get());
                            else d.peq[c][k].c = BiquadCoeffs{};
                            d.peqOn[k] |= on;
                        }
                    }
                    d.peqRev = rev; d.peqAB = ab;
                }
                for (int k = 0; k < kEqCells; ++k)
                    if (d.peqOn[k]) for (int c = 0; c < 2; ++c) d.peq[c][k].processBlock(ch[c], n);
                for (int c = 0; c < 2; ++c) {
                    const float t = sp.eq[ab][c].trim->get();
                    if (t != 0) { const float g = dbToLin(t); for (int i = 0; i < n; ++i) ch[c][i] *= g; }
                }
            }
            const ParamStore::Gate& gp = sp.gate_;
            d.gatePro.process(ch, 2, n, {gp.threshold->get(), gp.damping->get(), gp.bpSidechain->get(),
                                         gp.attack->get(), gp.hold->get(), gp.release->get()});
            const ParamStore::Comp& cp = sp.comp_;
            d.compPro.process(ch, 2, n, {cp.gainIn->get(), cp.ratio->get(), cp.threshold->get(), cp.attack->get(),
                                         cp.release->get(), cp.knee->get(), cp.gainOut->get(), cp.makeUp->on()});
            d.denoise.process(ch[0], ch[1], n, sp.denoiseThreshold->get(), sp.denoiseBypass->on());
            const ParamStore::Pitch& pt = sp.pitch;
            d.voice.process(ch[0], ch[1], n, {pt.on->on(), pt.dryWet->get(), pt.value->get(), pt.lo->get(), pt.med->get(), pt.hi->get()});
            d.color.process(ch[0], ch[1], n, sp.colorX->get(), sp.colorY->get());
            d.mod.process(ch[0], ch[1], n, sp.fxX->get(), sp.fxY->get());
            d.bin.process(ch[0], ch[1], n, sp.panX->get(), sp.panY->get());
            meters_.stripGate[st].store(d.gatePro.gain(), std::memory_order_relaxed);
            meters_.stripComp[st].store(d.compPro.reductionDb(), std::memory_order_relaxed);
            meterMax(meters_.stripCompIn[st], d.compPro.inputLevel());
        } else {
            applyStripMode(StripMode(sp.mode->i()), ch, n);
            d.eq3.process(ch, kVirtStripCh, n, sp.eq3[0]->get(), sp.eq3[1]->get(), sp.eq3[2]->get());
            const float mcT = sp.mc->on() ? 0.0f : 1.0f;
            if (mcT != d.mc || mcT == 0.0f) {
                const float step = (mcT - d.mc) / float(n);
                float g = d.mc;
                for (int i = 0; i < n; ++i) { g += step; ch[2][i] *= g; }
                d.mc = mcT;
            }
            d.kar.process(ch[0], ch[1], n, sp.karaoke->i());
            d.pan.process(ch, n, sp.panX->get(), sp.panY->get());
        }
        // strip limiter is before the faders
        d.lim.process(ch, n, sp.limit->get(), sp.limType->on());
        meters_.stripLimGain[st].store(d.lim.gain(), std::memory_order_relaxed);
        for (int c = 0; c < nch; ++c) meterMax(meters_.stripPre[st][c], PeakMeter::peak(ch[c], n));

        const float audible = (!sp.mute->on() && (!anySolo || sp.solo->on())) ? 1.0f : 0.0f;
        // per bus gain layers (8x8 matrix) with click free ramps
        float maxLayer = 0;
        for (int b = 0; b < kBuses; ++b) {
            const float lg = dbToLin(sp.gainLayer[b]->get());
            maxLayer = std::max(maxLayer, sp.bus[b]->on() ? lg : 0.0f);
            const float t = sp.bus[b]->on() ? lg * audible : 0.0f;
            if (!d.layerInit) d.layer[b] = t;
            const float g0 = d.layer[b];
            d.layer[b] = t;
            if (g0 == 0.0f && t == 0.0f) continue;
            for (int c = 0; c < nch; ++c) addRamp(s.bus[b][c].data(), ch[c], n, g0, t);
        }
        d.layerInit = true;
        // post-fader copy (meters, composite POST)
        const float postT = audible * dbToLin(sp.gain->get());
        {
            const float step = (postT - d.postGain) / float(n);
            for (int c = 0; c < nch; ++c) {
                float g = d.postGain;
                float* dst = s.post[st][c].data();
                for (int i = 0; i < n; ++i) { g += step; dst[i] = ch[c][i] * g; }
                meterMax(meters_.stripPost[st][c], PeakMeter::peak(dst, n));
            }
            d.postGain = postT;
        }
        // FX sends (pre: after limiter, post: after fader & mute)
        for (int k = 0; k < 4; ++k) {
            const float lvl = sendGain(sp.send[k]->get());
            const float t = lvl * (sp.post[k]->on() ? postT : 1.0f);
            const float g0 = d.sendG[k];
            d.sendG[k] = t;
            if (g0 == 0 && t == 0) continue;
            addRamp(s.fxIn[k][0].data(), ch[0], n, g0, t);
            addRamp(s.fxIn[k][1].data(), nch > 1 ? ch[1] : ch[0], n, g0, t);
        }
    }

    // ---------------------------------------------------------- tape deck playback -> buses
    float* busPtr[kBuses * kBusCh];
    for (int b = 0; b < kBuses; ++b)
        for (int c = 0; c < kBusCh; ++c) busPtr[b * kBusCh + c] = s.bus[b][c].data();
    if (tape_) tape_->rtPlay(busPtr, n, s.fs);

    // ---------------------------------------------------------- internal & external FX
    bool fxActive[4] = {false, false, false, false};
    if (P.reverb.on->on()) {
        const auto& M = P.reverb.m[P.reverb.ab->i()];
        ReverbSettings rs{M.preset->i(), M.dry->get(), M.wet->get(), M.preDelay->get(), M.er->get(), M.decay->get(),
                          {M.eqGain[0]->get(), M.eqGain[1]->get(), M.eqGain[2]->get()},
                          {M.eqFreq[0]->get(), M.eqFreq[1]->get(), M.eqFreq[2]->get()}, M.bypass->i()};
        s.reverb.process(s.fxIn[0][0].data(), s.fxIn[0][1].data(), s.fxOut[0][0].data(), s.fxOut[0][1].data(), n, rs);
        fxActive[0] = true;
    }
    if (P.delay.on->on()) {
        if (P.opt.fxType->i() == 0) {
            const auto& M = P.delay.m[P.delay.ab->i()];
            DelaySettings ds{M.ratio->get(), M.feedback->get(), M.dry->get(), M.wet->get(), M.preDelay->get(),
                             M.balL->get(), M.balR->get(), M.bypass->on(), {}};
            for (int t = 0; t < kDelayTaps; ++t)
                ds.tap[t] = {M.tap[t].on->on(), M.tap[t].time->get(), M.tap[t].gain->get(), M.tap[t].pan->get(),
                             M.tap[t].lpf->get(), M.tap[t].hpf->get()};
            s.delay.process(s.fxIn[1][0].data(), s.fxIn[1][1].data(), s.fxOut[1][0].data(), s.fxOut[1][1].data(), n, ds);
        } else {
            const auto& C = P.c5;
            C5Settings cs{};
            for (int k = 0; k < kC5Bands - 1; ++k) cs.xover[k] = C.xover[k]->get();
            for (int b = 0; b < kC5Bands; ++b) {
                const auto& B = C.band[b];
                cs.band[b] = {B.on->on(), B.gain->get(), B.threshold->get(), B.ratio->get(), B.attack->get(), B.release->get(),
                              B.knee->get(), B.outGain->get(), B.limit->get(), B.dist->get(), B.solo->on(), B.mute->on()};
            }
            cs.outGain = C.outGain->get(); cs.makeUp = C.makeUp->on(); cs.bypass = C.bypass->on();
            std::memcpy(s.fxOut[1][0].data(), s.fxIn[1][0].data(), bytes);
            std::memcpy(s.fxOut[1][1].data(), s.fxIn[1][1].data(), bytes);
            s.c5.process(s.fxOut[1][0].data(), s.fxOut[1][1].data(), n, cs);
        }
        fxActive[1] = true;
    }
    for (int e = 0; e < kExtFx; ++e) {
        for (int c = 0; c < 2; ++c) {
            if (float* o = out[kFxSendBase + e * 2 + c]) std::memcpy(o, s.fxIn[2 + e][c].data(), bytes);
            if (const float* r = in[kFxReturnBase + e * 2 + c]) { std::memcpy(s.fxOut[2 + e][c].data(), r, bytes); fxActive[2 + e] = true; }
        }
    }
    for (int k = 0; k < 4; ++k)
        if (fxActive[k]) meterMax(meters_.fx[k], std::max(PeakMeter::peak(s.fxOut[k][0].data(), n), PeakMeter::peak(s.fxOut[k][1].data(), n)));

    // ---------------------------------------------------------- buses
    const bool outLimiter = P.opt.outLimiter->on();
    for (int b = 0; b < kBuses; ++b) {
        const ParamStore::Bus& bp = P.bus[b];
        auto& d = s.bd[b];
        float* ch[kBusCh];
        for (int c = 0; c < kBusCh; ++c) ch[c] = s.bus[b][c].data();

        // FX returns
        for (int k = 0; k < 4; ++k) {
            const float t = fxActive[k] ? sendGain(bp.ret[k]->get()) : 0.0f;
            const float g0 = d.ret[k];
            d.ret[k] = t;
            if (g0 == 0 && t == 0) continue;
            addRamp(ch[0], s.fxOut[k][0].data(), n, g0, t);
            addRamp(ch[1], s.fxOut[k][1].data(), n, g0, t);
        }

        const BusMode mode = BusMode(bp.mode->i());
        if (mode == BusMode::Composite) {
            const bool post = P.opt.postFaderComposite->on();
            for (int c = 0; c < kBusCh; ++c) {
                const int src = P.opt.composite[c]->i();
                if (src <= 0) { std::memcpy(s.scratch[c].data(), ch[c], bytes); continue; }
                const int inCh = src - 1;
                const float* from = s.raw[inCh].data();
                if (post) {
                    for (int k = 0; k < kStrips; ++k)
                        if (inCh >= stripFirstInCh(k) && inCh < stripFirstInCh(k) + stripChannels(k))
                            from = s.post[k][inCh - stripFirstInCh(k)].data();
                }
                std::memcpy(s.scratch[c].data(), from, bytes);
            }
            for (int c = 0; c < kBusCh; ++c) std::memcpy(ch[c], s.scratch[c].data(), bytes);
        } else {
            applyBusMode(mode, ch, n);
        }

        // parametric EQ (6 cells x 8 channels) + trim + per channel delay
        const bool eqOn = bp.eqOn->on();
        const int ab = bp.eqAB->i();
        const uint32_t rev = P.eqRevision[b].load(std::memory_order_relaxed);
        if (eqOn && (rev != d.eqRev || ab != d.eqAB)) {
            for (int c = 0; c < kBusCh; ++c) {
                const ParamStore::EqChan& ec = bp.eq[ab][c];
                bool active = false;
                for (int k = 0; k < kEqCells; ++k) {
                    const bool on = ec.cell[k].on->on();
                    if (on) {
                        d.eq[c][k].c = makeBiquad(EqType(ec.cell[k].type->i()), s.fs, ec.cell[k].f->get(),
                                                  ec.cell[k].q->get(), ec.cell[k].gain->get());
                        if (!d.cellOn[c][k]) d.eq[c][k].reset();
                    }
                    d.cellOn[c][k] = on;
                    active |= on;
                }
                d.trim[c] = dbToLin(ec.trim->get());
                d.eqDelay[c] = int(ec.delay->get() * 0.001f * s.fs);
                d.chActive[c] = active || ec.trim->get() != 0;
            }
            d.eqRev = rev; d.eqAB = ab;
        }
        if (eqOn) {
            for (int c = 0; c < kBusCh; ++c) {
                if (!d.chActive[c]) continue;
                for (int k = 0; k < kEqCells; ++k)
                    if (d.cellOn[c][k]) d.eq[c][k].processBlock(ch[c], n);
                if (d.trim[c] != 1.0f) for (int i = 0; i < n; ++i) ch[c][i] *= d.trim[c];
            }
        }

        const int mono = bp.mono->i();
        if (mono == 1) for (int i = 0; i < n; ++i) ch[0][i] = ch[1][i] = 0.5f * (ch[0][i] + ch[1][i]);
        else if (mono == 2) for (int i = 0; i < n; ++i) std::swap(ch[0][i], ch[1][i]);

        d.fader.apply(ch, kBusCh, n, bp.mute->on() ? 0.0f : dbToLin(bp.gain->get()));

        const float lim = std::min(bp.limit->get(), 0.0f);
        if (outLimiter || bp.limit->get() < 0.0f) {
            d.lim.process(ch, n, lim, bp.limType->on());
            meters_.busLimGain[b].store(d.lim.gain(), std::memory_order_relaxed);
        } else {
            for (int c = 0; c < kBusCh; ++c) for (int i = 0; i < n; ++i) ch[c][i] = clampf(ch[c][i], -1.0f, 1.0f);
        }

        const int outDelay = int(bp.delay->get() * 0.001f * s.fs);
        for (int c = 0; c < kBusCh; ++c) d.delay[c].process(ch[c], n, outDelay + (eqOn ? d.eqDelay[c] : 0));
    }

    // ---------------------------------------------------------- monitor on SEL
    if (P.opt.monitorOnSel->on()) {
        int sel = -1, mon = -1;
        for (int b = 0; b < kBuses; ++b) {
            if (sel < 0 && P.bus[b].sel->on()) sel = b;
            if (mon < 0 && P.bus[b].monitor->on()) mon = b;
        }
        if (sel >= 0 && mon >= 0 && sel != mon)
            for (int c = 0; c < kBusCh; ++c) std::memcpy(s.bus[mon][c].data(), s.bus[sel][c].data(), bytes);
    }

    for (int b = 0; b < kBuses; ++b)
        for (int c = 0; c < kBusCh; ++c) {
            meterMax(meters_.bus[b][c], PeakMeter::peak(s.bus[b][c].data(), n));
            if (float* o = out[b * kBusCh + c]) std::memcpy(o, s.bus[b][c].data(), bytes);
        }

    // ---------------------------------------------------------- recorder & network taps
    const float* rawPtr[kInCh];
    for (int i = 0; i < kInCh; ++i) rawPtr[i] = s.raw[i].data();
    if (tape_) tape_->rtRecord(rawPtr, busPtr, n, s.fs);
    if (link_) link_->rtOutput(busPtr, n, s.fs);
}

} // namespace road
