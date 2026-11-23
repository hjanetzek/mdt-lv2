# eDrumSynth

Synthesized drums as an LV2 instrument: MicroDexed-touch's drum engine (the
voices of the OpenElixir-02 firmware for the LXR-02 drum machine, based on
the open source LXR V2 firmware by Erica Synths), made for Zynthian on small
ARM devices (target: postmarketOS / nura armv7, Cortex-A7). Plain LV2 in C,
no GUI: Zynthian shows the controls in pages, with its envelope and filter
widgets on the amp EG and filter pages; desktop hosts show their generic UIs.

- URI `https://github.com/hjanetzek/mdt-lv2/edrumsynth`, bundle
  `edrumsynth.lv2`, binary `edrumsynth.so`, `doap:name` "eDrumSynth" (the
  Zynthian engine key `JV/eDrumSynth`). GPL-3.0-only (the engine's licence
  and NOTICE: `engine/Synth_LXR/LICENSE`, `NOTICE.md`). "LXR" is an Erica
  Synths trademark; this is not affiliated with Erica Synths.
- 6 voices on 7 tracks: bass drum, tom 1, tom 2, snare, cymbal, hi-hat closed
  and open, the LXR's init kit with the stock engines, the FX bus
  (distortion, ring modulator, compressor, delay), the DJ filter and the
  limiter.
- One MIDI input (atom), one stereo output. One instance per process (the
  engine is global state; jalv / Zynthian run one plugin per process): a
  second instance in the same process is refused.
- The plan and its decisions: [PLAN.md](PLAN.md).

## MIDI: the General MIDI drum map

Note-ons on any channel trigger the tracks at their tuned pitch with the
note's velocity (`src/notemap.h`); note-offs, other notes and CCs are
ignored (Zynthian MIDI-learns the controls instead).

| Notes | Track |
|---|---|
| 35, 36 | bass drum |
| 45, 47, 48, 50 | tom 1 |
| 41, 43 | tom 2 |
| 37, 38, 39, 40 | snare |
| 49, 51, 52, 55, 57, 59 | cymbal |
| 42, 44 | hi-hat closed |
| 46 | hi-hat open (closed and open choke each other) |

Events take effect at the next 64-frame engine block (1.45 ms at 44.1 kHz,
as the LXR); the timing doesn't depend on the host's block size. A
`time:Position` with `time:beatsPerMinute` sets the tempo the LFOs' clock
sync follows (else 120).

## Ports and Zynthian pages

Port 0 is the MIDI input, 1 and 2 the outputs (`pg:mainOutput`), 3..237 the
235 controls: every voice parameter of the engine's menu pages (with the
stock engines), the FX bus and the master parameters. One `pg:group` per
voice sub page; Zynthian orders groups and controls by `lv2:displayPriority`
and shows 4 controls per page ("Bass Drum LFO 1", "Bass Drum LFO 2"):

| Group (per voice) | Controls | Pages |
|---|---|---|
| *Voice* Mix: volume, pan, sample rate, drive, audio out | 5 | 2 |
| *Voice* Osc: coarse, fine, waveform, pulse width (snare: + noise freq, noise mix) | 4 (6) | 1 (2) |
| *Voice* Amp EG: attack, decay, slope (snare, cymbal: + repeat; hi-hat: decay closed, decay open) | 3-4 | 1 |
| *Voice* Filter: cutoff, resonance, type, drive | 4 | 1 |
| *Voice* Transient: waveform, volume, frequency | 3 | 1 |
| *Voice* FM (drums: amount, freq, wave, mode) / Mod Osc (cymbal, hi-hat: freq 1/2, gain 1/2, wave 1/2); none for the snare | 4 / 6 | 1 / 2 |
| *Voice* Mod: mod EG decay and slope, pitch mod (drums, snare); velocity destination, amount, volume mod | 6 (cymbal, hi-hat 3) | 2 (1) |
| *Voice* LFO: frequency, clock sync, amount, waveform, retrigger, offset, destination voice and parameter | 8 | 2 |
| FX Bus: type, dry/wet, audio out, distortion type, FX param 1-4, compressor ratio, ring mod waveform, delay mode | 11 | 3 |
| Master: DJ filter, DJ resonance, limiter | 3 | 1 |

The voices in track order (bass drum, tom 1, tom 2, snare, cymbal, hi-hat),
each 11 pages, then FX Bus and Master: 49 groups, 70 pages. The list:
`build/gen_ttl --pages`.

- Symbols are `<voice>_<group>_<name>` (`bd_osc_coarse`,
  `hh_ampeg_decay_open`, `fx_p1`, `master_limiter`). **They are API**
  (Zynthian presets, snapshots, MIDI learn): `tests/symbols.txt` freezes
  them; never rename or remove one.
- Types: numbers are integer ports in the engine's range; signed values
  (tuning, pan, DJ filter) centred on 0 (coarse -60..67, pan -63..64);
  on/off as `lv2:toggled`; menus (waveforms, filter types, ...) and the
  modulation destinations (229 targets: "BD Osc Coarse", ...) as
  enumerations.
- Widgets: `param:attack` / `param:decay` on each amp EG's attack and decay
  (hi-hat: decay closed), `param:cutoffFrequency` / `param:resonance` on each
  filter: plain float ports 0..127 (Zynthian's widgets need that). The filter
  widget always draws a low pass, also for the other filter types.
- FX param 1-4 mean something else per FX type (distortion: drive, color,
  tone, volume; ring mod: frequency; compressor: attack, decay, threshold,
  gain; delay: time, range, tone, feedback): their `rdfs:comment` says it.
- `lv2:displayPriority` is in the lv2core namespace because Zynthian reads it
  there (the spec's is `port-props:displayPriority`);
  `tests/zynthian-schema.ttl` declares it for the validators.
- Not exposed in v1: the slots' engine selection (stock engines only), kit
  morph, the LO / HI outputs, parameters without a name in the menus.

## Audio

The engine renders 64-frame blocks of 16 bit stereo at a fixed 44.1 kHz
when the output needs them; at other host rates (48 kHz on the device) a
polyphase FIR resampler (`src/resampler.c`: Kaiser windowed sinc, 48 taps
per phase, exact phases for 44.1 -> 48 kHz, interpolated phases for odd
rates) converts them; at 44.1 kHz the output is the engine's, bit exact.
At 48 kHz / 960 frames per period that is 13 or 14 engine blocks per period.
`run()` does no allocations, locks, file I/O or libm calls of its own and
sets an engine parameter only when its port's rounded value changed.

## Build, test, install

Needs a C compiler, CMake (3.16 or newer), Ninja or make and the LV2
headers (`lv2-dev`). Everything else is in this directory (the engine is a
copy: `engine/Synth_LXR/ORIGIN.md`) and in `../common` (the generator's TTL
helpers `ttl.h` and the render test host `testhost/`, shared with the other
plugins of this repository).

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
cmake --install build --prefix /usr      # /usr/lib/lv2/edrumsynth.lv2
```

`build/edrumsynth.lv2/` is a complete bundle (`LV2_PATH=$PWD/build`). No
architecture flags in CMake: CFLAGS from the build decide.

Tests (`ctest`; the validators are skipped when not installed):

- `ttl_regenerate`: `edrumsynth.lv2/*.ttl` and `src/ports.h` as committed
  equal the generator's output. They are generated by `tools/gen_ttl.c`
  (linked with the engine) and committed, because cross builds can't run
  it; after a change to the generator or the engine's tables run
  `cmake --build build --target regenerate`.
- `symbols_frozen`: every symbol in `tests/symbols.txt` still exists.
- `sord_validate`, `lv2_validate`: the bundle against the LV2 specs (with
  `tests/zynthian-schema.ttl`).
- `lv2lint`: warnings are errors (notes remain: no units, no port comments,
  version 0.x).
- `render`: a minimal host (`tests/render_test.c` on
  `../common/testhost`: dlopen + `lv2_descriptor`, urid:map, no lilv). At 44.1 kHz the plugin equals the
  engine driven directly (bit exact) for host blocks of 1 to 4096 frames; at
  48 kHz blocks of 1, 64, 333, 480, 960, 1001 and 4096 frames give the same
  signal, no NaN or clipping; the bass drum's pitch and level at 22.05 to
  192 kHz match 44.1 kHz (1 %, 0.5 dB); a second instance is refused. It
  prints the CPU time per sample.
- `resampler`: sines through the resampler (44.1 -> 48, 47.999, 96, 32
  kHz): gain, phase, and the rest (aliasing, images, noise) below -80 dB.

The tests don't run jalv or anything that creates a JACK client.

## Packaging

`tools/mktarball.sh [--worktree] [OUTDIR]` writes
`edrumsynth-VERSION.tar.gz` (HEAD, or the working tree; `lxrdrums/` and
`common/`) and its sha512sum line; `packaging/APKBUILD` is the draft recipe for nura-builds
(`aports/lv2/edrumsynth-lv2/`, `arch="all"`, CMake, ctest in `check()`).

## Open decisions

- Engine selection per slot (ports with neutral names, or one URI per engine
  layout), kit morph (a second kit as LV2 state), the LO / HI outputs, a note
  map control and chromatic playing, the engine's own CC / NRPN map.
- Presets: MDT's drum kits and LXR `.snd` kits as LV2 presets.
- FX param 1-4 names (one meaning per FX type, static ports).
- Zynthian enabling: an `engine_config.json` default (`"JV/eDrumSynth"`,
  `ENABLED`, `CAT: "Percussion"`).
- Where and when `lv2/` is published (the recipe then downloads its archive).
