/*
 * lxr_glue.h - what the LXR voices use from the rest of the LXR firmware
 * (sequencer, MIDI parser, front panel, faders), declared for the MDT
 * engine (src/lxr_engine.c), which defines it. The firmware headers the
 * voices include (shim/) all include this one. Not part of the LXR firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_GLUE_H_
#define LXR_GLUE_H_

#include "stm32f7xx_hal.h"
#include "Soundengine/MIDI/MidiMessages.h"
#include "Parameters.h"

// sequencer.h: MDT plays the voices with its own sequencer, so the LXR's
// patterns (and their step automation, KitEngines.c) are empty
#define SEQ_DEFAULT_NOTE 60
#define NUM_PATTERN 0
#define NUM_TRACKS 7
#define NUM_STEPS 1
typedef struct StepStruct
{
	uint8_t volume, prob, note;
	uint8_t param1Nr, param1Val;
	uint8_t param2Nr, param2Val;
} Step;
typedef struct
{
	Step seq_subStepPattern[NUM_PATTERN][NUM_TRACKS][NUM_STEPS];
} PatternSet;
extern PatternSet seq_patternSet;
static inline uint16_t seq_automationTarget (const uint8_t stored)
{
	if (stored == 0 || stored == 0xff)
		return PAR_NONE;
	return stored < 128 ? (uint16_t) (stored - 1) : stored;
}
uint16_t seq_getBpm (void);
void seq_setMute (uint8_t trackNr, uint8_t isMuted);

// PresetManager.h: the autosave of the LXR's kit (none)
typedef enum { PRESET_KIT_TIMER, PRESET_PAT_TIMER } PresetTimer;
void preset_touchTimer (PresetTimer type);

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

#endif
