/*
 * lxr_glue.h - what the LXR voices and the LXR's sequencer use from the
 * rest of the LXR firmware (MIDI parser, front panel, menu, LEDs, presets,
 * faders), declared for the engine (src/lxr_engine.c) and the sequencer
 * glue (src/lxr_sequencer.c), which define it. The firmware headers they
 * include (shim/) all include this one. Not part of the LXR firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_GLUE_H_
#define LXR_GLUE_H_

#include "stm32f7xx_hal.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Parameters.h"

// sequencer.h, SeqStep.h: the LXR's own (lxr/Soundengine/Sequencer/)
#include "Soundengine/Sequencer/sequencer.h"

// PresetManager.h: the autosave of the LXR's kit and pattern (none); the
// project's kits (src/lxr_sequencer.c: the LV2 state's project)
typedef enum { PRESET_KIT_TIMER, PRESET_PAT_TIMER, PRESET_STOP_PAT_TIMER } PresetTimer;
void preset_touchTimer (PresetTimer type);
#define SKIP_NEXT_KIT_AUTOLOAD 2
void preset_loadDrumsetRam (uint8_t presetNr, uint8_t isMorph, uint8_t sendImmediate);

// MidiParser.h: the parameter setter (CC/NRPN values to voice parameters)
void midiParser_ccHandler (MidiMsg msg, uint8_t updateOriginalValue, uint8_t isFloatModulator, float v);
float midiParser_calcDetune (uint8_t value);

// frontPanelParser_ui.h (menu.h is lxr/menu/menu.h: the LXR's): the front
// panel's messages to the sound engine (lxr_engine.c routes them as the
// LXR's CortexParser.c)
#ifndef CC_2
#define CC_2 0xd6		// for parameters above 127
#endif
#define CC_LFO_TARGET 0xd7
#define CC_VELO_TARGET 0xd8
void frontPanel_sendData (uint8_t status, uint8_t data1, uint8_t data2);

// main.h: the output jack switches (mixer.c routes voices to plugged outputs):
// all plugged
typedef struct { uint32_t IDR; } GPIO_TypeDef;
extern GPIO_TypeDef lxr_gpio_plugged;
#define switch_l1_GPIO_Port (&lxr_gpio_plugged)
#define switch_r1_GPIO_Port (&lxr_gpio_plugged)
#define switch_l2_GPIO_Port (&lxr_gpio_plugged)
#define switch_r2_GPIO_Port (&lxr_gpio_plugged)
#define switch_l1_Pin 1
#define switch_r1_Pin 2
#define switch_l2_Pin 4
#define switch_r2_Pin 8
extern uint8_t activeParameters[NUM_PARAMS];	// the kit's parameter values (0..127)

// IO/adcPots.h: the channel faders (the LXR's front panel)
float adc_getFaderVolume (uint8_t idx);

// ---- what the sequencer uses (src/lxr_sequencer.c defines it) ----

// MidiParser.h: the parameter setter's values the step automation restores,
// the voices' MIDI channels, the MIDI filters (MIDI out: none here), MTC
extern uint8_t midiParser_originalCcValues[0xff];
extern uint8_t midi_MidiChannels[8];
extern uint8_t midiParser_txRxFilter;
enum { MIDI_FILTER_NOTES = 0x01, MIDI_FILTER_SYSTEM = 0x02, MIDI_FILTER_CC = 0x04, MIDI_FILTER_PROG_C = 0x08 };
void midiParser_checkMtc (void);

// MidiVoiceControl.h: the sequencer's triggers (the voices) and note offs
// (MIDI out only)
void voiceControl_noteOn (uint8_t voice, uint8_t note, uint8_t vel);
void voiceControl_noteOff (uint8_t voice);

// Uart.h, usb_manager.h: MIDI out and the front panel's UART (none here)
extern uint8_t midi_process_block;
void uart_sendMidi (MidiMsg msg);
void usb_sendMidi (MidiMsg msg);
void uart_sendFrontpanelByte (uint8_t data);
void uart_sendFrontpanelSysExByte (uint8_t data);

// CortexParser.h: what the front panel selects (the edited pattern, track
// and step)
extern uint8_t frontParser_shownPattern;
extern uint8_t frontParser_activeTrack;
extern uint8_t frontParser_activeStep;

// TriggerOut.h: the trigger I/O expansion (none here)
extern uint8_t trigger_clockReceived;
void trigger_reset (uint8_t value);
void trigger_clockTick (uint8_t tickType, uint8_t prescaleCounter);

#endif
