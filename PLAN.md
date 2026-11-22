# mdt-lv2: MicroDexed-touch's synths as LV2 plugins

MicroDexed-touch's sound engines as LV2 plugins for Zynthian, first on
postmarketOS / nura armv7 (Cortex-A7, the Galaxy S III Neo), built by nura's
CI for armv7, aarch64 and x86_64. Each plugin is a directory here with its
own plan; this file is the roadmap and the conventions they share.

Status (2026-11-23):
- `lxrdrums/`: **eDrumSynth** (the LXR drum engine). Done: desktop tests,
  armv7 package, phone benchmark (12 % of a Cortex-A7 core at 48 kHz),
  recipe handed to nura-builds CI. Plan: `lxrdrums/PLAN.md`.
- Next: §3.

## 1. What MDT has, and what is worth porting

From a survey of every MDT sound source (MicroDexed-touch at ca6518a8;
`.ino` = `MicroDexed-touch.ino`, `TP` = `third-party`):

| Engine | DSP | Existing LV2 | Effort | Port? |
|---|---|---|---|---|
| LXR drums | plain C engine | none | done | **done** (eDrumSynth) |
| Trio (`trio_engine.cpp` 295 lines + `synth_trio.cpp`) | self-contained C, Q15, own ADSR and SVF; 8 voices | none identical | small | **yes, next** |
| Analog (`analog_engine.cpp` 94 + `synth_analog.cpp`) | as Trio (shares `synth_shared.h`, `dsp_basics`) | none identical | small | **yes, with Trio** |
| Ambient Garden (`amb.c` 1227) | self-contained float C, generative, own clock, 28 params with info, 8 pieces | none | small engine, design questions | **yes, after a design round** |
| Sample players: drums, MultiSample, slices (`PitchSamplePlayer` + Teensy envelope / biquad), granular (`granular.cpp`) | shared voice pool and MDT's sample store | partly (fabla, avldrums, samplv1); no granular synth | medium: sample loading, kit format | **yes, later** |
| MicroSynth (`microsynth.cpp`, built from Teensy Audio objects) | no own DSP | generic VAs (synthv1, helm, odin2, surge) | medium | later, for MDT fidelity |
| Dexed (`TP/Synth_Dexed`) | standalone engine | **dexed-lv2** (same lineage) | small | no |
| ePiano (`TP/Synth_MDA_EPiano`) | mda EPiano | **mod-mda-lv2** | small | no |
| Braids (`TP/Synth_Braids`) | Teensy objects around the MacroOscillator | **zynmi-lv2** (`mi_braids`) | medium | no (maybe MDT's envelope / filter wrapper later) |
| NoiseMaker (`noisemaker.cpp`) | offline one-shot renderer, not real time | many drum synths | medium | optional |

Common facts:
- Everything runs at 44.1 kHz, int16 or Q15 / float; the phone's JACK runs at
  48 kHz.
- MIDI is handled in MDT's `handleNoteOn` / `handleNoteOff` (`.ino`
  4631-5538), including the voice allocation; only Dexed, the ePiano and the
  LXR get CCs, only Dexed pitch bend and aftertouch. A plugin takes the
  instrument's part of that code.
- MicroSynth's and Braids' LFOs and filter sweeps run in MDT's `loop()`
  every 30 ms: an LV2 version moves them into the render, at block rate.
- MDT's send effects (plate reverb, the two delays, sidechain, multiband
  compressor) stay out: Zynthian has its own FX chains. Inline effects worth
  keeping as optional ports: the Dexed and ePiano choruses, the Braids
  flanger (only relevant if those are ever ported).

## 2. Conventions (from eDrumSynth)

- **Self-contained**: engine sources are copied into the plugin directory
  (`<plugin>/engine/...`, `ORIGIN.md` with the MicroDexed-touch commit and
  the changes, marked `LV2:`). Nothing here builds from `MicroDexed-touch/`,
  and nobody changes MDT for a plugin.
- **Plain LV2 in C** (C++ where the engine is), no plugin GUI; Zynthian shows
  the controls. CMake, installs `<prefix>/lib/lv2/<bundle>.lv2`, exports only
  `lv2_descriptor`, no ARM flags (nura's build sets them; correctness flags
  like `-mfp16-format=ieee` via compiler checks).
- **Generated TTL**: a desktop generator writes the TTL and a `ports.h` from
  the engine's own parameter tables or MDT's editor tables (copied), both
  committed; ctest regenerates and diffs them; port symbols frozen in a test
  file once released.
- **Zynthian**: `doap:name` is the engine key (`JV/<name>`) and port symbols
  are API, both decided before the first package. Groups (`pg:group` with
  `lv2:symbol`, `lv2:name`, `lv2:displayPriority`) become pages of 4.
  Envelope (`param:attack/decay/sustain/release`) and filter
  (`param:cutoffFrequency/resonance`) designations only on plain float
  ports with minimum 0; a cutoff in Hz gets a 0..1 or 0..127 perceptual port.
- **Real time**: no allocation, locks, I/O or per-sample libm calls in
  `run()`; parameters applied only when changed; even work per period
  (48 kHz, 960 frames on the phone).
- **Sample rate**: run the engine at the host rate when its rate only enters
  computed tables and coefficients (Trio, Analog, Ambient Garden look like
  this); otherwise keep 44.1 kHz and resample (eDrumSynth's resampler).
- **One engine per process** is acceptable (Zynthian runs one jalv per
  instance); plugins refuse a second instance when the engine has global
  state, and say so.
- **Tests** (ctest): lint (lv2lint, sord_validate, lv2_validate), TTL
  regenerate-and-diff, frozen symbols, a render test host (dlopen,
  minimal `urid:map`) comparing against the engine, block size
  independence; on the phone: lv2bench / synthbench via the device-testing
  session, Zynthian pages and widgets with the user.
- **Packaging**: a recipe `aports/lv2/<name>-lv2/APKBUILD` in nura-builds
  (GitHub archive of this repository at a commit); the device-testing
  session takes it into nura's CI (sets, all architectures, the
  `engine_config` default in zynthian-data's `nura-engine-config.json`).

## 3. Shared code (`common/`)

Moved out of `lxrdrums/` when the second plugin needs it, so each piece has
two users before it becomes shared:
- `common/resampler.c/h` (eDrumSynth's polyphase resampler).
- `common/testhost/`: the render test host (dlopen, `urid:map`, MIDI atom
  writer, block size runs).
- `common/ttl.h`: TTL writing helpers for the generators (groups, ports,
  scale points, designations).
- **`common/teensyaudio/`**, for engines built from Teensy Audio objects
  (MicroSynth, the sample players' envelopes and biquads, a Braids wrapper):
  a minimal runtime copied from the port (`port/addon/teensyaudio/AudioStream.cpp`
  graph and block pool, a single-threaded platform with no-op locks, the
  port's `arm_math.h` stand-in, a minimal `Arduino.h` with only what DSP code
  needs), the teensy-audio objects an engine uses with the fork's generic C
  paths (`TEENSY_AUDIO_GENERIC_DSP`), and a sink object collecting each
  128-frame block (like the port's `AudioOutputI2S` stand-in). `run()` calls
  `AudioStream::update_all()` per block, converts int16 to float and
  resamples. The graph is global: one instance per process.

## 4. Order

1. **Trio and Analog** (one directory `va/`, two plugins in one bundle or
   two bundles; shared `synth_shared.h` / `dsp_basics`):
   - engine copied; the `dsp_basics` tables (built with 44100 in
     `dsp_basics.cpp`, the `tan(...)/44100` and pitch increments) and
     `synth_trio.cpp`'s 44100 made a run-time rate, so no resampler;
   - voices: MDT's 8-voice allocation (same note, idle, releasing, steal;
     `.ino` 5052-5117) moved into the plugin;
   - ports generated from MDT's editor tables (`UI.cpp` 8502-8610: names,
     ranges, `N_TRIO_WAVE` names), groups Osc / Filter / Amp EG / Mod / Out;
     ADSR and cutoff / resonance designations;
   - presets: the built-in `TRIO_PRESETS` (5) and `ANALOG_PRESETS` (12) as
     LV2 presets;
   - MDT ignores velocity and pitch bend for both; v1 adds pitch bend (±2
     semitones, CC-free) and a velocity → level / cutoff amount port (default
     0, MDT's behaviour). Mod wheel (CC1) → LFO / cutoff amount later.
   - Effort: about eDrumSynth's, minus the resampler and the parameter-table
     complexity.
2. **eDrumSynth optimisations** if the phone needs them (idle voices,
   mixer, SVF coefficients: `lxrdrums/PLAN.md` §12), any time.
3. **Ambient Garden**: its own plan first, with the user. Open: it sends the
   pad and bells as MIDI out (to Braids and Dexed in MDT) and renders only
   bass and pulse itself, so either an LV2 MIDI output (Zynthian chains the
   synths) or restored local pad / bell voices; reverb / echo sends as extra
   outputs (Zynthian routes only the first two) or mixed in; the clock from
   the host's transport.
4. **Sample player** (drums, MultiSample, slices, granular): its own plan.
   The DSP is small; the work is sample loading (LV2 state with
   `atom:Path`s, the worker extension for loading, no allocation in `run()`),
   a kit / zone format (MDT's `drums.json`, `msp_presets.json` and
   `/DRUMS/*.wav` as the reference), and the Teensy envelope / biquad per
   voice (`common/teensyaudio` or ported directly). Check the licence and
   provenance of MDT's built-in drum samples before shipping any.
5. **MicroSynth** on `common/teensyaudio` (control-rate sweeps and LFO moved
   into the render), when MDT fidelity is wanted.

Not planned: Dexed, ePiano, Braids (covered by dexed-lv2, mod-mda-lv2,
zynmi-lv2), NoiseMaker (offline renderer; maybe as a worker-rendered drum
synth later).

## 5. Decisions needed from the user

- Names (`doap:name`, the Zynthian engine key) for the Trio and Analog
  plugins, and whether they are two plugins in one bundle (`va/`) or two
  directories.
- Whether v1 adds pitch bend / velocity to Trio and Analog (default above:
  yes, with velocity amount 0 = MDT's sound) or stays exactly MDT.
- Ambient Garden's design questions (§4.3), when it is next.
