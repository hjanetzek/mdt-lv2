/*
 * notemap.h - MIDI notes -> eDrumSynth tracks (lxr_engine.h LXR_TRACK_*):
 * General MIDI drum notes play the tracks at their tuned pitch, others are
 * ignored; in the Multi MIDI mode only on channel 10, and channels 1-7 play
 * tracks 1-7 chromatically (note 60: the tuned pitch).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef EDS_NOTEMAP_H_
#define EDS_NOTEMAP_H_

#include <stdint.h>

#include "lxr_engine.h"

// the track of a note, -1: none
static inline int eds_note_track (uint8_t note)
{
	switch (note)
	{
	case 35: case 36:			return LXR_TRACK_DRUM1;		// acoustic / bass drum 1
	case 45: case 47: case 48: case 50:	return LXR_TRACK_DRUM2;		// low tom, low-mid, hi-mid, high tom
	case 41: case 43:			return LXR_TRACK_DRUM3;		// low floor, high floor tom
	case 37: case 38: case 39: case 40:	return LXR_TRACK_SNARE;		// side stick, snare, clap, electric snare
	case 49: case 51: case 52: case 55:
	case 57: case 59:			return LXR_TRACK_CYMBAL;	// crash 1, ride 1, chinese, splash, crash 2, ride 2
	case 42: case 44:			return LXR_TRACK_HIHAT_CLOSED;	// closed, pedal hi-hat
	case 46:				return LXR_TRACK_HIHAT_OPEN;	// open hi-hat
	default:				return -1;
	}
}

#define EDS_MIDI_MODE_KIT	0	// every channel: the GM drum map
#define EDS_MIDI_MODE_MULTI	1	// channel 10 the drum map, 1-7 the tracks chromatically
#define EDS_GM_CHANNEL		9	// channel 10

// the track a note on a channel (0..15) plays, and its pitch (note for
// lxr_engine_trigger()); -1: none
static inline int eds_note_trigger (uint8_t mode, uint8_t channel, uint8_t note, uint8_t *pitch)
{
	if (mode == EDS_MIDI_MODE_MULTI && channel != EDS_GM_CHANNEL)
	{
		*pitch = note;
		return channel < LXR_ENGINE_TRACKS ? channel : -1;
	}
	*pitch = LXR_ENGINE_NOTE;
	return eds_note_track (note);
}

#endif
