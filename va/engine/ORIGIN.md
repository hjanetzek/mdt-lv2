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

1. `dsp_basics.cpp`: no `#include "config.h"` and no `DMAMEM` (a Teensy
   memory section, the only thing it took from MDT's config.h).
2. `dsp_basics.h/.cpp`: the sample rate is a run-time value,
   `dsp_basics_init(double rate)` (MDT: `dsp_basics_init(void)` and 44100
   in two places): the SVF coefficient table `SVF_G` uses
   `tan(M_PI * fc / rate)` and `pitch_inc()` the increment of 440 Hz at that
   rate (`PITCH_BASE`, computed as MDT's integer division at integer rates).
   Below 35.6 kHz the table's top (16 kHz) is limited to 0.45 of the rate
   (tan() has its pole at the Nyquist frequency). The tables are global: one
   rate per process at a time (the plugin enforces it).
   `dsp_basics_init(44100)` builds MDT's tables exactly.
3. `analog_engine.cpp`: the detuned second oscillator gets the fine offset
   of `m->inc` from the note's pitch (`cents_inc(..., det, fine)`, computed
   as `trio_render()` does), so pitch bend between 1/16 semitone steps
   reaches both oscillators; without bend `fine` is 0, as MDT's
   `cents_inc(..., det, 0)`.
4. `trio_engine.cpp`, `analog_engine.cpp`: `y * 2` instead of `y << 1` for
   the filtered sample (a left shift of a negative value is undefined in
   C++ before C++20; UBSan found it in the render test). The same result on
   every target.

Everything else is unchanged; in particular MDT's behaviour where it may
look unintended:
- Analog's key tracking (`KTR`, `P_E7`) uses `v->pitch16`, which MDT's
  wrapper never sets (it stays 0): KTR lowers the cutoff by a fixed
  `KTR * 60 / 256` filter index steps (15 at KTR 64) instead of following
  the key. The presets were
  voiced with that, so it stays (see `va/PLAN.md`).
- The sustain level saturates at 50 (`sus * ENV_MAX / 50` in the wrapper's
  `set_env`, reimplemented in `va/src/voice.h`), the editor goes to 127.
