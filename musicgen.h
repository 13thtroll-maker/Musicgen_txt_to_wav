#pragma once
// Публичный API. Всё inline, чтобы плагины собирались без -rdynamic.
// ВАЖНО: getRegistry() в этом заголовке нет. Плагин получает реестр
// указателем из register_instruments(reg). Вызов instance() из плагина
// создал бы отдельный реестр внутри .so/.dll и инструмент не был бы виден main.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace musicgen {

inline constexpr double PI = 3.14159265358979323846;
inline constexpr double TWO_PI = 2.0 * PI;

struct Params {
    double tempo = 140.0;
    int stepsPerBar = 16;
    int bars = 2;
    unsigned int seed = 1337;
    double drive = 0.65;
    double filterCut = 0.8;
    double filterRes = 0.7;
    double delayTime = 0.25;
    double delayFeedback = 0.35;
    double delayMix = 0.25;
    double reverbMix = 0.2;
    double reverbDecay = 0.6;
    double reverbDamping = 0.3;
    double sidechainDepth = 0.7;
    double sidechainDuration = 0.25;
    double bassResonance = 0.3;
    double leadResonance = 0.5;
    int sampleRate = 44100;
    bool loop = true;
};

struct Step {
    bool on = false;
    double vel = 0.0;
    std::vector<double> freqs;
    std::vector<double> params;   // [0] = длительность в шагах (если задана)
};

struct RenderCtx {
    int sampleRate = 44100;
    int stepSamples = 1;
    int totalSteps = 0;
    int totalSamples = 0;
    int loopEnd = 0;                  // 0 = без wrap
    const Params* params = nullptr;
};

enum class InstrumentCategory { Drum, Melodic };

inline double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

inline std::string toUpper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

inline std::vector<std::string> split(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

inline double noteToFreq(const std::string& name) {
    if (name.empty() || name == ".") return 0.0;
    size_t i = 0;
    char ch = (char)std::toupper((unsigned char)name[i++]);
    int pc = -1;
    switch (ch) {
        case 'C': pc = 0;  break; case 'D': pc = 2;  break;
        case 'E': pc = 4;  break; case 'F': pc = 5;  break;
        case 'G': pc = 7;  break; case 'A': pc = 9;  break;
        case 'B': pc = 11; break; default: return 0.0;
    }
    int acc = 0;
    while (i < name.size() && (name[i] == '#' || name[i] == 'b' || name[i] == 'B')) {
        acc += (name[i] == '#') ? 1 : -1; i++;
    }
    int octave = 4;
    if (i < name.size()) {
        try { octave = std::stoi(name.substr(i)); } catch (...) { octave = 4; }
    }
    int midi = (octave + 1) * 12 + pc + acc;
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

inline std::vector<std::string> parseChord(const std::string& token) {
    std::vector<std::string> notes;
    std::istringstream iss(token);
    std::string note;
    while (std::getline(iss, note, '+')) if (!note.empty()) notes.push_back(note);
    return notes;
}

inline bool isPercOnlyToken(const std::string& t) {
    if (t.empty()) return false;
    for (char c : t)
        if (c != 'x' && c != 'X' && c != 'o' && c != 'O' && c != '.' && c != '-')
            return false;
    return true;
}

// Strip '|' visual separators; drop tokens that become empty.
inline std::vector<std::string> expandPercTokens(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    for (auto& t : in) {
        std::string clean;
        clean.reserve(t.size());
        for (char c : t) if (c != '|') clean += c;
        if (clean.empty()) continue;
        if (clean.size() > 1 && isPercOnlyToken(clean))
            for (char c : clean) out.push_back(std::string(1, c));
        else
            out.push_back(clean);
    }
    return out;
}

inline double polyBlep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}
inline double sawPolyBlep(double phase, double dt) {
    return (2.0 * phase - 1.0) - polyBlep(phase, dt);
}

struct TPTSVF {
    double a1 = 0, a2 = 0, a3 = 0, ic1eq = 0, ic2eq = 0;
    void setParams(double fc, double sr, double resonance) {
        fc = clampd(fc, 20.0, sr * 0.49);
        double g = std::tan(PI * fc / sr);
        double k = 2.0 / (1.0 + resonance * 20.0);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    double processLowpass(double x) {
        double v3 = x - ic2eq;
        double v1 = a1 * ic1eq + a2 * v3;
        double v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = 2.0 * v1 - ic1eq;
        ic2eq = 2.0 * v2 - ic2eq;
        return v2;
    }
    double processBandpass(double x) {
        double v3 = x - ic2eq;
        double v1 = a1 * ic1eq + a2 * v3;
        double v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = 2.0 * v1 - ic1eq;
        ic2eq = 2.0 * v2 - ic2eq;
        return v1;
    }
};

// loopEnd > 0: УСТАРЕВШЕЕ wrap-around поведение. Оставлено для обратной
// совместимости со старыми плагинами. Новый код передаёт 0 и полагается
// на обрезку по границе петли на стороне инструмента (см. availSamples
// в musicgen.cpp). При loopEnd > 0 хвост события, выходящего за границу
// петли, заворачивается в начало — на коротких лупах это даёт "двойной
// удар" (kick + его хвост в начале). Использование wrap в новом коде —
// на ваш риск.
inline void addSampleAt(std::vector<float>& L, std::vector<float>& R, int idx, int loopEnd,
                        float l, float r) {
    if (loopEnd > 0) { idx %= loopEnd; if (idx < 0) idx += loopEnd; }
    if (idx >= 0 && idx < (int)L.size()) { L[idx] += l; R[idx] += r; }
}

inline bool parsePercTokens(const std::vector<std::string>& tokens, int totalSteps,
                            std::mt19937& rng, std::vector<Step>& out, std::string& err) {
    if (tokens.empty()) { err = "empty pattern"; return false; }
    auto expanded = expandPercTokens(tokens);
    if (expanded.empty()) { err = "empty pattern"; return false; }
    out.assign(totalSteps, Step{});
    std::uniform_real_distribution<double> human(-0.03, 0.03);
    for (int i = 0; i < totalSteps; ++i) {
        const std::string& t = expanded[i % expanded.size()];
        if (t.empty()) continue;
        if (t.size() != 1) { err = "invalid percussion token: " + t; return false; }
        char c = t[0];
        if (c == 'x' || c == 'X' || c == 'o' || c == 'O') {
            out[i].on = true;
            if (c == 'X') out[i].vel = 1.15;
            else if (c == 'O') out[i].vel = 0.85;
            else if (c == 'o') out[i].vel = 0.65;
            else out[i].vel = 1.0;
            out[i].vel = std::max(0.05, out[i].vel + human(rng));
        } else if (c != '.' && c != '-') {
            err = "invalid percussion token: " + t;
            return false;
        }
    }
    return true;
}

// Поддерживает суффикс длительности: A2:4 -> нота на 4 шага,
// C4+E4+G4:2 -> аккорд на 2 шага. Длительность складывается в
// Step::params[0]. Старый синтаксис (без ":") работает как раньше.
//
// Битые длительности — это ОШИБКА (err и false), а не тихий дефолт:
//   A2:       -> err "empty duration"
//   A2:0      -> err "invalid duration"
//   A2:abc    -> err "invalid duration"
//   A2:-5     -> err "invalid duration"
//   A2:4:5    -> err "invalid note token (extra ':')"
// Это сделано специально: опечатка в паттерне должна быть замечена
// сразу, а не проиграна как нота на 1 шаг.
inline bool parseNoteTokens(const std::vector<std::string>& tokens, int totalSteps,
                            std::mt19937& rng, std::vector<Step>& out, std::string& err) {
    if (tokens.empty()) { err = "empty pattern"; return false; }
    out.assign(totalSteps, Step{});
    std::uniform_real_distribution<double> human(-0.02, 0.02);

    size_t tokIdx = 0;
    int step = 0;
    while (step < totalSteps) {
        const std::string& t = tokens[tokIdx % tokens.size()];
        tokIdx++;

        std::string notePart = t;
        int durationSteps = 1;
        size_t colon = t.find(':');
        if (colon != std::string::npos) {
            if (t.find(':', colon + 1) != std::string::npos) {
                err = "invalid note token (extra ':'): " + t;
                return false;
            }
            std::string durStr = t.substr(colon + 1);
            if (durStr.empty()) { err = "empty duration in: " + t; return false; }
            size_t consumed = 0;
            int d = 0;
            try { d = std::stoi(durStr, &consumed); }
            catch (...) { err = "invalid duration in: " + t; return false; }
            if (consumed != durStr.size() || d < 1) {
                err = "invalid duration in: " + t;
                return false;
            }
            durationSteps = d;
            notePart = t.substr(0, colon);
        }

        if (notePart.empty() || notePart == "." || notePart == "-") {
            step += durationSteps;
            continue;
        }

        auto notes = parseChord(notePart);
        if (notes.empty()) { err = "empty chord at step " + std::to_string(step); return false; }
        std::vector<double> valid;
        for (auto& n : notes) {
            double f = noteToFreq(n);
            if (f > 0.0) valid.push_back(f);
            else { err = "invalid note: " + n; return false; }
        }
        if (valid.empty()) { err = "no valid notes"; return false; }
        if (step < totalSteps) {
            out[step].on = true;
            out[step].vel = clampd(0.9 + human(rng), 0.05, 1.2);
            out[step].freqs = valid;
            out[step].params.push_back((double)durationSteps);
        }
        step += durationSteps;
    }
    return true;
}

class Instrument {
public:
    virtual ~Instrument() = default;
    virtual std::string name() const = 0;
    virtual InstrumentCategory category() const = 0;

    virtual bool isSidechainSource() const { return false; }

    virtual bool parse(const std::vector<std::string>& tokens, int totalSteps,
                       std::mt19937& rng, std::vector<Step>& out, std::string& err) = 0;

    virtual void render(const std::vector<Step>& steps, const RenderCtx& ctx,
                        std::mt19937& rng, std::vector<float>& L, std::vector<float>& R) = 0;
};

class InstrumentRegistry {
public:
    using Factory = std::function<std::unique_ptr<Instrument>()>;

    class Snapshot {
        friend class InstrumentRegistry;
        std::optional<std::map<std::string, Factory>> saved;
        explicit Snapshot(std::map<std::string, Factory> s)
            : saved(std::move(s)) {}
    public:
        Snapshot() = delete;
        Snapshot(Snapshot&& other) noexcept
            : saved(std::move(other.saved)) { other.saved.reset(); }
        Snapshot& operator=(Snapshot&& other) noexcept {
            if (this != &other) {
                saved = std::move(other.saved);
                other.saved.reset();
            }
            return *this;
        }
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
    };

    void add(const std::string& nameUpper, Factory f) {
        if (factories.count(nameUpper))
            throw std::runtime_error("duplicate instrument: " + nameUpper);
        factories[nameUpper] = std::move(f);
    }

    bool has(const std::string& n) const { return factories.count(n) > 0; }
    std::unique_ptr<Instrument> create(const std::string& n) const {
        auto it = factories.find(n);
        return it == factories.end() ? nullptr : it->second();
    }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (auto& kv : factories) out.push_back(kv.first);
        return out;
    }

    Snapshot snapshot() const { return Snapshot{factories}; }

    void restore(Snapshot s) {
        if (s.saved) factories = std::move(*s.saved);
    }

private:
    std::map<std::string, Factory> factories;
};

} // namespace musicgen
