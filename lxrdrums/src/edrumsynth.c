/*
 * edrumsynth.c - eDrumSynth, MicroDexed-touch's drum engine
 * (engine/Synth_LXR: the voices of the OpenElixir-02 firmware) as an LV2
 * instrument.
 *
 * - MIDI: General MIDI drum notes (notemap.h) trigger the tracks at their
 *   tuned pitch with the note's velocity; note-offs and CCs are ignored.
 *   time:Position sets the tempo the LFOs synchronize to.
 * - Control ports: one per engine parameter (ports.h, generated with the
 *   TTL by tools/gen_ttl.c); a parameter is set when its rounded value
 *   changes.
 * - Audio: the engine renders 64-frame blocks at 44.1 kHz, rendered when the
 *   output needs them; at other host rates through the resampler.
 * - Events take effect at the next engine block: the 64-frame grid of the
 *   LXR (1.45 ms), the same for any host block size.
 * - One instance per process (the engine has global state).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "lxr_engine.h"
#include "notemap.h"
#include "ports.h"
#include "resampler.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_RATE	44100.0

typedef struct
{
	const LV2_Atom_Sequence *midiIn;
	float *out[2];
	const float *control[EDS_NUM_CONTROLS];
	uint8_t value[EDS_NUM_CONTROLS];	// the engine's value of each port

	struct
	{
		LV2_URID midiEvent, atomBlank, atomObject, atomFloat, atomDouble, atomInt, atomLong;
		LV2_URID timePosition, timeBpm;
	} uris;
	LV2_Log_Logger logger;

	int resample;		// host rate != 44.1 kHz
	eds_rs rs;
	float block[2][LXR_ENGINE_BLOCK];	// the engine block being played (no resampling)
	int blockPos;
} EDrumSynth;

static atomic_flag instanceActive = ATOMIC_FLAG_INIT;

// ---- engine ----

static void renderBlock (EDrumSynth *s)
{
	int16_t l[LXR_ENGINE_BLOCK], r[LXR_ENGINE_BLOCK];
	memset (l, 0, sizeof l);	// lxr_engine_render() adds
	memset (r, 0, sizeof r);
	lxr_engine_render (l, r);
	const float scale = 1.f / 32768.f;
	for (int i = 0; i < LXR_ENGINE_BLOCK; i++)
	{
		s->block[0][i] = (float) l[i] * scale;
		s->block[1][i] = (float) r[i] * scale;
	}
	if (s->resample)
	{
		eds_rs_push (&s->rs, s->block[0], s->block[1], LXR_ENGINE_BLOCK);
	}
	s->blockPos = 0;
}

// output frames from..to, rendering engine blocks as needed
static void renderAudio (EDrumSynth *s, uint32_t from, uint32_t to)
{
	float *const l = s->out[0], *const r = s->out[1];
	uint32_t pos = from;
	while (pos < to)
	{
		if (s->resample)
		{
			pos += (uint32_t) eds_rs_process (&s->rs, l + pos, r + pos, (int) (to - pos));
			if (pos < to)
			{
				renderBlock (s);
			}
		}
		else
		{
			if (s->blockPos == LXR_ENGINE_BLOCK)
			{
				renderBlock (s);
			}
			uint32_t n = (uint32_t) (LXR_ENGINE_BLOCK - s->blockPos);
			if (n > to - pos)
			{
				n = to - pos;
			}
			memcpy (l + pos, s->block[0] + s->blockPos, n * sizeof (float));
			memcpy (r + pos, s->block[1] + s->blockPos, n * sizeof (float));
			s->blockPos += (int) n;
			pos += n;
		}
	}
}

// the control ports whose value changed; the modulation targets after the
// others (a target takes the value its parameter has then)
static void applyControls (EDrumSynth *s)
{
	for (int pass = 0; pass <= 1; pass++)
	{
		for (int i = 0; i < EDS_NUM_CONTROLS; i++)
		{
			const eds_control *c = &eds_controls[i];
			if (c->target != pass || !s->control[i])
			{
				continue;
			}
			const float v = *s->control[i];
			if (!(v == v))
			{
				continue;	// NaN
			}
			int x = (int) (v >= 0.f ? v + 0.5f : v - 0.5f) + c->offset;
			x = x < c->min ? c->min : x > c->max ? c->max : x;
			if ((uint8_t) x != s->value[i])
			{
				s->value[i] = (uint8_t) x;
				lxr_engine_set_parameter (c->param, (uint8_t) x);
			}
		}
	}
}

static void handleEvent (EDrumSynth *s, const LV2_Atom_Event *ev)
{
	if (ev->body.type == s->uris.midiEvent)
	{
		const uint8_t *const msg = (const uint8_t *) (ev + 1);
		if (ev->body.size >= 3 && lv2_midi_message_type (msg) == LV2_MIDI_MSG_NOTE_ON && msg[2] > 0)
		{
			const int track = eds_note_track (msg[1]);
			if (track >= 0)
			{
				lxr_engine_trigger ((uint8_t) track, msg[2], LXR_ENGINE_NOTE);
			}
		}
	}
	else if (ev->body.type == s->uris.atomObject || ev->body.type == s->uris.atomBlank)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *) &ev->body;
		if (obj->body.otype != s->uris.timePosition)
		{
			return;
		}
		const LV2_Atom *bpm = NULL;
		lv2_atom_object_get (obj, s->uris.timeBpm, &bpm, 0);
		if (!bpm)
		{
			return;
		}
		double v = 0;
		if (bpm->type == s->uris.atomFloat)
			v = ((const LV2_Atom_Float *) bpm)->body;
		else if (bpm->type == s->uris.atomDouble)
			v = ((const LV2_Atom_Double *) bpm)->body;
		else if (bpm->type == s->uris.atomInt)
			v = ((const LV2_Atom_Int *) bpm)->body;
		else if (bpm->type == s->uris.atomLong)
			v = (double) ((const LV2_Atom_Long *) bpm)->body;
		if (v >= 1 && v < 1000)
		{
			lxr_engine_set_tempo ((uint16_t) (v + 0.5));
		}
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
			map = features[i]->data;
		else if (!strcmp (features[i]->URI, LV2_LOG__log))
			log = features[i]->data;
	}
	LV2_Log_Logger logger;
	memset (&logger, 0, sizeof logger);
	lv2_log_logger_init (&logger, map, log);
	if (!map)
	{
		lv2_log_error (&logger, "eDrumSynth: the host has no urid:map\n");
		return NULL;
	}
	if (atomic_flag_test_and_set (&instanceActive))
	{
		lv2_log_error (&logger, "eDrumSynth: one instance per process (the drum engine is global)\n");
		return NULL;
	}

	EDrumSynth *s = calloc (1, sizeof *s);
	if (!s)
	{
		atomic_flag_clear (&instanceActive);
		return NULL;
	}
	s->logger = logger;
	s->uris.midiEvent = map->map (map->handle, LV2_MIDI__MidiEvent);
	s->uris.atomBlank = map->map (map->handle, LV2_ATOM__Blank);
	s->uris.atomObject = map->map (map->handle, LV2_ATOM__Object);
	s->uris.atomFloat = map->map (map->handle, LV2_ATOM__Float);
	s->uris.atomDouble = map->map (map->handle, LV2_ATOM__Double);
	s->uris.atomInt = map->map (map->handle, LV2_ATOM__Int);
	s->uris.atomLong = map->map (map->handle, LV2_ATOM__Long);
	s->uris.timePosition = map->map (map->handle, LV2_TIME__Position);
	s->uris.timeBpm = map->map (map->handle, LV2_TIME__beatsPerMinute);

	s->resample = rate != ENGINE_RATE;
	if (s->resample && eds_rs_init (&s->rs, ENGINE_RATE, rate, LXR_ENGINE_BLOCK))
	{
		lv2_log_error (&logger, "eDrumSynth: no resampler for %.0f Hz\n", rate);
		free (s);
		atomic_flag_clear (&instanceActive);
		return NULL;
	}

	// the init kit; the voices' mix as the kit's audio outputs say (summed
	// to the one stereo output)
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		s->value[i] = lxr_engine_get_parameter (eds_controls[i].param);
	}
	s->blockPos = LXR_ENGINE_BLOCK;
	return s;
}

static void connectPort (LV2_Handle instance, uint32_t port, void *data)
{
	EDrumSynth *s = instance;
	switch (port)
	{
	case EDS_PORT_MIDI_IN:	s->midiIn = data;	break;
	case EDS_PORT_OUT_L:	s->out[0] = data;	break;
	case EDS_PORT_OUT_R:	s->out[1] = data;	break;
	default:
		if (port >= EDS_PORT_CONTROL0 && port < EDS_NUM_PORTS)
		{
			s->control[port - EDS_PORT_CONTROL0] = data;
		}
		break;
	}
}

static void activate (LV2_Handle instance)
{
	(void) instance;
}

static void run (LV2_Handle instance, uint32_t frames)
{
	EDrumSynth *s = instance;
	if (!s->out[0] || !s->out[1])
	{
		return;
	}
	applyControls (s);

	uint32_t pos = 0;
	if (s->midiIn)
	{
		LV2_ATOM_SEQUENCE_FOREACH (s->midiIn, ev)
		{
			uint32_t t = ev->time.frames < 0 ? 0 : (uint32_t) ev->time.frames;
			if (t > frames)
			{
				t = frames;
			}
			if (t > pos)
			{
				renderAudio (s, pos, t);
				pos = t;
			}
			handleEvent (s, ev);
		}
	}
	renderAudio (s, pos, frames);
}

static void deactivate (LV2_Handle instance)
{
	(void) instance;
}

static void cleanup (LV2_Handle instance)
{
	EDrumSynth *s = instance;
	if (s->resample)
	{
		eds_rs_free (&s->rs);
	}
	free (s);
	atomic_flag_clear (&instanceActive);
}

static const void *extensionData (const char *uri)
{
	(void) uri;
	return NULL;
}

static const LV2_Descriptor descriptor = {
	EDS_URI,
	instantiate,
	connectPort,
	activate,
	run,
	deactivate,
	cleanup,
	extensionData,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor (uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
