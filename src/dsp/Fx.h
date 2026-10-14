// Advanced effects: detailed compressor / gate, spectral denoiser, voice
// modeler, FDN reverb, 8-tap delay and 5-band compressor.
// Realtime safe after prepare().
#pragma once

#include <array>
#include <complex>
#include <vector>

#include "dsp/Dsp.h"

namespace road::dsp {

// ---------------------------------------------------------------- FFT (radix 2, in place)
class FFT {
public:
    void prepare(int n) {
        n_ = n;
        tw_.resize(size_t(n / 2));
        for (int i = 0; i < n / 2; ++i) tw_[size_t(i)] = std::polar(1.0f, -2.0f * kPi * float(i) / float(n));
        rev_.resize(size_t(n));
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[size_t(i)] = r;
        }
    }
    void forward(std::complex<float>* x) const { run(x, false); }
    void inverse(std::complex<float>* x) const {
        run(x, true);
        const float s = 1.0f / float(n_);
        for (int i = 0; i < n_; ++i) x[i] *= s;
    }
private:
    void run(std::complex<float>* x, bool inv) const {
        for (int i = 0; i < n_; ++i) if (i < rev_[size_t(i)]) std::swap(x[i], x[rev_[size_t(i)]]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    std::complex<float> w = tw_[size_t(j * step)];
                    if (inv) w = std::conj(w);
                    const std::complex<float> u = x[i + j], v = x[i + j + len / 2] * w;
                    x[i + j] = u + v;
                    x[i + j + len / 2] = u - v;
                }
        }
    }
    int n_ = 0;
    std::vector<std::complex<float>> tw_;
    std::vector<int> rev_;
};

// ---------------------------------------------------------------- detailed compressor
struct CompSettings { float gainIn, ratio, threshold, attackMs, releaseMs, knee, gainOut; bool makeUp; };

class CompressorPro {
public:
    void prepare(float fs) { fs_ = fs; env_ = 0; }
    static bool bypassed(const CompSettings& c) { return c.ratio <= 1.001f && c.gainIn == 0 && c.gainOut == 0; }
    void process(float* const* ch, int nch, int n, const CompSettings& c) {
        if (bypassed(c)) { gr_ = 0; inLevel_ = 0; return; }
        const float gi = dbToLin(c.gainIn);
        const float kneeDb = c.knee * 20.0f;
        const float slope = 1.0f - 1.0f / c.ratio;
        const float makeup = c.makeUp ? -c.threshold * slope * 0.6f : 0.0f;
        const float go = dbToLin(c.gainOut + makeup);
        const float att = tc(std::max(c.attackMs, 0.05f) * 0.001f, fs_);
        const float rel = tc(std::max(c.releaseMs, 1.0f) * 0.001f, fs_);
        float grMax = 0, inMax = 0;
        for (int i = 0; i < n; ++i) {
            float x = 0;
            for (int k = 0; k < nch; ++k) { ch[k][i] *= gi; x = std::max(x, std::fabs(ch[k][i])); }
            inMax = std::max(inMax, x);
            const float over = linToDb(x) - c.threshold;
            float gr = 0;
            if (kneeDb > 0.01f && std::fabs(over) <= kneeDb / 2) { const float t = over + kneeDb / 2; gr = slope * t * t / (2 * kneeDb); }
            else if (over > 0) gr = slope * over;
            env_ = gr > env_ ? att * env_ + (1 - att) * gr : rel * env_ + (1 - rel) * gr;
            const float g = dbToLin(-env_) * go;
            grMax = std::max(grMax, env_);
            for (int k = 0; k < nch; ++k) ch[k][i] *= g;
        }
        gr_ = grMax; inLevel_ = inMax;
    }
    float reductionDb() const { return gr_; }
    float inputLevel() const { return inLevel_; }
private:
    float fs_ = 48000, env_ = 0, gr_ = 0, inLevel_ = 0;
};

// ---------------------------------------------------------------- detailed gate (band-pass side chain)
struct GateSettings { float threshold, damping, bpHz, attackMs, holdMs, releaseMs; };

class GatePro {
public:
    void prepare(float fs) { fs_ = fs; gain_ = 1; env_ = 0; hold_ = 0; lastBp_ = -1; sc_.reset(); }
    static bool bypassed(const GateSettings& g) { return g.threshold <= -59.95f; }
    void process(float* const* ch, int nch, int n, const GateSettings& g) {
        if (bypassed(g)) { gain_ = 1; return; }
        const bool useBp = g.bpHz >= 100;
        if (useBp && g.bpHz != lastBp_) { sc_.c = makeBiquad(EqType::BandPass, fs_, g.bpHz, 0.94, 0); lastBp_ = g.bpHz; }
        const float thr = dbToLin(g.threshold), thrClose = thr * 0.7f;
        const float floorG = g.damping <= -59.95f ? 0.0f : dbToLin(g.damping);
        const float envA = tc(0.0005f, fs_), envR = tc(0.03f, fs_);
        const float att = tc(std::max(g.attackMs, 0.05f) * 0.001f, fs_);
        const float rel = tc(std::max(g.releaseMs, 1.0f) * 0.001f, fs_);
        const int holdN = int(g.holdMs * 0.001f * fs_);
        for (int i = 0; i < n; ++i) {
            float x = 0;
            for (int k = 0; k < nch; ++k) x += ch[k][i];
            x /= float(std::max(nch, 1));
            if (useBp) x = sc_.process(x);
            x = std::fabs(x);
            env_ = x > env_ ? envA * env_ + (1 - envA) * x : envR * env_ + (1 - envR) * x;
            if (env_ > thr) { open_ = true; hold_ = holdN; }
            else if (env_ < thrClose) { if (hold_ > 0) --hold_; else open_ = false; }
            const float target = open_ ? 1.0f : floorG;
            const float k = target > gain_ ? att : rel;
            gain_ = k * gain_ + (1 - k) * target;
            for (int c = 0; c < nch; ++c) ch[c][i] *= gain_;
        }
    }
    float gain() const { return gain_; }
private:
    float fs_ = 48000, gain_ = 1, env_ = 0, lastBp_ = -1;
    int hold_ = 0;
    bool open_ = true;
    Biquad sc_;
};

// ---------------------------------------------------------------- spectral denoiser (noise floor remover)
class Denoiser {
public:
    static constexpr int N = 512, H = 256;
    void prepare(float) {
        fft_.prepare(N);
        win_.resize(N);
        for (int i = 0; i < N; ++i) win_[size_t(i)] = std::sqrt(0.5f - 0.5f * std::cos(2 * kPi * float(i) / float(N)));
        for (int c = 0; c < 2; ++c) {
            in_[c].assign(N, 0); out_[c].assign(N + H, 0);
            noise_[c].assign(N / 2 + 1, 1e-9f); pow_[c].assign(N / 2 + 1, 0); g_[c].assign(N / 2 + 1, 1);
        }
        buf_.resize(N);
        pos_ = 0; frames_ = 0;
    }
    void process(float* L, float* R, int n, float amount, bool bypass) {
        if (amount <= 0.01f || bypass) { active_ = false; return; }
        if (!active_) { for (int c = 0; c < 2; ++c) { std::fill(in_[c].begin(), in_[c].end(), 0.0f); std::fill(out_[c].begin(), out_[c].end(), 0.0f); } pos_ = 0; active_ = true; }
        amount_ = amount;
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                in_[c][size_t(N - H + pos_)] = io[c][i];
                io[c][i] = out_[c][size_t(pos_)];
            }
            if (++pos_ == H) {
                for (int c = 0; c < 2; ++c) frame(c);
                pos_ = 0;
                ++frames_;
            }
        }
    }
private:
    void frame(int c) {
        for (int i = 0; i < N; ++i) buf_[size_t(i)] = {in_[c][size_t(i)] * win_[size_t(i)], 0.0f};
        fft_.forward(buf_.data());
        const float over = 1.0f + amount_ * 0.35f;
        const float floorG = dbToLin(-3.0f * amount_ - 6.0f);
        for (int k = 0; k <= N / 2; ++k) {
            const float p = std::norm(buf_[size_t(k)]);
            float& ps = pow_[c][size_t(k)];
            ps = 0.6f * ps + 0.4f * p;
            float& nz = noise_[c][size_t(k)];
            if (frames_ < 8) nz = std::max(nz, ps);             // learn quickly at start
            else if (ps < nz) nz = 0.7f * nz + 0.3f * ps;       // follow minimum down fast
            else nz *= 1.003f;                                  // slowly rise (~2.5 dB/s)
            float g = 1.0f - over * nz / std::max(ps, 1e-12f);
            g = std::max(g, floorG);
            float& gs = g_[c][size_t(k)];
            gs = g < gs ? 0.5f * gs + 0.5f * g : 0.3f * gs + 0.7f * g;
            buf_[size_t(k)] *= gs;
            if (k > 0 && k < N / 2) buf_[size_t(N - k)] *= gs;
        }
        fft_.inverse(buf_.data());
        // shift output buffer, overlap-add the new frame
        std::copy(out_[c].begin() + H, out_[c].end(), out_[c].begin());
        std::fill(out_[c].end() - H, out_[c].end(), 0.0f);
        for (int i = 0; i < N; ++i) out_[c][size_t(i)] += buf_[size_t(i)].real() * win_[size_t(i)];
        std::copy(in_[c].begin() + H, in_[c].end(), in_[c].begin());
    }
    FFT fft_;
    std::vector<float> win_, in_[2], out_[2], noise_[2], pow_[2], g_[2];
    std::vector<std::complex<float>> buf_;
    int pos_ = 0;
    uint64_t frames_ = 0;
    float amount_ = 0;
    bool active_ = false;
};

// ---------------------------------------------------------------- voice modeler (pitch + formants)
struct VoiceSettings { bool on; float dryWet, pitch, lo, med, hi; };

class VoiceModeler {
public:
    void prepare(float fs) {
        fs_ = fs;
        win_ = std::max(64, int(0.040f * fs));
        for (auto& b : buf_) b.assign(size_t(win_) * 3 + 8, 0.0f);
        pos_ = 0; d_ = 0;
        last_ = {false, -999, -999, -999, -999, -999};
        for (auto& c : eq_) for (auto& b : c) b.reset();
    }
    void process(float* L, float* R, int n, const VoiceSettings& v) {
        if (!v.on) return;
        if (v.lo != last_.lo || v.med != last_.med || v.hi != last_.hi) {
            const BiquadCoeffs a = makeBiquad(EqType::LowShelf, fs_, 400, 0.7, v.lo * 0.6f);
            const BiquadCoeffs b = makeBiquad(EqType::Peak, fs_, 1400, 0.9, v.med * 0.6f);
            const BiquadCoeffs c = makeBiquad(EqType::HighShelf, fs_, 3500, 0.7, v.hi * 0.6f);
            for (int s = 0; s < 2; ++s) { eq_[s][0].c = a; eq_[s][1].c = b; eq_[s][2].c = c; }
        }
        last_ = v;
        const float ratio = std::pow(2.0f, v.pitch / 12.0f);
        const float wet = std::clamp((v.dryWet + 100.0f) / 200.0f, 0.0f, 1.0f);
        const size_t sz = buf_[0].size();
        const float W = float(win_);
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            buf_[0][pos_] = L[i];
            buf_[1][pos_] = R[i];
            // two read taps half a window apart, delay sweeping at (1 - ratio) samples per sample
            d_ += 1.0f - ratio;
            while (d_ < 0) d_ += W;
            while (d_ >= W) d_ -= W;
            float d2 = d_ + W * 0.5f;
            if (d2 >= W) d2 -= W;
            const float w1 = std::sin(kPi * d_ / W), w2 = std::sin(kPi * d2 / W);
            const float g1 = w1 * w1, g2 = w2 * w2;
            for (int s = 0; s < 2; ++s) {
                auto rd = [&](float d) {
                    const float rp = float(pos_) - d - 2.0f + float(sz);
                    const size_t i0 = size_t(rp) % sz, i1 = (i0 + 1) % sz;
                    const float fr = rp - std::floor(rp);
                    return buf_[s][i0] * (1 - fr) + buf_[s][i1] * fr;
                };
                float y = v.pitch == 0 ? io[s][i] : rd(d_) * g1 + rd(d2) * g2;
                for (auto& b : eq_[s]) y = b.process(y);
                io[s][i] = io[s][i] * (1.0f - wet) + y * wet;
            }
            pos_ = (pos_ + 1) % sz;
        }
    }
private:
    float fs_ = 48000, d_ = 0;
    int win_ = 1920;
    std::vector<float> buf_[2];
    size_t pos_ = 0;
    VoiceSettings last_{};
    Biquad eq_[2][3];
};

// ---------------------------------------------------------------- FDN reverb
struct ReverbSettings {
    int preset; float dryDb, wetDb, preDelayMs, er, decayPct, eqGain[3], eqFreq[3]; int bypass;
};

class ReverbFx {
public:
    static constexpr int kLines = 8;
    void prepare(float fs) {
        fs_ = fs;
        for (int i = 0; i < kLines; ++i) { line_[i].assign(size_t(0.25f * fs) + 8, 0.0f); lpos_[i] = 0; damp_[i] = 0; }
        pre_.assign(size_t(1.05f * fs) + 8, 0.0f); prePos_ = 0;
        er_.assign(size_t(0.12f * fs) + 8, 0.0f); erPos_ = 0;
        lastPreset_ = -1;
        for (auto& c : eq_) for (auto& b : c) b.reset();
        lastEq_[0] = -999;
    }
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const ReverbSettings& s) {
        if (s.bypass == 1) { for (int i = 0; i < n; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; } return; }
        configure(s);
        const float dry = s.dryDb <= -59.9f ? 0.0f : dbToLin(s.dryDb);
        const float wet = s.wetDb <= -59.9f ? 0.0f : dbToLin(s.wetDb);
        const int preN = std::clamp(int(s.preDelayMs * 0.001f * fs_), 0, int(pre_.size()) - 2);
        const float erAmt = s.er / 100.0f;
        static const float erT[6] = {0.0071f, 0.0137f, 0.0223f, 0.0311f, 0.0467f, 0.0619f};
        static const float erG[6] = {0.7f, -0.55f, 0.45f, -0.38f, 0.3f, -0.22f};
        for (int i = 0; i < n; ++i) {
            const float x = 0.5f * (inL[i] + inR[i]);
            pre_[prePos_] = x;
            const float xp = pre_[(prePos_ + pre_.size() - size_t(preN)) % pre_.size()];
            prePos_ = (prePos_ + 1) % pre_.size();
            // early reflections
            er_[erPos_] = xp;
            float erL = 0, erR = 0;
            for (int k = 0; k < 6; ++k) {
                const size_t d = size_t(erT[k] * size_ * fs_) + 1;
                const float v = er_[(erPos_ + er_.size() - std::min(d, er_.size() - 1)) % er_.size()] * erG[k];
                (k & 1 ? erR : erL) += v;
            }
            erPos_ = (erPos_ + 1) % er_.size();
            // FDN with Hadamard feedback
            float o[kLines];
            for (int k = 0; k < kLines; ++k) o[k] = line_[k][(lpos_[k] + line_[k].size() - size_t(len_[k])) % line_[k].size()];
            float h[kLines];
            for (int k = 0; k < kLines; ++k) h[k] = o[k];
            for (int len = 1; len < kLines; len <<= 1)
                for (int a = 0; a < kLines; a += 2 * len)
                    for (int b = a; b < a + len; ++b) { const float u = h[b], v = h[b + len]; h[b] = u + v; h[b + len] = u - v; }
            const float norm = 1.0f / std::sqrt(float(kLines));
            for (int k = 0; k < kLines; ++k) {
                float fb = h[k] * norm * g_[k];
                damp_[k] = fb * (1 - dampC_) + damp_[k] * dampC_;
                line_[k][lpos_[k]] = damp_[k] + xp * (k & 1 ? -0.35f : 0.35f) + (k < 2 ? (k ? erR : erL) * 0.2f : 0.0f);
                lpos_[k] = (lpos_[k] + 1) % line_[k].size();
            }
            float l = (o[0] + o[2] + o[4] + o[6]) * 0.5f + erL * erAmt;
            float r = (o[1] + o[3] + o[5] + o[7]) * 0.5f + erR * erAmt;
            if (s.bypass != 2) for (int b = 0; b < 3; ++b) { l = eq_[0][b].process(l); r = eq_[1][b].process(r); }
            outL[i] = inL[i] * dry + l * wet;
            outR[i] = inR[i] * dry + r * wet;
        }
        for (int k = 0; k < kLines; ++k) if (std::fabs(damp_[k]) < 1e-20f) damp_[k] = 0;
    }
private:
    void configure(const ReverbSettings& s) {
        const int p = std::clamp(s.preset, 0, kReverbPresetCount - 1);
        const ReverbPreset& rp = kReverbPresets[p];
        const float t60 = rp.decay * s.decayPct / 100.0f;
        if (p != lastPreset_ || t60 != lastT60_) {
            static const float base[kLines] = {0.0297f, 0.0371f, 0.0411f, 0.0437f, 0.0503f, 0.0571f, 0.0613f, 0.0683f};
            size_ = 0.4f + rp.size;
            for (int k = 0; k < kLines; ++k) {
                len_[k] = std::min(int(base[k] * size_ * fs_ * 1.3f), int(line_[k].size()) - 2);
                g_[k] = std::pow(10.0f, -3.0f * float(len_[k]) / (std::max(t60, 0.05f) * fs_));
            }
            dampC_ = std::clamp(rp.damping, 0.0f, 0.95f);
            lastPreset_ = p; lastT60_ = t60;
        }
        if (s.eqGain[0] != lastEq_[0] || s.eqGain[1] != lastEq_[1] || s.eqGain[2] != lastEq_[2] ||
            s.eqFreq[0] != lastEq_[3] || s.eqFreq[1] != lastEq_[4] || s.eqFreq[2] != lastEq_[5]) {
            const BiquadCoeffs a = makeBiquad(EqType::LowShelf, fs_, s.eqFreq[0], 0.7, s.eqGain[0]);
            const BiquadCoeffs b = makeBiquad(EqType::Peak, fs_, s.eqFreq[1], 0.8, s.eqGain[1]);
            const BiquadCoeffs c = makeBiquad(EqType::HighShelf, fs_, s.eqFreq[2], 0.7, s.eqGain[2]);
            for (int ch = 0; ch < 2; ++ch) { eq_[ch][0].c = a; eq_[ch][1].c = b; eq_[ch][2].c = c; }
            for (int k = 0; k < 3; ++k) { lastEq_[k] = s.eqGain[k]; lastEq_[3 + k] = s.eqFreq[k]; }
        }
    }
    float fs_ = 48000, size_ = 1, dampC_ = 0.3f, lastT60_ = -1;
    int lastPreset_ = -1;
    std::vector<float> line_[kLines], pre_, er_;
    size_t lpos_[kLines] = {}, prePos_ = 0, erPos_ = 0;
    int len_[kLines] = {};
    float g_[kLines] = {}, damp_[kLines] = {};
    Biquad eq_[2][3];
    float lastEq_[6] = {};
};

// ---------------------------------------------------------------- 8-tap stereo delay
struct DelayTap { bool on; float timeMs, gainDb, pan, lpf, hpf; };
struct DelaySettings { float ratio, feedback, dryDb, wetDb, preDelayMs, balL, balR; bool bypass; DelayTap tap[kDelayTaps]; };

class MultiTapDelay {
public:
    void prepare(float fs) {
        fs_ = fs;
        for (auto& b : buf_) b.assign(size_t(9.0f * fs) + 8, 0.0f);
        pos_ = 0;
        for (auto& t : f_) for (auto& b : t) b.reset();
        for (auto& l : last_) l = -1;
    }
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const DelaySettings& s) {
        if (s.bypass) { for (int i = 0; i < n; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; } return; }
        const float dry = s.dryDb <= -59.9f ? 0.0f : dbToLin(s.dryDb);
        const float wet = s.wetDb <= -59.9f ? 0.0f : dbToLin(s.wetDb);
        const float fb = s.feedback / 100.0f;
        int d[kDelayTaps]; float gl[kDelayTaps], gr[kDelayTaps];
        const size_t sz = buf_[0].size();
        for (int k = 0; k < kDelayTaps; ++k) {
            const DelayTap& t = s.tap[k];
            d[k] = std::clamp(int((t.timeMs * s.ratio + s.preDelayMs) * 0.001f * fs_), 1, int(sz) - 2);
            const float g = t.on ? dbToLin(t.gainDb) : 0.0f;
            const float a = (t.pan + 1) * 0.25f * kPi;
            gl[k] = g * std::cos(a); gr[k] = g * std::sin(a);
            if (t.lpf != last_[k * 2] || t.hpf != last_[k * 2 + 1]) {
                f_[k][0].c = makeBiquad(EqType::LowPass, fs_, t.lpf, 0.7071, 0);
                f_[k][1].c = makeBiquad(EqType::HighPass, fs_, t.hpf, 0.7071, 0);
                last_[k * 2] = t.lpf; last_[k * 2 + 1] = t.hpf;
            }
        }
        // input re-balance: each input channel is panned by its balance knob
        const float aL = (s.balL + 1) * 0.25f * kPi, aR = (s.balR + 1) * 0.25f * kPi;
        for (int i = 0; i < n; ++i) {
            const float l = inL[i] * std::cos(aL) + inR[i] * std::cos(aR);
            const float r = inL[i] * std::sin(aL) + inR[i] * std::sin(aR);
            float sumL = 0, sumR = 0, fbSum = 0;
            for (int k = 0; k < kDelayTaps; ++k) {
                if (!s.tap[k].on) continue;
                const size_t rp = (pos_ + sz - size_t(d[k])) % sz;
                float m = 0.5f * (buf_[0][rp] + buf_[1][rp]);
                m = f_[k][1].process(f_[k][0].process(m));
                sumL += m * gl[k]; sumR += m * gr[k];
                fbSum += m * (gl[k] + gr[k]) * 0.5f;
            }
            buf_[0][pos_] = l + fbSum * fb;
            buf_[1][pos_] = r + fbSum * fb;
            pos_ = (pos_ + 1) % sz;
            outL[i] = inL[i] * dry + sumL * wet;
            outR[i] = inR[i] * dry + sumR * wet;
        }
    }
private:
    float fs_ = 48000;
    std::vector<float> buf_[2];
    size_t pos_ = 0;
    Biquad f_[kDelayTaps][2];
    float last_[kDelayTaps * 2];
};

// ---------------------------------------------------------------- 5-band compressor / limiter
struct C5Band { bool on; float gain, threshold, ratio, attackMs, releaseMs, knee, outGain, limit, dist; bool solo, mute; };
struct C5Settings { float xover[kC5Bands - 1]; C5Band band[kC5Bands]; float outGain; bool makeUp, bypass; };

class MultibandComp {
public:
    void prepare(float fs) {
        fs_ = fs;
        for (auto& b : bands_) for (auto& v : b) v.assign(size_t(kMaxFrames), 0.0f);
        for (auto& s : split_) for (auto& c : s) for (auto& f : c) f.reset();
        for (int b = 0; b < kC5Bands; ++b) { comp_[b].prepare(fs); lim_[b] = 1; }
        for (auto& x : lastX_) x = -1;
    }
    void process(float* L, float* R, int n, const C5Settings& s) {
        if (s.bypass) return;
        for (int k = 0; k < kC5Bands - 1; ++k) {
            if (s.xover[k] != lastX_[k]) {
                const BiquadCoeffs lp = makeBiquad(EqType::LowPass, fs_, s.xover[k], 0.7071, 0);
                const BiquadCoeffs hp = makeBiquad(EqType::HighPass, fs_, s.xover[k], 0.7071, 0);
                for (int c = 0; c < 2; ++c) { split_[k][c][0].c = lp; split_[k][c][1].c = lp; split_[k][c][2].c = hp; split_[k][c][3].c = hp; }
                lastX_[k] = s.xover[k];
            }
        }
        // Linkwitz-Riley (2 x Butterworth) band split, cascaded
        float* io[2] = {L, R};
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < n; ++i) {
                float rest = io[c][i];
                for (int k = 0; k < kC5Bands - 1; ++k) {
                    const float lo = split_[k][c][1].process(split_[k][c][0].process(rest));
                    rest = split_[k][c][3].process(split_[k][c][2].process(rest));
                    bands_[k][c][size_t(i)] = lo;
                }
                bands_[kC5Bands - 1][c][size_t(i)] = rest;
            }
        }
        bool anySolo = false;
        for (auto& b : s.band) anySolo |= b.solo;
        float makeup = 0;
        int active = 0;
        for (int b = 0; b < kC5Bands; ++b) {
            const C5Band& B = s.band[b];
            float* ch[2] = {bands_[b][0].data(), bands_[b][1].data()};
            const bool audible = !B.mute && (!anySolo || B.solo);
            if (!audible) { std::fill(ch[0], ch[0] + n, 0.0f); std::fill(ch[1], ch[1] + n, 0.0f); continue; }
            if (!B.on) continue;
            CompSettings cs{B.gain, B.ratio, B.threshold, B.attackMs, B.releaseMs, B.knee, B.outGain, false};
            comp_[b].process(ch, 2, n, cs);
            makeup += -B.threshold * (1 - 1 / B.ratio) * 0.5f; ++active;
            // brick-wall limiter, "distortion" = faster release
            const float lim = dbToLin(B.limit);
            const float rel = tc(lerp(0.2f, 0.005f, B.dist), fs_);
            for (int i = 0; i < n; ++i) {
                const float pk = std::max(std::fabs(ch[0][i]), std::fabs(ch[1][i]));
                const float t = pk > lim ? lim / pk : 1.0f;
                lim_[b] = t < lim_[b] ? t : rel * lim_[b] + (1 - rel) * t;
                ch[0][i] *= lim_[b]; ch[1][i] *= lim_[b];
            }
        }
        const float og = dbToLin(s.outGain + (s.makeUp && active ? makeup / float(active) : 0.0f));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                float y = 0;
                for (int b = 0; b < kC5Bands; ++b) y += bands_[b][c][size_t(i)];
                io[c][i] = y * og;
            }
    }
private:
    float fs_ = 48000;
    std::vector<float> bands_[kC5Bands][2];
    Biquad split_[kC5Bands - 1][2][4];
    CompressorPro comp_[kC5Bands];
    float lim_[kC5Bands] = {};
    float lastX_[kC5Bands - 1];
};

} // namespace road::dsp
