/*
 * synth_lxr.h - the LXR V2 drum voices (lxr_engine.h) as a Teensy Audio
 * object: no inputs, stereo out (0 left, 1 right). One object per program.
 * The methods change the voices under AudioNoInterrupts(), as MDT's other
 * engines.
 *
 * Copyright (C) MicroDexed-touch contributors
 * SPDX-License-Identifier: GPL-3.0-only (see LICENSE and NOTICE.md)
 */
#ifndef synth_lxr_h_
#define synth_lxr_h_

#include <Arduino.h>
#include <AudioStream.h>
#include "lxr_engine.h"

// outputs: 0, 1 the voices (left, right); 2, 3 the LO mix; 4, 5 the HI mix
class AudioSynthLXR : public AudioStream
{
public:
	AudioSynthLXR (void);

	// track LXR_TRACK_*, velocity 1..127, note LXR_ENGINE_NOTE for the tuned pitch
	void noteOn (uint8_t track, uint8_t velocity, uint8_t note = LXR_ENGINE_NOTE);

	// the LXR's sound parameters (PAR_*), values 0..127
	void setParameter (uint16_t param, uint8_t value);
	uint8_t getParameter (uint16_t param);
	// a kit: its parameters and the engine of each slot (NULL: the stock ones)
	void loadKit (const uint8_t *params, uint16_t count, const uint8_t *engines = NULL);
	void loadInitKit (void);
	void loadMorphKit (const uint8_t *params, uint16_t count, const uint8_t *engines = NULL);	// NULL: none

	// the engine of a slot (lxr_engine_set_engine(): resets the slot's sound)
	void setEngine (uint8_t slot, uint8_t engine);
	uint8_t getEngine (uint8_t slot);

	// the tempo the LFOs synchronize to
	void setTempo (uint16_t bpm);

	// a MIDI control change for the sound parameters (lxr_engine_midi_cc())
	void midiCC (uint8_t cc, uint8_t value);

	// the voices' mixes: outputs 0, 1 both, 2, 3 LO, 4, 5 HI (LXR_SPLIT_*)
	void setSplit (uint8_t mode, uint8_t fx);

	virtual void update (void);
};

#endif
