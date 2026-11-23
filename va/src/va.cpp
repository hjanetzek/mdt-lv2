/*
 * va.cpp - mTrio and mAnalog, MicroDexed-touch's Trio and Analog synths as
 * LV2 instruments (one binary each: VA_ANALOG 0 or 1 selects the engine
 * and VA_PORTS the generated port table).
 *
 * - MIDI (any channel): notes (8 voices, MDT's allocation: voice.h), pitch
 *   bend (+-Bend Range semitones), CC 120 (all sound off), CC 123 (all
 *   notes off); other messages are ignored (Zynthian MIDI-learns the
 *   controls).
 * - Control ports: one per parameter (ports_<name>.h, generated with the
 *   TTL by tools/gen_ttl.cpp); a parameter is set when its rounded value
 *   changes, at the start of run().
 * - Audio: the engine renders 128-frame blocks at the host rate (its tables
 *   are built for it at instantiate), when the output needs them; events
 *   take effect at the next block (2.7 ms at 48 kHz), the same for any host
 *   block size.
 * - The engine's tables are global: instances in one process share one
 *   sample rate (another rate is refused while one runs).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#if VA_ANALOG
#include "analog_engine.h"
#else
#include "trio_engine.h"
#endif
#include "voice.h"
#include VA_PORTS

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <stdlib.h>
#include <string.h>

#if VA_ANALOG
static const va_engine engine = { 1, analog_note_on, analog_render, { 0, 0x40000000u, 0 }, 2.5f };
#else
static const va_engine engine = { 0, trio_note_on, trio_render, { 0, 0x1C000000u, 0x38000000u }, 3.5f };
#endif

typedef struct
{
	const LV2_Atom_Sequence *midiIn;
	float *out[2];
	const float *control[VA_NUM_CONTROLS];
	int value[VA_NUM_CONTROLS];	// the parameter value of each port

	LV2_URID midiEvent;
	LV2_Log_Logger logger;

	va_synth synth;
	float block[2][VA_BLOCK];	// the block being played
	int blockPos;
} Plugin;

// the engine's tables: built for the first instance's rate; instances at
// another rate are refused while one is active (instantiate and cleanup are
// LV2's instantiation class: not called concurrently with anything else in
// the library)
static int instances;
static double tablesRate;

// ---- audio ----

static void renderBlock (Plugin *pl)
{
	int32_t mix[VA_BLOCK];
	va_render (&pl->synth, mix);
	const float scale = 1.f / 32768.f;
	const float gl = pl->synth.gain[0] * scale, gr = pl->synth.gain[1] * scale;
	for (int i = 0; i < VA_BLOCK; i++)
	{
		pl->block[0][i] = (float) mix[i] * gl;
		pl->block[1][i] = (float) mix[i] * gr;
	}
	pl->blockPos = 0;
}

// output frames from..to, rendering blocks as needed
static void renderAudio (Plugin *pl, uint32_t from, uint32_t to)
{
	float *const l = pl->out[0], *const r = pl->out[1];
	uint32_t pos = from;
	while (pos < to)
	{
		if (pl->blockPos == VA_BLOCK)
		{
			renderBlock (pl);
		}
		uint32_t n = (uint32_t) (VA_BLOCK - pl->blockPos);
		if (n > to - pos)
		{
			n = to - pos;
		}
		memcpy (l + pos, pl->block[0] + pl->blockPos, n * sizeof (float));
		memcpy (r + pos, pl->block[1] + pl->blockPos, n * sizeof (float));
		pl->blockPos += (int) n;
		pos += n;
	}
}

static void applyControls (Plugin *pl)
{
	for (int i = 0; i < VA_NUM_CONTROLS; i++)
	{
		if (!pl->control[i])
		{
			continue;
		}
		const float v = *pl->control[i];
		if (!(v == v))
		{
			continue;	// NaN
		}
		const va_control *c = &va_controls[i];
		const float lo = (float) (c->min - c->offset), hi = (float) (c->max - c->offset);
		const float f = v < lo ? lo : v > hi ? hi : v;
		const int x = (int) (f >= 0.f ? f + 0.5f : f - 0.5f) + c->offset;
		if (x != pl->value[i])
		{
			pl->value[i] = x;
			va_set (&pl->synth, c->param, x);
		}
	}
}

static void handleMidi (Plugin *pl, const uint8_t *msg, uint32_t size)
{
	if (size < 3)
	{
		return;
	}
	switch (lv2_midi_message_type (msg))
	{
	case LV2_MIDI_MSG_NOTE_ON:
		if (msg[2])
			va_noteOn (&pl->synth, msg[1] & 127, msg[2] & 127);
		else
			va_noteOff (&pl->synth, msg[1] & 127);	// velocity 0: a note off
		break;
	case LV2_MIDI_MSG_NOTE_OFF:
		va_noteOff (&pl->synth, msg[1] & 127);
		break;
	case LV2_MIDI_MSG_BENDER:
		va_pitchBend (&pl->synth, (((int) (msg[2] & 127) << 7) | (msg[1] & 127)) - 8192);
		break;
	case LV2_MIDI_MSG_CONTROLLER:
		if (msg[1] == LV2_MIDI_CTL_ALL_SOUNDS_OFF)
			va_allSoundOff (&pl->synth);
		else if (msg[1] == LV2_MIDI_CTL_ALL_NOTES_OFF)
			va_allNotesOff (&pl->synth);
		break;
	default:
		break;
	}
}

// ---- LV2 ----

static LV2_Handle instantiate (const LV2_Descriptor *descriptor, double rate, const char *bundlePath,
			       const LV2_Feature *const *features)
{
	(void) descriptor;
	(void) bundlePath;

	LV2_URID_Map *map = NULL;
	LV2_Log_Log *log = NULL;
	for (int i = 0; features && features[i]; i++)
	{
		if (!strcmp (features[i]->URI, LV2_URID__map))
			map = (LV2_URID_Map *) features[i]->data;
		else if (!strcmp (features[i]->URI, LV2_LOG__log))
			log = (LV2_Log_Log *) features[i]->data;
	}
	LV2_Log_Logger logger;
	memset (&logger, 0, sizeof logger);
	lv2_log_logger_init (&logger, map, log);
	if (!map)
	{
		lv2_log_error (&logger, VA_NAME ": the host has no urid:map\n");
		return NULL;
	}
	if (!(rate >= 8000 && rate <= 384000))
	{
		lv2_log_error (&logger, VA_NAME ": no sample rate %.0f Hz\n", rate);
		return NULL;
	}
	if (instances && rate != tablesRate)
	{
		lv2_log_error (&logger, VA_NAME ": another instance in this process runs at %.0f Hz, not %.0f Hz\n",
			       tablesRate, rate);
		return NULL;
	}

	Plugin *pl = (Plugin *) calloc (1, sizeof *pl);
	if (!pl)
	{
		return NULL;
	}
	if (!instances)
	{
		dsp_basics_init (rate);
		tablesRate = rate;
	}
	instances++;

	pl->logger = logger;
	pl->midiEvent = map->map (map->handle, LV2_MIDI__MidiEvent);
	va_init (&pl->synth, &engine, rate);
	for (int i = 0; i < VA_NUM_CONTROLS; i++)
	{
		const va_control *c = &va_controls[i];
		pl->value[i] = (int) c->def + c->offset;
		va_set (&pl->synth, c->param, pl->value[i]);
	}
	pl->blockPos = VA_BLOCK;
	return pl;
}

static void connectPort (LV2_Handle instance, uint32_t port, void *data)
{
	Plugin *pl = (Plugin *) instance;
	switch (port)
	{
	case VA_PORT_MIDI_IN:	pl->midiIn = (const LV2_Atom_Sequence *) data;	break;
	case VA_PORT_OUT_L:	pl->out[0] = (float *) data;	break;
	case VA_PORT_OUT_R:	pl->out[1] = (float *) data;	break;
	default:
		if (port >= VA_PORT_CONTROL0 && port < VA_NUM_PORTS)
		{
			pl->control[port - VA_PORT_CONTROL0] = (const float *) data;
		}
		break;
	}
}

// (re)starts silent: no voices, no bend, the next block rendered fresh
static void activate (LV2_Handle instance)
{
	Plugin *pl = (Plugin *) instance;
	va_allSoundOff (&pl->synth);
	va_pitchBend (&pl->synth, 0);
	pl->blockPos = VA_BLOCK;
}

static void run (LV2_Handle instance, uint32_t frames)
{
	Plugin *pl = (Plugin *) instance;
	if (!pl->out[0] || !pl->out[1])
	{
		return;
	}
	applyControls (pl);

	uint32_t pos = 0;
	if (pl->midiIn)
	{
		LV2_ATOM_SEQUENCE_FOREACH (pl->midiIn, ev)
		{
			uint32_t t = ev->time.frames < 0 ? 0 : (uint32_t) ev->time.frames;
			if (t > frames)
			{
				t = frames;
			}
			if (t > pos)
			{
				renderAudio (pl, pos, t);
				pos = t;
			}
			if (ev->body.type == pl->midiEvent)
			{
				handleMidi (pl, (const uint8_t *) (ev + 1), ev->body.size);
			}
		}
	}
	renderAudio (pl, pos, frames);
}

static void deactivate (LV2_Handle instance)
{
	(void) instance;
}

static void cleanup (LV2_Handle instance)
{
	free (instance);
	instances--;
}

static const void *extensionData (const char *uri)
{
	(void) uri;
	return NULL;
}

static const LV2_Descriptor descriptor = {
	VA_URI,
	instantiate,
	connectPort,
	activate,
	run,
	deactivate,
	cleanup,
	extensionData,
};

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor (uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
