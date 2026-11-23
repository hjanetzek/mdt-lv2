# Origin of va/engine

Copied from MicroDexed-touch (`MicroDexed-touch/` in the MicroDexed-touch
repository, branch `circle-port`) at commit
**ca6518a82dec521f5f7798def5258d8ac9a18ee4** (2026-11-03). The files last
changed there in a1634c5636df1c8280f790ff2dc734411cca6bdf.

| File | From | What |
|---|---|---|
| `synth_shared.h` | `MicroDexed-touch/synth_shared.h` | types shared by Trio and Analog (`track_t`, `voice_t`, `vmod_t`, `preset_t`) |
| `dsp_basics.h`, `dsp_basics.cpp` | same names | Q15 oscillators, polyBLEP, soft clip, the trapezoidal (Simper) SVF, tables |
| `trio_engine.h`, `trio_engine.cpp` | same names | the Trio voice DSP, wave sets, `TRIO_PRESETS` (5) |
| `analog_engine.h`, `analog_engine.cpp` | same names | the Analog voice DSP, `ANALOG_PRESETS` (12) |
| `LICENSE` | `LICENSE-GPL3.txt` (repository root) | GPL-3.0 (the files say `License: GPL-3.0-only`) |

Not copied: the Teensy AudioStream wrappers `synth_trio.cpp/.h` and
`synth_analog.cpp/.h` (the per-voice envelope and the block update), the
settings code `trio.cpp` / `analog.cpp` (mixer gain, panorama, sends), the
editor tables (`UI.cpp`), the voice allocation (`MicroDexed-touch.ino`).
The plugin's own voice layer (`va/src/`) reimplements what it needs from
them; each place says what it follows.

## Changes

Changes for the plugin are marked `LV2:` in the code and listed here, so they
can be carried back to MDT by hand. None changes the sound at 44.1 kHz.

(none yet: the files are as in MicroDexed-touch)
