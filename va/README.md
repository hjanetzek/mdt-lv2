# mTrio and mAnalog

MicroDexed-touch's two small virtual analog synths as LV2 instruments, made
for Zynthian on small ARM devices (target: postmarketOS / nura armv7,
Cortex-A7). Plain LV2 (C style C++, the engine's language), no GUI: Zynthian
shows the controls in pages, with its envelope and filter widgets on the
Amp EG and Filter pages; desktop hosts show their generic UIs.

- **mTrio**: three oscillators per voice in 16 wave sets (saws, pulses,
  triangles, noise, hard sync, ring modulation), a state variable filter
  (low pass, band pass, high pass, notch), an ADSR envelope that also sweeps
  the cutoff and the pulse width. URI
  `https://github.com/hjanetzek/mdt-lv2/mtrio`, bundle `mtrio.lv2`, binary
  `mtrio.so`, `doap:name` "mTrio" (Zynthian engine key `JV/mTrio`).
- **mAnalog**: two band limited oscillators (saw, square, triangle, sine,
  PWM) with detune, mix and noise, drive, a low pass state variable filter,
  the same envelope. URI `https://github.com/hjanetzek/mdt-lv2/manalog`,
  bundle `manalog.lv2`, binary `manalog.so`, `doap:name` "mAnalog"
  (`JV/mAnalog`).
- Eight voices each, one MIDI input (atom), one stereo output. The DSP is
  MDT's (Q15 integer oscillators, polyBLEP, the trapezoidal SVF), copied into
  `engine/` (`engine/ORIGIN.md`: the MDT commit and every change); at
  44.1 kHz the output is MDT's, bit exact (see Tests).
- GPL-3.0-only (`engine/LICENSE`).
- One package for both: `mdt-va-lv2` (`packaging/APKBUILD`).
- The plan and the implementation notes: [PLAN.md](PLAN.md).

## MIDI

Any channel. Note on / off (velocity 0 is a note off), pitch bend, CC 120
(all sound off) and CC 123 (all notes off); other messages are ignored
(Zynthian MIDI-learns the controls).

- Voices as MDT allocates them: a note already playing retriggers its voice,
  else the first free idle voice, else the first free voice still
  releasing, else the next voice round robin is stolen (it restarts its
  attack from its current level, no release).
- Pitch bend: all voices, +-Bend Range semitones (default 2).
- Velocity: MDT ignores it; the Velocity control (0..100 %, default 0)
  scales the level ((velocity / 127)^2 at 100 %) and closes the cutoff (up
  to about 2.3 octaves at velocity 0). At velocity 127, or Velocity 0, the
  sound is MDT's.
- Events take effect at the next 128-frame engine block (2.7 ms at 48 kHz,
  as MDT's own block); the timing doesn't depend on the host's block size.

## Ports and Zynthian pages

Port 0 is the MIDI input, 1 and 2 the outputs (`pg:mainOutput`), 3..21 the
19 controls, in five groups of up to four (one Zynthian page each, in this
order):

| Page | mTrio | mAnalog |
|---|---|---|
| Osc | Wave, Interval 2, Interval 3, Detune | Wave, Detune, Osc Mix, Noise |
| Filter | Cutoff, Resonance, Mode | Cutoff, Resonance, Drive, Key Track |
| Amp EG | Attack, Decay, Sustain, Release | Attack, Decay, Sustain, Release |
| Mod | Pulse Width, Env to PW, Env to Cutoff, Velocity | Env to Cutoff, Env to PW, Velocity |
| Master | Level, Pan, Transpose, Bend Range | Level, Pan, Transpose, Bend Range |

`build/gen_ttl trio --pages` / `analog --pages` print it.

- Symbols are `<group>_<name>` (`osc_wave`, `filter_cutoff`,
  `ampeg_attack`, `mod_env_cutoff`, `master_bend_range`). **They are API**
  (Zynthian presets, snapshots, MIDI learn): `tests/symbols_mtrio.txt` and
  `tests/symbols_manalog.txt` freeze them; never rename or remove one.
- Ranges and defaults are MDT's editor ranges (`UI.cpp`) and its state after
  boot (preset 0, level 100, pan centre); Pan is MDT's 0..40 as -20..20
  (negative: left); Interval, Transpose and Bend Range in semitones,
  Detune in cents (mTrio: oscillator 2 up, 3 down; mAnalog: oscillator 2),
  Level and Velocity in percent.
- Widgets: `param:attack/decay/sustain/release` on the Amp EG, and
  `param:cutoffFrequency` / `param:resonance` on the filter: plain float
  ports 0..127. The filter widget always draws a low pass (also for
  mTrio's other modes).
- Enumerations: the waves and mTrio's filter mode, with readable names for
  MDT's short ones:

  | # | mTrio wave (MDT) | | # | mTrio wave (MDT) |
  |---|---|---|---|---|
  | 0 | Saw x3 (SAW3) | | 8 | Noise (NOIS) |
  | 1 | Pulse x3 (PLS3) | | 9 | Sync Saw (SYNC) |
  | 2 | Pulse Pulse Tri (PPT) | | 10 | Sync Pulse (SYNCP) |
  | 3 | Saw Saw Tri (SST) | | 11 | Sync Saw + Tri (SYNC3) |
  | 4 | Tri x3 (TRI3) | | 12 | Ring (RING) |
  | 5 | Saw&Tri (S&T) | | 13 | Ring + Tri (RING3) |
  | 6 | Pulsed Saw (P&S) | | 14 | Ring + Sync Saw (R+S) |
  | 7 | Pulse + Noise (P+N) | | 15 | Ring + Sync Pulse (R+SP) |

  mTrio filter mode: Low Pass, Band Pass, High Pass, Notch (LP, BP, HP,
  NOT). mAnalog wave: Saw, Square, Triangle, Sine, PWM.
- As in MDT: the sustain level is full from 50 up (the editor goes to 127);
  mAnalog's Key Track lowers the cutoff by a fixed amount (MDT doesn't pass
  the note to it) — see PLAN.md §6.
- Not exposed: MDT's reverb and delay sends (Zynthian has its own effects)
  and its MIDI channel (the host routes MIDI).

## Presets

MDT's built-in presets as LV2 presets in a bank "Factory" (`presets.ttl`,
listed in the manifest), with the values MDT's preset loader sets (the
oscillator, filter and envelope parameters; Level, Pan, Transpose, Bend
Range and Velocity stay as they are):

- mTrio (5): Fat Bass, Arp Lead, Sync Lead, Ring Bell, Chip Choir.
- mAnalog (12): Saw Lead, Soft Pad, Square Bass, PWM Strings, Acid,
  Sine Key, Rave, Sub Bass, Pluck, Brass, Wind, Strings.

The port defaults are the first preset (Fat Bass, Saw Lead), as MDT starts.

## Audio

The engine renders 128-frame blocks of eight voices at the host's sample
rate: the rate only enters its tables (the filter coefficients, the pitch
increments) and the envelope times, all computed at instantiate, so there is
no resampler. `run()` does no allocations, locks, file I/O or libm calls;
a parameter is set only when its port's rounded value changed; idle voices
cost nothing. The voices are summed like MDT's mixer, but without its
16-bit saturation: chords at Level 100 may exceed full scale (MDT clips
them), the host's volume follows.

Instances in one process share the engine's tables, so they share one
sample rate (an instance at another rate is refused while one runs; jalv /
Zynthian run one plugin per process anyway).

## Build, test, install

Needs a C and a C++ compiler, CMake (3.16 or newer), Ninja or make and the
LV2 headers (`lv2-dev`). Everything else is in this directory and in
`../common` (the generator's TTL helpers, the render test host and the
validators' schema waiver, shared with the other plugins here).

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
cmake --install build --prefix /usr      # /usr/lib/lv2/mtrio.lv2, manalog.lv2
```

`build/mtrio.lv2/` and `build/manalog.lv2/` are complete bundles
(`LV2_PATH=$PWD/build`). No architecture flags in CMake: CFLAGS / CXXFLAGS
from the build decide.

Tests (`ctest`; the validators are skipped when not installed):

- `ttl_regenerate`: `mtrio.lv2/*.ttl`, `manalog.lv2/*.ttl` and
  `src/ports_*.h` as committed equal the generator's output. They are
  generated by `tools/gen_ttl.cpp` (from MDT's tables in
  `tools/mdt_tables.h` and the engine's) and committed, because cross
  builds can't run it; after a change run `cmake --build build --target
  regenerate`.
- `symbols_frozen_*`: every frozen symbol still exists.
- `sord_validate_*`, `lv2_validate_*`: the bundles against the LV2 specs
  (with `../common/zynthian-schema.ttl`: Zynthian reads
  `lv2:displayPriority` in the lv2core namespace).
- `lv2lint_*`: warnings are errors (notes remain: version 0.x, no comments
  on some ports, no units on enumerations).
- `imports_*`: the plugin imports only an allowed set of libc / libm
  functions (no libstdc++, nothing per sample).
- `render_*` (`tests/render_test.cpp` on `../common/testhost`): at
  44.1 kHz the plugin equals MDT (`tests/mdt_reference.h`: MDT's wrapper,
  allocation and mixer driving the copied engine) bit exact, for host blocks
  of 1 to 4096 frames, for a polyphonic scenario with stealing, every wave and
  every preset (whose TTL values are checked against MDT's loader), and with
  MDT's own saturating mixer at Level 25; at 48 kHz host blocks of 1 to 4096
  give the same signal; at 22.05 to 192 kHz the pitch, the envelope's decay
  and release times and the level through the filter match 44.1 kHz; pitch
  bend, Velocity, CC 120 / 123, instances and rates. It prints the CPU time
  per sample.

The tests don't run jalv or anything that creates a JACK client.

## Packaging

`tools/mktarball.sh [--worktree] [OUTDIR]` writes
`mdt-va-lv2-VERSION.tar.gz` (HEAD, or the working tree; `va/` and
`common/`) and its sha512sum line; `packaging/APKBUILD` is the draft recipe
for nura-builds (`aports/lv2/mdt-va-lv2/`: the GitHub archive of this
repository at a commit, `builddir` `.../va`, `arch="all"`, CMake, ctest in
`check()`).
