// musicgen.cpp - final cross-platform version
// Windows MSVC:  cl /nologo /EHsc /O2 /std:c++17 musicgen.cpp /Fe:musicgen.exe
// Windows MinGW: g++ -std=c++17 -O2 musicgen.cpp -o musicgen.exe
// Linux/macOS:   g++ -std=c++17 -O2 -rdynamic musicgen.cpp -o musicgen -ldl
//
// Usage: ./musicgen input.txt output.wav

#include "musicgen.h"

#include <climits>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <unordered_map>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#  define MUSICGEN_HAS_PLUGINS 1
#  include <dlfcn.h>
#elif defined(_WIN32) || defined(_WIN64)
#  define MUSICGEN_HAS_PLUGINS 1
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  define MUSICGEN_HAS_PLUGINS 0
#endif

using namespace std;
using namespace musicgen;

// ============ INTERNAL REGISTRY ============

static InstrumentRegistry& getRegistry() {
    static InstrumentRegistry r;
    return r;
}

template<typename T>
struct AutoRegister {
    AutoRegister() {
        auto inst = make_unique<T>();
        getRegistry().add(toUpper(inst->name()), []{ return make_unique<T>(); });
    }
};
#define REGISTER_INSTRUMENT(CLS) static AutoRegister<CLS> _auto_reg_##CLS

// ============ SYNTH BLOCKS ============
//
// Общий принцип: для лупа (loopEnd > 0) все события ОБРЕЗАЮТСЯ по loopEnd,
// а не заворачиваются. Это убивает "двойной удар" на стыке петли для
// длинных kick/snare. addSampleAt(..., 0, ...) — без wrap.

static int availSamples(const vector<float>& L, int start, int loopEnd) {
    int end = (loopEnd > 0) ? loopEnd : (int)L.size();
    int avail = end - start;
    return avail > 0 ? avail : 0;
}

static void addKick(vector<float>& L, vector<float>& R, int start, double vel, int sr,
                    mt19937& rng, int loopEnd = 0) {
    const double dur = 0.32;
    int n = (int)(dur * sr);
    int nn = std::min(n, availSamples(L, start, loopEnd));
    if (nn <= 0) return;
    uniform_real_distribution<double> noise(-1.0, 1.0);
    double ph = 0.0;
    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double pitch = 44.0 + 115.0 * exp(-t * 28.0);
        ph += TWO_PI * pitch / sr;
        if (ph > TWO_PI) ph -= TWO_PI;
        double body = sin(ph) * exp(-t * 16.0);
        double click = 0.0;
        if (t < 0.004) {
            double taper = 1.0 - t / 0.004;
            click = noise(rng) * 1.2 * exp(-t * 600.0) * taper;
        }
        double s = (body * 1.15 + click) * vel;
        addSampleAt(L, R, start + i, 0, (float)s, (float)s);
    }
}

static void addSnare(vector<float>& L, vector<float>& R, int start, double vel, int sr,
                     mt19937& rng, int loopEnd = 0) {
    const double dur = 0.22;
    int n = (int)(dur * sr);
    int nn = std::min(n, availSamples(L, start, loopEnd));
    if (nn <= 0) return;
    uniform_real_distribution<double> noise(-1.0, 1.0);
    double ph = 0.0, prevNoise = 0.0;
    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double ns = noise(rng);
        double hp = ns - prevNoise; prevNoise = ns;
        double envN = exp(-t * 22.0);
        double envB = exp(-t * 30.0);
        ph += TWO_PI * 186.0 / sr;
        if (ph > TWO_PI) ph -= TWO_PI;
        double body = sin(ph) * envB * 0.45;
        double body2 = sin(ph * 1.5) * envB * 0.3;
        double s = (hp * 0.75 * envN + body + body2) * vel;
        addSampleAt(L, R, start + i, 0, (float)s, (float)s);
    }
}

static void addHat(vector<float>& L, vector<float>& R, int start, double vel, int sr,
                   mt19937& rng, int loopEnd = 0) {
    const double dur = 0.08;
    int n = (int)(dur * sr);
    int nn = std::min(n, availSamples(L, start, loopEnd));
    if (nn <= 0) return;
    uniform_real_distribution<double> noise(-1.0, 1.0);
    double prevNoise = 0.0;
    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double ns = noise(rng);
        double hp = ns - prevNoise; prevNoise = ns;
        double s = hp * 0.55 * exp(-t * 70.0) * vel;
        addSampleAt(L, R, start + i, 0, (float)s, (float)s);
    }
}

static void addOpenHat(vector<float>& L, vector<float>& R, int start, double vel, int sr,
                       mt19937& rng, int loopEnd = 0) {
    const double dur = 0.35;
    int n = (int)(dur * sr);
    int nn = std::min(n, availSamples(L, start, loopEnd));
    if (nn <= 0) return;
    uniform_real_distribution<double> noise(-1.0, 1.0);
    double prevNoise = 0.0;
    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double ns = noise(rng);
        double hp = ns - prevNoise; prevNoise = ns;
        double s = hp * 0.35 * exp(-t * 12.0) * vel;
        addSampleAt(L, R, start + i, 0, (float)s, (float)s);
    }
}

static void addTom(vector<float>& L, vector<float>& R, int start, double vel, int sr,
                   mt19937& /*rng*/, int loopEnd = 0) {
    const double dur = 0.25;
    int n = (int)(dur * sr);
    int nn = std::min(n, availSamples(L, start, loopEnd));
    if (nn <= 0) return;
    double ph = 0.0;
    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double pitch = 140.0 + 100.0 * exp(-t * 22.0);
        ph += TWO_PI * pitch / sr;
        if (ph > TWO_PI) ph -= TWO_PI;
        double body = sin(ph) * exp(-t * 12.0);
        double attack = std::min(t / 0.002, 1.0);
        double s = body * attack * vel * 0.9;
        addSampleAt(L, R, start + i, 0, (float)s, (float)s);
    }
}

// Мелодические: TPTSVF + filter envelope, обрезка по loopEnd.
static void addBass(vector<float>& L, vector<float>& R, int start,
                    const vector<double>& freqs, double vel, int samples, int sr,
                    double resonance, int loopEnd = 0) {
    if (samples <= 0 || freqs.empty()) return;
    int nn = std::min(samples, availSamples(L, start, loopEnd));
    if (nn <= 0) return;

    TPTSVF filt;
    const double baseCut = clampd(freqs[0] * 6.0 + 500.0, 500.0, 5000.0);
    const double envAmt = 3500.0;
    const double envRate = 9.0;

    vector<double> phases(freqs.size(), 0.0);
    vector<double> dts(freqs.size());
    for (size_t f = 0; f < freqs.size(); ++f) dts[f] = freqs[f] / sr;

    const double releaseStart = 0.7;
    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double tNorm = (double)i / samples;

        double fe = exp(-t * envRate);
        double fc = baseCut + envAmt * fe;
        filt.setParams(fc, sr, resonance);

        double attack = std::min(t / 0.004, 1.0);
        double decay = exp(-t * 3.2);
        double rel = 1.0;
        if (tNorm > releaseStart) {
            double rp = (tNorm - releaseStart) / (1.0 - releaseStart);
            rel = 0.5 * (1.0 + cos(PI * rp));
        }
        double env = attack * decay * rel;

        double raw = 0.0;
        for (size_t f = 0; f < freqs.size(); ++f) {
            phases[f] += dts[f];
            if (phases[f] >= 1.0) phases[f] -= 1.0;
            raw += sawPolyBlep(phases[f], dts[f]);
        }
        raw /= sqrt((double)freqs.size());

        double s = filt.processLowpass(raw) * env * vel * 0.85;
        addSampleAt(L, R, start + i, 0, (float)s, (float)s);
    }
}

static void addLead(vector<float>& L, vector<float>& R, int start,
                    const vector<double>& freqs, double vel, int samples, int sr,
                    double resonance, int loopEnd = 0) {
    if (samples <= 0 || freqs.empty()) return;
    int nn = std::min(samples, availSamples(L, start, loopEnd));
    if (nn <= 0) return;

    TPTSVF filtL, filtR;
    const double maxSafe = std::min(8000.0, sr / 6.0);
    const double baseCut = clampd(freqs[0] * 4.0 + 2000.0, 2000.0, maxSafe);
    const double envAmt = 5000.0;
    const double envRate = 7.0;

    const int unisonVoices = 3;
    vector<vector<double>> phases(freqs.size(), vector<double>(unisonVoices, 0.0));
    vector<vector<double>> dts(freqs.size(), vector<double>(unisonVoices, 0.0));
    for (size_t ni = 0; ni < freqs.size(); ++ni)
        for (int v = 0; v < unisonVoices; ++v) {
            double detune = 1.0 + (v - (unisonVoices - 1) / 2.0) * 0.007;
            dts[ni][v] = freqs[ni] * detune / sr;
        }

    const double releaseStart = 0.7;
    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double tNorm = (double)i / samples;

        double fe = exp(-t * envRate);
        double fc = baseCut + envAmt * fe * (1.0 - tNorm * 0.5);
        filtL.setParams(fc, sr, resonance);
        filtR.setParams(fc * 1.02, sr, resonance);

        double attack = std::min(t / 0.006, 1.0);
        double decay = exp(-t * 4.5);
        double rel = 1.0;
        if (tNorm > releaseStart) {
            double rp = (tNorm - releaseStart) / (1.0 - releaseStart);
            rel = 0.5 * (1.0 + cos(PI * rp));
        }
        double env = attack * decay * rel;

        double raw = 0.0;
        int totalOsc = 0;
        for (size_t ni = 0; ni < freqs.size(); ++ni)
            for (int v = 0; v < unisonVoices; ++v) {
                phases[ni][v] += dts[ni][v];
                if (phases[ni][v] >= 1.0) phases[ni][v] -= 1.0;
                raw += sawPolyBlep(phases[ni][v], dts[ni][v]);
                ++totalOsc;
            }
        raw /= sqrt((double)totalOsc);

        double s = raw * env * vel * 0.5;
        double sL = filtL.processLowpass(s);
        double sR = filtR.processLowpass(s);
        addSampleAt(L, R, start + i, 0, (float)(sL * 0.8), (float)(sR * 0.7));
    }
}

static void addPluck(vector<float>& L, vector<float>& R, int start,
                     const vector<double>& freqs, double vel, int samples, int sr,
                     double resonance, int loopEnd = 0) {
    if (samples <= 0 || freqs.empty()) return;
    int nn = std::min(samples, availSamples(L, start, loopEnd));
    if (nn <= 0) return;

    TPTSVF filt;
    const double baseCut = clampd(freqs[0] * 6.0 + 800.0, 800.0, 6000.0);

    vector<double> phases(freqs.size(), 0.0);
    vector<double> dts(freqs.size());
    for (size_t f = 0; f < freqs.size(); ++f) dts[f] = freqs[f] / sr;

    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double fe = exp(-t * 15.0);
        double fc = baseCut + 4000.0 * fe;
        filt.setParams(fc, sr, resonance);

        double env = std::min(t / 0.001, 1.0) * exp(-t * 6.0);

        double raw = 0.0;
        for (size_t f = 0; f < freqs.size(); ++f) {
            phases[f] += dts[f];
            if (phases[f] >= 1.0) phases[f] -= 1.0;
            raw += sawPolyBlep(phases[f], dts[f]);
        }
        raw /= sqrt((double)freqs.size());

        double s = filt.processLowpass(raw) * env * vel * 0.7;
        addSampleAt(L, R, start + i, 0, (float)s, (float)s);
    }
}

static void addPad(vector<float>& L, vector<float>& R, int start,
                   const vector<double>& freqs, double vel, int samples, int sr,
                   double resonance, int loopEnd = 0) {
    if (samples <= 0 || freqs.empty()) return;
    int nn = std::min(samples, availSamples(L, start, loopEnd));
    if (nn <= 0) return;

    TPTSVF filtL, filtR;
    const double baseCut = clampd(freqs[0] * 3.0 + 500.0, 500.0, 4000.0);

    const int voices = 5;
    vector<vector<double>> phases(freqs.size(), vector<double>(voices, 0.0));
    vector<vector<double>> dts(freqs.size(), vector<double>(voices, 0.0));
    for (size_t ni = 0; ni < freqs.size(); ++ni)
        for (int v = 0; v < voices; ++v) {
            double detune = 1.0 + (v - (voices - 1) / 2.0) * 0.008;
            phases[ni][v] = std::fmod(v * 0.13, 1.0);
            dts[ni][v] = freqs[ni] * detune / sr;
        }

    for (int i = 0; i < nn; ++i) {
        double t = i / (double)sr;
        double tNorm = (double)i / samples;
        double attack = std::min(t / 0.15, 1.0);
        double release = 1.0;
        if (tNorm > 0.8) {
            double rp = (tNorm - 0.8) / 0.2;
            release = 0.5 * (1.0 + cos(PI * rp));
        }
        double env = attack * release;

        double fe = exp(-t * 1.5);
        double fc = baseCut + 2500.0 * fe * (1.0 - tNorm);
        filtL.setParams(fc, sr, resonance);
        filtR.setParams(fc * 1.02, sr, resonance);

        double rawL = 0.0, rawR = 0.0;
        int totalOsc = 0;
        for (size_t ni = 0; ni < freqs.size(); ++ni)
            for (int v = 0; v < voices; ++v) {
                phases[ni][v] += dts[ni][v];
                if (phases[ni][v] >= 1.0) phases[ni][v] -= 1.0;
                double s = sawPolyBlep(phases[ni][v], dts[ni][v]);
                if (v < voices / 2) rawL += s;
                else if (v > voices / 2) rawR += s;
                else { rawL += s * 0.7; rawR += s * 0.7; }
                ++totalOsc;
            }
        double norm = sqrt((double)totalOsc);
        rawL /= norm; rawR /= norm;

        double sL = filtL.processLowpass(rawL) * env * vel * 0.35;
        double sR = filtR.processLowpass(rawR) * env * vel * 0.35;
        addSampleAt(L, R, start + i, 0, (float)sL, (float)sR);
    }
}

// ============ BUILT-IN INSTRUMENTS ============

static int readDurationSteps(const vector<Step>& steps, int s) {
    if (s < 0 || s >= (int)steps.size() || steps[s].params.empty()) return 1;
    int d = (int)steps[s].params[0];
    return (d >= 1) ? d : 1;
}

class KickInstrument : public Instrument {
public:
    string name() const override { return "KICK"; }
    InstrumentCategory category() const override { return InstrumentCategory::Drum; }
    bool isSidechainSource() const override { return true; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parsePercTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& rng,
                vector<float>& L, vector<float>& R) override {
        for (int s = 0; s < (int)steps.size(); ++s)
            if (steps[s].on) addKick(L, R, s * ctx.stepSamples, steps[s].vel,
                                     ctx.sampleRate, rng, ctx.loopEnd);
    }
};
REGISTER_INSTRUMENT(KickInstrument);

class SnareInstrument : public Instrument {
public:
    string name() const override { return "SNARE"; }
    InstrumentCategory category() const override { return InstrumentCategory::Drum; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parsePercTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& rng,
                vector<float>& L, vector<float>& R) override {
        for (int s = 0; s < (int)steps.size(); ++s)
            if (steps[s].on) addSnare(L, R, s * ctx.stepSamples, steps[s].vel,
                                      ctx.sampleRate, rng, ctx.loopEnd);
    }
};
REGISTER_INSTRUMENT(SnareInstrument);

class HatInstrument : public Instrument {
public:
    string name() const override { return "HAT"; }
    InstrumentCategory category() const override { return InstrumentCategory::Drum; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parsePercTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& rng,
                vector<float>& L, vector<float>& R) override {
        for (int s = 0; s < (int)steps.size(); ++s)
            if (steps[s].on) addHat(L, R, s * ctx.stepSamples, steps[s].vel,
                                    ctx.sampleRate, rng, ctx.loopEnd);
    }
};
REGISTER_INSTRUMENT(HatInstrument);

class OpenHatInstrument : public Instrument {
public:
    string name() const override { return "OPENHAT"; }
    InstrumentCategory category() const override { return InstrumentCategory::Drum; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parsePercTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& rng,
                vector<float>& L, vector<float>& R) override {
        for (int s = 0; s < (int)steps.size(); ++s)
            if (steps[s].on) addOpenHat(L, R, s * ctx.stepSamples, steps[s].vel,
                                        ctx.sampleRate, rng, ctx.loopEnd);
    }
};
REGISTER_INSTRUMENT(OpenHatInstrument);

class TomInstrument : public Instrument {
public:
    string name() const override { return "TOM"; }
    InstrumentCategory category() const override { return InstrumentCategory::Drum; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parsePercTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& rng,
                vector<float>& L, vector<float>& R) override {
        for (int s = 0; s < (int)steps.size(); ++s)
            if (steps[s].on) addTom(L, R, s * ctx.stepSamples, steps[s].vel,
                                    ctx.sampleRate, rng, ctx.loopEnd);
    }
};
REGISTER_INSTRUMENT(TomInstrument);

class BassInstrument : public Instrument {
public:
    string name() const override { return "BASS"; }
    InstrumentCategory category() const override { return InstrumentCategory::Melodic; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parseNoteTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& /*rng*/,
                vector<float>& L, vector<float>& R) override {
        double res = ctx.params ? ctx.params->bassResonance : 0.3;
        for (int s = 0; s < (int)steps.size(); ++s) {
            if (!steps[s].on || steps[s].freqs.empty()) continue;
            int durSteps = readDurationSteps(steps, s);
            int dur = (int)(ctx.stepSamples * 0.92 * durSteps);
            addBass(L, R, s * ctx.stepSamples, steps[s].freqs, steps[s].vel, dur,
                    ctx.sampleRate, res, ctx.loopEnd);
        }
    }
};
REGISTER_INSTRUMENT(BassInstrument);

class LeadInstrument : public Instrument {
public:
    string name() const override { return "LEAD"; }
    InstrumentCategory category() const override { return InstrumentCategory::Melodic; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parseNoteTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& /*rng*/,
                vector<float>& L, vector<float>& R) override {
        double res = ctx.params ? ctx.params->leadResonance : 0.5;
        for (int s = 0; s < (int)steps.size(); ++s) {
            if (!steps[s].on || steps[s].freqs.empty()) continue;
            int durSteps = readDurationSteps(steps, s);
            int dur = (int)(ctx.stepSamples * 0.88 * durSteps);
            addLead(L, R, s * ctx.stepSamples, steps[s].freqs, steps[s].vel, dur,
                    ctx.sampleRate, res, ctx.loopEnd);
        }
    }
};
REGISTER_INSTRUMENT(LeadInstrument);

class PluckInstrument : public Instrument {
public:
    string name() const override { return "PLUCK"; }
    InstrumentCategory category() const override { return InstrumentCategory::Melodic; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parseNoteTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& /*rng*/,
                vector<float>& L, vector<float>& R) override {
        double res = ctx.params ? ctx.params->leadResonance : 0.4;
        for (int s = 0; s < (int)steps.size(); ++s) {
            if (!steps[s].on || steps[s].freqs.empty()) continue;
            int durSteps = readDurationSteps(steps, s);
            int dur = (int)(ctx.stepSamples * 0.95 * durSteps);
            addPluck(L, R, s * ctx.stepSamples, steps[s].freqs, steps[s].vel, dur,
                     ctx.sampleRate, res, ctx.loopEnd);
        }
    }
};
REGISTER_INSTRUMENT(PluckInstrument);

class PadInstrument : public Instrument {
public:
    string name() const override { return "PAD"; }
    InstrumentCategory category() const override { return InstrumentCategory::Melodic; }
    bool parse(const vector<string>& toks, int ts, mt19937& rng, vector<Step>& out, string& err) override {
        return parseNoteTokens(toks, ts, rng, out, err);
    }
    void render(const vector<Step>& steps, const RenderCtx& ctx, mt19937& /*rng*/,
                vector<float>& L, vector<float>& R) override {
        double res = ctx.params ? ctx.params->bassResonance : 0.2;
        for (int s = 0; s < (int)steps.size(); ++s) {
            if (!steps[s].on || steps[s].freqs.empty()) continue;
            int durSteps = readDurationSteps(steps, s);
            int dur = (int)(ctx.stepSamples * 1.0 * durSteps);
            addPad(L, R, s * ctx.stepSamples, steps[s].freqs, steps[s].vel, dur,
                   ctx.sampleRate, res, ctx.loopEnd);
        }
    }
};
REGISTER_INSTRUMENT(PadInstrument);

// ============ EFFECTS ============

// Обрезка по loopEnd: не заворачиваем хвост sidechain в начало петли.
static void applySidechain(vector<float>& L, vector<float>& R,
                           const vector<Step>& src, int stepSamples,
                           double depth, double durationSec, int sr, int loopEnd = 0) {
    int duration = (int)(durationSec * sr);
    if (duration <= 0) return;
    int N = (int)L.size();
    int end = (loopEnd > 0) ? loopEnd : N;
    vector<float> gain(N, 1.0f);
    for (int step = 0; step < (int)src.size(); ++step) {
        if (!src[step].on) continue;
        int start = step * stepSamples;
        if (start >= end) continue;
        int len = std::min(duration, end - start);
        for (int i = 0; i < len; ++i) {
            int idx = start + i;
            if (idx < 0 || idx >= N) continue;
            double t = (double)i / duration;
            double sm = t * t * (3.0 - 2.0 * t);
            float g = (float)(1.0 - depth * (1.0 - sm));
            if (g < gain[idx]) gain[idx] = g;
        }
    }
    for (int i = 0; i < N; ++i) { L[i] *= gain[i]; R[i] *= gain[i]; }
}

// Allpass-диффузор Schroeder.
struct Allpass1 {
    vector<float> buf;
    int pos = 0;
    float feedback = 0.7f;
    void init(int n) { buf.assign(std::max(1, n), 0.0f); pos = 0; }
    float process(float in) {
        float d = buf[pos];
        float out = -in + d;
        buf[pos] = in + d * feedback;
        if (++pos >= (int)buf.size()) pos = 0;
        return out;
    }
};

// Реверб: 3 allpass в серию + 4 comb. Wet — моно (было так и раньше).
static void applyReverb(vector<float>& L, vector<float>& R,
                        double mix, double decay, double damping, int sr,
                        bool doLoop, int loopSamples) {
    vector<int> delays = {(int)(0.029*sr),(int)(0.037*sr),(int)(0.041*sr),(int)(0.043*sr)};
    vector<vector<float>> bufs(delays.size());
    for (size_t i = 0; i < delays.size(); ++i) bufs[i].resize(delays[i], 0.0f);
    vector<int> pos(delays.size(), 0);
    vector<float> damped(delays.size(), 0.0f);

    vector<Allpass1> aps(3);
    aps[0].init((int)(0.0050 * sr)); aps[0].feedback = 0.7f;
    aps[1].init((int)(0.0017 * sr)); aps[1].feedback = 0.7f;
    aps[2].init((int)(0.0031 * sr)); aps[2].feedback = 0.7f;

    auto tick = [&](size_t idx, bool writeOut, const vector<float>& sL, const vector<float>& sR) {
        float inp = (sL[idx] + sR[idx]) * 0.5f;
        for (auto& ap : aps) inp = ap.process(inp);

        float wet = 0.0f;
        for (int j = 0; j < (int)delays.size(); ++j) {
            float d = bufs[j][pos[j]];
            damped[j] += (float)damping * (d - damped[j]);
            wet += damped[j];
            bufs[j][pos[j]] = inp + damped[j] * (float)decay;
            if (++pos[j] >= delays[j]) pos[j] = 0;
        }
        wet /= delays.size();
        if (writeOut) {
            L[idx] = sL[idx] * (float)(1.0 - mix) + wet * (float)mix;
            R[idx] = sR[idx] * (float)(1.0 - mix) + wet * (float)mix;
        }
    };

    if (doLoop && loopSamples > 0 && loopSamples <= (int)L.size()) {
        auto dryL = L, dryR = R;
        int warmup = 12;
        double dSafe = std::max(0.01, std::min(0.99, decay));
        if (dSafe > 0.5) {
            int needed = (int)std::ceil(std::log(0.1) / std::log(dSafe));
            warmup = std::min(128, std::max(warmup, needed));
        }
        for (int r = 0; r < warmup; ++r)
            for (int i = 0; i < loopSamples; ++i) tick(i, false, dryL, dryR);
        for (int i = 0; i < loopSamples; ++i) tick(i, true, dryL, dryR);
        if ((int)L.size() > loopSamples) { L.resize(loopSamples); R.resize(loopSamples); }
    } else {
        for (size_t i = 0; i < L.size(); ++i) tick(i, true, L, R);
    }
}

// Настоящий lookahead-лимитер без delay-линии.
//
// 1. peak[i] = max(|L[i]|, |R[i]|)
// 2. smax[i] = max(peak[i..i+look]) — forward sliding max, монотонный deque.
//    Loop: окно заворачивается через [N-1] -> [0]. Non-loop: хвост zero-pad.
// 3. hardTg[i] = ceiling / peak[i]   — немедленный target для ТЕКУЩЕГО sample.
//    softTg[i] = ceiling / smax[i]   — target с учётом будущего окна.
//    Всегда softTg[i] <= hardTg[i], так как smax[i] >= peak[i].
// 4. gain ramp: атака за look сэмплов (attackCoef = exp(-5/look)),
//    release 100 мс. Атака гарантирует плавный спад перед пиком.
// 5. finalGain[i] = min(rampedGain[i], hardTg[i]) — СТРОГИЙ ceiling.
//    min срабатывает только если ramp не успел (пик в самом начале окна
//    обнаружения) — редкий случай, ступенька в 1 сэмпл не слышна.
//
// Что даёт по сравнению с простым brickwall:
//   - Скачок gain 1.0 -> ceiling/P за look сэмплов (плавно) вместо 1 сэмпла.
//     Для sustained+transient (пэд + удар) клика больше нет.
//   - Ceiling 0.95 строгий математически (hard min).
//
// Ограничение (known, не чиню): в non-loop последние ~100 мс могут быть
// over-attenuated — release от предыдущего пика не успевает вернуться к 1.0.
// Для loop нерелевантно (wrap + второй проход).
static void applyLimiter(std::vector<float>& L, std::vector<float>& R,
                         float ceiling, int sr, int lookaheadMs = 4,
                         bool doLoop = false) {
    int N = (int)L.size();
    if (N == 0) return;
    int look = std::max(1, sr * lookaheadMs / 1000);

    if (N <= look) {
        for (int i = 0; i < N; ++i) {
            L[i] = (float)clampd(L[i], -ceiling, ceiling);
            R[i] = (float)clampd(R[i], -ceiling, ceiling);
        }
        return;
    }

    std::vector<float> peak(N);
    for (int i = 0; i < N; ++i)
        peak[i] = std::max(std::fabs(L[i]), std::fabs(R[i]));

    int ExtN = N + look + 1;
    std::vector<float> peakExt(ExtN, 0.0f);
    for (int i = 0; i < N; ++i) peakExt[i] = peak[i];
    if (doLoop) {
        for (int i = 0; i <= look; ++i) peakExt[N + i] = peak[i % N];
    }

    // Forward sliding max по peakExt[i..i+look] через монотонный deque, O(N).
    std::vector<float> smax(N, 0.0f);
    std::deque<int> dq;
    for (int k = 0; k <= look && k < ExtN; ++k) {
        while (!dq.empty() && peakExt[dq.back()] <= peakExt[k]) dq.pop_back();
        dq.push_back(k);
    }
    for (int i = 0; i < N; ++i) {
        while (!dq.empty() && dq.front() < i) dq.pop_front();
        smax[i] = dq.empty() ? 0.0f : peakExt[dq.front()];
        int next = i + look + 1;
        if (next < ExtN) {
            while (!dq.empty() && peakExt[dq.back()] <= peakExt[next]) dq.pop_back();
            dq.push_back(next);
        }
    }

    // Target arrays.
    std::vector<float> hardTg(N), softTg(N), gainArr(N);
    for (int i = 0; i < N; ++i) {
        hardTg[i] = (peak[i]  > ceiling) ? ceiling / peak[i]  : 1.0f;
        softTg[i] = (smax[i]  > ceiling) ? ceiling / smax[i]  : 1.0f;
    }

    // Attack ramp за look сэмплов: за look gain доходит до exp(-5) ≈ 0.67%
    // отклонения от target. Release — TC 100 мс (full recovery ~300-500 мс).
    float attackCoef = std::exp(-5.0f / look);
    float relCoef    = std::exp(-1.0f / (sr * 0.100f));

    auto rampInto = [&](float startGain, bool writeOut) {
        float gain = startGain;
        for (int i = 0; i < N; ++i) {
            if (softTg[i] < gain) gain = softTg[i] + (gain - softTg[i]) * attackCoef;
            else                  gain = softTg[i] + (gain - softTg[i]) * relCoef;
            if (writeOut) gainArr[i] = std::min(gain, hardTg[i]);
        }
        return gain;
    };

    if (doLoop) {
        // Pre-warmup: первый проход устанавливает равновесное gain на границе.
        // Второй проход пишет gainArr уже с правильным начальным состоянием.
        float g0 = rampInto(1.0f, false);
        rampInto(g0, true);
    } else {
        rampInto(1.0f, true);
    }

    for (int i = 0; i < N; ++i) {
        L[i] *= gainArr[i];
        R[i] *= gainArr[i];
    }
}

static void processMaster(vector<float>& L, vector<float>& R, const Params& p, int loopSamples) {
    int sr = p.sampleRate;
    double driveAmt = 1.0 + p.drive * 4.0;
    double norm = std::tanh(driveAmt);
    if (norm < 1e-6) norm = 1.0;

    double maxCut = std::min(18000.0, sr * 0.45);
    double cutHz;
    if (p.filterCut <= 0.0) cutHz = 120.0;
    else if (p.filterCut >= 1.0) cutHz = maxCut;
    else cutHz = 120.0 * std::pow(maxCut / 120.0, p.filterCut);

    TPTSVF fL, fR;
    fL.setParams(cutHz, sr, p.filterRes);
    fR.setParams(cutHz, sr, p.filterRes);

    auto softclip = [&](double x) { return std::tanh(x * driveAmt) / norm; };
    bool doLoop = loopSamples > 0 && loopSamples <= (int)L.size();

    auto tickF = [&](double inL, double inR, float& oL, float& oR) {
        double dL = (1.0 - p.drive) * inL + p.drive * softclip(inL);
        double dR = (1.0 - p.drive) * inR + p.drive * softclip(inR);
        oL = (float)fL.processLowpass(dL);
        oR = (float)fR.processLowpass(dR);
    };

    if (doLoop) {
        L.resize(loopSamples); R.resize(loopSamples);
        auto dryL = L, dryR = R;
        for (int r = 0; r < 16; ++r)
            for (int i = 0; i < loopSamples; ++i) { float oL, oR; tickF(dryL[i], dryR[i], oL, oR); }
        for (int i = 0; i < loopSamples; ++i) {
            float oL, oR; tickF(dryL[i], dryR[i], oL, oR);
            L[i] = oL; R[i] = oR;
        }
    } else {
        for (size_t i = 0; i < L.size(); ++i) {
            float oL, oR; tickF(L[i], R[i], oL, oR);
            L[i] = oL; R[i] = oR;
        }
    }

    int ds = std::max(1, (int)std::lround(p.delayTime * sr));
    vector<float> dL(ds, 0.0f), dR(ds, 0.0f);
    int w = 0;
    auto tickD = [&](float inL, float inR, float& oL, float& oR) {
        float aL = dL[w], aR = dR[w];
        oL = inL + aL * (float)p.delayMix;
        oR = inR + aR * (float)p.delayMix;
        dL[w] = inL + aL * (float)p.delayFeedback;
        dR[w] = inR + aR * (float)p.delayFeedback;
        if (++w == ds) w = 0;
    };

    if (doLoop) {
        int warmup = 8;
        double fbSafe = std::max(0.01, std::min(0.99, p.delayFeedback));
        if (fbSafe > 0.5) {
            int needed = (int)std::ceil(std::log(0.1) / std::log(fbSafe));
            warmup = std::min(128, std::max(warmup, needed));
        }
        for (int r = 0; r < warmup; ++r)
            for (int i = 0; i < loopSamples; ++i) { float oL, oR; tickD(L[i], R[i], oL, oR); }
        for (int i = 0; i < loopSamples; ++i) {
            float oL, oR; tickD(L[i], R[i], oL, oR);
            L[i] = oL; R[i] = oR;
        }
    } else {
        for (size_t i = 0; i < L.size(); ++i) {
            float oL, oR; tickD(L[i], R[i], oL, oR);
            L[i] = oL; R[i] = oR;
        }
    }

    applyLimiter(L, R, 0.95f, sr, 4, doLoop);
}

// ============ WAV ============

static void writeU16(std::ostream& o, uint16_t v) { o.put((char)(v & 0xFF)); o.put((char)((v >> 8) & 0xFF)); }
static void writeU32(std::ostream& o, uint32_t v) {
    o.put((char)(v & 0xFF)); o.put((char)((v >> 8) & 0xFF));
    o.put((char)((v >> 16) & 0xFF)); o.put((char)((v >> 24) & 0xFF));
}
static bool writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, int sr) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    uint32_t frames = (uint32_t)L.size();
    uint16_t ch = 2, bps = 16, ba = ch * bps / 8;
    uint32_t br = (uint32_t)sr * ba, ds = frames * ba, cs = 36 + ds;
    o.write("RIFF", 4); writeU32(o, cs); o.write("WAVE", 4);
    o.write("fmt ", 4); writeU32(o, 16);
    writeU16(o, 1); writeU16(o, ch);
    writeU32(o, (uint32_t)sr); writeU32(o, br);
    writeU16(o, ba); writeU16(o, bps);
    o.write("data", 4); writeU32(o, ds);
    for (size_t i = 0; i < L.size(); ++i) {
        double l = clampd(L[i], -1, 1), r = clampd(R[i], -1, 1);
        writeU16(o, (uint16_t)(int16_t)std::lround(l * 32767.0));
        writeU16(o, (uint16_t)(int16_t)std::lround(r * 32767.0));
    }
    return true;
}

// ============ PLUGINS ============

using PluginRegisterFn = bool(*)(InstrumentRegistry*);
static std::vector<void*> g_pluginHandles;
static std::unordered_map<std::string, void*> g_loadedPaths;

enum class PluginLoadStatus { Loaded, AlreadyLoaded, Failed };

#if defined(_WIN32) || defined(_WIN64)
static std::string winErrorMessage() {
    DWORD code = GetLastError();
    if (code == 0) return "unknown error";
    char* buf = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buf, 0, nullptr);
    std::string msg = "error " + std::to_string(code);
    if (len && buf) {
        msg.assign(buf, len);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
            msg.pop_back();
    }
    if (buf) LocalFree(buf);
    return msg;
}
#endif

static PluginLoadStatus loadPlugin(const std::string& path, std::string& err) {
#if MUSICGEN_HAS_PLUGINS
    std::string key;
#if defined(_WIN32) || defined(_WIN64)
    char realBuf[4096];
    if (_fullpath(realBuf, path.c_str(), sizeof(realBuf))) key = realBuf;
    else key = path;
#else
    char realBuf[PATH_MAX];
    if (realpath(path.c_str(), realBuf)) key = realBuf;
    else key = path;
#endif

    if (g_loadedPaths.count(key)) {
        return PluginLoadStatus::AlreadyLoaded;
    }

#if defined(_WIN32) || defined(_WIN64)
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h) { err = winErrorMessage(); return PluginLoadStatus::Failed; }

    auto fn = reinterpret_cast<PluginRegisterFn>(
                  reinterpret_cast<void*>(GetProcAddress(h, "register_instruments")));
    if (!fn) {
        err = "missing 'register_instruments'";
        FreeLibrary(h);
        return PluginLoadStatus::Failed;
    }
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { err = dlerror(); return PluginLoadStatus::Failed; }

    auto fn = (PluginRegisterFn)dlsym(h, "register_instruments");
    if (!fn) {
        err = "missing 'register_instruments'";
        dlclose(h);
        return PluginLoadStatus::Failed;
    }
#endif

    auto saved = getRegistry().snapshot();

    bool ok = false;
    try {
        ok = fn(&getRegistry());
        if (!ok) err = "register_instruments returned false";
    } catch (const std::exception& e) {
        err = std::string("plugin threw: ") + e.what();
        ok = false;
    } catch (...) {
        err = "plugin threw unknown";
        ok = false;
    }

    if (!ok) {
        getRegistry().restore(std::move(saved));
#if defined(_WIN32) || defined(_WIN64)
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        return PluginLoadStatus::Failed;
    }

    void* handle = static_cast<void*>(h);
    g_loadedPaths.emplace(std::move(key), handle);
    g_pluginHandles.push_back(handle);
    return PluginLoadStatus::Loaded;
#else
    (void)path;
    err = "plugin loading not supported on this platform";
    return PluginLoadStatus::Failed;
#endif
}

// ============ SAFE RENDER ============

struct RenderResult {
    bool ok = false;
    std::string error;
    double peak = 0.0;
    size_t nonFinite = 0;
};

static RenderResult safeRender(Instrument& ins, const std::vector<Step>& steps,
                               const RenderCtx& ctx, std::mt19937& rng,
                               std::vector<float>& outL, std::vector<float>& outR) {
    outL.assign(ctx.totalSamples, 0.0f);
    outR.assign(ctx.totalSamples, 0.0f);
    RenderResult res;

    try { ins.render(steps, ctx, rng, outL, outR); }
    catch (const std::exception& e) { res.error = std::string("render threw: ") + e.what(); return res; }
    catch (...) { res.error = "render threw unknown"; return res; }

    double peak = 0.0;
    size_t bad = 0;
    for (size_t i = 0; i < outL.size(); ++i) {
        if (!std::isfinite(outL[i])) { outL[i] = 0; ++bad; }
        if (!std::isfinite(outR[i])) { outR[i] = 0; ++bad; }
        peak = std::max(peak, (double)std::fabs(outL[i]));
        peak = std::max(peak, (double)std::fabs(outR[i]));
    }
    res.peak = peak;
    res.nonFinite = bad;
    if (bad > 0) { res.error = "produced non-finite samples (" + std::to_string(bad) + ")"; return res; }
    if (peak > 8.0) { res.error = "peak exceeds 8.0 (" + std::to_string(peak) + ")"; return res; }
    res.ok = true;
    return res;
}

// ============ MAIN PIPELINE ============

struct PreparedInstrument {
    std::unique_ptr<Instrument> inst;
    std::string name;
    std::vector<Step> steps;
};

static unsigned int hashName(const std::string& s) {
    unsigned int h = 2166136261u;
    for (char c : s) { h ^= (unsigned char)c; h *= 16777619u; }
    return h;
}

// Strip comments.
// LOAD_PLUGIN: только ';' считается комментарием. '//' НЕ режется, потому
//   что Windows-пути вида C://plugins/x.dll содержат '//'. Для комментариев
//   на строке LOAD_PLUGIN используйте ';'.
// Остальные строки: '//' и ';' оба режутся.
static void scrub(std::string& line) {
    size_t start = line.find_first_not_of(" \t");
    if (start != std::string::npos) {
        size_t end = line.find_first_of(" \t", start);
        std::string first = line.substr(start, (end == std::string::npos ? line.size() : end) - start);
        if (toUpper(first) == "LOAD_PLUGIN") {
            size_t pos = line.find(';');
            if (pos != std::string::npos) line.erase(pos);
            return;
        }
    }
    size_t pos = line.find("//"); if (pos != std::string::npos) line.erase(pos);
    pos = line.find(';');         if (pos != std::string::npos) line.erase(pos);
}

static bool renderTxtToWav(const std::string& txtPath, const std::string& wavPath) {
    // ---- Pass 1: LOAD_PLUGIN ----
    {
        std::ifstream in(txtPath);
        if (!in) { std::cerr << "Cannot open input: " << txtPath << "\n"; return false; }
        std::string line;
        while (std::getline(in, line)) {
            scrub(line);
            auto tok = split(line);
            if (tok.size() < 2) continue;
            if (toUpper(tok[0]) == "LOAD_PLUGIN") {
                std::string perr;
                switch (loadPlugin(tok[1], perr)) {
                    case PluginLoadStatus::Loaded:
                        std::cerr << "Loaded plugin: " << tok[1] << "\n";
                        break;
                    case PluginLoadStatus::AlreadyLoaded:
                        std::cerr << "Note: plugin '" << tok[1] << "' already loaded, skipped\n";
                        break;
                    case PluginLoadStatus::Failed:
                        std::cerr << "Warning: plugin '" << tok[1] << "' not loaded: " << perr << "\n";
                        break;
                }
            }
        }
    }

    // ---- Pass 2: full parse ----
    std::ifstream in(txtPath);
    if (!in) { std::cerr << "Cannot open input: " << txtPath << "\n"; return false; }

    Params p;
    std::map<std::string, std::vector<std::string>> instrumentTokens;
    std::string delayTimeToken, line;

    while (std::getline(in, line)) {
        scrub(line);
        auto tok = split(line);
        if (tok.empty() || tok.size() < 2) continue;
        std::string key = toUpper(tok[0]);
        try {
            if (key == "LOAD_PLUGIN") {
                // Уже обработано в pass 1.
            }
            else if (key == "TEMPO")                    p.tempo = std::max(20.0, std::stod(tok[1]));
            else if (key == "BARS")                     p.bars = std::max(1, std::min(128, std::stoi(tok[1])));
            else if (key == "STEPS_PER_BAR")            p.stepsPerBar = std::max(4, std::min(64, std::stoi(tok[1])));
            else if (key == "SEED")                     p.seed = (unsigned int)std::stoul(tok[1]);
            else if (key == "FX_DRIVE")                 p.drive = clampd(std::stod(tok[1]), 0, 1);
            else if (key == "FX_FILTER_CUT")            p.filterCut = clampd(std::stod(tok[1]), 0, 1);
            else if (key == "FX_FILTER_RES")            p.filterRes = clampd(std::stod(tok[1]), 0, 0.95);
            else if (key == "FX_DELAY_FEEDBACK")        p.delayFeedback = clampd(std::stod(tok[1]), 0, 0.95);
            else if (key == "FX_DELAY_MIX")             p.delayMix = clampd(std::stod(tok[1]), 0, 1);
            else if (key == "FX_DELAY_TIME")            delayTimeToken = tok[1];
            else if (key == "FX_REVERB_MIX")            p.reverbMix = clampd(std::stod(tok[1]), 0, 1);
            else if (key == "FX_REVERB_DECAY")          p.reverbDecay = clampd(std::stod(tok[1]), 0, 0.95);
            else if (key == "FX_REVERB_DAMPING")        p.reverbDamping = clampd(std::stod(tok[1]), 0, 1);
            else if (key == "FX_SIDECHAIN_DEPTH")       p.sidechainDepth = clampd(std::stod(tok[1]), 0, 1);
            else if (key == "FX_SIDECHAIN_DURATION")    p.sidechainDuration = clampd(std::stod(tok[1]), 0.05, 2.0);
            else if (key == "FX_BASS_RES")              p.bassResonance = clampd(std::stod(tok[1]), 0, 0.95);
            else if (key == "FX_LEAD_RES")              p.leadResonance = clampd(std::stod(tok[1]), 0, 0.95);
            else if (key == "SAMPLE_RATE")              p.sampleRate = std::max(8000, std::min(192000, std::stoi(tok[1])));
            else if (key == "LOOP")                     p.loop = std::stoi(tok[1]) != 0;
            else if (getRegistry().has(key)) {
                auto& v = instrumentTokens[key];
                if (!v.empty())
                    std::cerr << "Note: repeated directive " << key
                              << "; concatenating (this is a feature for long patterns)\n";
                for (size_t i = 1; i < tok.size(); ++i) v.push_back(tok[i]);
            }
            else {
                std::cerr << "Warning: unknown directive: " << key << "\n";
            }
        } catch (...) {
            std::cerr << "Warning: cannot parse line: " << line << "\n";
        }
    }
    in.close();

    if (delayTimeToken.empty()) {
        p.delayTime = (240.0 / p.tempo) * (3.0 / 16.0);
    } else {
        try {
            size_t slash = delayTimeToken.find('/');
            if (slash != std::string::npos) {
                double num = std::stod(delayTimeToken.substr(0, slash));
                double den = std::stod(delayTimeToken.substr(slash + 1));
                p.delayTime = den > 0 ? (240.0 / p.tempo) * num / den : 0.25;
            } else {
                p.delayTime = std::stod(delayTimeToken);
            }
        } catch (...) { p.delayTime = (240.0 / p.tempo) * (3.0 / 16.0); }
        p.delayTime = clampd(p.delayTime, 0.001, 4.0);
    }

    int totalSteps = p.bars * p.stepsPerBar;
    double stepDur = 240.0 / (p.tempo * p.stepsPerBar);
    int stepSamples = std::max(1, (int)(stepDur * p.sampleRate));
    int loopSamples = totalSteps * stepSamples;
    int tailSamples = p.loop ? 0 : p.sampleRate * 2;
    int totalSamples = loopSamples + tailSamples;

    if ((long long)totalSamples > (long long)p.sampleRate * 600) {
        std::cerr << "Error: requested render exceeds 10 minutes ("
                  << totalSamples / (double)p.sampleRate << " s). Aborting.\n";
        return false;
    }

    // Квантование delayTime к длине петли: ds = loopSamples / k, k целое.
    // Если delay длиннее петли — квантование невозможно, печатаем warning.
    if (p.loop && loopSamples > 0) {
        int ds = (int)std::lround(p.delayTime * p.sampleRate);
        if (ds > 0 && ds < loopSamples) {
            int div = std::max(1, (int)std::lround((double)loopSamples / ds));
            int qds = loopSamples / div;
            if (qds >= 1) p.delayTime = (double)qds / p.sampleRate;
        } else if (ds >= loopSamples) {
            std::cerr << "Warning: FX_DELAY_TIME (" << ds
                      << " samples) >= loop length (" << loopSamples
                      << "); delay will not be loop-aligned.\n";
        }
    }

    int loopEnd = p.loop ? loopSamples : 0;

    RenderCtx ctx;
    ctx.sampleRate = p.sampleRate;
    ctx.stepSamples = stepSamples;
    ctx.totalSteps = totalSteps;
    ctx.totalSamples = totalSamples;
    ctx.loopEnd = loopEnd;
    ctx.params = &p;

    // ---- Prepare ----
    std::vector<PreparedInstrument> prepared;
    for (auto& kv : instrumentTokens) {
        const std::string& name = kv.first;
        auto inst = getRegistry().create(name);
        if (!inst) { std::cerr << "Warning: instrument '" << name << "' not registered\n"; continue; }
        unsigned int h = hashName(name);
        std::mt19937 parseRng(p.seed + h);
        std::vector<Step> steps;
        std::string err;
        bool ok = false;
        try { ok = inst->parse(kv.second, totalSteps, parseRng, steps, err); }
        catch (const std::exception& e) { err = e.what(); ok = false; }
        catch (...) { err = "unknown parse error"; ok = false; }
        if (!ok) {
            std::cerr << "Warning: instrument '" << name << "' disabled: " << err << "\n";
            continue;
        }
        prepared.push_back({std::move(inst), name, std::move(steps)});
    }

    // ---- Render ----
    std::vector<float> drumsL(totalSamples, 0.0f), drumsR(totalSamples, 0.0f);
    std::vector<float> melL(totalSamples, 0.0f),  melR(totalSamples, 0.0f);

    std::vector<Step> sidechainPattern;
    bool haveSidechain = false;

    for (auto& prep : prepared) {
        unsigned int h = hashName(prep.name);
        std::mt19937 renderRng(p.seed + h * 2654435761u);
        std::vector<float> iL, iR;
        RenderResult r = safeRender(*prep.inst, prep.steps, ctx, renderRng, iL, iR);
        if (!r.ok) {
            std::cerr << "Warning: instrument '" << prep.name << "' disabled: " << r.error << "\n";
            continue;
        }
        bool isDrum = (prep.inst->category() == InstrumentCategory::Drum);
        auto& dL = isDrum ? drumsL : melL;
        auto& dR = isDrum ? drumsR : melR;
        for (size_t i = 0; i < dL.size(); ++i) { dL[i] += iL[i]; dR[i] += iR[i]; }

        if (prep.inst->isSidechainSource()) {
            if (!haveSidechain) { sidechainPattern = prep.steps; haveSidechain = true; }
            else for (size_t i = 0; i < sidechainPattern.size() && i < prep.steps.size(); ++i) {
                if (prep.steps[i].on) {
                    sidechainPattern[i].on = true;
                    sidechainPattern[i].vel = std::max(sidechainPattern[i].vel, prep.steps[i].vel);
                }
            }
        }
    }

    applySidechain(melL, melR, sidechainPattern, stepSamples,
                   p.sidechainDepth, p.sidechainDuration, p.sampleRate, loopEnd);
    applyReverb(melL, melR, p.reverbMix, p.reverbDecay, p.reverbDamping,
                p.sampleRate, p.loop, loopSamples);

    std::vector<float> mixL(totalSamples, 0.0f), mixR(totalSamples, 0.0f);
    for (int i = 0; i < totalSamples; ++i) {
        mixL[i] = drumsL[i] + melL[i];
        mixR[i] = drumsR[i] + melR[i];
    }

    {
        double peak = 0.0;
        for (size_t i = 0; i < mixL.size(); ++i) {
            peak = std::max(peak, (double)std::fabs(mixL[i]));
            peak = std::max(peak, (double)std::fabs(mixR[i]));
        }
        if (peak > 1.0)
            std::cerr << "Warning: pre-master mix peak = " << peak
                      << " > 1.0; lookahead limiter will bring it down.\n";
    }

    processMaster(mixL, mixR, p, p.loop ? loopSamples : 0);

    if (!writeWav(wavPath, mixL, mixR, p.sampleRate)) {
        std::cerr << "Cannot write WAV: " << wavPath << "\n";
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: musicgen input.txt output.wav\n";
        std::cerr << "Registered instruments:";
        for (auto& n : getRegistry().names()) std::cerr << " " << n;
        std::cerr << "\n";
        return 1;
    }
    if (!renderTxtToWav(argv[1], argv[2])) return 1;
    std::cout << "OK: " << argv[2] << "\n";
    return 0;
}
