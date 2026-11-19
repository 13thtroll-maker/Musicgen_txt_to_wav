// clap_plugin.cpp
// Windows MSVC:  cl /nologo /EHsc /O2 /std:c++17 /LD clap_plugin.cpp /Fe:clap.dll
// Windows MinGW: g++ -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ clap_plugin.cpp -o clap.dll
// Linux/macOS:   g++ -std=c++17 -O2 -fPIC -shared clap_plugin.cpp -o clap.so

#include "musicgen.h"
#include <random>

using namespace musicgen;

class ClapInstrument : public Instrument {
public:
    std::string name() const override { return "CLAP"; }
    InstrumentCategory category() const override { return InstrumentCategory::Drum; }

    bool parse(const std::vector<std::string>& toks, int ts,
               std::mt19937& rng, std::vector<Step>& out, std::string& err) override {
        return parsePercTokens(toks, ts, rng, out, err);
    }

    void render(const std::vector<Step>& steps, const RenderCtx& ctx, std::mt19937& rng,
                std::vector<float>& L, std::vector<float>& R) override {
        std::uniform_real_distribution<double> noise(-1.0, 1.0);
        int sr = ctx.sampleRate;
        const double dur = 0.14;
        int n = (int)(dur * sr);

        for (int s = 0; s < (int)steps.size(); ++s) {
            if (!steps[s].on) continue;
            int start = s * ctx.stepSamples;
            double vel = steps[s].vel;

            // Bandpass через TPT SVF, ~1.2 kHz, средняя резонансность.
            // Состояния фильтра — свои, независимы от main.
            TPTSVF bp;
            bp.setParams(1200.0, sr, 0.5);

            for (int i = 0; i < n; ++i) {
                int idx = start + i;
                if (ctx.loopEnd == 0 && idx >= (int)L.size()) break;

                double t = i / (double)sr;

                // Три burst-а с задержками — типичная форма хлопка.
                // 1 ms attack на каждом — без щелчка в самом начале.
                double env = 0.0;
                for (int b = 0; b < 3; ++b) {
                    double t0 = b * 0.008;
                    if (t >= t0) {
                        double dt = t - t0;
                        double att = std::min(dt / 0.001, 1.0);
                        env += att * std::exp(-dt * 60.0) * 0.6;
                    }
                }

                double filtered = bp.processBandpass(noise(rng));
                double s2 = filtered * env * vel * 0.9;
                addSampleAt(L, R, idx, ctx.loopEnd, (float)s2, (float)s2);
            }
        }
    }
};

#if defined(_WIN32) || defined(_WIN64)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

PLUGIN_EXPORT bool register_instruments(InstrumentRegistry* reg) {
    reg->add("CLAP", []{ return std::make_unique<ClapInstrument>(); });
    return true;
}
