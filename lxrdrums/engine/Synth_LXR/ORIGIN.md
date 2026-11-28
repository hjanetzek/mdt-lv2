# Origin of this copy

`engine/Synth_LXR/` is a copy of MicroDexed-touch's `third-party/Synth_LXR`
(src/, lxr/, LICENSE, NOTICE.md, README.md, library.json), taken unchanged
from:

- repository: MicroDexed-touch (https://codeberg.org/hjanetzek/MicroDexed-touch),
  branch `useq` of the cynthian port (rebased onto upstream 2.0.13)
- commit: `1ed8cce5` (2026-11-26)
- last commits touching `third-party/Synth_LXR`: `f2a9085b` ("Synth_LXR: don't
  send the MIDI state slots of the sound parameters"), `1ed8cce5` ("Synth_LXR:
  reset blown up filter states by their exponent bits (OpenElixir-02 2ef496e)")

The plugin builds only `src/lxr_engine.c` (with `-std=gnu17 -I lxr -I lxr/shim`);
`src/synth_lxr.cpp/.h` (the Teensy Audio object) and `library.json` are kept
for comparison with MDT and are not built.

## The sequencer (not in MicroDexed-touch)

For eDrumSynth 0.3.0 the LXR's own step sequencer was added from
OpenElixir-02 (https://github.com/tunefish777/openelixir-02, commit `383b012`,
version 1.2, the base of the engine sources), unchanged at first:

- `Core/Src/Soundengine/Sequencer/sequencer.c`, `clockSync.c`,
  `EuklidGenerator.c` and `Core/Inc/Soundengine/Sequencer/sequencer.h`,
  `clockSync.h`, `EuklidGenerator.h` -> `lxr/Soundengine/Sequencer/`
- `Core/Inc/Preset/SeqStep.h` -> `lxr/Preset/SeqStep.h`
- `Core/Src/Soundengine/DSPAudio/automationNode.c` ->
  `lxr/Soundengine/DSPAudio/` (the step automation: parameter locks)

The shim's stand-ins `lxr/shim/Soundengine/Sequencer/sequencer.h` and
`clockSync.h` are gone; `lxr/shim/lxr_glue.h` includes the real header.

## Changes in this copy

Changes for the LV2 plugin are marked `LV2:` in the code and listed here, so
they can be carried back to MicroDexed-touch by hand.

- none yet
