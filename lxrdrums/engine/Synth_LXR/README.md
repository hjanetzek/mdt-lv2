# Synth_LXR

The drum voices of OpenElixir-02 (custom firmware for the Erica Synths LXR-02, based on the open-source LXR V2 firmware) as a Teensy Audio object, for MicroDexed-touch's synthesized drums (`LXR_DRUMS`).

- 6 voice slots on 7 tracks. Each slot runs one of 4 engines: drum, snare, cymbal or hi-hat (stock: slots 1-3 drum, 4 snare, 5 cymbal, 6 hi-hat). Track 7 is the second trigger of slot 6 with its own decay (the open hi-hat); tracks 6 and 7 cut each other off.
- Kits are the LXR's sound parameters (`lxr/Parameters.h`, `PAR_*`, values 0..127) and the engine of each slot, set as the LXR's front panel does: the engines first, then the parameters through the LXR's parameter setter. The default is the LXR's init kit with the stock engines. The velocity and LFO modulation targets are indices into the LXR's `modTargets` table, as in its kit files. LXR kit files (`.snd`: 8 byte name, 247 parameters, OpenElixir-02's engine block) load and save both ways.
- MIDI control as the LXR: CC n sets parameter n-1, NRPN n (CC 99, 98, data entry CC 6) parameter 128+n; NRPN 98 sets an engine (value = slot × 4 + engine).
- Parameter info from the LXR's menu tables, following the slots' engines: names (long, short, category), ranges, value text, and the voice pages (7 pages: the 6 slots and track 7, 8 sub pages of up to 8 parameters).
- The LXR's mixer: the voices summed in 32 bit with a limiter per output, the DJ filter (off); its two outputs summed to one stereo output, through a DC blocker (about 10 Hz: the LXR's outputs are AC coupled; the tube and fold distortion have DC). 64 frames per engine block, 44.1 kHz, 16 bit.
- Two mixes (LO, HI: the LXR's two DACs; `lxr_engine_render_split()`, the audio object's outputs 2-5; outputs 0, 1 both): which voices play in which is MDT's (`lxr_engine_set_split()`): by engine (drum LO, snare, cymbal, hi-hat HI), the kit's audio outputs ("St1" LO, "St2" HI), or all LO; voices routed to the FX bus stay there, and the FX bus plays in one of the two.
- The FX bus: the voices whose audio output is the FX bus go through distortion (tube, fold, rat), ring modulator, compressor or delay (normal, ping pong), as on the LXR (the kit's `PAR_FX_*`); `lxr_engine_fx_param()` lists its page for the FX type. OpenElixir-02's reverb is off there too (`USE_REVERB 0`).
- Kit morph as the LXR's (`preset_morph_tick()`, applied at once): a second kit (`loadMorphKit()`) and the amount (`PAR_MORPH`, 0..255, a global parameter; NRPN 120 0..127); the voices get the values in between, the kit keeps its own. Not morphed (as on the LXR): modulation targets, the LFOs' voices, audio outputs, and the engine specific parameters of slots whose engine differs in the morph kit.

## Licence

GPL-3.0-only with the additional permission and terms of the LXR firmware: [LICENSE](LICENSE), [NOTICE.md](NOTICE.md). "Erica Synths", "LXR" and "LXR-02" are trademarks of Erica Synths; this is not official Erica Synths firmware and not affiliated with or endorsed by Erica Synths.

## Files

- `src/synth_lxr.h/.cpp`: `AudioSynthLXR`, the Teensy Audio object (outputs 0 left, 1 right). `noteOn (track, velocity, note)`, `setParameter()`, `loadKit()`, `loadInitKit()`, `setEngine()`, `midiCC()`. Parameter info, engines and kit access: `lxr_engine.h`.
- `src/lxr_engine.h/.c`: the C engine. `lxr_engine.c` compiles the sources as one unit (with `-I lxr -I lxr/shim`: `library.json`, and the MDT port's build files); a block is the LXR's `mixer_calcNextSampleBlock()`.
- `lxr/`: the firmware's sound engine sources, from `Core/Inc` and `Core/Src` (headers next to their sources):
  - `Soundengine/DSPAudio/`: the voices, voice slots (`VoiceSlot.c`), oscillators, filters, envelopes, LFOs, modulation, tables, the mixer and the DJ filter.
  - `Soundengine/FxBus.c`, `Soundengine/DSPAudio/FX/`: the FX bus and its effects (the reverb's headers only: `FxBus.h` includes them).
  - `Soundengine/MIDI/ParameterArray.c`: the parameter table.
  - `Soundengine/MIDI/MidiParserCC.c`: `midiParser_ccHandler()` and what it uses, from `MidiParser.c` (the parameter setter; MIDI parsing, routing and MTC are not taken).
  - `Preset/initKit.c`: the init kit. `Preset/KitEngines.c`: the engines of the kit (assignment, the kit file block).
  - `menu/`: the menu headers (`menu.h`, `MenuText.h`, `menuPages.h`, `CcNr2Text.h`) and `Cc2Text.c` (the modulation targets); `MenuData.c`: the parameter names and data types, the voice pages of non-stock engines and the menu item names, from `menu.c` (the display, encoder, load/save and front panel code is not taken).
- `lxr/Soundengine/Sequencer/` (eDrumSynth, not in MDT): the LXR's step sequencer (`sequencer.c`, `clockSync.c`, `EuklidGenerator.c`), with `Preset/SeqStep.h` and `Soundengine/DSPAudio/automationNode.c` (the parameter locks); `src/lxr_sequencer.c/.h` builds it on the engine (clock per engine block, the project, a front panel).
- `lxr/shim/`: stand-ins for the firmware parts the sources include: the STM32 HAL (integer types, `__SSAT`, `__QADD16`, `__PKHBT`, ...: instructions on ARMv7E-M, C elsewhere; interrupt masking: none, the object works under `AudioNoInterrupts()`), the output jack switches (all plugged), front panel, LED, button and UI headers. `lxr_glue.h` declares what `lxr_engine.c` and `lxr_sequencer.c` provide; the front panel's messages go to the parameter setter as the LXR's `CortexParser.c` routes them.

Not taken: the reverb (off in OpenElixir-02), the MIDI parser, presets (SD card) and the UI.

## Source

OpenElixir-02 (https://github.com/tunefish777/openelixir-02), commit `383b012` (version 1.2), by tunefish.at, a fork of the LXR V2 desktop firmware open sourced by Erica Synths (https://github.com/erica-synths/lxr-desktop-app, commit `a43cd4f`). Synth_LXR started from the Erica Synths sources (MicroDexed-touch commit `0481a525`) and switched to OpenElixir-02 (`da7a235b`: its sources unchanged). Changes to the sources are marked `MDT:` in the code:

- Flash: `menu/menu.h` defines `PROGMEM` (empty in the LXR) as `LXR_FLASH_TABLE`; the menu tables (`MenuText.h`, `menuPages.h`, `Cc2Text.c`, `MenuData.c`) are marked `PROGMEM`: on the Teensy 4 they would take 8 KB of RAM1 otherwise.
- Portability: `Soundengine/config.h`: `REAL_FS` 44100 (the LXR runs at 44140 Hz), no TCM sections (`INDTCM`, `IN_ITCM_RAM` empty), `LXR_FLASH_TABLE` (wavetables and transient samples in flash on the Teensy 4: its const data is in RAM otherwise). `CymbalVoice.h`, `Snare.h`: `"Soundengine/config.h"` instead of `"config.h"` (the front panel's config, unused, and a clash with MDT's `config.h`). `Oscillator.c` `calcNoise()`: the signed overflow of the phase add computed instead of read from the APSR.
- Bug fixes (out of bounds accesses, found by AddressSanitizer on the desktop build; also in OpenElixir-02 1.2):
  - `modulationNode.c` `modNode_setOriginalValueChanged()`: read through the null pointer of parameters without one (parameter 0, "no destination"; address 0 is readable on the STM32).
  - `MidiParserCC.c` `midiParser_ccHandler()`: for CC 0 (parameter 127) wrote `midiParser_originalCcValues[65536]` (`paramNr+1` is an `int`; OpenElixir-02 bounds the NRPN branch, not this one).
  - `Oscillator.c`: the interpolating wavetable reads read one value past the end of a 1024 value table row.
- FX bus: `__fp16` (the FX buffer) is `_Float16` with GCC off ARM (`config.h`); the Teensy builds with `-mfp16-format=ieee` (`library.json`). The FX buffer (64 KB) is in RAM2 on the Teensy 4 (`LXR_FX_RAM`, `FxRamBuffer.c/.h`; cleared by `FxBus_init()`, DMAMEM is not cleared at startup), the FX code in flash but for `FxBus_process()` and the plain delay (`LXR_FX_FLASH`: RAM1 has no room).
- Undefined behaviour (UBSan on the desktop build): `Oscillator.c`: the FM oscillators convert signed products to `uint32_t` (negative: saturated to 0 by the Cortex-M7's VCVT, wrapped on x86): `lxr_f2u()` (the HAL shim) converts as the Cortex-M7 does.
- `lxr/` needs C17 or older (`f()` declarations of functions with parameters): the builds compile `lxr_engine.c` with `-std=gnu17` (GCC 15 defaults to C23).
