# lxrdrums: MDT's LXR drum engine as an LV2 plugin for Zynthian

Goal: MicroDexed-touch's synthesized drums (the LXR / OpenElixir-02 drum
engine, `MicroDexed-touch/third-party/Synth_LXR`) as an LV2 instrument for
Zynthian on a low-spec 32-bit ARM device: postmarketOS / nura "armv7"
(ARMv7-A, hard float, musl, NEON; test device Samsung Galaxy S III Neo,
Cortex-A7). Zynthian shows its controls in pages and its envelope and filter
widgets for them. This is the first MDT synth as LV2; the others follow the
same pattern (§10).

Status: steps 1-4 implemented (2026-11-22; notes: §11). Implementation: §9.
v2 (0.1.0, §13) implemented on the desktop 2026-11-25 (notes: §14); armv7
build and phone check open.

## 1. Facts this plan rests on

**The engine** (`Synth_LXR/src/lxr_engine.h`, README there):
- Plain C, one translation unit (`lxr_engine.c` includes the firmware
  sources; `-std=gnu17 -I lxr -I lxr/shim`). The HAL shim has C versions of
  the Cortex-M instructions, so it builds anywhere (the desktop build and
  AddressSanitizer / UBSan already run it).
- Global state: **one engine per process**.
- Renders 64-frame blocks of int16 stereo (`lxr_engine_render()`), **fixed
  44.1 kHz** (`REAL_FS`; envelope, LFO and transient-sample rates are per
  sample, not only the 14 formulas that use `REAL_FS`).
- 7 tracks (bass drum, tom 1, tom 2, snare, cymbal, closed and open hi-hat),
  6 voice slots with an engine each (stock: drum, drum, drum, snare, cymbal,
  hi-hat), 247 kit parameters of 0..127 (`lxr_engine_set_parameter()`), plus
  globals (morph 248, and 266, 267, 302: DJ filter, limiter).
- Parameter info from the LXR's menu tables: names (long, short, category),
  ranges, kind (number, signed, switch, text with value names), and the voice
  pages: 7 pages (the slots, page 7 = open hi-hat) × 8 sub pages of up to 8
  parameters, following the slots' engines. With the stock engines the sub
  pages are: 0 oscillator, 1 amp EG ("Veloc EG": attack, decay, slope), 2 mod
  EG / pitch mod / velocity, 3 FM (drums) or mod oscillators (cymbal,
  hi-hat; empty for the snare), 4 transient, 5 filter (frequency, resonance,
  type, drive), 6 LFO, 7 voice (volume, pan, sample rate, drive, audio out).
  Page 7 shares the hi-hat's parameters except its own decay (61).
- FX bus parameters 234..245, its page depends on the FX type
  (`lxr_engine_fx_param()`). Parameters without a name (5, 97, 98, 127,
  221..227, 246, 247, 249..) are not exposed.

**Zynthian** (zynthian-ui fork, `zyngine/zynthian_lv2.py`,
`zynthian_engine_jalv.py`, `zyngui/zynthian_widget_*.py`; its own guide:
`zynthian-ui/.claude/skills/zynthian-lv2-plugin/`, `docs/zynthian-widgets.md`):
- Discovery with lilv, one `jalv` process per plugin instance (so the
  engine's global state is fine), controlled over stdin (`symbol=value`).
- Engine key `"JV/" + doap:name`: **the name is API.** Port symbols are API
  too (presets, snapshots, MIDI learn store them).
- Type: `lv2:InstrumentPlugin` → MIDI Synth. Category from the LV2 class
  ("Synth"); "Percussion" only via `engine_config.json` defaults. New plugins
  are added disabled.
- Pages: controllers sorted by `lv2:displayPriority` (group priority ×
  1000000 + port priority), bucketed by `pg:group` (the group needs an
  `lv2:symbol` and `lv2:name`), 4 per page ("Name 1", "Name 2" when a group
  has more than 4). Ungrouped controls go to "Params".
- Widgets come from `lv2:designation` on **plain float** ports (no
  `lv2:integer`, no scale points, not toggled):
  - envelope: `param:attack`, `param:decay`, `param:sustain`,
    `param:release` (...): the widget draws the page's group, value /
    range, so **minimum 0**;
  - filter: `param:cutoffFrequency`, `param:resonance`: always a low-pass
    curve, cutoff mapped linearly from the port range onto 20 Hz..20 kHz
    log, so a 0..127 "perceptual" cutoff displays right.
- MIDI: only the first MIDI input is connected; only the first two audio
  outputs are routed. By default only CC 64/66/67/69 reach the plugin;
  controls are MIDI-learned onto ports by zynthian.

**nura** (nura-builds, branch `main`; answers from the device-testing
session):
- Recipe `aports/lv2/edrumsynth-lv2/APKBUILD`, CMake like `artyfx-lv2`,
  `arch="all"`, installs `/usr/lib/lv2/<bundle>.lv2`; listed in
  `sets/plugins.sh` `PACKAGES_armv7`.
- Built by pmbootstrap in its armv7 chroot inside the x86_64 builder VM
  (`localhost:2240`, user `user`; `tools/s3ve3g-builder.sh build --arch
  armv7 edrumsynth-lv2`, `... fetch`). The VM and the phone are shared with
  the device-testing session: coordinate.
- Flags come from the build (`-mfpu=neon-vfpv4 -mtune=cortex-a7`, and for
  the plugins set `-O3 -funsafe-math-optimizations`); the recipe and the
  CMake files set no ARM flags.
- On the phone JACK runs at **48 kHz, 960 frames** per period.
- Cortex-A7 / musl costs: libm calls are real calls (no `expf`, `floorf`,
  ... per sample), no allocations or large memsets in `run()`, even work
  per period.
- Testing on the phone: `lv2bench` with `LV2_PATH` limited to the bundle,
  device-testing's `synthbench.py` (µs per sample via jalv on a dummy JACK
  server). Install: copy the `.apk`, `sudo apk add ./x.apk`.

## 2. Decisions

1. **Plain LV2 in C**, no DPF, no plugin GUI: Zynthian shows the controls;
   desktop hosts use their generic UIs. DPF would generate the TTL by running
   the plugin at build time, i.e. under qemu on armv7, and adds C++ and a
   GUI library for nothing here.
2. **Self-contained: the engine is copied in.** `lv2/` will become its own
   git repository, so it doesn't build from the MicroDexed-touch submodule
   and nobody works in MDT for it. `MicroDexed-touch/third-party/Synth_LXR`
   (src, lxr, LICENSE, NOTICE.md, README.md) is copied to
   `lv2/lxrdrums/engine/Synth_LXR/` unchanged, with `ORIGIN.md` naming the
   MicroDexed-touch commit it came from. Changes the plugin needs in the
   engine are made in the copy, marked `LV2:` and listed in `ORIGIN.md`, so
   they can be carried back to MDT by hand later. (Code shared by several
   plugins later moves to `lv2/common/`.)
3. **The engine runs at 44.1 kHz; the plugin resamples** to the host rate
   (bypassed at 44100). Running the engine at the host rate would mean
   changing every per-sample rate in the firmware (envelopes, LFOs, the
   transient samples' playback), a fork of its sound; a resampler keeps the
   sound exact. A fixed-ratio polyphase FIR (44100 → 48000 = 147:160, and a
   general fractional one for other rates) costs a few MAC per output sample
   and is NEON friendly. Revisit only if the phone measurement says so.
4. **Stock engines in v1, no engine selection.** The ports are static, but
   an engine change changes what the slot's engine-specific parameters mean
   (and resets them). v1 exposes the stock-engine parameters with their
   stock names. Later: engine ports with neutral names, or one plugin URI per
   engine layout (§10).
5. **Ports for every exposed parameter, generated.** A generator program
   links `lxr_engine.c` and writes the TTL from the menu tables; the
   generated TTL is committed (cross builds don't run it), and a test
   regenerates and diffs it. Port symbols are frozen once released.
   - Mapping: number → float 0..max; signed → float min-center..max-center
     (converted back in the plugin); switch → `lv2:toggled`; text →
     `lv2:integer` + `lv2:enumeration` with scale points from
     `lxr_engine_param_text()` (also the 229 modulation targets).
   - Widget ports stay plain float: amp EG attack → `param:attack`, decay →
     `param:decay`; filter frequency → `param:cutoffFrequency`, resonance →
     `param:resonance`. The filter widget always draws a low-pass, also for
     the LXR's other filter types (accepted).
   - The plugin rounds the floats to the engine's integers and calls the
     setter only when a port's value changed.
6. **Groups = the voice sub pages**, one `pg:group` per voice and sub page
   ("Bass Drum Osc", "Bass Drum Amp EG", "Bass Drum Filter", ...), shared
   parameters once (page 7's open-hi-hat decay goes into the hi-hat's amp EG
   group, plain, as "Decay Open"). Order by `lv2:displayPriority`: voices in
   track order; within a voice: voice (volume, pan, ...), oscillator, amp
   EG, filter, transient, FM / mod osc, mod EG / velocity, LFO. Then the FX
   bus group and a global group (DJ filter, limiter). Morph is left out in
   v1 (it needs a second kit as state).
7. **MIDI in, one stereo out.** An atom MIDI input first, then a stereo
   output group (`pg:mainOutput`), `lxr_engine_render()` (both mixes). LO /
   HI outputs later (Zynthian routes only the first two anyway).
8. **Note map: General MIDI drums**, notes trigger tracks at the tuned pitch
   (`LXR_ENGINE_NOTE`, velocity passed): 35/36 bass drum, 45/47/48/50 tom 1,
   41/43 tom 2, 38/40/37/39 snare, 49/57/51/52/55/59 cymbal, 42/44 closed
   hi-hat, 46 open hi-hat. Note-offs are ignored. Chromatic playing and a
   note map control later.
9. **No MIDI CC to the engine in v1**: the LXR's CC n = parameter n-1 would
   collide with what Zynthian passes (64 sustain → parameter 63, ...).
   Zynthian MIDI-learns the ports instead. Tempo for the LFO clock sync from
   the host (`time:Position` via the atom input) if the host sends it, else
   120.
10. **One instance per process**: a second `instantiate()` in the same
    process fails (returns NULL, logs why). jalv / Zynthian run one per
    process.
11. **Identity** (name decided 2026-11-22): `doap:name` **"eDrumSynth"**
    (the Zynthian engine key `JV/eDrumSynth`: don't change it), symbol
    `edrumsynth`: bundle `edrumsynth.lv2`, binary `edrumsynth.so`, URI
    `https://github.com/hjanetzek/mdt-lv2/edrumsynth`, package
    `edrumsynth-lv2`; GPL-3.0-only (the engine's licence, with its NOTICE;
    no "LXR" in names, it is an Erica Synths trademark). The source
    directory stays `lv2/lxrdrums` for now.
12. **Packaging source**: one archive. Until `lv2/` has its own repository,
    a local tarball (`tools/mktarball.sh`) for builds in the builder VM;
    then the recipe downloads that repository's archive at a commit. Where
    and when it is published is the user's call.
13. **Zynthian enabling**: an `engine_config.json` default entry
    (`"JV/<name>"`, `ENABLED`, `CAT: "Percussion"`) where nura ships
    zynthian-sys' defaults; prepared as a commit, not pushed.

## 3. Layout

```
lv2/lxrdrums/
  PLAN.md            this file
  README.md          what it is, build, test, install
  CMakeLists.txt     plugin, generator, tests
  src/lxrdrums.c     the LV2 plugin (instantiate, connect_port, run, ...)
  src/resampler.c/h  44.1 kHz → host rate
  src/notemap.h      GM note → track
  engine/Synth_LXR/  copy of MDT's third-party/Synth_LXR (ORIGIN.md: commit, changes)
  tools/gen_ttl.c    writes lxrdrums.ttl and manifest.ttl from the engine tables
  tools/mktarball.sh source tarball for the recipe
  edrumsynth.lv2/    manifest.ttl, edrumsynth.ttl (generated, committed)
  tests/             render test host, TTL check, resampler test
```

## 4. The plugin

- `instantiate`: refuse a second instance; require `urid:map` (MIDI and
  time URIDs); `lxr_engine_init()`, init kit; resampler for the host rate;
  all buffers allocated here (FIFO of engine frames, resampler history).
- `run(n)`:
  1. Control ports: for each, if the value changed, convert (round, signed
     offset) and `lxr_engine_set_parameter()`.
  2. Atom input in time order: split `n` at event frames; note-ons →
     `lxr_engine_trigger()`; `time:Position` → `lxr_engine_set_tempo()`.
     Events take effect at their frame, rounded to the engine's 64-frame
     grid at 44.1 kHz (1.45 ms; the LXR itself triggers per 64-frame block).
  3. Audio: keep a FIFO of rendered 44.1 kHz frames; render engine blocks as
     the resampler needs input; int16 → float (× 1/32768); resample into the
     outputs. At 48 kHz / 960 frames that is 13 or 14 engine blocks per
     period: even work.
- No allocations, no locks, no libm per sample in `run()`; denormals are not
  an issue (int16 engine output; the resampler is FIR).
- Latency: the FIFO's and the FIR's, reported via a `lv2:reportsLatency`
  output port only if hosts need it (instruments usually don't).

## 5. TTL generation

`tools/gen_ttl.c` (a desktop program) walks the voice pages with the stock
engines, then the FX bus and globals, and writes:
- groups with `lv2:symbol`, `lv2:name`, `lv2:displayPriority`;
- ports with symbol, name (long name; the group gives the voice and
  category), range, default (the init kit's value), type and scale points,
  designation, `pg:group`, `lv2:displayPriority` (descending in page order,
  so the pages show the LXR's order);
- symbols `<voice>_<category>_<name>` in `[a-z0-9_]`, e.g. `bd_osc_coarse`,
  `hh_ampeg_decay_open`; unique; frozen in a table the test checks.

The plugin uses the same tables at run time (port index → parameter number,
conversion), from a header the generator also writes (`src/ports.h`), so the
TTL and the binary can't disagree.

## 6. Tests

On the desktop (x86_64), all in `ctest`:
- `lv2lint` and `sord_validate` on the bundle, clean (or with documented
  waivers).
- TTL check: regenerate, diff against the committed files.
- Render test host (dlopen + `lv2_descriptor`, a minimal `urid:map`, no
  lilv): at 44.1 kHz the plugin's output equals `lxr_engine_render()` driven
  directly with the same triggers and parameters (bit exact after the int16
  → float scale); at 48 kHz: no NaN or clipping storm, the bass drum's pitch
  within 1 % of the 44.1 kHz render, block sizes 1, 64, 480, 960, 4096 and
  odd ones give the same signal.
- Resampler test: a 1 kHz sine through 44.1 → 48 kHz: frequency right,
  aliasing / noise below -80 dB.
- jalv only with the user's OK (it creates a client on the user's JACK).

On the phone (with the device-testing session's coordination): `lv2bench`
and `synthbench.py` (µs per sample at 48 kHz / 960); listen in Zynthian;
pages and widgets on screen.

## 7. Packaging

- `nura-builds` (a worktree on a new branch from `main`, not pushed without
  the user's OK): `aports/lv2/edrumsynth-lv2/APKBUILD` (one archive source,
  `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=None -DCMAKE_INSTALL_PREFIX=/usr`,
  `options="!check"` or ctest without the lint tools), and
  `sets/plugins.sh` `PACKAGES_armv7`.
- Build in the builder VM from the local tarball (`tools/mktarball.sh`)
  until `lv2/` is published.
- The `engine_config.json` entry (§2.13).

## 8. Steps

1. Copy the engine (`engine/Synth_LXR`, `ORIGIN.md`); generator and TTL
   (stock engines), `ports.h`; lint clean.
2. Plugin at 44.1 kHz: MIDI notes, control ports, render; render test bit
   exact.
3. Resampler; 48 kHz tests; block size tests.
4. README; commit (parent repo).
5. Local tarball, APKBUILD, armv7 build in the builder VM (coordinated);
   commit on the nura-builds branch.
6. Phone: install, benchmark, Zynthian pages and widgets (coordinated with
   device-testing and the user).
7. `lv2/` published (§2.12): final recipe,
   engine_config entry, push with the user's OK.

## 9. Implementation

By an agent, steps 1-5 (desktop and the VM build); steps 6-7 with the user
and the device-testing session. Rules: CLAUDE.md (commit per milestone, no
pushes); nothing changes in `MicroDexed-touch/`: engine changes go into the
copy (`LV2:` marks, listed in `ORIGIN.md`), as separate commits, and are
reported. Everything under `lv2/` builds without anything outside it.

## 10. Later

- Presets: MDT's synth kit banks and LXR `.snd` kits as LV2 presets (a
  generator that loads each kit and writes `pset:Preset` values per symbol).
- Engine selection (decision 4), morph (second kit as LV2 state), LO / HI
  outputs, note map control, chromatic mode, LXR CC/NRPN option.
- A zyngui widget (pads with trigger counts, `lxr_engine_status()`).
- The other MDT synths (Dexed instances, MicroSynth, Braids, ePiano,
  MultiSample, Trio / Analog, granular) the same way, copied into `lv2/`:
  their engines need separating from the Teensy Audio objects in the copy,
  one plan each.

## 11. Implementation notes (steps 1-4, 2026-11-22)

Where the implementation differs from or settles the plan:

- File names follow the plugin's name: `src/edrumsynth.c` (not
  `src/lxrdrums.c`), `edrumsynth.lv2/edrumsynth.ttl`.
- `lv2:displayPriority` is written in the lv2core namespace, as Zynthian
  reads it (`world.ns.lv2.displayPriority`); the spec's property is
  `port-props:displayPriority`. The validators get it declared by
  `../common/zynthian-schema.ttl` (not installed): the documented waiver.
- Port symbols and names come from a naming table in `gen_ttl.c` (by group,
  the engine's category and long name), not from the engine's 8-character
  display names ("Frequncy", "Resnance"); unknown parameters stop the
  generator. Duplicates in a group are numbered (`cy_modosc_wave1/2`).
- Group names: "<Voice> Mix / Osc / Amp EG / Filter / Transient / FM or
  Mod Osc / Mod / LFO" (sub page 7 is "Mix", sub page 2 "Mod"); the snare
  has no FM group (its sub page 3 is empty). Groups also carry
  `param:EnvelopeControls` / `FilterControls` / `OscillatorControls`.
- Non-widget number ports are `lv2:integer` (integer steps in Zynthian and
  for MIDI learn); widget ports stay plain floats.
- FX bus: all its parameters as static ports (11; the reverb type, a single
  value, is left out); P1-P4 are "FX Param 1-4" with an `rdfs:comment` for
  their meaning per FX type. Master: DJ filter, DJ resonance, limiter.
  `PAR_VOICE_DECIMATION_ALL` (114) is not exposed (it is on no page; it
  stays a modulation target, "All Sample Rate").
- Modulation destination labels: "<voice> <group> <name>" from the ports
  ("BD Osc Coarse"), not the engine's "1 coa".
- The plugin runs the engine in `LXR_SPLIT_KIT` mode: the kit's audio
  outputs (St1, St2, L1/R1, L2/R2, FX) count; both mixes are summed to the
  one stereo output.
- Event timing: triggers start at the next engine block not yet rendered
  (the engine renders when the output needs frames, so at 44.1 kHz that is
  block ceil(frame / 64); with the resampler up to its 24-frame lookahead
  later). Control ports are read at the start of `run()`.
- Resampler: 48 taps per phase, Kaiser beta 9; exact polyphase up to 1024
  phases, else 256 interpolated phases; measured rest (aliasing, images,
  noise) about -100 dB at 1 kHz, gain flat to 0.0001 dB up to 16 kHz.
- The render test runs each render in a forked child (the engine is global;
  a fresh process starts it the same way), so no engine change was needed.
  The engine copy is unchanged (`engine/Synth_LXR/ORIGIN.md`).
- The engine itself calls `fmaf` per sample in its filters, `sqrtf` per
  sample in some pan paths (depending on its config), and `powf` / `tanf` /
  `expf` when parameters change or voices trigger. With `-mfpu=neon-vfpv4`
  GCC inlines `fmaf` (VFPv4 has FMA) and `sqrtf` (VSQRT); on x86_64 without
  `-mfma` `fmaf` is a libm call. To check in the armv7 build's disassembly
  and on the device (steps 5, 6).

## 12. Measurements on the phone (2026-11-23)

Step 5 and 6, armv7 package (mdt-lv2 fc5053e, nura plugins set flags:
`-mfpu=neon-vfpv4 -mtune=cortex-a7 -O3 -funsafe-math-optimizations`), Galaxy
S III Neo (Cortex-A7, 1.19 GHz, performance governor), 48 kHz, 960-frame
periods, measured by the device-testing session:
- lv2bench (no MIDI): 2.27-2.35 µs per sample.
- synthbench idle: 2.39 µs/sample (11.5 % of a core); all 7 voices
  retriggered every 16th / 32nd: 2.44-2.47 µs/sample (11.7-11.9 %). About
  2.3-2.4 ms per 20 ms period. Idle costs as much as playing: the LXR
  engine computes every voice all the time.
- perf (dense pattern, idle nearly the same): `mixer_calcNextSampleBlock`
  32.8 %, `SVF_calcBlockZDF` 23.4 %, the resampler `eds_rs_process` 13.4 %,
  `calcNextOscSampleBlock` 9.7 %, `calcNextOscSampleFmBlock` 7.3 %,
  `mixer_limitBlock` 2.9 %, `lxr_engine_render_split` 2.1 %, memset 1 %,
  `FxBus_process` 0.7 %. libm: only `exp` at 0.05 % (the imported expf,
  powf, tanf, ... run on parameter changes, not per sample).
- No crashes, xruns or denormal slowdowns.
- In the armv7 binary `fmaf` and `sqrtf` are inlined (`vfma`, `vsqrt`;
  `sqrtf` stays imported as GCC's errno fallback).

Possible gains (engine changes in the copy, `LV2:` marks), if needed:
skip voices whose amplitude envelope has finished (the biggest: idle is
most of the time); per-block instead of per-sample work in the mixer (pan,
gain, conversions); the ZDF SVF's coefficients per block; the resampler
with fewer taps (48 → 32). Running the engine at 48 kHz would save the
resampler but changes the sound (decision 3).

## 13. v2: engines per slot, MIDI channel modes, kit presets (planned 2026-11-25)

MDT lets every voice slot run any of the four engines (drum, snare,
cymbal, hi-hat); v1 has the stock engines only (decision 4). And MDT plays
the synth drums chromatically as an instrument besides the kit. v2 adds
both, plus kit presets. Version 0.1.0; port symbols change (§13.4), which
is acceptable now (0.0.x, no presets or users depending on them yet).

### 13.1 Facts

- Switching a slot's engine (`lxr_engine_set_engine()`) keeps the slot's
  parameter numbers; about 6 of its roughly 35 parameters change meaning,
  and the engine resets the slot's parameters to the engine's defaults.
  Slot 1 (`lxr_engine_page_param()` per engine):

  | Parameter | drum | snare | cymbal | hi-hat |
  |---|---|---|---|---|
  | 82 | FM Amount | Noise/Osc Mix | Mod Osc Gain 1 | Mod Osc Gain 1 |
  | 83 | FM Frequency | Noise Frequency | Mod Osc Freq 1 | Mod Osc Freq 1 |
  | 74 | Pitch Mod Amount | Pitch Mod Amount | Mod Osc Freq 2 | Mod Osc Freq 2 |
  | 78 | Mod EG Slope | Mod EG Slope | Mod Osc Gain 2 | Mod Osc Gain 2 |
  | 70 | Mod EG Decay | Mod EG Decay | Osc Waveform 3 | Osc Waveform 3 |
  | 20 | FM Waveform | - | Osc Waveform 2 | Osc Waveform 2 |
  | 134 | Osc Mode | EG Repeat Count | EG Repeat Count | - |

  The other slots use their own parameter numbers the same way (each slot
  has its own set, e.g. slot 2: 84, 85, 75, 79, 71, 21, 135).
  Oscillator (coarse, fine, waveform, pulse width), amp EG (attack, decay,
  slope), velocity (destination, amount, volume mod), transient, filter,
  LFO and mix are the same for every engine.
- LV2 ports are static (names, ranges, groups in the TTL), and a plugin
  can't change its input ports' values (the host owns them).
- LV2 MIDI input carries all 16 channels. Zynthian passes one channel per
  chain by default (the chain's MIDI channel, filtered, not translated);
  with the chain's "ALL MIDI CHANNELS" setting (`zmop_set_midi_chan_all()`,
  "Multitimbral or unfiltered control of the engine") all channels pass
  unchanged. The plugin can't see which.
- `lxr_engine_trigger(track, velocity, note)` plays a track at a pitch
  (`LXR_ENGINE_NOTE` 60 = its tuned pitch); MDT plays a voice chromatically
  as instrument 10.
- MDT's synth kits: LXR `.snd` files (8-byte name, 247 parameters,
  OpenElixir-02's engine block if any), banks as folders under
  `/SYNTHKITS` (`port/tools/synthkits.py`); not in git (their licences), so
  none are shipped.

### 13.2 Engines per slot

- One engine selector port per slot (enumeration Drum / Snare / Cymbal /
  Hi-hat, default the stock engine), in the slot's Mix group or its own
  "Engine" group at the top of the voice.
- Shared parameters: ports as in v1.
- Engine-specific parameters: one group per slot and engine family, the
  plugin sends only the active engine's group to the engine:
  - "<Voice> Mod EG" (drum and snare engines: Mod EG decay, slope, pitch
    mod amount);
  - "<Voice> FM" (drum: FM amount, frequency, waveform, osc mode);
  - "<Voice> Noise" (snare: noise frequency, noise/osc mix, EG repeat);
  - "<Voice> Mod Osc" (cymbal and hi-hat: freq 1 / 2, gain 1 / 2,
    waveforms 2 / 3, EG repeat for the cymbal).
  Velocity (destination, amount, volume mod) becomes its own group (it
  was in "Mod" with the mod EG).
  Group names say which engines they apply to where it isn't obvious
  ("BD Noise (snare engine)").
- Engine change in `run()`: `lxr_engine_set_engine()` (which resets the
  slot in the engine), then all of the slot's port values (shared and the
  active engine's group) are sent again, so the ports win. Engine defaults
  are therefore not loaded by an engine switch; kit presets provide
  sensible values (§13.4).
- Modulation targets (velocity / LFO destination enumerations): the
  engine-specific targets get labels covering the engine families (e.g.
  "BD FM Amt / Noise Mix / Mod Gain 1"), or neutral ones; decide in the
  implementation and document.
- Open hi-hat (track 7, note 46) is the second trigger of slot 6; check
  and document what it does when slot 6 runs another engine (keep it
  working as the LXR does, or silence it).
- Pages grow from 70 to roughly 85-90.

### 13.3 MIDI channel modes

A "MIDI Mode" port (enumeration, in the Master group):
- Kit (default): every note on every channel uses the GM drum map, as v1.
  Right for a Zynthian chain on one channel.
- Multi: channel 10 plays the GM kit; channels 1-7 play tracks 1-7
  chromatically (note 60 = the tuned pitch, velocity as usual, note-offs
  ignored as on the kit). For a chain set to all MIDI channels. Other
  channels are ignored.

### 13.4 Kit presets

- Factory presets shipped in the bundle: only ones made here (no third
  party kits): the init kit ("Init"), and one or two kits showing other
  engine layouts (e.g. all-drum, two snares), with sensible engine-specific
  values.
- `tools/snd2lv2.c` (or `.py`): converts LXR / MDT `.snd` kits (a folder
  or a whole `/SYNTHKITS` tree, one bank per folder) into an LV2 presets
  bundle (`pset:Preset` with the port values and the engine selectors,
  `pset:Bank` per folder, `lv2:appliesTo` the plugin URI) that the user
  installs (e.g. `~/.lv2/` or Zynthian's presets directory). It decodes the
  engine block with `lxr_engine_decode_engines()` and maps parameters to
  the right engine group's ports. Test: a kit converted and loaded renders
  the same as the engine loaded with the kit directly.
- Port symbols: shared parameters keep their v1 symbols where possible; the
  engine-specific ones get new symbols per engine group; the frozen symbol
  list is updated once and frozen again for 0.1.0.

### 13.5 Tests

On top of v1's: engine switching per slot (each engine on each slot renders
like the engine set up directly with that engine and the same values); the
inactive engine groups have no effect; Multi mode (channel 10 = kit, 1-7 =
tracks at pitch: a note 12 above sounds an octave higher), Kit mode ignores
channels; the converter round trip; lint, regenerate-and-diff, frozen
symbols, block-size independence as before. Then the armv7 build and the
phone check as in steps 5-6 (coordinated with the device-testing session).

## 14. Implementation notes for v2 (0.1.0, 2026-11-25)

Where the implementation settles or differs from §13:

- **Ports derived from the engine**: `gen_ttl` sets every engine on every
  slot (`lxr_engine_set_engine()`) and reads the slot's pages
  (`lxr_engine_page_param()`, slot 6 with page 7). A parameter with the same
  name, kind, range and place for all four engines is shared (one port per
  voice, as in v1); the others are the engine specific ones (exactly the
  engine's `slot_rolePar`, 7 per slot) and get a port per engine group and
  name; the generator stops unless every parameter of every engine on every
  slot has exactly one port sent for that engine. §13.1's table holds for
  all slots, each with its own parameter numbers (`gen_ttl --engines`):

  | Slot | role 0 | 1 | 2 | 3 | 4 | 5 | 6 |
  |---|---|---|---|---|---|---|---|
  | 1 BD | 20 | 70 | 74 | 78 | 82 | 83 | 134 |
  | 2 T1 | 21 | 71 | 75 | 79 | 84 | 85 | 135 |
  | 3 T2 | 22 | 72 | 76 | 80 | 86 | 87 | 136 |
  | 4 SD | 223 | 73 | 77 | 81 | 28 | 27 | 68 |
  | 5 CY | 23 | 24 | 30 | 32 | 31 | 29 | 69 |
  | 6 HH | 25 | 26 | 34 | 36 | 35 | 33 | 224 |
  | drum | FM wave | mod EG decay | pitch mod | mod EG slope | FM amount | FM freq | FM mode |
  | snare | - | mod EG decay | pitch mod | mod EG slope | noise mix | noise freq | repeat |
  | cymbal | mod wave 1 | mod wave 2 | mod freq 2 | mod gain 2 | mod gain 1 | mod freq 1 | repeat |
  | hi-hat | mod wave 1 | mod wave 2 | mod freq 2 | mod gain 2 | mod gain 1 | mod freq 1 | - |

  (223, 224: "unused" parameters of the stock firmware, used by OpenElixir-02
  for the snare and hi-hat slots' roles 0 and 6.) Kinds differ per engine
  (FM freq and mod osc freqs signed around 60, noise freq 0..127; waveforms
  0..5; FM mode a menu, repeat 0..127), each port has its engine's.
- **Groups**: per voice Mix (the engine selector first), Osc, Amp EG,
  Filter, Transient, Mod EG (Drum/Snare), FM (Drum), Noise (Snare), Mod Osc
  (Cym/HH), Velocity, LFO; the engine groups in this fixed order on every
  voice, their names carrying the engines. The repeat (role 6) is the snare
  engine's in Noise and the cymbal engine's in Mod Osc: two ports for one
  parameter, each sent only for its engine. The snare's noise controls and
  the snare's and cymbal's repeat moved out of Osc / Amp EG into the engine
  groups. 305 controls, 68 groups, 89 pages (v1: 235, 49, 70).
- **Engine selector placement**: in the Mix group (no own group: it would
  add 6 one-control pages).
- **Defaults of the engine groups**: the engine's own values: for the
  slot's stock engine the init kit's; for another engine the values
  `lxr_engine_set_engine()` gives (the engine's stock slot in the init kit)
  for the group's first engine (Mod EG on the cymbal / hi-hat slots: the
  drum engine's; Mod Osc on the drum / snare slots: the cymbal's, gains 0).
- **Engine switch in `run()`**: the engine selectors first; on a change
  `lxr_engine_set_engine()`, then the port cache is reloaded from the
  engine (`lxr_engine_get_parameter()` for every port) and the normal
  changed-value pass sends every port of the active engines that differs:
  the slot's own parameters and also the other slots' modulation targets
  the engine turned off (`kitEngines_turnOffRoleModulation()`), targets last.
  Equivalent to resending all of the slot's ports (equal values are already
  in the engine), and cheaper. The test's reference does the same against
  the engine directly. Switching costs one `applyMorph()` (all 247
  parameters resent by the engine): a user action, not per period.
- **Modulation target labels**: an engine specific parameter is labelled
  with what it is in each engine group, in group order, duplicates once:
  "BD FM Amount / Noise Mix / Mod Gain 1", "BD FM Mode / Repeat" (decided
  over neutral "role" names: Zynthian shows the label alone).
- **Track 7 (open hi-hat) with another engine on slot 6**: kept working as
  the engine does: `slot_trigger()` retriggers slot 6's voice with that
  engine's trigger and sets its amp decay to decay B (`hh_ampeg_decay_open`,
  shared by all engines); only the hi-hat engine also gets `isOpen`. So with
  any engine notes 42 and 46 are one voice with two decays, choking each
  other. Covered by the engine tests (slot 6 plays tracks 6 and 7).
- **MIDI Mode** in the Master group; Multi: channels 1-7 -> tracks 1-7 with
  the note as pitch (`lxr_engine_trigger()`'s note), channel 10 the GM map,
  others ignored.
- **Added**: `master_sample_rate` (parameter 114, the LXR's overall
  "sample rate" of the performance page): in kits, so kit presets carry it
  (v1 left it out). Mod target label "All Sample Rate" as before.
- **Kit presets** set the kit ports: the engine selectors, every parameter
  port below 247 (inactive engine groups at their defaults), not the
  globals (DJ filter, reso, limiter, MIDI mode). Factory presets Init, Two
  Snares (tom 2 as a clap with the snare engine), All Drums (every slot the
  drum engine), built by `gen_ttl` from the engine (init kit, engines
  switched, settings) and checked by the `presets` test. Not listened to:
  the values are chosen from the parameters' meaning; to be checked by ear.
- **Port map**: `tools/edrumsynth_ports.json`, written by `gen_ttl`,
  compared by `ttl_regenerate` (requested for the LXR project converter,
  `CONVERTER_PLAN.md`): per port symbol, index, parameter (null for the
  selectors and MIDI mode), slot, engines, port range, offset (engine value
  = port value + offset), default, kind, target, kit, the init kit's value;
  the slots with their engine selector symbols; the MIDI mode port; the
  unmapped kit parameters (0, 5, 97, 98, 127, 221, 222, 225-227, 244, 246:
  unused).
- **Converter in Python** (`tools/snd2lv2.py`, not C): no build needed by
  users, and it shares the port map with the project converter. It doesn't
  run the engine; it reproduces `lxr_engine_load_kit_engines()`: the engine
  block decoded as `kitEngines_decode()`, values out of a port's range keep
  the init kit's (the engine ignores them), missing parameters of short
  (older LXR) kits are 0. One preset file per kit (Zynthian loads presets
  one by one), banks in the manifest (`lv2:appliesTo`, as Zynthian finds
  them), bank labels the folder names (with the folder above where they
  repeat). The round trip test is bit exact.
- **Symbols** (0.1.0, `tests/symbols.txt` frozen again): renamed
  `<bd,t1,t2,sd>_mod_eg_decay/_mod_eg_slope/_mod_pitch_amount` ->
  `_modeg_decay/_modeg_slope/_modeg_pitch`, `sd_osc_noise_freq/_mix` ->
  `sd_noise_freq/_mix`, `sd_ampeg_repeat` -> `sd_noise_repeat`,
  `cy_ampeg_repeat` -> `cy_modosc_repeat` (16); kept all others, among them
  the velocity ports `<voice>_mod_vel_*` (now in the Velocity group) and
  the drums' `_fm_*` and cymbal / hi-hat `_modosc_*` (the new scheme's
  names); 84 new.
- **Engine copy unchanged** (`engine/Synth_LXR/ORIGIN.md`).
- **CPU** (desktop, x86_64, the render test's kit scenario, 5 runs each):
  v1 216.7 ns/sample at 44.1 kHz (block 256), 228.4 at 48 kHz (block 960);
  v2 219.0 and 229.0 (+1 % / +0.3 %, about the noise): the extra work is
  the engine mask check per control per `run()`.
- **Test host**: `common/testhost` got `th_control_symbol()`.
- Open: the armv7 package and the phone check (steps 5-6, coordinated with
  the device-testing session), the nura recipe's bump to 0.1.0 (the draft
  `packaging/APKBUILD` here says 0.1.0), listening to the factory presets.
  A v1 quirk kept: the FX bus' modulation targets are labelled with the
  bass drum ("BD FX Param 1"), as the engine's target table groups them.

## 15. v2.1 (0.2.0, 2026-11-26): the LXR CC switch

`master_lxr_cc` (Master group, a toggle, off by default): CCs and NRPNs on
any channel to `lxr_engine_midi_cc()` (CC n = parameter n-1, NRPN n =
128+n), except the engine selection NRPN 98 (the selector ports own the
engines); the ports keep their values. Off by default because a Zynthian
chain passes pedal (64, 66, 67, 69), keyboard (7, 10, 11) and panic
(120-123) CCs that would change drum parameters unseen. For the LXR
project converter's parameter locks (`CONVERTER_PLAN.md`, its §8 has the
implementation notes). Port 308 after the others, symbol frozen; tests in
the render test (bit exact against the engine driven directly).

