/*
 * edrumsynth.c - eDrumSynth, MicroDexed-touch's drum engine
 * (engine/Synth_LXR: the voices of the OpenElixir-02 firmware) as an LV2
 * instrument, with the LXR's own step sequencer (0.3.0).
 *
 * - MIDI: General MIDI drum notes (notemap.h) trigger the tracks at their
 *   tuned pitch with the note's velocity, on every channel (MIDI mode Kit)
 *   or on channel 10, channels 1-7 playing tracks 1-7 chromatically (Multi);
 *   note-offs are ignored. CCs and NRPNs on any channel set engine
 *   parameters as the LXR's MIDI implementation when the "LXR CC" switch is
 *   on (lxr_engine_midi_cc(); not the engine selection, which belongs to the
 *   ports), else they are ignored. time:Position: the host's tempo (the
 *   LFOs' sync) and transport (the sequencer).
 * - Control ports: one per engine parameter and engine family (ports.h,
 *   generated with the TTL by tools/gen_ttl.c); a parameter is sent when
 *   its port's value changes, and only from the ports of the engine its
 *   slot runs. An engine selector per slot: on a change the engine switches
 *   the slot (and resets its parameters), then the ports are sent again
 *   where they differ from the engine's values, so the ports win. A kit the
 *   sequencer loads (a pattern's kit) or a restored state's kit is the
 *   engine's until a port changes.
 * - The sequencer (engine/Synth_LXR/src/lxr_sequencer.h): ticked before
 *   each engine block; its clock follows the host's transport (Host mode)
 *   or its own tempo (Internal); the panel ports (seq_*) select and edit
 *   like the LXR's front panel and show its state.
 * - State: the sequencer's project (patterns, kits, songs) as one chunk;
 *   restore prepares a copy, run() swaps it in at a block boundary.
 * - Audio: the engine renders 64-frame blocks at 44.1 kHz, rendered when the
 *   output needs them; at other host rates through the resampler.
 * - Events take effect at the next engine block: the 64-frame grid of the
 *   LXR (1.45 ms), the same for any host block size; the sequencer's steps
 *   play at the first engine block at or after their time, measured where
 *   that block is heard (the resampler's delay included).
 * - One instance per process (the engine has global state).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "lxr_engine.h"
#include "lxr_sequencer.h"
#include "notemap.h"
#include "ports.h"
#include "resampler.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_RATE	44100.0
#define PROJECT_URI	EDS_URI "#project"
#define FLASH_SECONDS	0.08	// a trigger's flash on seq_out_flash
#define RETIRED		4	// projects replaced, waiting to be freed outside run()

// the LXR's MIDI implementation (lxr_engine_midi_cc()): CC 99 / 98 select
// NRPN n, data entry CC 6 sets parameter 128 + n; NRPN 98 (PAR_SLOT_ENGINE,
// OpenElixir-02) switches a slot's engine, which here belongs to the engine
// selector ports: the plugin drops it
#define CC_DATA_ENTRY	6
#define CC_NRPN_FINE	98
#define CC_NRPN_COARSE	99
#define NRPN_ENGINE	98

enum { MODE_OFF, MODE_HOST, MODE_INTERNAL };

typedef struct
{
	const LV2_Atom_Sequence *midiIn;
	float *out[2];
	const float *control[EDS_NUM_CONTROLS];
	float last[EDS_NUM_CONTROLS];		// each port's value last applied (NaN: none yet)
	uint8_t value[EDS_NUM_CONTROLS];	// the engine's value of each port
	uint8_t engine[EDS_SLOTS + 1];		// each slot's engine; [EDS_GLOBAL]: 0
	uint8_t midiMode;			// 0 kit, 1 multi
	uint8_t lxrCc;				// CCs / NRPNs to the engine
	uint16_t nrpn;				// the NRPN selected by CC 99 / 98 (as the engine's)
	uint32_t kitLoads;			// the sequencer's kit loads seen

	// the sequencer's panel
	float *panel[EDS_NUM_PANEL];
	float panelLast[EDS_NUM_PANEL];
	int started;				// run() ran (the first run: edits not applied)
	int mode;				// MODE_*
	int play;				// Internal: the play port
	double tempo;				// Internal: the tempo port
	uint8_t selStep;			// the step selected (0..63)
	uint32_t triggers[LXR_ENGINE_TRACKS];	// the engine's trigger counts seen
	uint32_t flash[LXR_ENGINE_TRACKS];	// frames each track's flash shows still

	// the host's transport (time:Position)
	struct
	{
		int known;		// a position with a speed came
		int rolling;
		int bbt;		// with bar and beat
		double speed;
		double qbpm;		// quarter notes per minute
		double bpm;		// as sent (the LFOs' sync)
		double q;		// the position (quarter notes) at frame
		uint32_t frame;		// in this run()
	} host;

	// the state: a restored project, waiting for run(); the replaced ones
	_Atomic (lxr_project *) pending;
	_Atomic (lxr_project *) retired[RETIRED];

	struct
	{
		LV2_URID midiEvent, atomBlank, atomObject, atomFloat, atomDouble, atomInt, atomLong, atomChunk;
		LV2_URID timePosition, timeBpm, timeSpeed, timeFrame, timeBar, timeBarBeat, timeBeatsPerBar, timeBeatUnit;
		LV2_URID project;
	} uris;
	LV2_Log_Logger logger;

	double rate;
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
	lxr_seq_block ();		// the sequencer's tick due (triggers, locks, kits)
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

// host frames from the output written so far until the next engine block
// (not rendered yet) is heard
static double pendingFrames (const EDrumSynth *s)
{
	if (s->resample)
	{
		return eds_rs_pending (&s->rs);
	}
	return s->blockPos == LXR_ENGINE_BLOCK ? 0 : LXR_ENGINE_BLOCK - s->blockPos;
}

static int roundPort (float v)
{
	return (int) (v >= 0.f ? v + 0.5f : v - 0.5f);
}

// the port cache from the engine after it changed by itself (an engine
// switch, a kit the sequencer loaded, a restored kit)
static void reloadCache (EDrumSynth *s)
{
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		s->value[i] = lxr_engine_get_parameter (eds_controls[i].param);
	}
	lxr_engine_get_engines (s->engine);
	s->engine[EDS_GLOBAL] = 0;
}

static int portChanged (float v, float *last)
{
	if (v == *last || (!(v == v) && !(*last == *last)))
	{
		return 0;
	}
	*last = v;
	return 1;
}

// the engine of each slot first: a switch resets the slot's parameters (and
// turns off modulation targets on its engine specific ones), so the cache
// is reloaded from the engine and the ports are sent again where they
// differ. Then the control ports whose value changed, of the engines the
// slots run; the modulation targets after the others (a target takes the
// value its parameter has then). A kit the sequencer loaded: the cache from
// the engine, so only ports changed since are sent
static void applyControls (EDrumSynth *s)
{
	lxr_seq_status st;
	lxr_seq_get_status (&st);
	if (st.kitLoads != s->kitLoads)
	{
		s->kitLoads = st.kitLoads;
		reloadCache (s);
	}

	int switched = 0;
	for (int slot = 0; slot < EDS_SLOTS; slot++)
	{
		const int c = eds_engine_controls[slot];
		const float *port = s->control[c];
		if (!port || !(*port == *port) || !portChanged (*port, &s->last[c]))
		{
			continue;
		}
		int e = roundPort (*port);
		e = e < 0 ? 0 : e > 3 ? 3 : e;
		if (e != s->engine[slot])
		{
			lxr_engine_set_engine ((uint8_t) slot, (uint8_t) e);
			switched = 1;
		}
	}
	if (switched)
	{
		reloadCache (s);
	}
	const float *mode = s->control[EDS_MIDI_MODE_CONTROL];
	if (mode && *mode == *mode)
	{
		s->midiMode = roundPort (*mode) >= 1;
	}
	const float *lxrCc = s->control[EDS_LXR_CC_CONTROL];
	if (lxrCc && *lxrCc == *lxrCc)
	{
		s->lxrCc = *lxrCc > 0.5f;
	}

	for (int pass = 0; pass <= 1; pass++)
	{
		for (int i = 0; i < EDS_NUM_CONTROLS; i++)
		{
			const eds_control *c = &eds_controls[i];
			if (c->target != pass || c->type != EDS_CTL_PARAM || !s->control[i] || !(c->engines >> s->engine[c->slot] & 1))
			{
				continue;
			}
			const float v = *s->control[i];
			if (!(v == v))
			{
				continue;	// NaN
			}
			if (!portChanged (v, &s->last[i]) && !switched)
			{
				continue;
			}
			int x = roundPort (v) + c->offset;
			x = x < c->min ? c->min : x > c->max ? c->max : x;
			if ((uint8_t) x != s->value[i])
			{
				s->value[i] = (uint8_t) x;
				lxr_engine_set_parameter (c->param, (uint8_t) x);
			}
		}
	}
}

// the kit ports' current values as applied (a restored state's kit wins
// over them until they change)
static void markControlsSeen (EDrumSynth *s)
{
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		if (s->control[i])
		{
			s->last[i] = *s->control[i];
		}
	}
}

// ---- the host's transport ----

static int atomNumber (const EDrumSynth *s, const LV2_Atom *a, double *v)
{
	if (!a)
		return 0;
	if (a->type == s->uris.atomFloat)
		*v = ((const LV2_Atom_Float *) a)->body;
	else if (a->type == s->uris.atomDouble)
		*v = ((const LV2_Atom_Double *) a)->body;
	else if (a->type == s->uris.atomInt)
		*v = ((const LV2_Atom_Int *) a)->body;
	else if (a->type == s->uris.atomLong)
		*v = (double) ((const LV2_Atom_Long *) a)->body;
	else
		return 0;
	return *v == *v;
}

// the host's quarter notes per output frame
static double hostQpf (const EDrumSynth *s)
{
	return s->host.rolling ? s->host.speed * s->host.qbpm / 60.0 / s->rate : 0;
}

static void hostAdvance (EDrumSynth *s, uint32_t frame)
{
	if (frame > s->host.frame)
	{
		s->host.q += (frame - s->host.frame) * hostQpf (s);
		s->host.frame = frame;
	}
}

// the sequencer after the host (Host mode): started, stopped, located, or
// its position corrected; at the host's position where the next engine
// block will be heard
static void syncHost (EDrumSynth *s)
{
	if (s->mode != MODE_HOST || !s->host.known)
	{
		return;
	}
	if (!s->host.rolling)
	{
		lxr_seq_stop ();
		return;
	}
	lxr_seq_set_tempo (s->host.qbpm * s->host.speed);
	const double tick = (s->host.q + pendingFrames (s) * hostQpf (s)) * LXR_SEQ_TICKS_PER_QUARTER;
	if (!lxr_seq_running ())
	{
		lxr_seq_start (tick);
	}
	else if (fabs (tick - lxr_seq_position ()) > 0.5)
	{
		lxr_seq_locate (tick);
	}
	else
	{
		lxr_seq_set_position (tick);
	}
}

static void handlePosition (EDrumSynth *s, const LV2_Atom_Object *obj, uint32_t frame)
{
	const LV2_Atom *bpm = NULL, *speed = NULL, *pframe = NULL, *bar = NULL, *barBeat = NULL, *beatsPerBar = NULL,
		       *beatUnit = NULL;
	lv2_atom_object_get (obj, s->uris.timeBpm, &bpm, s->uris.timeSpeed, &speed, s->uris.timeFrame, &pframe,
			     s->uris.timeBar, &bar, s->uris.timeBarBeat, &barBeat, s->uris.timeBeatsPerBar, &beatsPerBar,
			     s->uris.timeBeatUnit, &beatUnit, 0);
	hostAdvance (s, frame);
	double v, unit = 4;
	if (atomNumber (s, beatUnit, &v) && v >= 1 && v <= 64)
	{
		unit = v;
	}
	if (atomNumber (s, bpm, &v) && v >= 1 && v < 1000)
	{
		s->host.bpm = v;
		s->host.qbpm = v * 4.0 / unit;
	}
	if (atomNumber (s, speed, &v))
	{
		s->host.known = 1;
		s->host.speed = v > 0 ? v : 0;
		s->host.rolling = v > 0;
	}
	double b, bb, bpb = 4;
	if (atomNumber (s, bar, &b) && atomNumber (s, barBeat, &bb))
	{
		if (atomNumber (s, beatsPerBar, &v) && v > 0)
		{
			bpb = v;
		}
		s->host.bbt = 1;
		s->host.q = (b * bpb + bb) * 4.0 / unit;
	}
	else if (atomNumber (s, pframe, &v))
	{
		// no bar and beat: the frame at the tempo
		s->host.bbt = 0;
		s->host.q = v / s->rate * (s->host.qbpm > 0 ? s->host.qbpm : s->tempo) / 60.0;
	}
	s->host.frame = frame;

	// the LFOs' sync: the host's tempo, but in Internal mode
	if (s->mode != MODE_INTERNAL && s->host.bpm >= 1)
	{
		lxr_engine_set_tempo ((uint16_t) (s->host.bpm + 0.5));
	}
	syncHost (s);
}

static void handleEvent (EDrumSynth *s, const LV2_Atom_Event *ev, uint32_t frame)
{
	if (ev->body.type == s->uris.midiEvent)
	{
		const uint8_t *const msg = (const uint8_t *) (ev + 1);
		if (ev->body.size >= 3 && lv2_midi_message_type (msg) == LV2_MIDI_MSG_NOTE_ON && msg[2] > 0)
		{
			uint8_t pitch;
			const int track = eds_note_trigger (s->midiMode, msg[0] & 0x0F, msg[1], &pitch);
			if (track >= 0)
			{
				lxr_engine_trigger ((uint8_t) track, msg[2], pitch);
			}
		}
		else if (ev->body.size >= 3 && lv2_midi_message_type (msg) == LV2_MIDI_MSG_CONTROLLER && s->lxrCc)
		{
			// a parameter change in the engine only: the ports keep their
			// values (the next change of a port, or of its engine, wins)
			const uint8_t cc = msg[1] & 0x7F, value = msg[2] & 0x7F;
			if (cc == CC_NRPN_COARSE)
			{
				s->nrpn = (uint16_t) ((s->nrpn & 0x7F) | value << 7);
			}
			else if (cc == CC_NRPN_FINE)
			{
				s->nrpn = (uint16_t) ((s->nrpn & ~0x7F) | value);
			}
			else if (cc == CC_DATA_ENTRY && s->nrpn == NRPN_ENGINE)
			{
				return;
			}
			lxr_engine_midi_cc (cc, value);
		}
	}
	else if (ev->body.type == s->uris.atomObject || ev->body.type == s->uris.atomBlank)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *) &ev->body;
		if (obj->body.otype == s->uris.timePosition)
		{
			handlePosition (s, obj, frame);
		}
	}
}

// ---- the panel ----

// an input's value: rounded, in its range (unconnected: the default)
static int panelValue (const EDrumSynth *s, int i)
{
	const eds_panel_port *p = &eds_panel[i];
	float v = s->panel[i] ? *s->panel[i] : p->def;
	if (!(v == v))
	{
		v = p->def;
	}
	v = v < p->min ? p->min : v > p->max ? p->max : v;
	return roundPort (v);
}

static int panelChanged (EDrumSynth *s, int i)
{
	const float v = (float) panelValue (s, i);
	if (v == s->panelLast[i])
	{
		return 0;
	}
	s->panelLast[i] = v;
	return 1;
}

static int panelTriggered (EDrumSynth *s, int i)
{
	const float old = s->panelLast[i];
	return panelChanged (s, i) && s->panelLast[i] > 0.5f && !(old > 0.5f);
}

// what each input is: a setting (applied from the first run on), a project
// value (the pattern, song: on change; a restored state's win), an edit of
// the selection (on change), a trigger
enum { IN_SETTING, IN_PROJECT, IN_EDIT, IN_TRIGGER };

static int inputClass (int i)
{
	switch (i)
	{
	case EDS_P_PATTERN:
	case EDS_P_SONG_MODE:
	case EDS_P_SONG:
		return IN_PROJECT;
	case EDS_P_SHUFFLE:
	case EDS_P_STEP_VELOCITY:
	case EDS_P_STEP_NOTE:
	case EDS_P_STEP_PROB:
	case EDS_P_STEP_FLAM:
	case EDS_P_STEP_SHIFT:
	case EDS_P_LOCK1_TARGET:
	case EDS_P_LOCK1_VALUE:
	case EDS_P_LOCK2_TARGET:
	case EDS_P_LOCK2_VALUE:
	case EDS_P_TRACK_LENGTH:
	case EDS_P_EUCLID_FILL:
	case EDS_P_EUCLID_ROTATION:
	case EDS_P_KIT:
		return IN_EDIT;
	default:
		return eds_panel[i].kind == EDS_PK_TRIGGER ? IN_TRIGGER : IN_SETTING;
	}
}

// the inputs but the settings as they are now (not acted on): the first
// run, and a restored project's (its port values come with it)
static void markPanelSeen (EDrumSynth *s)
{
	for (int i = 0; i < EDS_NUM_PANEL; i++)
	{
		if (!eds_panel[i].output && inputClass (i) != IN_SETTING)
		{
			s->panelLast[i] = (float) panelValue (s, i);
		}
	}
}

static void setMode (EDrumSynth *s, int mode)
{
	s->mode = mode;
	switch (mode)
	{
	case MODE_HOST:
		lxr_seq_stop ();
		syncHost (s);
		break;
	case MODE_INTERNAL:
		lxr_seq_stop ();
		lxr_seq_set_tempo (s->tempo);
		if (s->play)
		{
			lxr_seq_start (0);
		}
		break;
	default:
		lxr_seq_stop ();
		break;
	}
}

static void applyPanel (EDrumSynth *s)
{
	// settings
	if (panelChanged (s, EDS_P_TEMPO))
	{
		s->tempo = panelValue (s, EDS_P_TEMPO);
		if (s->mode == MODE_INTERNAL)
		{
			lxr_seq_set_tempo (s->tempo);
		}
	}
	if (panelChanged (s, EDS_P_PLAY))
	{
		s->play = panelValue (s, EDS_P_PLAY);
		if (s->mode == MODE_INTERNAL)
		{
			if (s->play)
				lxr_seq_start (0);
			else
				lxr_seq_stop ();
		}
	}
	if (panelChanged (s, EDS_P_MODE))
	{
		setMode (s, panelValue (s, EDS_P_MODE));
	}
	if (panelChanged (s, EDS_P_KIT_AUTOLOAD))
	{
		lxr_seq_set_kit_autoload (panelValue (s, EDS_P_KIT_AUTOLOAD));
	}
	for (int t = 0; t < LXR_SEQ_TRACKS; t++)
	{
		if (panelChanged (s, EDS_P_MUTE_BD + t))
		{
			lxr_seq_set_mute ((uint8_t) t, panelValue (s, EDS_P_MUTE_BD + t));
		}
	}
	const int pageChanged = panelChanged (s, EDS_P_PAGE), stepChanged = panelChanged (s, EDS_P_STEP);
	panelChanged (s, EDS_P_EDIT_PATTERN);
	panelChanged (s, EDS_P_TRACK);
	const int page = panelValue (s, EDS_P_PAGE);
	if (pageChanged || stepChanged)
	{
		s->selStep = (uint8_t) (page * 16 + panelValue (s, EDS_P_STEP));
	}
	lxr_seq_select ((uint8_t) panelValue (s, EDS_P_EDIT_PATTERN), (uint8_t) panelValue (s, EDS_P_TRACK), s->selStep);

	// the project's pattern and song
	if (panelChanged (s, EDS_P_SONG))
	{
		lxr_seq_set_song ((uint8_t) panelValue (s, EDS_P_SONG));
	}
	if (panelChanged (s, EDS_P_SONG_MODE))
	{
		lxr_seq_set_song_mode (panelValue (s, EDS_P_SONG_MODE));
	}
	if (panelChanged (s, EDS_P_PATTERN))
	{
		lxr_seq_set_pattern ((uint8_t) panelValue (s, EDS_P_PATTERN));
	}

	// the step buttons: toggle, and select
	for (int i = 0; i < 16; i++)
	{
		if (panelTriggered (s, EDS_P_BUTTON1 + i))
		{
			s->selStep = (uint8_t) (page * 16 + i);
			lxr_seq_toggle_step (s->selStep);
		}
	}

	// edits of the selection
	static const struct
	{
		int port, field, offset;
	} fields[] = {
		{ EDS_P_STEP_VELOCITY, LXR_STEP_VELOCITY, 0 }, { EDS_P_STEP_NOTE, LXR_STEP_NOTE, 0 },
		{ EDS_P_STEP_PROB, LXR_STEP_PROB, 0 }, { EDS_P_STEP_FLAM, LXR_STEP_FLAM, 0 },
		{ EDS_P_STEP_SHIFT, LXR_STEP_SHIFT, 7 }, { EDS_P_LOCK1_TARGET, LXR_STEP_LOCK1_TARGET, 0 },
		{ EDS_P_LOCK1_VALUE, LXR_STEP_LOCK1_VALUE, 0 }, { EDS_P_LOCK2_TARGET, LXR_STEP_LOCK2_TARGET, 0 },
		{ EDS_P_LOCK2_VALUE, LXR_STEP_LOCK2_VALUE, 0 },
	};
	for (unsigned k = 0; k < sizeof fields / sizeof fields[0]; k++)
	{
		if (panelChanged (s, fields[k].port))
		{
			lxr_seq_set_step_field (fields[k].field, (uint8_t) (panelValue (s, fields[k].port) + fields[k].offset));
		}
	}
	if (panelChanged (s, EDS_P_TRACK_LENGTH))
	{
		lxr_seq_set_track_length ((uint8_t) panelValue (s, EDS_P_TRACK_LENGTH));
	}
	if (panelChanged (s, EDS_P_SHUFFLE))
	{
		lxr_seq_set_shuffle ((uint8_t) panelValue (s, EDS_P_SHUFFLE));
	}
	if (panelChanged (s, EDS_P_KIT))
	{
		lxr_seq_set_kit ((uint8_t) panelValue (s, EDS_P_KIT));
	}
	panelChanged (s, EDS_P_EUCLID_FILL);
	panelChanged (s, EDS_P_EUCLID_ROTATION);

	// actions
	if (panelTriggered (s, EDS_P_EUCLID))
	{
		lxr_seq_euclid ((uint8_t) panelValue (s, EDS_P_EUCLID_FILL), (uint8_t) panelValue (s, EDS_P_EUCLID_ROTATION));
	}
	if (panelTriggered (s, EDS_P_STORE_KIT))
	{
		lxr_seq_store_kit ();
	}
	if (panelTriggered (s, EDS_P_CLEAR_TRACK))
	{
		lxr_seq_clear_track ();
	}
	if (panelTriggered (s, EDS_P_CLEAR_PATTERN))
	{
		lxr_seq_clear_pattern ();
	}
	if (panelTriggered (s, EDS_P_COPY_PATTERN))
	{
		lxr_seq_copy_pattern ();
	}
	if (panelTriggered (s, EDS_P_PASTE_PATTERN))
	{
		lxr_seq_paste_pattern ();
	}
}

static void setOutput (EDrumSynth *s, int i, float v)
{
	if (s->panel[i])
	{
		*s->panel[i] = v;
	}
}

static void writePanel (EDrumSynth *s, uint32_t frames)
{
	lxr_seq_status st;
	lxr_seq_get_status (&st);
	const uint8_t edit = lxr_seq_edit_pattern (), track = (uint8_t) panelValue (s, EDS_P_TRACK);
	const uint8_t first = (uint8_t) (panelValue (s, EDS_P_PAGE) * 16);
	unsigned flash = 0;
	for (int t = 0; t < LXR_ENGINE_TRACKS; t++)
	{
		const uint32_t n = lxr_engine_trigger_count ((uint8_t) t);
		if (n != s->triggers[t])
		{
			s->triggers[t] = n;
			s->flash[t] = (uint32_t) (FLASH_SECONDS * s->rate);
		}
		else
		{
			s->flash[t] = s->flash[t] > frames ? s->flash[t] - frames : 0;
		}
		flash |= (s->flash[t] > 0) << t;
	}
	setOutput (s, EDS_P_OUT_RUNNING, (float) st.running);
	setOutput (s, EDS_P_OUT_PLAYING, st.playing);
	setOutput (s, EDS_P_OUT_NEXT, st.next);
	setOutput (s, EDS_P_OUT_SONG_POS, st.songPos);
	setOutput (s, EDS_P_OUT_EDIT_PATTERN, edit);
	setOutput (s, EDS_P_OUT_BPM, (float) lxr_seq_get_tempo ());
	setOutput (s, EDS_P_OUT_FLASH, (float) flash);
	setOutput (s, EDS_P_OUT_MUTES, st.muted);
	setOutput (s, EDS_P_OUT_PATTERN_LENGTH, lxr_seq_pattern_length (edit));
	setOutput (s, EDS_P_OUT_TRACK_LENGTH, lxr_seq_track_length (edit, track));
	setOutput (s, EDS_P_OUT_SHUFFLE, lxr_seq_shuffle (edit));
	const uint8_t kit = lxr_seq_kit (edit);
	setOutput (s, EDS_P_OUT_KIT, kit);
	setOutput (s, EDS_P_OUT_KIT_PRESENT, (float) lxr_seq_has_kit (kit));
	setOutput (s, EDS_P_OUT_SELECTED, s->selStep);
	for (int t = 0; t < LXR_SEQ_TRACKS; t++)
	{
		setOutput (s, EDS_P_OUT_POS_BD + t, st.step[t]);
		setOutput (s, EDS_P_OUT_GRID_BD + t, lxr_seq_step_mask (edit, (uint8_t) t, first));
	}
	setOutput (s, EDS_P_OUT_MASK, lxr_seq_step_mask (edit, track, first));
	for (int i = 0; i < 16; i++)
	{
		setOutput (s, EDS_P_OUT_VEL1 + i, lxr_seq_get_step_field (edit, track, (uint8_t) (first + i), LXR_STEP_VELOCITY));
	}
	static const struct
	{
		int port, field, offset;
	} fields[] = {
		{ EDS_P_OUT_STEP_ACTIVE, LXR_STEP_ACTIVE, 0 }, { EDS_P_OUT_STEP_VELOCITY, LXR_STEP_VELOCITY, 0 },
		{ EDS_P_OUT_STEP_NOTE, LXR_STEP_NOTE, 0 }, { EDS_P_OUT_STEP_PROB, LXR_STEP_PROB, 0 },
		{ EDS_P_OUT_STEP_FLAM, LXR_STEP_FLAM, 0 }, { EDS_P_OUT_STEP_SHIFT, LXR_STEP_SHIFT, -7 },
		{ EDS_P_OUT_LOCK1_TARGET, LXR_STEP_LOCK1_TARGET, 0 }, { EDS_P_OUT_LOCK1_VALUE, LXR_STEP_LOCK1_VALUE, 0 },
		{ EDS_P_OUT_LOCK2_TARGET, LXR_STEP_LOCK2_TARGET, 0 }, { EDS_P_OUT_LOCK2_VALUE, LXR_STEP_LOCK2_VALUE, 0 },
	};
	for (unsigned k = 0; k < sizeof fields / sizeof fields[0]; k++)
	{
		setOutput (s, fields[k].port, (float) (lxr_seq_get_step_field (edit, track, s->selStep, fields[k].field) + fields[k].offset));
	}
}

// ---- the state's project ----

static void retire (EDrumSynth *s, lxr_project *p)
{
	for (int i = 0; i < RETIRED && p; i++)
	{
		lxr_project *expected = NULL;
		if (atomic_compare_exchange_strong (&s->retired[i], &expected, p))
		{
			return;
		}
	}
	// all slots taken: more restores than run() calls in between, while
	// restore() runs concurrently with run() (state:threadSafeRestore,
	// which this plugin doesn't ask for): this one is not freed
}

static void freeRetired (EDrumSynth *s)
{
	for (int i = 0; i < RETIRED; i++)
	{
		lxr_seq_project_free (atomic_exchange (&s->retired[i], NULL));
	}
}

// a restored project into the sequencer, at the start of run(): its kit is
// the engine's (the kit ports as they are now don't change it), the panel's
// edits and triggers as they are now are not acted on
static void installPending (EDrumSynth *s)
{
	lxr_project *p = atomic_exchange (&s->pending, NULL);
	if (!p)
	{
		return;
	}
	retire (s, lxr_seq_project_install (p));
	lxr_seq_status st;
	lxr_seq_get_status (&st);
	s->kitLoads = st.kitLoads;
	reloadCache (s);
	markControlsSeen (s);
	markPanelSeen (s);
	if (s->mode == MODE_INTERNAL && s->play && !lxr_seq_running ())
	{
		lxr_seq_start (0);
	}
	syncHost (s);
}

static LV2_State_Status save (LV2_Handle instance, LV2_State_Store_Function store, LV2_State_Handle handle,
			      uint32_t flags, const LV2_Feature *const *features)
{
	(void) flags;
	(void) features;
	EDrumSynth *s = instance;
	const lxr_project *p = lxr_seq_project_current ();
	// the largest a chunk can be: the project may change while it's written
	// (save runs alongside run())
	const size_t cap = 48 + 64 * (LXR_SEQ_PATTERN_FILE + LXR_SEQ_KIT_FILE + LXR_SEQ_SONG_FILE) + 256 + LXR_SEQ_KIT_FILE;
	uint8_t *buf = malloc (cap);
	if (!buf)
	{
		return LV2_STATE_ERR_NO_SPACE;
	}
	const size_t size = lxr_seq_chunk_write (p, buf, cap);
	LV2_State_Status status = LV2_STATE_ERR_UNKNOWN;
	if (size)
	{
		status = store (handle, s->uris.project, buf, size, s->uris.atomChunk, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
	}
	free (buf);
	return status;
}

static LV2_State_Status restore (LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
				 uint32_t flags, const LV2_Feature *const *features)
{
	(void) flags;
	(void) features;
	EDrumSynth *s = instance;
	size_t size = 0;
	uint32_t type = 0, vflags = 0;
	const void *data = retrieve (handle, s->uris.project, &size, &type, &vflags);
	if (!data)
	{
		return LV2_STATE_SUCCESS;	// a state without a project (a 0.2.0 preset): the project stays
	}
	if (type != s->uris.atomChunk)
	{
		lv2_log_error (&s->logger, "eDrumSynth: the project is not a chunk\n");
		return LV2_STATE_ERR_BAD_TYPE;
	}
	lxr_project *p = lxr_seq_project_new ();
	if (!p)
	{
		return LV2_STATE_ERR_NO_SPACE;
	}
	const char *err = lxr_seq_chunk_read (p, data, size);
	if (err)
	{
		lv2_log_error (&s->logger, "eDrumSynth: the project not restored: %s\n", err);
		lxr_seq_project_free (p);
		return LV2_STATE_ERR_UNKNOWN;
	}
	freeRetired (s);
	lxr_seq_project_free (atomic_exchange (&s->pending, p));
	return LV2_STATE_SUCCESS;
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
	s->uris.atomChunk = map->map (map->handle, LV2_ATOM__Chunk);
	s->uris.timePosition = map->map (map->handle, LV2_TIME__Position);
	s->uris.timeBpm = map->map (map->handle, LV2_TIME__beatsPerMinute);
	s->uris.timeSpeed = map->map (map->handle, LV2_TIME__speed);
	s->uris.timeFrame = map->map (map->handle, LV2_TIME__frame);
	s->uris.timeBar = map->map (map->handle, LV2_TIME__bar);
	s->uris.timeBarBeat = map->map (map->handle, LV2_TIME__barBeat);
	s->uris.timeBeatsPerBar = map->map (map->handle, LV2_TIME__beatsPerBar);
	s->uris.timeBeatUnit = map->map (map->handle, LV2_TIME__beatUnit);
	s->uris.project = map->map (map->handle, PROJECT_URI);

	s->rate = rate;
	s->resample = rate != ENGINE_RATE;
	if (s->resample && eds_rs_init (&s->rs, ENGINE_RATE, rate, LXR_ENGINE_BLOCK))
	{
		lv2_log_error (&logger, "eDrumSynth: no resampler for %.0f Hz\n", rate);
		free (s);
		atomic_flag_clear (&instanceActive);
		return NULL;
	}

	// the init kit; the voices' mix as the kit's audio outputs say (summed
	// to the one stereo output); the sequencer stopped with an empty project
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	// the engine's NRPN selection (a static of the engine) as the plugin's: 0
	lxr_engine_midi_cc (CC_NRPN_COARSE, 0);
	lxr_engine_midi_cc (CC_NRPN_FINE, 0);
	reloadCache (s);
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		s->last[i] = NAN;
	}
	for (int i = 0; i < EDS_NUM_PANEL; i++)
	{
		s->panelLast[i] = NAN;
	}
	s->mode = MODE_OFF;
	s->tempo = 120;
	s->host.qbpm = s->host.bpm = 120;
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
		if (port >= EDS_PORT_CONTROL0 && port < EDS_PORT_PANEL0)
		{
			s->control[port - EDS_PORT_CONTROL0] = data;
		}
		else if (port >= EDS_PORT_PANEL0 && port < EDS_NUM_PORTS)
		{
			s->panel[port - EDS_PORT_PANEL0] = data;
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
	s->host.frame = 0;
	installPending (s);
	applyControls (s);
	if (!s->started)
	{
		// the first run: the settings apply, the edits and triggers are
		// where they are
		markPanelSeen (s);
		s->started = 1;
	}
	applyPanel (s);

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
			handleEvent (s, ev, t);
		}
	}
	renderAudio (s, pos, frames);
	hostAdvance (s, frames);
	writePanel (s, frames);
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
	lxr_seq_project_free (atomic_exchange (&s->pending, NULL));
	freeRetired (s);
	// the engine back to its own project (the current one freed)
	lxr_project *current = lxr_seq_project_current ();
	lxr_seq_init ();
	lxr_seq_project_free (current);
	free (s);
	atomic_flag_clear (&instanceActive);
}

static const void *extensionData (const char *uri)
{
	static const LV2_State_Interface state = { save, restore };
	if (!strcmp (uri, LV2_STATE__interface))
	{
		return &state;
	}
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
