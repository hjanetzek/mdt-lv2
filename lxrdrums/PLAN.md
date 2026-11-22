# lxrdrums: MDT's LXR drum engine as an LV2 plugin for Zynthian

Goal: MicroDexed-touch's synthesized drums (the LXR / OpenElixir-02 drum
engine, `MicroDexed-touch/third-party/Synth_LXR`) as an LV2 instrument for
Zynthian on a low-spec 32-bit ARM device: postmarketOS / nura "armv7"
(ARMv7-A, hard float, musl, NEON; test device Samsung Galaxy S III Neo,
Cortex-A7). Zynthian shows its controls in pages and its envelope and filter
widgets for them. This is the first MDT synth as LV2; the others follow the
same pattern (§10).

Status: plan (2026-11-22). Implementation: §9.

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
    `https://codeberg.org/hjanetzek/cynthian/lv2/edrumsynth`, package
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
