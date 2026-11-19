# musicgen

A tiny text-to-WAV music synthesizer for games.

You write a plain-text file describing a pattern, `musicgen` renders a 16-bit
stereo WAV. No samples, no external dependencies, no runtime — everything is
synthesized from oscillators and filters. 9 built-in instruments, FX chain,
lookahead limiter, and a plugin system.

```bash
./musicgen level3.txt level3.wav
```

---

## ⚠️ Read this first

**I am not a C++ developer.** This project was generated with AI assistance and used for my own Godot game. I wrote
the DSL and iterated on the design; the C++ implementation was largely
produced by AI and reviewed by me at the level of "does it compile and
sound right".

This means:

- **I cannot provide reliable C++ support.** If you file an issue about
  a crash inside `TPTSVF`, I will probably not be able to help you.
- **I do not plan to actively maintain this.** PRs and issues are welcome,
  but responses may be slow or absent. Use as-is.
- **The API is not stable.** Between versions, plugins and the DSL may
  break. Pin a commit if you depend on it.
- **There is no 1.0.** This is `v0.9.0` — functional, tested on the basic
  scenarios, with known limitations documented below.

If that's a dealbreaker — fair. If you're OK with a "found this on GitHub,
used it, forked it" project — welcome. The Known limitations section
lists exactly what's broken, and the How to help section lists exactly
what would be most valuable.

---

## Why

WAV files are heavy. A 30-second 16-bit stereo loop at 44.1 kHz is ~5 MB.
Five tracks in a small indie game is 25 MB of audio. On mobile and web
that hurts.

The source `.txt` for the same loop is ~2 KB. If you reuse one pattern
with different effects, the savings multiply. For desktop builds you can
generate audio at level-load time; for other platforms you can pre-render
once and ship the `.wav` (or `.ogg`).

The secondary motivation: **loop-aware rendering**. Most hobby music
generators either click at the loop boundary or "double-hit" — the tail of
a kick at the end of the bar wraps around and collides with the attack at
the start. `musicgen` truncates tails at the loop boundary and warms up
reverb/delay state before recording, so the resulting WAV loops cleanly.
This is the hard part that's usually missing, and it's here.

---

## Quick start (60 seconds)

```bash
git clone <this-repo>
cd musicgen
g++ -std=c++17 -O2 musicgen.cpp -o musicgen
```

Create `demo.txt`:

```
TEMPO 120
BARS 2
STEPS_PER_BAR 16

KICK  x...x...x...x...
SNARE ....x.......x...
HAT   x.x.x.x.x.x.x.x.
BASS  C2 . . . G2 . . . C2 . . . A2 . . .
LEAD  E4+G4 . . . C5 . . B4 . . A4 . . G4 . .
```

Render:

```bash
./musicgen demo.txt demo.wav
```

You should hear a 4-second, 120 BPM loop. Open `demo.wav` and press play.

---

## Building

### Linux / macOS

```bash
# Without plugins (no -rdynamic, no -ldl):
g++ -std=c++17 -O2 musicgen.cpp -o musicgen

# With plugin support:
g++ -std=c++17 -O2 -rdynamic musicgen.cpp -o musicgen -ldl
```

### Windows — MinGW (MSYS2 or portable)

If MinGW is installed via the MSYS2 installer (with `bin/` on `PATH`):

    g++ -std=c++17 -O2 musicgen.cpp -o musicgen.exe

If MinGW is a portable extraction (no installer, `bin/` not on `PATH`),
add `-static` so the exe doesn't depend on runtime DLLs being findable
at launch:

    g++ -std=c++17 -O2 -static musicgen.cpp -o musicgen.exe

Without `-static`, you'll get "The code execution cannot proceed because
libstdc++-6.dll was not found" on systems that don't have MinGW's
`bin/` on `PATH`. The resulting exe is ~2 MB larger but fully portable.

For plugins on top of a static exe, use
`-shared -static-libgcc -static-libstdc++` for the DLL — that keeps
both sides on a static CRT (see the CRT note above). Mixing fully-static
exe with default-dynamic DLL is the combination that triggers heap
corruption on Windows.

---

## The DSL

A `.txt` file is a line-by-line list of directives. Case-insensitive,
whitespace-insensitive, `//` and `;` both start a comment (except on
`LOAD_PLUGIN` lines — see below).

### Meta

| Directive | Range | Default | Meaning |
|---|---|---|---|
| `TEMPO <bpm>` | ≥ 20 | 140 | Beats per minute |
| `BARS <n>` | 1–128 | 2 | Number of bars |
| `STEPS_PER_BAR <n>` | 4–64 | 16 | Subdivision (16 = 16th notes) |
| `SAMPLE_RATE <hz>` | 8000–192000 | 44100 | Output sample rate |
| `SEED <n>` | any | 1337 | RNG seed (humanize + noise) |
| `LOOP <0\|1>` | 0/1 | 1 | Loop mode — see below |

Total pattern length = `BARS * STEPS_PER_BAR` steps.
One step = `240 / (TEMPO * STEPS_PER_BAR)` seconds.

Comment characters are `//` and `;` only. `#` is NOT a comment —
lines starting with `#` will be reported as `Warning: unknown directive: #`
and ignored. If you're used to `#` from Python/shell configs, use `;`
or `//` instead.

### Loop mode

- **`LOOP 1`** (default): renders exactly `BARS * STEPS_PER_BAR` steps.
  - Events are **truncated** at the loop boundary, not wrapped. A long
    kick at the end does not collide with its own attack at the start.
  - Reverb and delay are pre-warmed over several loop iterations so
    filter/delay state is at steady state at sample 0.
  - Delay time is quantized to `loopLength / k` (integer `k`) so echoes
    stay on the grid across loop iterations.
- **`LOOP 0`**: adds 2 seconds of tail so reverb/delay decay naturally.
  For one-shot pieces, not for looping.

### Drums: `KICK`, `SNARE`, `HAT`, `OPENHAT`, `TOM`

One character = one step. No spaces needed.

| Symbol | Meaning | Velocity |
|---|---|---|
| `x` | hit | 1.0 |
| `X` | accent | 1.15 |
| `o` | quiet | 0.65 |
| `O` | medium | 0.85 |
| `.` | rest | — |
| `-` | rest (synonym) | — |
| `\|` | visual separator | ignored |

```
KICK  x...x...x...x...
HAT   x.x.x.x.x.x.x.x.
```

`|` is ignored — use it to group by beat:

```
KICK  x...x...|..x.x...|x...x...|x.x.....
```

Multi-character tokens are auto-expanded if they contain only valid
percussion characters, so these are equivalent:

```
KICK  x...x...x...x...
KICK  x . . . x . . . x . . . x . . .
```

Repeated directives are **concatenated** (feature, not a bug):

```
KICK  x...x...x...x...
KICK  ....x.......x...    # 32 steps total
```

### Melodic: `BASS`, `LEAD`, `PLUCK`, `PAD`

One **token** (space-separated) = one step. This is different from drums.

| Syntax | Meaning |
|---|---|
| `C4` | note C in octave 4 |
| `C#4`, `Db4` | black keys |
| `C4+E4+G4` | chord (all notes at the same step) |
| `C4:4` | note lasting 4 steps |
| `C4+E4+G4:2` | chord lasting 2 steps |
| `.` / `-` | rest |

**⚠️ Spaces are required.** `C4..E4` is **not** shorthand for `C4 . . E4` —
it's one malformed token that parses as `C4` with garbage discarded. Use
spaces:

```
BASS  C2..C2..G2..A2..          # WRONG: one token, plays only C2
BASS  C2 . . C2 . . G2 . . A2 . .   # RIGHT: 12 steps, 4 notes
```

Durations are validated strictly: `A2:`, `A2:0`, `A2:abc`, `A2:4:5` will
disable the instrument with a clear error. Misspelled *notes* (`H4`,
`C4..E4`) are **not** caught — `noteToFreq` parses the valid prefix and
silently discards trailing garbage. See Known limitations for how to
harden this.

If a melodic pattern is shorter than the total step count, it repeats.
If a `:N` duration overflows the loop, the note is truncated at the
boundary (same rule as drums).

### Instruments

| Name | Category | Synthesis |
|---|---|---|
| `KICK` | Drum | Sine with pitch envelope (44→115 Hz) + noise click |
| `SNARE` | Drum | Filtered noise + two sine bodies (186 Hz, ×1.5) |
| `HAT` | Drum | HP noise, fast decay (~70/s) |
| `OPENHAT` | Drum | HP noise, slow decay (~12/s) |
| `TOM` | Drum | Sine with pitch envelope (140 Hz ↓) |
| `BASS` | Melodic | polyBLEP saw + TPT SVF with filter envelope |
| `LEAD` | Melodic | 3-voice unison saw, stereo-split filters |
| `PLUCK` | Melodic | polyBLEP saw, fast decay, for arpeggios |
| `PAD` | Melodic | 5-voice unison saw, slow attack, stereo-spread |

`KICK` is the default sidechain source (`isSidechainSource() == true`).
Plugins can add more sources.

### Effects

| Directive | Range | Default | Applies to |
|---|---|---|---|
| `FX_DRIVE` | 0–1 | 0.65 | Master (tanh saturation) |
| `FX_FILTER_CUT` | 0–1 | 0.8 | Master LP cutoff (log scale) |
| `FX_FILTER_RES` | 0–0.95 | 0.7 | Master LP resonance |
| `FX_DELAY_TIME` | 0.001–4 s or `a/b` bar | 3/16 bar | Delay time |
| `FX_DELAY_FEEDBACK` | 0–0.95 | 0.35 | Delay feedback |
| `FX_DELAY_MIX` | 0–1 | 0.25 | Delay wet |
| `FX_REVERB_MIX` | 0–1 | 0.2 | Reverb wet (melodic bus only) |
| `FX_REVERB_DECAY` | 0–0.95 | 0.6 | Reverb decay |
| `FX_REVERB_DAMPING` | 0–1 | 0.3 | Reverb HF damping |
| `FX_SIDECHAIN_DEPTH` | 0–1 | 0.7 | Melodic bus duck amount |
| `FX_SIDECHAIN_DURATION` | 0.05–2 s | 0.25 | Duck duration |
| `FX_BASS_RES` | 0–0.95 | 0.3 | Bass filter resonance (**also used by PAD**) |
| `FX_LEAD_RES` | 0–0.95 | 0.5 | Lead filter resonance (**also used by PLUCK**) |

**Note on resonances:** PLUCK uses `FX_LEAD_RES` and PAD uses
`FX_BASS_RES` — there are no separate `FX_PLUCK_RES` / `FX_PAD_RES`.
If you set `FX_LEAD_RES 0.8` for a screaming lead, PLUCK screams too.

`FX_DELAY_TIME` accepts musical fractions: `3/16` = dotted eighth,
`1/8` = eighth, `1/4` = quarter.

Reverb and sidechain only affect the **melodic bus** (BASS + LEAD + PLUCK
+ PAD + any melodic plugin). Drums are always dry. Sidechain is applied
*before* reverb — so the reverb tail is **not** ducked by the kick. If you
want a ducking tail, lower `FX_REVERB_MIX`.

### Plugins

```
LOAD_PLUGIN ./my_plugin.so    # Linux/macOS
LOAD_PLUGIN C://plugins/clap.dll    ; Windows, note: `;` for comments
```

On `LOAD_PLUGIN` lines, **only `;` is treated as a comment**. `//` is kept
literally to support Windows paths like `C://plugins/clap.dll`. On all
other lines both `//` and `;` work normally.

After loading, the plugin's instrument names become regular directives.

**🚨 `LOAD_PLUGIN` executes arbitrary code.** If the `.txt` comes from an
untrusted source (user-generated mods, cloud saves, shared presets), this
is remote code execution. For studio assets it's fine. For end-user
content: strip `LOAD_PLUGIN` before parsing, or use a build without
plugin support.

### Full example

```
TEMPO 115
BARS 4
STEPS_PER_BAR 16
SEED 42
LOOP 1

FX_DRIVE 0.5
FX_FILTER_CUT 0.75
FX_FILTER_RES 0.6

FX_DELAY_TIME 3/16
FX_DELAY_FEEDBACK 0.4
FX_DELAY_MIX 0.3

FX_REVERB_MIX 0.35
FX_REVERB_DECAY 0.7
FX_REVERB_DAMPING 0.4

FX_SIDECHAIN_DEPTH 0.8
FX_SIDECHAIN_DURATION 0.3

KICK  x...x...x...x... x...x...x...X...
SNARE ....o.......o... ....o.......o...
HAT   x.x.x.x.x.x.x.x. x.x.x.x.x.x.x.x.

BASS  C2 . . . G2 . . . A2 . . . F2 . . .
BASS  C2 . . . G2 . . . A2 . . . D2 . . .

LEAD  E4+G4 . . . C5 . . B4 . . A4 . . G4 . .
LEAD  E4+G4 . . . C5 . . B4 . . A4 . . C5 . .

PAD   C4+E4+G4:8 . . . . . . . A3+C4+E4:8 . . . . . . .
```

Result: 4 bars, 115 BPM ≈ 8.35 seconds, loops cleanly.

---

## Writing a plugin

A plugin is a shared library exposing one C function:

```cpp
// my_plugin.cpp
#include "musicgen.h"
#include <random>
using namespace musicgen;

class MyInstrument : public Instrument {
public:
    std::string name() const override { return "MY_INST"; }
    InstrumentCategory category() const override { return InstrumentCategory::Melodic; }

    bool parse(const std::vector<std::string>& toks, int ts,
               std::mt19937& rng, std::vector<Step>& out, std::string& err) override {
        return parseNoteTokens(toks, ts, rng, out, err);
    }

    void render(const std::vector<Step>& steps, const RenderCtx& ctx,
                std::mt19937& rng, std::vector<float>& L, std::vector<float>& R) override {
        for (int s = 0; s < (int)steps.size(); ++s) {
            if (!steps[s].on) continue;
            int start = s * ctx.stepSamples;

            // ⚠️ Truncate at loop boundary — do NOT pass ctx.loopEnd to
            // addSampleAt. That's the deprecated wrap behavior.
            int end = (ctx.loopEnd > 0) ? ctx.loopEnd : (int)L.size();
            int avail = end - start;
            if (avail <= 0) continue;
            int n = std::min((int)(0.1 * ctx.sampleRate), avail);

            for (int i = 0; i < n; ++i) {
                double t = i / (double)ctx.sampleRate;
                float v = (float)(std::sin(2.0 * PI * 440.0 * t) * std::exp(-t * 20.0));
                addSampleAt(L, R, start + i, 0, v, v);
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
    reg->add("MY_INST", []{ return std::make_unique<MyInstrument>(); });
    return true;
}
```

Build:

```bash
# Linux/macOS
g++ -std=c++17 -O2 -fPIC -shared my_plugin.cpp -o my_plugin.so

# Windows MSVC
cl /nologo /EHsc /O2 /std:c++17 /LD my_plugin.cpp /Fe:my_plugin.dll

# Windows MinGW
g++ -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ my_plugin.cpp -o my_plugin.dll
```

`PLUGIN_EXPORT` is **required** on Windows — without `__declspec(dllexport)`,
`register_instruments` won't be in the export table and `GetProcAddress`
returns null.

**⚠️ `ctx.loopEnd` and `addSampleAt`:** the fourth argument of
`addSampleAt` is deprecated. Passing `ctx.loopEnd` triggers the old
"wrap tail into the beginning of the loop" behavior, which causes the
double-hit issue this project exists to avoid. Follow the pattern in the
example: compute `end = ctx.loopEnd > 0 ? ctx.loopEnd : L.size()`, clip
your sample count to `end - start`, and pass `0` as the loopEnd argument.

Also: **do not call `getRegistry()` from a plugin** — it's not in the
header, and if it were, it would create a *second* registry inside the
plugin's `.so` and your instrument wouldn't be visible to `main`. Use
the `reg` pointer passed to `register_instruments`.

---

## Using in a game engine

### Desktop: pre-render at build time (simplest)

Generate `.wav` (or convert to `.ogg`) once, ship the audio file. The
binary is not part of the game.

```bash
./musicgen level3.txt level3.wav
ffmpeg -i level3.wav -c:a libvorbis -q:a 5 level3.ogg   # optional
```

### Desktop: render at level load (requires process spawn)

```gdscript
# Godot 4
func _load_level_music(txt_path: String) -> AudioStreamWAV:
    var out := ProjectSettings.globalize_path("user://level_music.wav")
    var argv := [OS.get_executable_path().get_base_dir() + "/musicgen",
                 txt_path, out]
    var exit_code := OS.execute(argv[0], argv.slice(1))
    if exit_code != 0:
        push_error("musicgen failed")
        return null
    var stream := AudioStreamWAV.load_from_file(out)
    stream.loop_mode = AudioStreamWAV.LOOP_FORWARD
    stream.loop_end = stream.get_length() * stream.mix_rate
    return stream
```

**The WAV file has no loop metadata.** You must set `loop_mode` and
`loop_end` in the engine after loading. `loop_end` should be
`wav_sample_count` — which is `BARS * STEPS_PER_BAR * step_samples`, not
`BARS * 240/TEMPO * SAMPLE_RATE` (these differ by up to ~60 samples due
to step-sample truncation — see Known limitations).

### Mobile / Web

- **Android:** possible but fiddly with `OS.execute` and paths.
- **iOS:** not feasible — sandbox forbids subprocesses.
- **Godot HTML5 / Web:** impossible without a WASM rebuild of musicgen,
  which is a separate project. Ship pre-rendered `.ogg`.

---

## Known limitations

These are real. They're documented so you know what you're getting, not
so you avoid the project.

**Rendering / correctness**

- **WAV has no loop metadata.** You set `loop_mode` / `loop_end` in the
  engine, not in the file. Easy to forget.
- **Loop length ≠ `BARS * 240/TEMPO * SAMPLE_RATE`.** `loopSamples` is
  computed as `totalSteps * (int)(stepDur * sampleRate)`, which truncates
  per step. At 128 BPM / 44.1 kHz / 4 bars, the actual WAV is ~62 samples
  (~1.4 ms) shorter than the "theoretical" length. If you align musical
  events to `BARS * 240/TEMPO` in your engine, audio and gameplay will
  drift. Use `WAV_samples / sampleRate` as the authoritative loop length.
- **No dither on 16-bit output.** The float→int16 conversion uses
  rounding. Fine for games, not mastering-grade.
- **Not bit-identical across platforms.** `std::sin`, `std::exp`, `std::tanh`
  differ ±1–2 ULP between MSVC / glibc / Apple libm. Same seed + same
  `.txt` + same platform = identical WAV. Across platforms = very close
  but not identical. Don't use golden WAVs as CI checks.

**Musical**

- **Envelopes are hardcoded per instrument.** No `FX_ATTACK` / `FX_DECAY` /
  `FX_RELEASE`. Bass and lead have filter envelopes, but no ADSR exposed.
- **Reverb is a Schroeder comb (3 allpass + 4 comb), not convolution.**
  Sounds "cheap" — fine for lo-fi, not for realistic space. **Wet is mono**
  — no stereo image from the reverb itself.
- **Delay feedback is the same for L and R.** No ping-pong.
- **Sidechain is applied before reverb.** Reverb tail is not ducked.
- **No per-instrument volume or pan.** Everything sums into drums/melodic
  buses. A real arrangement needs this.
- **Melodic patterns are monophonic per step** — the duration-bearing
  token consumes N steps, nothing else can sound on those steps.
- **`FX_BASS_RES` also controls PAD, `FX_LEAD_RES` also controls PLUCK.**
  No independent control.

**DSL / parser**

- **Trailing garbage in note names is silently discarded.** `C4..E4` and
  `A2abc` parse as `C4` / `A2`. Durations are validated strictly; notes
  are not. See "Hardening note parsing" below.
- **Melodic compact notation doesn't exist.** `C4..E4` is not shorthand
  for `C4 . . E4` — it's a bug waiting to happen. Spaces are required.

**Reference benchmark:** 
2-minute loop (64 bars @ 128 BPM,
9 instruments, full FX chain) renders in ~28 s with `-O2` on an
Intel Celeron N2940 @ 1.83 GHz (Bay Trail, 2014, 7.5 W TDP) —
that's ~8× faster than real-time on 2014 mobile-class hardware.

Expect ~4–6 s on modern desktop, ~3–8 s on modern mobile SoCs.
The render is single-threaded; parallelizing instrument render is
on the roadmap (see "How to help").


**Plugins / safety**

- **The bundled `clap_plugin.cpp` example uses the deprecated
  `addSampleAt(..., ctx.loopEnd, ...)` pattern.** At `LOOP 1` it wraps
  the tail into the beginning of the loop — the exact double-hit bug the
  main engine avoids. **Do not copy this pattern** — see the corrected
  version in "Writing a plugin" above. Fixing the bundled example is on
  the roadmap.
- **`LOAD_PLUGIN` is arbitrary code execution.** Do not parse untrusted
  `.txt` with plugin support enabled.
- **`LoadLibraryA` on Windows searches CWD by default.** Use
  `LOAD_PLUGIN ./plugin.dll` (explicit path), not `LOAD_PLUGIN plugin.dll`.
- **Windows: mixing CRT between exe and DLL causes heap corruption.**
  `/MD` with `/MD`, `/MT` with `/MT`. Don't mix.

**Performance**

- **Single-threaded render.** All instruments render sequentially. A
  4-minute track with 9 instruments and full FX takes a few seconds on
  desktop, possibly 5–15 s on mobile.
- **No parallel instrument rendering, no streaming.** Output is fully
  rendered in memory before writing. Max output is 10 minutes (600 s).

**Integration**

- **No C ABI, no in-memory WAV, no progress callbacks.** The only entry
  point is `int main()`. If you want to call this from a game engine
  without a subprocess, you'll need to add a small `extern "C"` wrapper.
  This is the #1 item on the roadmap.

---

## Roadmap / how to help

If you'd like to contribute, these are the highest-impact items. Roughly
in order of "what would help the most users".

1. **Fix `clap_plugin.cpp`.** It currently demonstrates the deprecated
   wrap pattern. ~15 min fix. Also extract `availSamples` into the
   header as `inline` so plugins can reuse it.
2. **C ABI wrapper + in-memory WAV.** One `extern "C"` function taking
   a `.txt` string and returning a byte buffer. Unblocks direct
   integration with Godot GDExtension and Unity P/Invoke. ~1 day.
3. **Expose stems.** Render drums and melodic bus into separate WAVs (or
   an array of buffers). Enables adaptive / layered game music. ~2 days.
4. **Parallelize instrument rendering.** `std::async` per instrument,
   then sum. 2–4× speedup. ~1 day.
5. **Fix loop-length computation.** Compute `loopSamples` once with
   `lround`, distribute the remainder across steps. Fixes the sub-ms
   drift described above.
6. **Strict note parsing.** Reject `C4..E4`, `A2abc` as errors instead of
   silently truncating. See docs §13.1 for the patch. ~30 min.
7. **Per-instrument volume / pan directives.** `KICK_VOL`, `BASS_PAN`,
   etc. Essential for real arrangements.
8. **Progress callback in the C ABI.** For loading screens that take
   more than a second.

If you're picking one: **#1 and #2** are the most valuable. #1 is
embarrassing that it's still broken. #2 is the difference between "CLI
tool" and "embeddable component".

---

## File layout

```
musicgen.h       — public API (all inline, single header)
musicgen.cpp     — engine (main pipeline + built-in instruments + FX)
clap_plugin.cpp  — example plugin (⚠️ see Known limitations)
LICENSE          — MIT
README.md        — this file
```

---

## Diagnostics

`musicgen` writes only `OK: out.wav` to stdout. Everything else goes to
stderr:

```
Loaded plugin: ./clap.so
Note: plugin './clap.so' already loaded, skipped
Note: repeated directive KICK; concatenating (this is a feature for long patterns)
Warning: unknown directive: BASSX
Warning: instrument 'LEAD' disabled: invalid note: H4
Warning: instrument 'LEAD' disabled: invalid duration in: A2:abc
Warning: instrument 'CLAP' disabled: produced non-finite samples (5)
Warning: instrument 'MY_LEAD' disabled: peak exceeds 8.0 (12.3)
Warning: FX_DELAY_TIME (44100 samples) >= loop length (35280); delay will not be loop-aligned.
Warning: pre-master mix peak = 1.8 > 1.0; lookahead limiter will bring it down.
Warning: plugin './broken.so' not loaded: missing 'register_instruments'
```

**"I don't hear an instrument" checklist:**

1. Melody written without spaces (`C4..E4` — one token, most notes lost).
2. Typo in directive name (`BAS` instead of `BASS`).
3. `FX_SIDECHAIN_DEPTH 1.0` + long duration — melody is ducked for 2 sec
   after every kick.
4. `FX_FILTER_CUT 0.0` — 120 Hz, only kick and bass survive.
5. `FX_DRIVE 1.0` + hot input — sounds like clipping (it is).

---

## License

MIT. See `LICENSE`.

---

## Acknowledgments

Built with AI assistance as a tool for a
personal Godot project. The DSL design, the loop-aware rendering
approach, and the effect chain ordering are human decisions; the C++
implementation is largely AI-generated and iterated on. If you're going
to maintain a fork, you're probably more qualified than I am — and
that's fine.
