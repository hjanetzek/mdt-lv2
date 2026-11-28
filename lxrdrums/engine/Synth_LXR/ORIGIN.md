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

`src/lxr_sequencer.c` (new, `src/lxr_sequencer.h`) compiles the four
sources as one unit, as `lxr_engine.c` does the engine's, and holds what
they use from the rest of the firmware (the front panel's selection, the
menu's and LEDs' state, MIDI out, the UART, the trigger outputs, the
project's kits: `lxr/shim/lxr_glue.h`, `ledHandler.h`, `buttonHandler.h`),
the clock (`seq_tick()`'s internal clock per engine block, counted in ticks
instead of the 4 kHz system tick), locating, the project (patterns, kits,
songs, globals: the LV2 state's chunk) and the front panel's edits (as the
LXR's `CortexParser.c`). Not taken: `MidiVoiceControl.c` (its note on is in
the glue: `slot_trigger()` through `lxr_engine_trigger()`, without MIDI out
and the front panel's LED pulse), `presetManager.c` (the kits in RAM are the
project's; no SD card), `seq_tick()` (compiled, not called), the MIDI clock
(`clockSync.c` compiled, not called: the host's transport instead).

## Changes in this copy

Changes for the LV2 plugin are marked `LV2:` in the code and listed here, so
they can be carried back to MicroDexed-touch by hand.

- `lxr/Soundengine/Sequencer/sequencer.h`: `seq_patternSet` and
  `seq_songData` are macros for `(*seq_patternSetPtr)` and
  `(*seq_songDataPtr)`: the pattern set and the songs are the project's
  (`lxr_sequencer.c`), swapped as a whole when the LV2 state is restored. The
  declarations in the header and the definitions in `sequencer.c` (marked)
  are unchanged text that now declares and defines the pointers.
- `sequencer.c` `seq_setPendingPatternActive()`: a pattern switch does not
  copy the backup buffer back into the pattern left (the LXR discards
  unsaved edits there; the plugin has no pattern save, the project keeps
  the edits).
- `src/lxr_engine.c`: the sequencer is the one of `lxr/` now: no
  `seq_patternSet` of its own (it was empty), the tempo (`seq_getBpm()`,
  `lxr_engine_set_tempo()` through `seq_setBpm()`) and the track mutes
  (`seq_setMute()`, `lxr_engine_trigger()` asks `seq_isTrackMuted()`) are the
  sequencer's; `lxr_engine_init()` calls `lxr_seq_init()`; new
  `lxr_engine_trigger_count()`.
- The probabilities' random numbers: `lxr_sequencer.c` compiles
  `sequencer.c` with `GetRngValue` defined as its own seeded xorshift32 (a
  macro around the `#include`, the source unchanged), so the noise
  oscillators keep theirs and a render repeats.
