// Realtime-safe DSP building blocks. Nothing in this file allocates after
// prepare()/constructor, so every process() call is safe on the audio thread.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/Params.h"

namespace road::dsp {

constexpr float kPi = 3.14159265358979f;

inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float linToDb(float l) { return l > 1e-9f ? 20.0f * std::log10(l) : -180.0f; }
inline float clampf(float x, float a, float b) { return std::min(std::max(x, a), b); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
// one-pole coefficient for a time constant in seconds
inline float tc(float seconds, float fs) { return seconds <= 0 ? 0.0f : std::exp(-1.0f / (seconds * fs)); }

// ---------------------------------------------------------------- Biquad
struct BiquadCoeffs {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    bool identity() const { return b0 == 1 && b1 == 0 && b2 == 0 && a1 == 0 && a2 == 0; }
};

inline BiquadCoeffs makeBiquad(EqType type, double fs, double f, double q, double gainDb) {
    BiquadCoeffs c;
    if (fs <= 0) return c;
    f = std::clamp(f, 10.0, fs * 0.49);
    q = std::max(q, 0.05);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * M_PI * f / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case EqType::Peak:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case EqType::Notch:
        b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case EqType::BandPass:
        b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case EqType::LowPass:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case EqType::HighPass:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case EqType::LowShelf: {
        const double sa = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
        break;
    }
    case EqType::HighShelf: {
        const double sa = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
        break;
    }
    default: break;
    }
    c.b0 = float(b0 / a0); c.b1 = float(b1 / a0); c.b2 = float(b2 / a0);
    c.a1 = float(a1 / a0); c.a2 = float(a2 / a0);
    return c;
}

// magnitude response in dB (for drawing EQ curves)
inline double biquadMagDb(const BiquadCoeffs& c, double fs, double f) {
    const double w = 2 * M_PI * f / fs;
    const double cr1 = std::cos(w), ci1 = -std::sin(w), cr2 = std::cos(2 * w), ci2 = -std::sin(2 * w);
    const double nr = c.b0 + c.b1 * cr1 + c.b2 * cr2, ni = c.b1 * ci1 + c.b2 * ci2;
    const double dr = 1 + c.a1 * cr1 + c.a2 * cr2, di = c.a1 * ci1 + c.a2 * ci2;
    const double mag = std::sqrt((nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di));
    return 20 * std::log10(std::max(mag, 1e-12));
}

struct Biquad {
    BiquadCoeffs c;
    float z1 = 0, z2 = 0;
    inline float process(float x) {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void processBlock(float* buf, int n) {
        for (int i = 0; i < n; ++i) buf[i] = process(buf[i]);
        if (std::fabs(z1) < 1e-20f) z1 = 0;
        if (std::fabs(z2) < 1e-20f) z2 = 0;
    }
    void reset() { z1 = z2 = 0; }
};

// ---------------------------------------------------------------- smoothing gain ramp
struct GainRamp {
    float cur = 1.0f;
    bool init = false;
    // multiply buf by a ramp from current to target gain
    void apply(float* const* ch, int nch, int n, float target) {
        if (!init) { cur = target; init = true; }
        if (std::fabs(target - cur) < 1e-6f) {
            if (cur == 1.0f) return;
            for (int c = 0; c < nch; ++c) for (int i = 0; i < n; ++i) ch[c][i] *= cur;
            return;
        }
        const float step = (target - cur) / float(std::max(n, 1));
        for (int c = 0; c < nch; ++c) {
            float g = cur;
            for (int i = 0; i < n; ++i) { g += step; ch[c][i] *= g; }
        }
        cur = target;
    }
};

// ---------------------------------------------------------------- delay line
class DelayLine {
public:
    void prepare(int maxSamples) { buf_.assign(size_t(maxSamples) + 1, 0.0f); pos_ = 0; }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); }
    int capacity() const { return int(buf_.size()) - 1; }
    void process(float* x, int n, int delay) {
        if (buf_.empty()) return;
        delay = std::clamp(delay, 0, capacity());
        if (delay == 0 && lastDelay_ == 0) { // keep history coherent anyway
            for (int i = 0; i < n; ++i) { buf_[pos_] = x[i]; pos_ = (pos_ + 1) % buf_.size(); }
            return;
        }
        lastDelay_ = delay;
        const size_t sz = buf_.size();
        for (int i = 0; i < n; ++i) {
            buf_[pos_] = x[i];
            size_t rd = (pos_ + sz - size_t(delay)) % sz;
            x[i] = buf_[rd];
            pos_ = (pos_ + 1) % sz;
        }
    }
private:
    std::vector<float> buf_;
    size_t pos_ = 0;
    int lastDelay_ = 0;
};

// ---------------------------------------------------------------- level meter
struct PeakMeter {
    static float peak(const float* x, int n) {
        float m = 0;
        for (int i = 0; i < n; ++i) m = std::max(m, std::fabs(x[i]));
        return m;
    }
};

// ---------------------------------------------------------------- Gate (knob 0..10)
class Gate {
public:
    void prepare(float fs) { fs_ = fs; env_ = 0; gain_ = 1; hold_ = 0; }
    void process(float* const* ch, int nch, int n, float knob) {
        if (knob <= 0.01f) { gain_ = 1; return; }
        const float thr = dbToLin(-70.0f + 5.0f * knob);     // -70 dB .. -20 dB
        const float thrClose = thr * 0.63f;                    // 4 dB hysteresis
        const float floorG = dbToLin(-30.0f - 3.0f * knob);    // deeper range when stronger
        const float envA = tc(0.0005f, fs_), envR = tc(0.05f, fs_);
        const float gOpen = tc(0.001f, fs_), gClose = tc(0.12f, fs_);
        const int holdSamples = int(0.08f * fs_);
        for (int i = 0; i < n; ++i) {
            float x = 0;
            for (int c = 0; c < nch; ++c) x = std::max(x, std::fabs(ch[c][i]));
            env_ = x > env_ ? envA * env_ + (1 - envA) * x : envR * env_ + (1 - envR) * x;
            if (env_ > thr) { open_ = true; hold_ = holdSamples; }
            else if (env_ < thrClose) { if (hold_ > 0) --hold_; else open_ = false; }
            const float target = open_ ? 1.0f : floorG;
            const float k = target > gain_ ? gOpen : gClose;
            gain_ = k * gain_ + (1 - k) * target;
            for (int c = 0; c < nch; ++c) ch[c][i] *= gain_;
        }
    }
    float reduction() const { return gain_; }
private:
    float fs_ = 48000, env_ = 0, gain_ = 1;
    int hold_ = 0;
    bool open_ = true;
};

// ---------------------------------------------------------------- Compressor (knob 0..10, auto make-up)
class Compressor {
public:
    void prepare(float fs) { fs_ = fs; envDb_ = -120; }
    void process(float* const* ch, int nch, int n, float knob) {
        if (knob <= 0.01f) { grDb_ = 0; return; }
        const float k = knob / 10.0f;
        const float thr = -2.6f * knob;              // 0 .. -26 dB
        const float ratio = 1.0f + 5.0f * k;         // 1:1 .. 6:1
        const float knee = 6.0f;
        const float makeup = -thr * (1.0f - 1.0f / ratio) * 0.8f + 2.0f * k;
        const float att = tc(0.003f, fs_), rel = tc(0.12f, fs_);
        const float mk = dbToLin(makeup);
        float grMax = 0;
        for (int i = 0; i < n; ++i) {
            float x = 0;
            for (int c = 0; c < nch; ++c) x = std::max(x, std::fabs(ch[c][i]));
            const float xd = linToDb(x);
            float over = xd - thr, gr = 0;
            if (over > knee / 2) gr = over * (1 - 1 / ratio);
            else if (over > -knee / 2) { float t = over + knee / 2; gr = (1 - 1 / ratio) * t * t / (2 * knee); }
            envDb_ = gr > envDb_ ? att * envDb_ + (1 - att) * gr : rel * envDb_ + (1 - rel) * gr;
            const float g = dbToLin(-envDb_) * mk;
            grMax = std::max(grMax, envDb_);
            for (int c = 0; c < nch; ++c) ch[c][i] *= g;
        }
        grDb_ = grMax;
    }
    float reductionDb() const { return grDb_; }
private:
    float fs_ = 48000, envDb_ = 0, grDb_ = 0;
};

// ---------------------------------------------------------------- Brick wall limiter
// Yellow: zero latency, instantaneous gain computer.
// Green : 5 ms look-ahead, smooth gain (adds 5 ms latency).
class Limiter {
public:
    static constexpr int kMaxCh = 8;
    void prepare(float fs, int nch) {
        fs_ = fs; nch_ = nch;
        la_ = std::max(1, int(0.005f * fs));
        for (int c = 0; c < nch; ++c) delay_[c].assign(size_t(la_) + 1, 0.0f);
        minQ_.assign(size_t(la_) * 2 + 4, 0); minV_.assign(minQ_.size(), 1.0f);
        avg_.assign(size_t(la_), 1.0f);
        avgSum_ = float(la_); avgPos_ = 0; qHead_ = qTail_ = 0; t_ = 0; dPos_ = 0;
        g_ = 1; held_ = 1;
    }
    void process(float* const* ch, int n, float thrDb, bool green) {
        if (green != wasGreen_) { reset(); wasGreen_ = green; }
        // yellow limiter at its maximum value (+12 dB) is "off"
        if (!green && thrDb >= 11.99f) { gr_ = 1; g_ = 1; return; }
        const float thr = dbToLin(std::min(thrDb, 12.0f));
        const float rel = tc(0.08f, fs_);
        if (!green) {
            float gmin = 1;
            for (int i = 0; i < n; ++i) {
                float pk = 0;
                for (int c = 0; c < nch_; ++c) pk = std::max(pk, std::fabs(ch[c][i]));
                const float target = pk > thr ? thr / pk : 1.0f;
                g_ = target < g_ ? target : rel * g_ + (1 - rel) * target;
                gmin = std::min(gmin, g_);
                for (int c = 0; c < nch_; ++c) ch[c][i] *= g_;
            }
            gr_ = gmin;
            return;
        }
        const size_t dsz = delay_[0].size();
        float gmin = 1;
        for (int i = 0; i < n; ++i) {
            float pk = 0;
            for (int c = 0; c < nch_; ++c) pk = std::max(pk, std::fabs(ch[c][i]));
            const float target = pk > thr ? thr / pk : 1.0f;
            // sliding minimum over the look-ahead window (monotonic queue)
            const size_t qs = minQ_.size();
            while (qHead_ != qTail_ && minV_[(qTail_ + qs - 1) % qs] >= target) qTail_ = (qTail_ + qs - 1) % qs;
            minQ_[qTail_] = t_; minV_[qTail_] = target; qTail_ = (qTail_ + 1) % qs;
            while (minQ_[qHead_] + uint64_t(la_) <= t_) qHead_ = (qHead_ + 1) % qs;
            float h = minV_[qHead_];
            // release smoothing (only upwards)
            held_ = h < held_ ? h : rel * held_ + (1 - rel) * h;
            h = std::min(h, held_);
            // boxcar average -> smooth attack that reaches target exactly in time
            avgSum_ += h - avg_[avgPos_];
            avg_[avgPos_] = h;
            avgPos_ = (avgPos_ + 1) % avg_.size();
            const float g = std::min(1.0f, avgSum_ / float(la_));
            gmin = std::min(gmin, g);
            for (int c = 0; c < nch_; ++c) {
                const float in = ch[c][i];
                const float out = delay_[c][(dPos_ + dsz - size_t(la_)) % dsz];
                delay_[c][dPos_] = in;
                ch[c][i] = clampf(out * g, -thr, thr);
            }
            dPos_ = (dPos_ + 1) % dsz;
            ++t_;
        }
        gr_ = gmin;
    }
    float gain() const { return gr_; }
    void reset() {
        for (int c = 0; c < nch_; ++c) std::fill(delay_[c].begin(), delay_[c].end(), 0.0f);
        std::fill(avg_.begin(), avg_.end(), 1.0f);
        avgSum_ = float(la_); qHead_ = qTail_ = 0; g_ = 1; held_ = 1;
    }
private:
    float fs_ = 48000; int nch_ = 2, la_ = 240;
    std::vector<float> delay_[kMaxCh];
    std::vector<uint64_t> minQ_; std::vector<float> minV_;
    std::vector<float> avg_;
    float avgSum_ = 0; size_t avgPos_ = 0, qHead_ = 0, qTail_ = 0, dPos_ = 0; uint64_t t_ = 0;
    float g_ = 1, held_ = 1, gr_ = 1;
    bool wasGreen_ = false;
};

// ---------------------------------------------------------------- small stereo reverb (Freeverb style)
class SmallReverb {
public:
    void prepare(float fs) {
        const float s = fs / 44100.0f;
        static const int combL[4] = {1116, 1188, 1277, 1356};
        static const int apL[2] = {556, 341};
        for (int i = 0; i < 4; ++i) {
            for (int side = 0; side < 2; ++side) {
                int len = int((combL[i] + side * 23) * s * 0.6f);
                comb_[side][i].assign(size_t(std::max(len, 8)), 0.0f);
                cpos_[side][i] = 0; cstore_[side][i] = 0;
            }
        }
        for (int i = 0; i < 2; ++i)
            for (int side = 0; side < 2; ++side) {
                int len = int((apL[i] + side * 23) * s * 0.6f);
                ap_[side][i].assign(size_t(std::max(len, 8)), 0.0f);
                apos_[side][i] = 0;
            }
    }
    // adds wet signal (mix) to L/R in place
    void process(float* L, float* R, int n, float mix, float room = 0.72f, float damp = 0.35f) {
        if (mix <= 0.0005f) return;
        for (int i = 0; i < n; ++i) {
            const float in = (L[i] + R[i]) * 0.08f;
            float out[2] = {0, 0};
            for (int side = 0; side < 2; ++side) {
                for (int k = 0; k < 4; ++k) {
                    auto& b = comb_[side][k];
                    size_t& p = cpos_[side][k];
                    float y = b[p];
                    cstore_[side][k] = y * (1 - damp) + cstore_[side][k] * damp;
                    b[p] = in + cstore_[side][k] * room;
                    p = (p + 1) % b.size();
                    out[side] += y;
                }
                for (int k = 0; k < 2; ++k) {
                    auto& b = ap_[side][k];
                    size_t& p = apos_[side][k];
                    float bo = b[p];
                    float y = -out[side] + bo;
                    b[p] = out[side] + bo * 0.5f;
                    p = (p + 1) % b.size();
                    out[side] = y;
                }
            }
            L[i] += out[0] * mix;
            R[i] += out[1] * mix;
        }
    }
private:
    std::vector<float> comb_[2][4], ap_[2][2];
    size_t cpos_[2][4] = {}, apos_[2][2] = {};
    float cstore_[2][4] = {};
};

// ---------------------------------------------------------------- RoadPan: Color panel
class ColorPanel {
public:
    void prepare(float fs) { fs_ = fs; rev_.prepare(fs); for (auto& b : f_) for (auto& q : b) q.reset(); lastX_ = lastY_ = -9; }
    static bool neutral(float x, float y) { return std::fabs(x) < 0.004f && std::fabs(y - 0.5f) < 0.004f; }
    void process(float* L, float* R, int n, float x, float y) {
        if (neutral(x, y)) return;
        if (x != lastX_ || y != lastY_) {
            const float t = 2 * x;              // tilt  -1 dark .. +1 bright
            const float c = 2 * (y - 0.5f);     // -1 thin/presence .. +1 warm/room
            const float low = -6 * t + (c < 0 ? 7 * c : 3 * c);
            const float mid = c < 0 ? -8 * c : -4 * c;
            const float high = 6 * t + (c > 0 ? -1.5f * c : 2 * -c);
            const BiquadCoeffs lc = makeBiquad(EqType::LowShelf, fs_, 220, 0.7, low);
            const BiquadCoeffs mc = makeBiquad(EqType::Peak, fs_, 1700, 0.8, mid);
            const BiquadCoeffs hc = makeBiquad(EqType::HighShelf, fs_, 4200, 0.7, high);
            for (int s = 0; s < 2; ++s) { f_[s][0].c = lc; f_[s][1].c = mc; f_[s][2].c = hc; }
            revMix_ = c > 0 ? 0.35f * c : 0.0f;
            lastX_ = x; lastY_ = y;
        }
        for (int k = 0; k < 3; ++k) { f_[0][k].processBlock(L, n); f_[1][k].processBlock(R, n); }
        rev_.process(L, R, n, revMix_, 0.62f, 0.4f);
    }
private:
    float fs_ = 48000, lastX_ = -9, lastY_ = -9, revMix_ = 0;
    Biquad f_[2][3];
    SmallReverb rev_;
};

// ---------------------------------------------------------------- RoadPan: Modulation (chorus family)
class Modulation {
public:
    void prepare(float fs) {
        fs_ = fs;
        size_t len = size_t(0.06f * fs) + 4;
        for (auto& b : buf_) b.assign(len, 0.0f);
        pos_ = 0; phase_ = 0;
    }
    static bool neutral(float x, float y) { return std::hypot(2 * x, 2 * (y - 0.5f)) < 0.02f; }
    void process(float* L, float* R, int n, float x, float y) {
        if (neutral(x, y)) { phase_ = 0; return; }
        const float dx = 2 * x, dy = 2 * (y - 0.5f);
        const float depth = std::min(1.0f, std::hypot(dx, dy));
        float baseMs, modMs, rate, wet;
        if (dy <= 0) {               // chorus / flanger / phasing at the bottom
            const float t = -dy;
            baseMs = lerp(9.0f, 0.8f, t); modMs = lerp(3.0f, 0.7f, t);
            rate = lerp(0.45f, 0.18f, t); wet = 0.5f * depth;
        } else {                     // modulation / vibrato at the top
            baseMs = 6.0f; modMs = lerp(1.5f, 4.0f, dy);
            rate = lerp(0.8f, 6.5f, dy); wet = lerp(0.4f, 0.85f, dy) * depth;
        }
        const float fb = dx < 0 ? 0.78f * (-dx) : 0.0f;
        const float dryG = 1.0f - 0.5f * wet;
        const size_t sz = buf_[0].size();
        const float inc = rate / fs_;
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            for (int s = 0; s < 2; ++s) {
                const float ph = phase_ + (s ? 0.25f : 0.0f);
                const float lfo = std::sin(2 * kPi * ph);
                const float dly = std::clamp((baseMs + modMs * lfo) * 0.001f * fs_, 1.0f, float(sz - 3));
                const float rp = float(pos_) - dly + float(sz);
                const size_t i0 = size_t(rp) % sz, i1 = (i0 + 1) % sz;
                const float fr = rp - std::floor(rp);
                const float d = buf_[s][i0] * (1 - fr) + buf_[s][i1] * fr;
                buf_[s][pos_] = io[s][i] + d * fb;
                io[s][i] = io[s][i] * dryG + d * wet;
            }
            pos_ = (pos_ + 1) % sz;
            phase_ += inc; if (phase_ >= 1) phase_ -= 1;
        }
    }
private:
    float fs_ = 48000, phase_ = 0;
    std::vector<float> buf_[2];
    size_t pos_ = 0;
};

// ---------------------------------------------------------------- RoadPan: 3D binaural position
class Binaural {
public:
    void prepare(float fs) {
        fs_ = fs;
        dl_[0].assign(size_t(0.002f * fs) + 4, 0.0f); dl_[1] = dl_[0];
        pos_ = 0; rev_.prepare(fs); lp_[0] = lp_[1] = 0; lastX_ = lastY_ = -9;
        for (auto& b : prox_) b.reset();
    }
    static bool neutral(float x, float y) { return std::fabs(x) < 0.004f && std::fabs(y - 0.5f) < 0.004f; }
    void process(float* L, float* R, int n, float x, float y) {
        if (neutral(x, y)) return;
        const float az = clampf(2 * x, -1, 1);       // -1 left .. +1 right
        const float dist = clampf(2 * (y - 0.5f), -1, 1); // -1 close .. +1 far
        const float s = std::sin(az * kPi * 0.5f);
        const float itd = 0.00066f * std::fabs(s) * fs_;
        const float shadowHz = lerp(18000.0f, 1600.0f, std::fabs(s));
        const float a = std::exp(-2 * kPi * shadowHz / fs_);
        const float nearG = dbToLin(1.5f * std::fabs(s));
        const float farG = dbToLin(-4.5f * std::fabs(s));
        const float distG = dbToLin(dist > 0 ? -5.0f * dist : 2.0f * -dist);
        const float blend = std::min(1.0f, (std::fabs(az) + std::fabs(dist)) * 12.0f);
        if (x != lastX_ || y != lastY_) {
            const BiquadCoeffs pc = makeBiquad(EqType::LowShelf, fs_, 180, 0.7, dist < 0 ? 5.0f * -dist : -2.0f * dist);
            prox_[0].c = pc; prox_[1].c = pc; lastX_ = x; lastY_ = y;
        }
        const size_t sz = dl_[0].size();
        const int farSide = s >= 0 ? 0 : 1;          // source on the right -> left ear is far
        for (int i = 0; i < n; ++i) {
            const float m = 0.5f * (L[i] + R[i]);
            dl_[0][pos_] = m;
            const float rp = float(pos_) - itd + float(sz);
            const size_t i0 = size_t(rp) % sz, i1 = (i0 + 1) % sz;
            const float fr = rp - std::floor(rp);
            float farS = dl_[0][i0] * (1 - fr) + dl_[0][i1] * fr;
            lp_[farSide] = (1 - a) * farS + a * lp_[farSide];
            farS = lp_[farSide] * farG;
            const float nearS = m * nearG;
            const float outL = farSide == 0 ? farS : nearS;
            const float outR = farSide == 0 ? nearS : farS;
            L[i] = lerp(L[i], outL * distG, blend);
            R[i] = lerp(R[i], outR * distG, blend);
            pos_ = (pos_ + 1) % sz;
        }
        prox_[0].processBlock(L, n); prox_[1].processBlock(R, n);
        // simple room: always a touch, more when far away
        rev_.process(L, R, n, (0.06f + (dist > 0 ? 0.3f * dist : 0.0f)) * blend, 0.55f, 0.45f);
    }
private:
    float fs_ = 48000, lp_[2] = {0, 0}, lastX_ = -9, lastY_ = -9;
    std::vector<float> dl_[2];
    size_t pos_ = 0;
    SmallReverb rev_;
    Biquad prox_[2];
};

// ---------------------------------------------------------------- Karaoke voice removal (stereo)
class Karaoke {
public:
    void prepare(float fs) { fs_ = fs; mode_ = -1; }
    void process(float* L, float* R, int n, int mode) {
        if (mode <= 0) return;
        if (mode != mode_) {
            mode_ = mode;
            float lpF = 120, hpF = 7000;
            if (mode == 3) { lpF = 250; hpF = 4500; }
            if (mode == 4) { lpF = 200; hpF = 4000; }
            for (int s = 0; s < 2; ++s) {
                lp_[s][0].c = lp_[s][1].c = makeBiquad(EqType::LowPass, fs_, lpF, 0.7071, 0);
                hp_[s][0].c = hp_[s][1].c = makeBiquad(EqType::HighPass, fs_, hpF, 0.7071, 0);
            }
        }
        for (int i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            if (mode == 4) { // Kv: remove voice band 200-4000 Hz on both channels
                L[i] = lp_[0][1].process(lp_[0][0].process(l)) + hp_[0][1].process(hp_[0][0].process(l));
                R[i] = lp_[1][1].process(lp_[1][0].process(r)) + hp_[1][1].process(hp_[1][0].process(r));
                continue;
            }
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r);
            float keep = 0;
            if (mode >= 2) keep = lp_[0][1].process(lp_[0][0].process(mid)) + hp_[0][1].process(hp_[0][0].process(mid));
            L[i] = side + keep;
            R[i] = -side + keep;
        }
    }
private:
    float fs_ = 48000; int mode_ = -1;
    Biquad lp_[2][2], hp_[2][2];
};

// ---------------------------------------------------------------- 3 band EQ (virtual strips)
class ThreeBandEq {
public:
    void prepare(float fs) { fs_ = fs; last_[0] = last_[1] = last_[2] = 999; for (auto& c : f_) for (auto& b : c) b.reset(); }
    void process(float* const* ch, int nch, int n, float bass, float mid, float treble) {
        if (bass == 0 && mid == 0 && treble == 0) return;
        if (bass != last_[0] || mid != last_[1] || treble != last_[2]) {
            const BiquadCoeffs b = makeBiquad(EqType::LowShelf, fs_, 120, 0.7, bass);
            const BiquadCoeffs m = makeBiquad(EqType::Peak, fs_, 1000, 0.7, mid);
            const BiquadCoeffs t = makeBiquad(EqType::HighShelf, fs_, 5000, 0.7, treble);
            for (int c = 0; c < kBusCh; ++c) { f_[c][0].c = b; f_[c][1].c = m; f_[c][2].c = t; }
            last_[0] = bass; last_[1] = mid; last_[2] = treble;
        }
        for (int c = 0; c < nch; ++c) {
            if (bass != 0) f_[c][0].processBlock(ch[c], n);
            if (mid != 0) f_[c][1].processBlock(ch[c], n);
            if (treble != 0) f_[c][2].processBlock(ch[c], n);
        }
    }
private:
    float fs_ = 48000, last_[3];
    Biquad f_[kBusCh][3];
};

// ---------------------------------------------------------------- 8 channel transformations
// Channel order: 0 FL, 1 FR, 2 FC, 3 LFE, 4 RL, 5 RR, 6 SL, 7 SR
inline void zeroFrom(float* const* ch, int from, int n) {
    for (int c = from; c < kBusCh; ++c) std::memset(ch[c], 0, sizeof(float) * size_t(n));
}

inline void applyBusMode(BusMode mode, float* const* ch, int n) {
    float *FL = ch[0], *FR = ch[1], *FC = ch[2], *SW = ch[3], *RL = ch[4], *RR = ch[5], *SL = ch[6], *SR = ch[7];
    switch (mode) {
    case BusMode::Normal:
    case BusMode::Composite:
    case BusMode::Count:
        return;
    case BusMode::MixDownA:
        for (int i = 0; i < n; ++i) {
            const float c = 0.7f * FC[i] + SW[i];
            const float l = FL[i] + c + RL[i] - SL[i];
            const float r = FR[i] + c - RR[i] + SR[i];
            FL[i] = l; FR[i] = r;
        }
        zeroFrom(ch, 2, n);
        return;
    case BusMode::MixDownB:
        for (int i = 0; i < n; ++i) {
            const float c = 0.7f * FC[i] + SW[i];
            const float l = FL[i] + c + RL[i] + SL[i];
            const float r = FR[i] + c + RR[i] + SR[i];
            FL[i] = l; FR[i] = r;
        }
        zeroFrom(ch, 2, n);
        return;
    case BusMode::Repeat:
        for (int c = 2; c < kBusCh; ++c) std::memcpy(ch[c], ch[c & 1], sizeof(float) * size_t(n));
        return;
    case BusMode::TVMix:
        for (int i = 0; i < n; ++i) {
            const float l = FL[i], r = FR[i];
            FC[i] = 0.2f * (l + r); SW[i] = 0.5f * (l + r);
            RL[i] = SL[i] = 0.7f * (l - r);
            RR[i] = SR[i] = 0.7f * (r - l);
        }
        return;
    case BusMode::UpMix21:
    case BusMode::UpMix41:
    case BusMode::UpMix61:
        for (int i = 0; i < n; ++i) {
            const float l = FL[i], r = FR[i];
            FC[i] = 0; SW[i] = 0.5f * (l + r);
            RL[i] = mode != BusMode::UpMix21 ? l : 0; RR[i] = mode != BusMode::UpMix21 ? r : 0;
            SL[i] = mode == BusMode::UpMix61 ? l : 0; SR[i] = mode == BusMode::UpMix61 ? r : 0;
        }
        return;
    case BusMode::LeftOnly:   std::memcpy(FR, FL, sizeof(float) * size_t(n)); zeroFrom(ch, 2, n); return;
    case BusMode::RightOnly:  std::memcpy(FL, FR, sizeof(float) * size_t(n)); zeroFrom(ch, 2, n); return;
    case BusMode::CenterOnly: std::memcpy(FL, FC, sizeof(float) * size_t(n)); std::memcpy(FR, FC, sizeof(float) * size_t(n)); zeroFrom(ch, 2, n); return;
    case BusMode::LFEOnly:    std::memcpy(FL, SW, sizeof(float) * size_t(n)); std::memcpy(FR, SW, sizeof(float) * size_t(n)); zeroFrom(ch, 2, n); return;
    case BusMode::RearOnly:   std::memcpy(FL, RL, sizeof(float) * size_t(n)); std::memcpy(FR, RR, sizeof(float) * size_t(n)); zeroFrom(ch, 2, n); return;
    }
}

inline void applyStripMode(StripMode mode, float* const* ch, int n) {
    switch (mode) {
    case StripMode::Normal: case StripMode::Count: return;
    case StripMode::Mono:
        for (int i = 0; i < n; ++i) ch[0][i] = ch[1][i] = 0.5f * (ch[0][i] + ch[1][i]);
        return;
    case StripMode::RepeatMono:
        for (int c = 1; c < kBusCh; ++c) std::memcpy(ch[c], ch[0], sizeof(float) * size_t(n));
        return;
    case StripMode::RepeatStereo: applyBusMode(BusMode::Repeat, ch, n); return;
    case StripMode::MixDownA: applyBusMode(BusMode::MixDownA, ch, n); return;
    case StripMode::MixDownB: applyBusMode(BusMode::MixDownB, ch, n); return;
    case StripMode::TVMix: applyBusMode(BusMode::TVMix, ch, n); return;
    case StripMode::UpMix21: applyBusMode(BusMode::UpMix21, ch, n); return;
    case StripMode::UpMix41: applyBusMode(BusMode::UpMix41, ch, n); return;
    case StripMode::UpMix61: applyBusMode(BusMode::UpMix61, ch, n); return;
    case StripMode::LeftOnly: applyBusMode(BusMode::LeftOnly, ch, n); return;
    case StripMode::RightOnly: applyBusMode(BusMode::RightOnly, ch, n); return;
    case StripMode::CenterOnly: applyBusMode(BusMode::CenterOnly, ch, n); return;
    case StripMode::LFEOnly: applyBusMode(BusMode::LFEOnly, ch, n); return;
    case StripMode::RearOnly: applyBusMode(BusMode::RearOnly, ch, n); return;
    }
}

// 5.1 pan pot of the virtual strips. x: -0.5 left .. +0.5 right, y: -0.5 rear .. +0.5 front
class PanPot51 {
public:
    void process(float* const* ch, int n, float x, float y) {
        if (std::fabs(x) < 0.003f && std::fabs(y) < 0.003f) { cur_.fill(1.0f); moveCur_ = 0; return; }
        const float gl = x <= 0 ? 1.0f : 1.0f - 2.0f * x;   // left channels attenuation
        const float gr = x >= 0 ? 1.0f : 1.0f + 2.0f * x;
        const float front = y >= 0 ? 1.0f : 1.0f + 2.0f * y;
        const float rear = y <= 0 ? 1.0f : 1.0f - 2.0f * y;
        const float move = y < 0 ? -2.0f * y : 0.0f;        // move front content to the rear
        std::array<float, kBusCh> g = {gl * front, gr * front, front * std::min(gl, gr) * (1 - 0.5f * std::fabs(2 * x)),
                                       1.0f, gl * rear, gr * rear, gl, gr};
        for (int i = 0; i < n; ++i) {
            const float t = float(i + 1) / float(n);
            const float mv = lerp(moveCur_, move, t);
            const float fl = ch[0][i], fr = ch[1][i];
            for (int c = 0; c < kBusCh; ++c) ch[c][i] *= lerp(cur_[size_t(c)], g[size_t(c)], t);
            ch[4][i] += fl * mv * gl;
            ch[5][i] += fr * mv * gr;
        }
        cur_ = g; moveCur_ = move;
    }
private:
    std::array<float, kBusCh> cur_ = {1, 1, 1, 1, 1, 1, 1, 1};
    float moveCur_ = 0;
};

} // namespace road::dsp
