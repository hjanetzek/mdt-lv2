# va: MDT's Trio and Analog as LV2 instruments (mTrio, mAnalog)

Goal: MicroDexed-touch's two small virtual analog synths, Trio
(`trio_engine.cpp`) and Analog (`analog_engine.cpp`), as LV2 instruments for
Zynthian on postmarketOS / nura armv7 (Cortex-A7, musl, 48 kHz / 960-frame
periods), built by nura's CI for armv7, aarch64 and x86_64. Second plugin
of mdt-lv2 after eDrumSynth (`../lxrdrums`); conventions: `../PLAN.md` §2.

Status (2026-11-23): implemented and tested on the desktop (steps 1-5 of
§5). Open: §8.

## 1. Facts this plan rests on

**The engines** (MDT `ca6518a8`, `engine/ORIGIN.md`):
- Plain C in `.cpp` files: `synth_shared.h` (track / voice / modulation
  structs, `preset_t`), `dsp_basics.{h,cpp}` (Q15 sine table, polyBLEP
  saw / pulse, triangle, soft clip from a tanh table, xorshift noise, the
  Simper trapezoidal SVF with Q12 coefficients from a 129-entry table, the
  pitch increments), `trio_engine.cpp` (three oscillators in 16 wave sets,
  ring modulation and hard sync with their own BLEP corrections, an SVF
  with LP / BP / HP / notch mixes and a random cutoff drift),
  `analog_engine.cpp` (two oscillators, noise, drive, LP SVF).
- Integer DSP; render functions add 128 frames (`CTL`) of one voice into an
  int32 buffer, given a `vmod_t` (pitch, increment, amplitude ramp,
  cutoff and pulse width modulation) for the block.
- The sample rate enters only `dsp_basics.cpp`'s SVF table
  (`tan(M_PI * fc / 44100)`) and `pitch_inc()` (`440 * 2^32 / 44100`), and
  the wrappers' envelope times (`synth_trio.cpp`: `44100`, `44.1f`).
  Everything else is per sample or per block of the phase increments.
- The Teensy wrappers (`AudioSynthTrio`, `AudioSynthAnalog`) hold one voice
  each: a linear ADSR in Q16.16 per sample (attack `atk * 4 ms`, decay
  `(dec/127)^2 * 3048 ms`, release `(rel/127)^3 * 13000 ms`, sustain
  `sus / 50`, saturating), the block update (envelope to amplitude ramp,
  envelope amount to cutoff and pulse width at the block start, idle voices
  reset their filter), clipping to int16.
- MDT runs 8 of each (`NUM_TRIO`, `NUM_ANALOG`), allocated in
  `handleNoteOn` (`.ino` 5052-5117: same note, free and idle, free and
  releasing, round robin steal), released in `handleNoteOff` (5455-5463),
  mixed by an `AudioMixer<8>` (gain `mapfloat(level, 0, 100, 0, 3.5 / 2.5) /
  sqrt(8)`, int16 saturation) and panned (`AudioEffectStereoPanorama`:
  balance, one side attenuated). No velocity, no pitch bend, no CCs. The
  reverb / delay sends are MDT's effects.
- Parameters: `trio_t` / `analog_t` (`config.h` 1444-1478); names and ranges
  in the Analog Synths page's editors (`UI.cpp` 8502-8610); presets
  `TRIO_PRESETS` (5) and `ANALOG_PRESETS` (12), applied by
  `as_apply_*_preset()` (`UI.cpp` 8311-8359); after boot: level 100, pan
  centre, preset 0 (`.ino` 1818-1838, 2170-2180).

**Zynthian** and **nura**: as `../lxrdrums/PLAN.md` §1 (engine key
`JV/<doap:name>`, symbols are API, groups as pages of 4, envelope / filter
widgets on plain float ports from 0, one jalv per instance, only the first
two outputs routed; nura builds with `-mfpu=neon-vfpv4 -mtune=cortex-a7 -O3
-funsafe-math-optimizations`, no libm per sample on the A7 / musl).

## 2. Decisions

Given (2026-11-23, `../PLAN.md` §4.1): directory `va/`; bundles
`mtrio.lv2` / `manalog.lv2`, binaries `mtrio.so` / `manalog.so`;
`doap:name` "mTrio" / "mAnalog" (`JV/mTrio`, `JV/mAnalog`); URIs
`https://github.com/hjanetzek/mdt-lv2/mtrio` / `.../manalog`;
GPL-3.0-only; `lv2:InstrumentPlugin`, atom MIDI input, stereo output group
(`pg:mainOutput`); pitch bend with a range port (default 2) and a velocity
amount port (default 0 = MDT's sound); mod wheel later; one nura package
`mdt-va-lv2`.

Made here:
1. **The engine at the host rate, no resampler.** `dsp_basics_init(rate)`
   builds the SVF table and the pitch base for the host rate; the envelope
   times use it. The 128-frame block stays 128 frames at every rate (the
   block-rate parts: the envelope's amplitude ramp and cutoff / pulse width
   modulation, Trio's cutoff drift; see §6). Verified at 22.05..192 kHz
   (§7).
2. **One source, two binaries**: `src/va.cpp` with `VA_ANALOG` selecting the
   engine and `VA_PORTS` the generated port table; the voice layer
   `src/voice.h` is shared.
3. **The voice layer reimplements MDT's wrapper, allocation and mixer**
   (no AudioStream): one `track_t` per voice (as MDT), every parameter to
   every voice, the envelope per sample, the block update, MDT's four-pass
   allocation, the Q16 mixer gain. Its own: a `held` flag instead of
   MDT's `note_buffer` value 0 (so MIDI note 0 works), pitch bend,
   velocity, CC 120 / 123, `activate()` resets the voices.
4. **The mixer doesn't saturate** (deviation from MDT): MDT's int16 mixer
   clips a three-note chord at level 100 (each voice peaks near
   24000 x 1.24); the plugin sums in 32 bits and outputs floats above full
   scale, so the host's volume can take it down. Bit exact to MDT wherever
   MDT doesn't clip (tested at level 25 with MDT's saturating mixer).
5. **Pitch bend**: the bend in 1/16 semitones goes into the engine's pitch
   (`pitch16`), the rest as a fine ratio of the increment (a 256-entry
   table), from which the engines derive their other oscillators' fine
   offset (Analog's detuned oscillator needed `LV2:` change 3). Per block.
6. **Velocity amount** `a` (0..1): level `1 - a (1 - (v/127)^2)` on the
   amplitude ramp, cutoff `- a (127 - v) / 127 x 32` filter steps (about
   2.3 octaves). Velocity 127 or amount 0: MDT exactly.
7. **Ports** (generated, §4): MDT's editors except the sends and the MIDI
   channel, plus Velocity and Bend Range; five groups of at most four, each
   one Zynthian page: Osc, Filter, Amp EG, Mod, Master. Pan as -20..20.
8. **Presets**: MDT's built-in ones, the values its loader sets, as
   `pset:Preset` in a `pset:Bank` "Factory" (Zynthian finds banks and
   presets through `lv2:appliesTo` in the manifest). Level, Pan, Transpose
   and the new ports are not in presets (as MDT). Port defaults: preset 0
   and MDT's boot state.
9. **Global tables**: the engine's tables are global (`dsp_basics`): one
   sample rate per process; an instance at another rate is refused while one
   is active (same rate: any number).
10. **Events and controls**: events take effect at the next 128-frame
   block not yet rendered (independent of the host block size); controls
   are read at the start of `run()`, set when their rounded value changes.

## 3. Layout

```
va/
  PLAN.md, README.md
  CMakeLists.txt        plugins, generator, tests
  engine/               copy of MDT's engine files (ORIGIN.md, LICENSE)
  src/va.cpp            the LV2 plugin (both synths)
  src/voice.h           the voice layer (MDT's wrapper, allocation, mixer)
  src/params.h          the voice layer's parameters
  src/ports_mtrio.h     generated port tables
  src/ports_manalog.h
  src/va.map            exports lv2_descriptor only
  tools/gen_ttl.cpp     the TTL generator
  tools/mdt_tables.h    MDT's editors, boot defaults and preset loader, transcribed
  tools/mktarball.sh    source tarball
  mtrio.lv2/, manalog.lv2/   manifest.ttl, <name>.ttl, presets.ttl (generated, committed)
  tests/                render test, MDT reference, CMake checks, frozen symbols
  packaging/APKBUILD    draft recipe for nura-builds
../common/              testhost/, ttl.h, zynthian-schema.ttl (shared with lxrdrums)
```

## 4. Ports

Generated by `tools/gen_ttl.cpp` from a layout table per synth (group,
MDT editor label, symbol, name, designation, unit, comment) over MDT's
editor table (`tools/mdt_tables.h`: label, parameter, range) and the
engine's enumeration names (`N_TRIO_WAVE`, `N_TRIO_MODE`,
`N_ANALOG_WAVE`, with readable labels from a table; unknown names stop the
generator). The generator checks that every exposed MDT editor has a port,
symbols are unique and `[a-z0-9_]`, designations only on plain numbers
from 0. It writes `ports_<name>.h` for the plugin (port -> parameter,
offset, range, default) so the TTL and the binary can't disagree.

| Group | mTrio | mAnalog |
|---|---|---|
| Osc (`param:OscillatorControls`) | wave (enum 16), interval2, interval3 (-24..24), detune (0..50 ct) | wave (enum 5), detune (0..127 ct), mix, noise |
| Filter (`param:FilterControls`) | cutoff, resonance (designations), mode (enum 4) | cutoff, resonance (designations), drive, key_track |
| Amp EG (`param:EnvelopeControls`) | attack, decay, sustain, release (designations) | the same |
| Mod | pw, env_pw, env_cutoff, velocity (0..100 %) | env_cutoff, env_pw, velocity |
| Master | level (0..100 %), pan (-20..20), transpose (-24..24), bend_range (0..24) | the same |

## 5. Steps

1. Copy the engine (`engine/`, `ORIGIN.md`); run-time rate (`LV2:`).
2. Generator, TTL, presets, port tables; lint clean.
3. Plugin and voice layer; render test bit exact against MDT at 44.1 kHz.
4. `common/`: the render test host and the TTL helpers moved out of
   lxrdrums (its TTL, symbols and tests unchanged).
5. Tests (rates, block sizes, bend, velocity, presets), README, this plan,
   tarball script, draft APKBUILD.
6. armv7 package in nura's builder (device-testing session), phone
   measurements (lv2bench, synthbench), Zynthian pages and widgets.
7. Recipe in nura-builds with a commit of the published repository,
   `engine_config` entries (`JV/mTrio`, `JV/mAnalog`, category "Synth").

## 6. Implementation notes (steps 1-5, 2026-11-23)

- **Engine changes** (`LV2:` marks, `engine/ORIGIN.md`): no `config.h` /
  `DMAMEM`; `dsp_basics_init(double rate)` (SVF table and pitch base for the
  rate; below 35.6 kHz the table's 16 kHz top is limited to 0.45 of the
  rate); Analog's detuned oscillator takes the fine pitch offset; `y * 2`
  for `y << 1` on negative values (UBSan). `dsp_basics_init(44100)` is MDT.
- **MDT behaviour kept**, though it looks unintended: Analog's key tracking
  uses `v->pitch16`, which MDT's wrapper never sets (0), so KTR is a fixed
  cutoff offset (`KTR * 60 / 256` filter steps, 15 at the presets' 64) and
  doesn't follow the key; the presets were voiced with it. The sustain
  level is full from 50 up (`sus / 50`). The port comments say so. A real
  key tracking would be a new port (or a change of KTR's meaning) later.
- **Block-rate parts at other rates**: the envelope is per sample with
  rate-scaled increments, so its times are right (within a block); the
  128-frame block is shorter at higher rates (1.33 ms at 96 kHz), so the
  amplitude ramp and the envelope-to-cutoff / pulse width steps are finer;
  Trio's random cutoff drift (a one-pole low pass per block, `>> 3`) runs
  faster in time (its amount is the same). Analog's white noise has the
  same level, spread to the higher Nyquist frequency (0.4 dB less below
  20 kHz at 48 kHz).
- **Filter precision**: the SVF's coefficients are Q12 (`g * 4096`); at
  higher rates `g` is smaller, so low cutoffs are coarser (at 96 kHz the
  lowest table entries are 4-5 steps of `g`); the level through the filter
  at cutoff 70 stays within 0.1 dB from 22.05 to 192 kHz (§7). At 48 kHz
  the difference to 44.1 kHz is negligible.
- **Pitch bend accuracy**: oscillator 1 follows the bend exactly; Trio's
  oscillators 2 and 3 get the fine part in steps of 1/4096 of the increment
  (0.42 cent), so a bent Trio chord beats by less than half a cent (measured
  0.2 cent low at +2 semitones).
- **The mixer**: `(mult * x) >> 16` per voice like MDT's, summed in 32 bits
  (MDT: int16 saturating); float output `sum / 32768` times the pan gain
  (1.0 at the centre: bit exact).
- **Output level**: one voice at Level 100 peaks near 0.9 (24000 x 1.24 /
  32768); eight voices can reach about 8. Zynthian's chain volume follows.
- **C++**: the engine is C in `.cpp` files; the plugin is C style C++
  without exceptions and RTTI, linked with `--as-needed` (no libstdc++ in
  the binary; `imports_*` test). The plugin's own float code: `set_env`
  and the mixer gain when a port changes, the output scaling per sample
  (no denormals: integer DSP).
- **lv2lint** passes with `-Mpack -Ewarn`; notes remain (version 0.x, port
  comments, units on enumerations).
- **Presets in lilv**: checked with a small lilv program (not a test:
  lilv isn't a build dependency) as Zynthian reads them
  (`lilv_plugin_get_related` for `pset:Bank` and `pset:Preset`, then
  `pset:bank`): the bank "Factory" and all presets in it, for both bundles.

## 7. Measurements (desktop)

AMD Ryzen Threadripper 3960X, GCC, `-O3`, render test:

| | mTrio | mAnalog |
|---|---|---|
| 8 voices held, 48 kHz, 960 frames | 93-98 ns / sample (0.45 % of a core) | 89-94 ns / sample (0.44 %) |

Rates (render test; A4 of a steady tone, the envelope until it reaches 0
with decay 100 / release 70, the RMS of a saw through cutoff 70,
resonance 60):

| Rate | mTrio A4 | decay | release | filter | mAnalog A4 | decay | release | filter |
|---|---|---|---|---|---|---|---|---|
| MDT's set_env | 440 | 1.8898 s | 2.1768 s | | 440 | 1.8898 s | 2.1768 s | |
| 44.1 kHz | 440.000 | 1.8924 | 2.1790 | -9.18 dB | 440.000 | 1.8924 | 2.1790 | -13.01 dB |
| 48 kHz | 440.000 | 1.8907 | 2.1799 | -9.20 | 440.000 | 1.8906 | 2.1793 | -13.03 |
| 96 kHz | 440.000 | 1.8907 | 2.1773 | -9.18 | 440.000 | 1.8906 | 2.1770 | -13.01 |
| 22.05 / 32 / 88.2 / 192 kHz | 440.000 | within 3 ms | within 5 ms | within 0.07 dB | 440.000 | within 3 ms | within 5 ms | within 0.08 dB |

(The times include the block granularity: the envelope ends in the block
it reaches 0, events start at a block.)

On the phone (to do, step 6): expect a fraction of eDrumSynth's 2.3 µs /
sample: no resampler, idle voices skipped, eight voices of integer DSP.

## 7a. Measurements on the phone (2026-11-23)

armv7 package (mdt-lv2 c25cd61, nura plugins set flags, NEON with fused
MAC), Galaxy S III Neo (Cortex-A7), 48 kHz, 960-frame periods, measured by
the device-testing session with zynthian running in the background:
- lv2bench (no notes): mTrio 0.013-0.033, mAnalog 0.012-0.013 µs per sample
  (about 0.06 % of a core).
- jalv + MIDI, the whole jalv process (with jalv's and JACK's overhead),
  mTrio / mAnalog: idle 0.174 / 0.208 µs/sample (0.8 / 1.0 %), 4-note
  chords 0.208 / 0.191 (1.0 / 0.9 %), 8 notes held 0.191 / 0.174
  (0.9 / 0.8 %): within the noise of jalv's own cost.
- perf, 8 notes held, default preset: the plugin is 21 % (mTrio) / 14 %
  (mAnalog) of the jalv process, i.e. about 0.17 % / 0.12 % of a core; the
  rest is libjack, musl's memcpy and the kernel. 50-100 times cheaper than
  eDrumSynth; nothing to optimise.
- No crashes or errors. Heavier presets not measured (zynthian's jalv
  can't load a preset URI from the command line).

## 8. Open

- armv7 / aarch64 package builds in nura's CI and the phone tests
  (lv2bench, synthbench, Zynthian pages and widgets, listening): step 6,
  with the device-testing session.
- The recipe in nura-builds (`aports/lv2/mdt-va-lv2/`, draft:
  `packaging/APKBUILD`) with the commit once pushed; `engine_config`
  entries.
- Mod wheel (CC 1: to cutoff or a vibrato?), sustain pedal (CC 64: MDT
  ignores it), a real key tracking for mAnalog, MDT's monophonic flag in the
  presets (`preset_t.mono`, unused by MDT's Trio / Analog), a per-voice
  pan spread: later, with the user.
