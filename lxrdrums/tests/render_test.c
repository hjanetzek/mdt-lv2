/*
 * render_test.c - a minimal LV2 host for eDrumSynth (dlopen +
 * lv2_descriptor, a urid:map, no lilv) and checks against the engine:
 *
 *   render_test PATH/edrumsynth.so [-v]
 *   render_test PATH/edrumsynth.so --presets "python3 -I PATH/snd2lv2.py" WORKDIR [-v]
 *
 * The render checks:
 * - 44.1 kHz: the plugin's output is the engine's (lxr_engine_render() with
 *   the same triggers and parameters), bit exact after the int16 -> float
 *   scale, for several host block sizes;
 * - every engine on every slot: the plugin with the slot's engine port set
 *   and values on that engine's ports renders as the engine switched with
 *   lxr_engine_set_engine() and set to the same values; an engine switch
 *   while playing; the ports of the engines the slots don't run have no
 *   effect;
 * - MIDI modes: Kit ignores the channel; Multi plays channel 10 as the kit
 *   and channels 1-7 the tracks at the note's pitch (an octave up: twice
 *   the frequency), other channels not;
 * - a track's note 60 + k (Multi, its channel) renders exactly as its kit
 *   note with the slot's tuning k semitones up (coarse, and the FM / mod
 *   oscillators the note moves): every engine on every track;
 * - LXR CC: on, CCs and NRPNs on any channel render as the engine with
 *   lxr_engine_midi_cc() (except the engine selection NRPN 98, dropped);
 *   off, they change nothing;
 * - 48 kHz: no NaN, no clipping storm; the same signal for block sizes 1,
 *   64, 480, 960, 4096 and odd ones; the bass drum's pitch within 1 % of
 *   the 44.1 kHz render, its level within 0.5 dB;
 * - other rates (22.05, 32, 88.2, 96, 192 kHz and 47999 Hz: interpolated
 *   phases): sane output;
 * - a second instance in the same process is refused.
 * It also prints the CPU time per output sample (this machine, this build).
 *
 * With --presets: the factory presets (the bundle's presets.ttl: every kit
 * port, "Init" = the defaults, each renders as the engine set up with its
 * values), and the kit converter (tools/snd2lv2.py): synthetic .snd kits
 * written to WORKDIR (a /SYNTHKITS tree with a bank folder), converted, and
 * each converted preset renders as the engine with the kit loaded directly
 * (lxr_engine_load_kit_engines()).
 *
 * The host is common/testhost (dlopen, urid:map, atom sequences, forked
 * renders, measurements).
 * Each render runs in a child process (fork): the engine is global state,
 * a fresh process starts it the same way each time. The plugin keeps its
 * engine's symbols to itself (only lv2_descriptor is exported), so this
 * program links its own engine for the reference.
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "lxr_engine.h"
#include "ports.h"
#include "testhost.h"

#include <lv2/midi/midi.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *pluginPath;

#define CHECK TH_CHECK
#define NELEM(a) (int) (sizeof (a) / sizeof ((a)[0]))

// ---- the scenario ----

typedef struct
{
	int frame44;	// at 44.1 kHz; other rates: scaled
	uint8_t note, velocity;
	uint8_t channel;	// 0..15 (9: the GM drum channel 10)
} Note;

typedef struct
{
	const char *symbol;
	float value;
} Control;

typedef struct
{
	int frame44;	// a multiple of the host block (controls are read per run())
	Control control;
} Change;

typedef struct
{
	const Note *notes;
	int numNotes;
	const Control *preset;	// applied first (a preset's values), or NULL
	int numPreset;
	const Control *controls;
	int numControls;
	const Change *changes;
	int numChanges;
	double seconds;
	int tempoEvent;		// send a time:Position (120.0 bpm) with the first note
	const uint8_t *kit;	// the reference loads this kit (LXR_ENGINE_KIT bytes) instead of setting the controls
	const uint8_t *kitEngines;
} Scenario;

// other MIDI messages (CCs) of the next renders (set before render(): the
// forked renders inherit them), in time order
typedef struct
{
	int frame44;
	uint8_t msg[3];
} Msg;
static const Msg *ccMsgs;
static int numCcMsgs;

// the kit scenario: channel 10 (9)
static const Note kitNotes[] = {
	{ 0, 36, 127, 9 }, { 1000, 42, 90, 9 }, { 5513, 38, 110, 9 }, { 11025, 46, 100, 9 }, { 11030, 36, 64, 9 },
	{ 16537, 45, 120, 9 }, { 22050, 41, 80, 9 }, { 22051, 49, 127, 9 }, { 27563, 42, 30, 9 }, { 33075, 39, 100, 9 },
	{ 38587, 47, 70, 9 }, { 44100, 36, 127, 9 }, { 44164, 44, 50, 9 }, { 50000, 51, 100, 9 }, { 60000, 37, 127, 9 },
	{ 64000, 60, 100, 9 },	// not mapped
};
// some parameters away from the init kit: filter, pan, a velocity
// modulation target, the FX bus (delay) with the cymbal on it
static const Control kitControls[] = {
	{ "bd_filter_cutoff", 90 }, { "bd_filter_resonance", 40 }, { "sd_mix_pan", -30 },
	{ "bd_mod_vel_dest", 2 }, { "bd_mod_vel_amount", 60 }, { "fx_type", 4 }, { "fx_dry_wet", 50 },
	{ "fx_p1", 40 }, { "fx_p4", 70 }, { "cy_mix_out", 6 }, { "hh_ampeg_decay_open", 60 },
	{ "t1_osc_coarse", -10 },
};
static const Scenario kit = { kitNotes, NELEM (kitNotes), NULL, 0, kitControls, NELEM (kitControls), NULL, 0, 2.0, 1,
			      NULL, NULL };

// the bass drum alone, a steady pitch (no pitch modulation), long decay
static const Note bdNotes[] = { { 441, 36, 127, 9 } };
static const Control bdControls[] = { { "bd_modeg_pitch", 0 }, { "bd_ampeg_decay", 100 }, { "bd_osc_coarse", 0 } };
static const Scenario bd = { bdNotes, 1, NULL, 0, bdControls, NELEM (bdControls), NULL, 0, 1.0, 0, NULL, NULL };

// ---- the plugin ----

typedef th_output Output;

static int controlIndex (const char *symbol)
{
	return th_control_index (symbol);
}

static int scaledFrame (int frame44, double rate)
{
	return (int) ((double) frame44 * rate / 44100.0 + 0.5);
}

// the scenario's control values at the start: defaults, preset, controls
static void initialControls (const Scenario *sc, float *controls)
{
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		controls[i] = eds_controls[i].def;
	}
	for (int i = 0; i < sc->numPreset; i++)
	{
		controls[controlIndex (sc->preset[i].symbol)] = sc->preset[i].value;
	}
	for (int i = 0; i < sc->numControls; i++)
	{
		controls[controlIndex (sc->controls[i].symbol)] = sc->controls[i].value;
	}
}

// renders the scenario with the plugin, in this process
static void renderPlugin (const Scenario *sc, double rate, int block, Output *out)
{
	const LV2_Descriptor *d = th_load (pluginPath, EDS_URI);
	const LV2_Feature *const *features = th_features ();
	LV2_Handle h = d->instantiate (d, rate, "", features);
	if (!h)
	{
		fprintf (stderr, "instantiate failed at %.0f Hz\n", rate);
		exit (2);
	}

	float controls[EDS_NUM_CONTROLS];
	initialControls (sc, controls);
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		d->connect_port (h, (uint32_t) (EDS_PORT_CONTROL0 + i), &controls[i]);
	}

	static uint64_t seqBuf[8192];
	LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *) seqBuf;
	d->connect_port (h, EDS_PORT_MIDI_IN, seq);
	float *outL = malloc (sizeof (float) * (size_t) block), *outR = malloc (sizeof (float) * (size_t) block);
	d->connect_port (h, EDS_PORT_OUT_L, outL);
	d->connect_port (h, EDS_PORT_OUT_R, outR);
	d->activate (h);

	const int frames = out->frames;
	double cpu = 0;
	for (int pos = 0; pos < frames; pos += block)
	{
		const int n = pos + block <= frames ? block : frames - pos;
		for (int i = 0; i < sc->numChanges; i++)
		{
			const int f = scaledFrame (sc->changes[i].frame44, rate);
			if (f >= pos && f < pos + n)
			{
				controls[controlIndex (sc->changes[i].control.symbol)] = sc->changes[i].control.value;
			}
		}
		th_seq_clear (seq);
		int nextMsg = 0;
		for (int i = 0; i < sc->numNotes; i++)
		{
			const int f = scaledFrame (sc->notes[i].frame44, rate);
			if (f >= pos && f < pos + n)
			{
				// the CCs before this note first (the sequence is in time order)
				for (; nextMsg < numCcMsgs && scaledFrame (ccMsgs[nextMsg].frame44, rate) < f; nextMsg++)
				{
					const int fm = scaledFrame (ccMsgs[nextMsg].frame44, rate);
					if (fm >= pos)
					{
						th_seq_midi (seq, sizeof seqBuf, fm - pos, ccMsgs[nextMsg].msg, 3);
					}
				}
				const uint8_t msg[3] = { (uint8_t) (LV2_MIDI_MSG_NOTE_ON | sc->notes[i].channel), sc->notes[i].note,
							 sc->notes[i].velocity };
				if (sc->tempoEvent && i == 0)
				{
					th_seq_tempo (seq, sizeof seqBuf, f - pos, 120.f);
				}
				th_seq_midi (seq, sizeof seqBuf, f - pos, msg, 3);
			}
		}
		for (; nextMsg < numCcMsgs; nextMsg++)
		{
			const int fm = scaledFrame (ccMsgs[nextMsg].frame44, rate);
			if (fm >= pos && fm < pos + n)
			{
				th_seq_midi (seq, sizeof seqBuf, fm - pos, ccMsgs[nextMsg].msg, 3);
			}
		}
		const double t0 = th_now ();
		d->run (h, (uint32_t) n);
		cpu += th_now () - t0;
		memcpy (out->data + pos, outL, sizeof (float) * (size_t) n);
		memcpy (out->data + frames + pos, outR, sizeof (float) * (size_t) n);
	}
	out->cpuSeconds = cpu;

	// one instance per process
	const int messages = th_log_messages;
	LV2_Handle second = d->instantiate (d, rate, "", features);
	out->ok = second == NULL && th_log_messages == messages + 1;
	if (second)
	{
		d->cleanup (second);
	}
	d->deactivate (h);
	d->cleanup (h);
	// after cleanup, a new instance is allowed
	LV2_Handle again = d->instantiate (d, rate, "", features);
	out->ok = out->ok && again != NULL;
	if (again)
	{
		d->cleanup (again);
	}
	free (outL);
	free (outR);
}

// ---- the reference: the engine driven directly, 44.1 kHz ----

static int roundValue (float v)
{
	return (int) lrintf (v);
}

// the controls as engine settings: each slot's engine switched
// (lxr_engine_set_engine(), which resets the slot), then every parameter
// of the engines the slots run set where the engine has another value; the
// modulation targets last
static void engineApply (const float *controls)
{
	for (int slot = 0; slot < EDS_SLOTS; slot++)
	{
		const uint8_t e = (uint8_t) roundValue (controls[eds_engine_controls[slot]]);
		if (e != lxr_engine_get_engine ((uint8_t) slot))
		{
			lxr_engine_set_engine ((uint8_t) slot, e);
		}
	}
	uint8_t engines[EDS_SLOTS + 1] = { 0 };
	lxr_engine_get_engines (engines);
	for (int pass = 0; pass <= 1; pass++)
	{
		for (int i = 0; i < EDS_NUM_CONTROLS; i++)
		{
			const eds_control *c = &eds_controls[i];
			if (c->type != EDS_CTL_PARAM || c->target != pass || !(c->engines >> engines[c->slot] & 1))
			{
				continue;
			}
			const int v = roundValue (controls[i]) + c->offset;
			if (v != lxr_engine_get_parameter (c->param))
			{
				lxr_engine_set_parameter (c->param, (uint8_t) v);
			}
		}
	}
}

// the GM drum map (as src/notemap.h, written out again)
static int gmTrack (uint8_t note)
{
	switch (note)
	{
	case 35: case 36: return 0;
	case 45: case 47: case 48: case 50: return 1;
	case 41: case 43: return 2;
	case 37: case 38: case 39: case 40: return 3;
	case 49: case 51: case 52: case 55: case 57: case 59: return 4;
	case 42: case 44: return 5;
	case 46: return 6;
	default: return -1;
	}
}

// the plugin starts a trigger at the next engine block: the block
// ceil (frame / 64); control changes apply before the block at their frame
static void renderEngine (const Scenario *sc, Output *out)
{
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	float controls[EDS_NUM_CONTROLS];
	initialControls (sc, controls);
	if (sc->kit)
	{
		lxr_engine_load_kit_engines (sc->kit, lxr_engine_kit_size (), sc->kitEngines);
	}
	else
	{
		engineApply (controls);
	}
	const int frames = out->frames;
	for (int b = 0; b * LXR_ENGINE_BLOCK < frames; b++)
	{
		int changed = 0;
		for (int i = 0; i < sc->numChanges; i++)
		{
			if (sc->changes[i].frame44 == b * LXR_ENGINE_BLOCK)
			{
				controls[controlIndex (sc->changes[i].control.symbol)] = sc->changes[i].control.value;
				changed = 1;
			}
		}
		if (changed)
		{
			engineApply (controls);
		}
		const int multi = roundValue (controls[EDS_MIDI_MODE_CONTROL]) == 1;
		const int lxrCc = roundValue (controls[EDS_LXR_CC_CONTROL]) == 1;
		// the CCs of this block (none share a block with a note in the
		// scenarios): straight to the engine's MIDI implementation, without
		// the engine selection NRPN (98)
		static uint16_t nrpn;
		for (int i = 0; i < numCcMsgs && lxrCc; i++)
		{
			const int f = ccMsgs[i].frame44;
			const uint8_t *m = ccMsgs[i].msg;
			if (f <= b * LXR_ENGINE_BLOCK && f > (b - 1) * LXR_ENGINE_BLOCK && (m[0] & 0xF0) == 0xB0)
			{
				if (m[1] == 99)
					nrpn = (uint16_t) ((nrpn & 0x7F) | m[2] << 7);
				else if (m[1] == 98)
					nrpn = (uint16_t) ((nrpn & ~0x7F) | m[2]);
				else if (m[1] == 6 && nrpn == 98)
					continue;
				lxr_engine_midi_cc (m[1], m[2]);
			}
		}
		for (int i = 0; i < sc->numNotes; i++)
		{
			const Note *nt = &sc->notes[i];
			const int f = nt->frame44;
			if (f <= b * LXR_ENGINE_BLOCK && f > (b - 1) * LXR_ENGINE_BLOCK)
			{
				int track = gmTrack (nt->note);
				uint8_t pitch = LXR_ENGINE_NOTE;
				if (multi && nt->channel != 9)
				{
					track = nt->channel < 7 ? nt->channel : -1;
					pitch = nt->note;
				}
				if (track >= 0)
				{
					lxr_engine_trigger ((uint8_t) track, nt->velocity, pitch);
				}
			}
		}
		int16_t l[LXR_ENGINE_BLOCK] = { 0 }, r[LXR_ENGINE_BLOCK] = { 0 };
		lxr_engine_render (l, r);
		for (int i = 0; i < LXR_ENGINE_BLOCK && b * LXR_ENGINE_BLOCK + i < frames; i++)
		{
			out->data[b * LXR_ENGINE_BLOCK + i] = (float) l[i] / 32768.f;
			out->data[frames + b * LXR_ENGINE_BLOCK + i] = (float) r[i] / 32768.f;
		}
	}
	out->ok = 1;
}

// a render in a child process, the output in shared memory
typedef struct
{
	const Scenario *sc;
	double rate;
	int block;
} Job;

static void renderJob (void *arg, th_output *out)
{
	const Job *job = arg;
	if (job->block)
		renderPlugin (job->sc, job->rate, job->block, out);
	else
		renderEngine (job->sc, out);
}

static Output *render (const Scenario *sc, double rate, int block)
{
	Output *out = th_output_new ((int) (sc->seconds * rate));
	Job job = { sc, rate, block };
	char what[64];
	snprintf (what, sizeof what, "at %.0f Hz, block %d", rate, block);
	th_fork (renderJob, &job, out, what);
	return out;
}

static void release (Output *o)
{
	th_output_free (o);
}

// ---- analysis ----

static double rms (const Output *o, int from, int to)
{
	return th_rms (o, from, to);
}

static void sanity (const Output *o, const char *what)
{
	const int nan = th_count_nan (o), clip = th_count_clipped (o);
	const float peak = th_peak (o);
	CHECK (!nan, "%s: %d NaN / inf samples", what, nan);
	CHECK (clip < o->frames / 1000, "%s: %d clipped samples", what, clip);
	CHECK (peak > 0.01f, "%s: silent (peak %f)", what, peak);
	CHECK (o->ok, "%s: a second instance was not refused, or a new one after cleanup failed", what);
}

// the plugin at 44.1 kHz (block 256) == the engine
static void checkAgainstEngine (const Scenario *sc, const char *what, int needSound)
{
	Output *ref = render (sc, 44100, 0);
	Output *o = render (sc, 44100, 256);
	if (needSound)
	{
		sanity (o, what);
	}
	CHECK (th_identical (o, ref), "%s: not the engine's output (first difference at frame %d)", what,
	       th_first_difference (o, ref));
	release (o);
	release (ref);
}

// ---- engines per slot ----

static const char *const engineNames[4] = { "drum", "snare", "cymbal", "hi-hat" };
static const uint8_t trackNotes[7] = { 36, 45, 41, 38, 49, 42, 46 };

// a value of a control (port value) away from its default, in its range
static float otherValue (int i, int k)
{
	const eds_control *c = &eds_controls[i];
	const int span = c->max - c->min + 1;
	int x = c->min + (i * 37 + k * 11 + 5) % span;
	if (x - c->offset == (int) c->def)
	{
		x = c->min + (x - c->min + 1) % span;
	}
	return (float) (x - c->offset);
}

static const char *symbolOf (int i)
{
	return th_control_symbol (i);
}

// every engine on every slot, with values on the engine's own ports
static void checkEngines (void)
{
	for (int slot = 0; slot < EDS_SLOTS; slot++)
	{
		for (int e = 0; e < 4; e++)
		{
			Control controls[64];
			int n = 0;
			controls[n++] = (Control) { symbolOf (eds_engine_controls[slot]), (float) e };
			for (int i = 0; i < EDS_NUM_CONTROLS && n < NELEM (controls); i++)
			{
				const eds_control *c = &eds_controls[i];
				if (c->type == EDS_CTL_PARAM && c->slot == slot && c->engines != EDS_ALL_ENGINES && c->engines >> e & 1)
				{
					controls[n++] = (Control) { symbolOf (i), otherValue (i, e) };
				}
			}
			// the slot's track (slot 6: closed and open), a few velocities
			Note notes[8];
			int nn = 0;
			const int tracks[2] = { slot, slot == EDS_SLOTS - 1 ? 6 : slot };
			const int times[4] = { 0, 6000, 13000, 20000 }, velocities[4] = { 127, 60, 100, 30 };
			for (int k = 0; k < 4; k++)
			{
				notes[nn++] = (Note) { times[k], trackNotes[tracks[k & 1]], (uint8_t) velocities[k], 9 };
			}
			const Scenario sc = { notes, nn, NULL, 0, controls, n, NULL, 0, 0.7, 0, NULL, NULL };
			char what[96];
			snprintf (what, sizeof what, "slot %d with the %s engine", slot + 1, engineNames[e]);
			checkAgainstEngine (&sc, what, 1);
		}
	}

	// switching while playing: the bass drum to the snare engine and back,
	// the hi-hat to the drum engine (the open hi-hat on it), with controls
	// changed at the same time
	static const Note notes[] = {
		{ 0, 36, 127, 9 }, { 4000, 46, 100, 9 }, { 11000, 36, 120, 9 }, { 15000, 42, 90, 9 }, { 15100, 46, 90, 9 },
		{ 25000, 36, 110, 9 }, { 26000, 46, 127, 9 }, { 34000, 36, 127, 9 },
	};
	static const Change changes[] = {
		{ 10240, { "bd_mix_engine", 1 } }, { 10240, { "bd_noise_repeat", 2 } }, { 10240, { "bd_ampeg_decay", 40 } },
		{ 14848, { "hh_mix_engine", 0 } }, { 14848, { "hh_ampeg_decay_open", 70 } }, { 14848, { "hh_fm_amount", 90 } },
		{ 24576, { "bd_mix_engine", 0 } }, { 24576, { "bd_fm_amount", 50 } }, { 33792, { "bd_osc_coarse", 5 } },
	};
	const Scenario sw = { notes, NELEM (notes), NULL, 0, NULL, 0, changes, NELEM (changes), 1.0, 0, NULL, NULL };
	checkAgainstEngine (&sw, "engine switches while playing", 1);
}

// the ports of the engines the slots don't run change nothing
static void checkInactive (void)
{
	static const uint8_t layouts[2][EDS_SLOTS] = { { 0, 0, 0, 1, 2, 3 }, { 1, 2, 3, 0, 3, 0 } };
	for (int l = 0; l < 2; l++)
	{
		Control base[EDS_SLOTS], all[EDS_SLOTS + EDS_NUM_CONTROLS];
		int n = 0;
		for (int s = 0; s < EDS_SLOTS; s++)
		{
			base[s] = all[n++] = (Control) { symbolOf (eds_engine_controls[s]), layouts[l][s] };
		}
		int inactive = 0;
		for (int i = 0; i < EDS_NUM_CONTROLS; i++)
		{
			const eds_control *c = &eds_controls[i];
			if (c->type == EDS_CTL_PARAM && c->slot < EDS_SLOTS && !(c->engines >> layouts[l][c->slot] & 1))
			{
				all[n++] = (Control) { symbolOf (i), otherValue (i, 3) };
				inactive++;
			}
		}
		const Scenario a = { kitNotes, NELEM (kitNotes), NULL, 0, base, EDS_SLOTS, NULL, 0, 1.5, 0, NULL, NULL };
		const Scenario b = { kitNotes, NELEM (kitNotes), NULL, 0, all, n, NULL, 0, 1.5, 0, NULL, NULL };
		Output *oa = render (&a, 44100, 256), *ob = render (&b, 44100, 256);
		char what[64];
		snprintf (what, sizeof what, "engine layout %d", l + 1);
		sanity (oa, what);
		CHECK (th_identical (oa, ob), "%s: the %d inactive engine ports change the sound (frame %d)", what, inactive,
		       th_first_difference (oa, ob));
		printf ("engine layout %d: %d inactive engine ports without effect\n", l + 1, inactive);
		release (oa);
		release (ob);
	}
}

// ---- MIDI modes ----

static void checkMidiModes (void)
{
	// Kit: the channel doesn't matter
	Note ch1[NELEM (kitNotes)];
	for (int i = 0; i < NELEM (kitNotes); i++)
	{
		ch1[i] = kitNotes[i];
		ch1[i].channel = (uint8_t) (i % 16);
	}
	const Scenario kitCh = { ch1, NELEM (ch1), NULL, 0, kitControls, NELEM (kitControls), NULL, 0, 2.0, 1, NULL, NULL };
	Output *k10 = render (&kit, 44100, 256), *kAll = render (&kitCh, 44100, 256);
	CHECK (th_identical (k10, kAll), "Kit mode: the channels change the output (frame %d)", th_first_difference (k10, kAll));

	// Multi: channel 10 is the kit
	Control multiKit[NELEM (kitControls) + 1];
	memcpy (multiKit, kitControls, sizeof kitControls);
	multiKit[NELEM (kitControls)] = (Control) { "master_midi_mode", 1 };
	const Scenario mk = { kitNotes, NELEM (kitNotes), NULL, 0, multiKit, NELEM (multiKit), NULL, 0, 2.0, 1, NULL, NULL };
	Output *m10 = render (&mk, 44100, 256);
	CHECK (th_identical (k10, m10), "Multi mode: channel 10 is not the kit (frame %d)", th_first_difference (k10, m10));
	release (k10);
	release (kAll);
	release (m10);

	// Multi: other channels than 1-7 and 10 are ignored
	Note others[16];
	int n = 0;
	for (uint8_t ch = 7; ch < 16; ch++)
	{
		if (ch != 9)
		{
			others[n++] = (Note) { 441 * ch, 36, 127, ch };
		}
	}
	const Control multi[] = { { "master_midi_mode", 1 } };
	const Scenario silent = { others, n, NULL, 0, multi, 1, NULL, 0, 0.5, 0, NULL, NULL };
	Output *s = render (&silent, 44100, 256);
	CHECK (th_peak (s) == 0.f, "Multi mode: channels 8, 9, 11-16 play (peak %f)", th_peak (s));
	release (s);

	// Multi: channels 1-7 play their tracks at the note's pitch, as the
	// engine; the open hi-hat on channel 7
	Note tracks[14];
	n = 0;
	for (uint8_t t = 0; t < 7; t++)
	{
		tracks[n++] = (Note) { 2000 * t, (uint8_t) (48 + 3 * t), 120, t };
		tracks[n++] = (Note) { 2000 * t + 15000, (uint8_t) (64 + 2 * t), 90, t };
	}
	const Scenario mt = { tracks, n, NULL, 0, multi, 1, NULL, 0, 1.0, 0, NULL, NULL };
	checkAgainstEngine (&mt, "Multi mode, channels 1-7", 1);

	// an octave up: twice the bass drum's frequency; note 60 on channel 1 =
	// the kit's bass drum
	const Control bdMulti[] = { { "bd_modeg_pitch", 0 }, { "bd_ampeg_decay", 100 }, { "bd_osc_coarse", 0 },
				    { "master_midi_mode", 1 } };
	static const Note n60[] = { { 441, 60, 127, 0 } }, n72[] = { { 441, 72, 127, 0 } };
	const Scenario s60 = { n60, 1, NULL, 0, bdMulti, NELEM (bdMulti), NULL, 0, 1.0, 0, NULL, NULL };
	const Scenario s72 = { n72, 1, NULL, 0, bdMulti, NELEM (bdMulti), NULL, 0, 1.0, 0, NULL, NULL };
	Output *bdKit = render (&bd, 44100, 512), *o60 = render (&s60, 44100, 512), *o72 = render (&s72, 44100, 512);
	CHECK (th_identical (bdKit, o60), "Multi mode: note 60 on channel 1 is not the bass drum's tuned pitch (frame %d)",
	       th_first_difference (bdKit, o60));
	const double f60 = th_frequency (o60, 44100, 0.05, 0.6), f72 = th_frequency (o72, 44100, 0.05, 0.6);
	printf ("Multi mode: bass drum note 60 %.2f Hz, note 72 %.2f Hz (ratio %.4f)\n", f60, f72, f72 / f60);
	CHECK (f60 > 20 && fabs (f72 / f60 - 2) < 0.02, "Multi mode: an octave up is %.4f times the frequency", f72 / f60);
	release (bdKit);
	release (o60);
	release (o72);
}

// ---- a constant step note folded into the tuning (the LXR project converter) ----

// A track played at note 60 + k (Multi, its channel) renders exactly as the
// kit note on channel 10 with the slot's tuning k semitones up: the coarse
// tune and the other oscillators the trigger's note moves (osc_setBaseNote():
// the drum engine's FM oscillator = FM Freq, the cymbal's and hi-hat's mod
// oscillators = Mod Osc Freq 1 / 2; the snare engine moves only its main
// oscillator). Every engine on every slot, the open hi-hat too.
static void checkTuningFold (void)
{
	static const char *const prefixes[EDS_SLOTS] = { "bd", "t1", "t2", "sd", "cy", "hh" };
	static const int shifts[] = { 3, -5, 12 };
	int checked = 0;
	for (int slot = 0; slot < EDS_SLOTS; slot++)
	{
		for (int e = 0; e < 4; e++)
		{
			for (int track = slot; track <= (slot == EDS_SLOTS - 1 ? 6 : slot); track++)
			{
				for (int s = 0; s < NELEM (shifts); s++)
				{
					const int k = shifts[s];
					// the tuned controls (port values: engine value - 60) and
					// whether the note moves them
					char sym[4][32];
					const float base[4] = { 5, 2, -3, 4 };
					const int moved[4] = { 1, e == 0, e >= 2, e >= 2 };
					snprintf (sym[0], sizeof sym[0], "%s_osc_coarse", prefixes[slot]);
					snprintf (sym[1], sizeof sym[1], "%s_fm_freq", prefixes[slot]);
					snprintf (sym[2], sizeof sym[2], "%s_modosc_freq1", prefixes[slot]);
					snprintf (sym[3], sizeof sym[3], "%s_modosc_freq2", prefixes[slot]);
					// the moved oscillators audible: FM amount, mod gains
					char amt[3][32];
					snprintf (amt[0], sizeof amt[0], "%s_fm_amount", prefixes[slot]);
					snprintf (amt[1], sizeof amt[1], "%s_modosc_gain1", prefixes[slot]);
					snprintf (amt[2], sizeof amt[2], "%s_modosc_gain2", prefixes[slot]);
					Control played[12], folded[12];
					int n = 0;
					for (int i = 0; i < 3; i++, n++)
					{
						played[n] = folded[n] = (Control) { amt[i], 80 };
					}
					played[n] = folded[n] = (Control) { "master_midi_mode", 1 };
					n++;
					played[n] = folded[n] = (Control) { symbolOf (eds_engine_controls[slot]), (float) e };
					n++;
					for (int i = 0; i < 4; i++, n++)
					{
						played[n] = (Control) { sym[i], base[i] };
						folded[n] = (Control) { sym[i], base[i] + (moved[i] ? (float) k : 0.f) };
					}
					const Note np[] = { { 0, (uint8_t) (60 + k), 127, (uint8_t) track },
							    { 9000, (uint8_t) (60 + k), 70, (uint8_t) track } };
					const Note nf[] = { { 0, trackNotes[track], 127, 9 }, { 9000, trackNotes[track], 70, 9 } };
					const Scenario sp = { np, 2, NULL, 0, played, n, NULL, 0, 0.5, 0, NULL, NULL };
					const Scenario sf = { nf, 2, NULL, 0, folded, n, NULL, 0, 0.5, 0, NULL, NULL };
					Output *op = render (&sp, 44100, 256), *of = render (&sf, 44100, 256);
					CHECK (th_peak (op) > 0.001f, "tuning fold: track %d (%s engine) silent", track + 1, engineNames[e]);
					CHECK (th_identical (op, of),
					       "tuning fold: track %d, %s engine, note %d is not the kit note with the tuning %+d "
					       "(first difference at frame %d)",
					       track + 1, engineNames[e], 60 + k, k, th_first_difference (op, of));
					release (op);
					release (of);
					checked++;
				}
			}
		}
	}
	printf ("tuning fold: %d cases (every engine on every track, shifts +3, -5, +12): note 60 + k = tuning + k, "
		"bit exact\n", checked);
}

// ---- LXR CC ----

// bass drum hits, CCs and NRPNs between them (never in a hit's engine
// block): CC 51 = parameter 50 (bd_ampeg_decay), CC 38 = 37 (cutoff), NRPN
// 69 = 197 (bd_trans_volume), NRPN 0 = 128 (bd_filter_drive), on several
// channels; CC 0 / 1 (no parameter in the engine)
static const Note ccNotes[] = { { 0, 36, 120, 9 }, { 8820, 36, 120, 9 }, { 17640, 36, 120, 9 }, { 26460, 36, 120, 9 },
				{ 35280, 36, 120, 9 }, { 44100, 38, 120, 9 } };
static const Msg ccLocks[] = {
	{ 4410, { 0xB4, 51, 90 } },  { 4500, { 0xB0, 38, 40 } },	// decay, cutoff (channels 5, 1)
	{ 13230, { 0xB9, 99, 0 } }, { 13231, { 0xB9, 98, 69 } }, { 13232, { 0xB9, 6, 110 } },	// NRPN 69 (transient volume)
	{ 22050, { 0xBF, 51, 5 } },  { 22100, { 0xB2, 0, 3 } }, { 22150, { 0xB2, 1, 100 } },	// decay short; bank, mod wheel
	{ 30870, { 0xB0, 99, 0 } }, { 30871, { 0xB0, 98, 0 } }, { 30872, { 0xB0, 6, 127 } },	// NRPN 0 (filter drive)
	{ 30900, { 0xB3, 38, 127 } },
};
// the engine selection (NRPN 98: slot 1 to the snare engine) between the
// same hits: dropped (the engine selector ports own it)
static const Msg ccEngine[] = {
	{ 4410, { 0xB0, 99, 0 } }, { 4411, { 0xB0, 98, 98 } }, { 4412, { 0xB0, 6, 1 } },
	{ 13230, { 0xB0, 6, 2 } },	// data entry again: still NRPN 98
};

static void checkLxrCc (void)
{
	static const Control off[] = { { "bd_ampeg_decay", 40 } };
	static const Control on[] = { { "bd_ampeg_decay", 40 }, { "master_lxr_cc", 1 } };
	const Scenario sOff = { ccNotes, NELEM (ccNotes), NULL, 0, off, NELEM (off), NULL, 0, 1.2, 0, NULL, NULL };
	const Scenario sOn = { ccNotes, NELEM (ccNotes), NULL, 0, on, NELEM (on), NULL, 0, 1.2, 0, NULL, NULL };

	// without CCs: the switch alone changes nothing
	ccMsgs = NULL;
	numCcMsgs = 0;
	Output *plain = render (&sOff, 44100, 256), *plainOn = render (&sOn, 44100, 256);
	CHECK (th_identical (plain, plainOn), "LXR CC: the switch alone changes the output (frame %d)",
	       th_first_difference (plain, plainOn));

	// off: the CCs and NRPNs change nothing
	ccMsgs = ccLocks;
	numCcMsgs = NELEM (ccLocks);
	Output *o = render (&sOff, 44100, 256);
	CHECK (th_identical (plain, o), "LXR CC off: the CCs change the output (frame %d)", th_first_difference (plain, o));
	release (o);

	// on: as the engine driven by lxr_engine_midi_cc(), for several host
	// blocks; and audibly different from off
	Output *ref = render (&sOn, 44100, 0);
	static const int blocks[] = { 1, 64, 256, 1001, 4096 };
	for (int i = 0; i < NELEM (blocks); i++)
	{
		o = render (&sOn, 44100, blocks[i]);
		CHECK (th_identical (o, ref), "LXR CC on, block %d: not the engine with lxr_engine_midi_cc() (frame %d)",
		       blocks[i], th_first_difference (o, ref));
		release (o);
	}
	const int diff = th_first_difference (ref, plain);
	CHECK (diff > 4410 && diff < 4410 + 2 * 8820, "LXR CC on: the first CC isn't heard at the next hit (first difference "
	       "at frame %d)", diff);
	// the NRPNs: the renders differ from one with the CCs only
	static const Msg ccOnly[] = { { 4410, { 0xB4, 51, 90 } }, { 4500, { 0xB0, 38, 40 } }, { 22050, { 0xBF, 51, 5 } },
				      { 30900, { 0xB3, 38, 127 } } };
	ccMsgs = ccOnly;
	numCcMsgs = NELEM (ccOnly);
	Output *noNrpn = render (&sOn, 44100, 256);
	const int nrpnDiff = th_first_difference (ref, noNrpn);
	CHECK (nrpnDiff > 13230 && nrpnDiff < 17640 + 4410, "LXR CC on: NRPN 69 isn't heard at the next hit (first difference "
	       "at frame %d)", nrpnDiff);
	printf ("LXR CC: first CC heard at frame %d, first NRPN at %d\n", diff, nrpnDiff);
	release (noNrpn);
	release (ref);

	// the engine selection NRPN is dropped
	ccMsgs = ccEngine;
	numCcMsgs = NELEM (ccEngine);
	o = render (&sOn, 44100, 256);
	CHECK (th_identical (plain, o), "LXR CC on: NRPN 98 (engine selection) changes the output (frame %d)",
	       th_first_difference (plain, o));
	release (o);
	release (plain);
	release (plainOn);
	ccMsgs = NULL;
	numCcMsgs = 0;
}

// ---- presets ----

#define MAX_PRESETS	32

typedef struct
{
	char label[64], bank[256], file[512];
	int numValues;
	Control values[EDS_NUM_CONTROLS];
	char symbols[EDS_NUM_CONTROLS][64];
} Preset;

static Preset presets[MAX_PRESETS];
static int numPresets;

// the presets of a TTL file as tools/gen_ttl.c and tools/snd2lv2.py write
// them: "a pset:Preset", rdfs:label, pset:bank, then lv2:symbol / pset:value
static void parsePresets (const char *path)
{
	FILE *f = fopen (path, "r");
	if (!f)
	{
		perror (path);
		exit (2);
	}
	char line[1024];
	Preset *p = NULL;
	char symbol[64] = "";
	while (fgets (line, sizeof line, f))
	{
		char text[256];
		float v;
		if (strstr (line, "a pset:Preset"))
		{
			if (numPresets == MAX_PRESETS)
			{
				fprintf (stderr, "%s: too many presets\n", path);
				exit (2);
			}
			p = &presets[numPresets++];
			memset (p, 0, sizeof *p);
			snprintf (p->file, sizeof p->file, "%s", path);
		}
		else if (p && sscanf (line, " rdfs:label \"%63[^\"]\"", text) == 1)
			snprintf (p->label, sizeof p->label, "%.63s", text);
		else if (p && sscanf (line, " pset:bank <%255[^>]>", text) == 1)
			snprintf (p->bank, sizeof p->bank, "%s", text);
		else if (p && sscanf (line, " lv2:symbol \"%63[a-z0-9_]\"", symbol) == 1)
			continue;
		else if (p && *symbol && sscanf (line, " pset:value %f", &v) == 1 && p->numValues < EDS_NUM_CONTROLS)
		{
			snprintf (p->symbols[p->numValues], sizeof p->symbols[0], "%s", symbol);
			p->values[p->numValues].symbol = p->symbols[p->numValues];
			p->values[p->numValues].value = v;
			p->numValues++;
			*symbol = '\0';
		}
	}
	fclose (f);
}

// a preset has a value for every kit port and only those, in range
static void checkPresetPorts (const Preset *p)
{
	int count = 0;
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		count += eds_controls[i].kit;
	}
	CHECK (p->numValues == count, "preset %s: %d values, %d kit ports", p->label, p->numValues, count);
	for (int k = 0; k < p->numValues; k++)
	{
		const int i = controlIndex (p->values[k].symbol);
		const eds_control *c = &eds_controls[i];
		const int x = roundValue (p->values[k].value) + c->offset;
		CHECK (c->kit, "preset %s: %s is not a kit port", p->label, p->values[k].symbol);
		CHECK (x >= c->min && x <= c->max, "preset %s: %s = %g out of range", p->label, p->values[k].symbol,
		       p->values[k].value);
	}
}

static void checkFactoryPresets (void)
{
	char path[4096];
	snprintf (path, sizeof path, "%s", pluginPath);
	char *slash = strrchr (path, '/');
	snprintf (slash ? slash + 1 : path, sizeof path - (size_t) (slash ? slash + 1 - path : 0), "presets.ttl");
	numPresets = 0;
	parsePresets (path);
	CHECK (numPresets >= 2, "%d factory presets", numPresets);
	Output *defaults = render (&kit, 44100, 256);
	for (int k = 0; k < numPresets; k++)
	{
		const Preset *p = &presets[k];
		checkPresetPorts (p);
		const Scenario sc = { kitNotes, NELEM (kitNotes), p->values, p->numValues, NULL, 0, NULL, 0, 2.0, 1, NULL, NULL };
		char what[96];
		snprintf (what, sizeof what, "factory preset %.63s", p->label);
		checkAgainstEngine (&sc, what, 1);
		if (!strcmp (p->label, "Init"))
		{
			const Scenario init = { kitNotes, NELEM (kitNotes), p->values, p->numValues, kitControls,
						NELEM (kitControls), NULL, 0, 2.0, 1, NULL, NULL };
			Output *o = render (&init, 44100, 256);
			CHECK (th_identical (o, defaults), "factory preset Init: not the defaults (frame %d)",
			       th_first_difference (o, defaults));
			release (o);
		}
		printf ("factory preset %s: %d values\n", p->label, p->numValues);
	}
	release (defaults);
}

// ---- the kit converter ----

#define KIT_NAME	8

typedef struct
{
	const char *dir, *file, *name;
	uint8_t engines[EDS_SLOTS];
	int withBlock;		// the OpenElixir-02 engine block (else: an LXR kit, stock engines)
	int numParams;		// in the file (fewer: an older LXR kit), 0: all
	unsigned seed;
	uint8_t params[256];	// written
} Kit;

static Kit kits[] = {
	{ "SYNTHKITS", "00-USER1.snd", "USER 1", { 1, 2, 3, 0, 0, 1 }, 1, 0, 1, { 0 } },
	{ "SYNTHKITS/BANKA", "01-METAL.snd", "METAL", { 3, 2, 2, 3, 2, 0 }, 1, 0, 2, { 0 } },
	{ "SYNTHKITS/BANKA", "02-LXR.SND", "LXR KIT", { 0, 0, 0, 1, 2, 3 }, 0, 0, 3, { 0 } },
	{ "SYNTHKITS/BANKA", "05-OLD.SND", "OLD KIT", { 0, 0, 0, 1, 2, 3 }, 0, 205, 4, { 0 } },
};

static unsigned lcg (unsigned *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 16;
}

// a kit: the init kit, the engines switched, random values on the ports of
// those engines (as the converter can express them: the audio outputs on
// St1 / St2, volumes from 60 so it is not silent); written as a .snd
static void makeKit (void *arg, th_output *out)
{
	Kit *k = arg;
	lxr_engine_init ();
	for (int s = 0; s < EDS_SLOTS; s++)
	{
		lxr_engine_set_engine ((uint8_t) s, k->engines[s]);
	}
	unsigned seed = k->seed;
	for (int pass = 0; pass <= 1; pass++)
	{
		for (int i = 0; i < EDS_NUM_CONTROLS; i++)
		{
			const eds_control *c = &eds_controls[i];
			if (c->type != EDS_CTL_PARAM || !c->kit || c->target != pass
			    || (c->slot < EDS_SLOTS && !(c->engines >> k->engines[c->slot] & 1)))
			{
				continue;
			}
			int x = c->min + (int) (lcg (&seed) % (unsigned) (c->max - c->min + 1));
			const char *sym = symbolOf (i);
			if (strstr (sym, "_out"))
				x = x & 1;
			else if (strstr (sym, "_volume") && x < 60)
				x += 60;
			lxr_engine_set_parameter (c->param, (uint8_t) x);
		}
	}
	const uint16_t size = lxr_engine_kit_size ();
	uint8_t data[KIT_NAME + 256 + LXR_ENGINE_BLOCK_SIZE];
	memset (data, 0, sizeof data);
	memcpy (data, k->name, strlen (k->name) < KIT_NAME ? strlen (k->name) : KIT_NAME);
	lxr_engine_get_kit (data + KIT_NAME, size);
	uint8_t engines[EDS_SLOTS];
	lxr_engine_get_engines (engines);
	size_t length = KIT_NAME + size;
	if (k->numParams)
	{
		// an older kit: the parameters after numParams are 0 when loaded
		length = KIT_NAME + (size_t) k->numParams;
		memset (data + length, 0, size - (size_t) k->numParams);
	}
	if (k->withBlock)
	{
		length += lxr_engine_encode_engines (engines, data + length);
	}
	// for the parent: the parameters at byte 0, the file at byte 512 (its size in ok)
	memcpy (out->data, data + KIT_NAME, size);
	memcpy ((uint8_t *) out->data + 512, data, length);
	out->ok = (int) length;
}

static void writeFile (const char *path, const void *data, size_t size)
{
	FILE *f = fopen (path, "wb");
	if (!f || fwrite (data, 1, size, f) != size)
	{
		perror (path);
		exit (2);
	}
	fclose (f);
}

static void mkdirs (const char *path)
{
	char p[4096];
	snprintf (p, sizeof p, "%s", path);
	for (char *c = p + 1; *c; c++)
	{
		if (*c == '/')
		{
			*c = '\0';
			mkdir (p, 0755);
			*c = '/';
		}
	}
	mkdir (p, 0755);
}

static void checkConverter (const char *converter, const char *work)
{
	char cmd[8192], path[4096];
	snprintf (cmd, sizeof cmd, "rm -rf '%s/SYNTHKITS' '%s/kits.lv2'", work, work);
	CHECK (system (cmd) == 0, "%s failed", cmd);
	for (int i = 0; i < NELEM (kits); i++)
	{
		Kit *k = &kits[i];
		th_output *o = th_output_new (1024);
		th_fork (makeKit, k, o, "of a kit");
		memcpy (k->params, o->data, lxr_engine_kit_size ());
		snprintf (path, sizeof path, "%s/%s", work, k->dir);
		mkdirs (path);
		snprintf (path, sizeof path, "%s/%s/%s", work, k->dir, k->file);
		writeFile (path, (const uint8_t *) o->data + 512, (size_t) o->ok);
		th_output_free (o);
	}
	// a placeholder kit (left out) and a file that isn't a kit
	snprintf (path, sizeof path, "%s/SYNTHKITS/BANKA/03-INITK.snd", work);
	uint8_t init[KIT_NAME + 256] = "Initkit";
	writeFile (path, init, KIT_NAME + lxr_engine_kit_size ());
	snprintf (path, sizeof path, "%s/SYNTHKITS/BANKA/04-SHORT.snd", work);
	writeFile (path, init, 100);

	snprintf (cmd, sizeof cmd, "%s -o '%s/kits.lv2' '%s/SYNTHKITS' %s", converter, work, work, th_verbose ? "" : "2>/dev/null");
	const int rc = system (cmd);
	CHECK (rc == 0, "%s: exit status %d", cmd, rc);

	// the manifest: two banks, a preset per kit
	snprintf (path, sizeof path, "%s/kits.lv2/manifest.ttl", work);
	FILE *f = fopen (path, "r");
	CHECK (f != NULL, "no %s", path);
	int banks = 0, presetFiles = 0;
	char files[8][512];
	if (f)
	{
		char line[1024], file[512];
		while (fgets (line, sizeof line, f))
		{
			banks += strstr (line, "a pset:Bank") != NULL;
			if (sscanf (line, " rdfs:seeAlso <%511[^>]>", file) == 1 && presetFiles < 8)
			{
				snprintf (files[presetFiles++], sizeof files[0], "%s", file);
			}
		}
		fclose (f);
	}
	CHECK (banks == 2, "the converter wrote %d banks", banks);
	CHECK (presetFiles == NELEM (kits), "the converter wrote %d presets", presetFiles);

	numPresets = 0;
	for (int i = 0; i < presetFiles; i++)
	{
		snprintf (path, sizeof path, "%.2000s/kits.lv2/%.511s", work, files[i]);
		parsePresets (path);
	}
	CHECK (numPresets == NELEM (kits), "%d presets parsed", numPresets);
	for (int k = 0; k < numPresets; k++)
	{
		const Preset *p = &presets[k];
		checkPresetPorts (p);
		// which kit: by its name in the label
		const Kit *kit = NULL;
		for (int i = 0; i < NELEM (kits); i++)
		{
			if (strstr (p->label, kits[i].name))
			{
				kit = &kits[i];
			}
		}
		CHECK (kit != NULL, "converted preset %s: no such kit", p->label);
		if (!kit)
		{
			continue;
		}
		const Scenario plugin = { kitNotes, NELEM (kitNotes), p->values, p->numValues, NULL, 0, NULL, 0, 2.0, 1, NULL, NULL };
		const Scenario engine = { kitNotes, NELEM (kitNotes), NULL, 0, NULL, 0, NULL, 0, 2.0, 1, kit->params,
					  kit->withBlock ? kit->engines : NULL };
		Output *o = render (&plugin, 44100, 256), *ref = render (&engine, 44100, 0);
		char what[400];
		snprintf (what, sizeof what, "converted kit %.63s (bank %.255s)", p->label, strrchr (p->bank, '#') ? strrchr (p->bank, '#') + 1 : p->bank);
		// random values: loud, maybe clipping; not NaN, not silent
		CHECK (!th_count_nan (o) && th_peak (o) > 0.01f, "%s: NaN or silent", what);
		CHECK (th_identical (o, ref), "%s: not the engine with the kit loaded (first difference at frame %d)", what,
		       th_first_difference (o, ref));
		printf ("%s: %d values, renders as the kit\n", what, p->numValues);
		release (o);
		release (ref);
	}
}

// ---- main ----

static void checkRates (void)
{
	// 44.1 kHz: bit exact against the engine
	Output *ref = render (&kit, 44100, 0);
	const int blocks44[] = { 1, 63, 64, 256, 1000, 4096 };
	for (unsigned i = 0; i < sizeof blocks44 / sizeof blocks44[0]; i++)
	{
		Output *o = render (&kit, 44100, blocks44[i]);
		char what[64];
		snprintf (what, sizeof what, "44.1 kHz, block %d", blocks44[i]);
		sanity (o, what);
		CHECK (th_identical (o, ref), "%s: not the engine's output (first difference at frame %d)", what,
		       th_first_difference (o, ref));
		if (blocks44[i] == 256)
		{
			printf ("44.1 kHz, block 256: %.1f ns per sample (%.2f %% of real time)\n",
				1e9 * o->cpuSeconds / o->frames, 100 * o->cpuSeconds / kit.seconds);
		}
		release (o);
	}

	// 48 kHz: block sizes give the same signal
	Output *ref48 = render (&kit, 48000, 960);
	sanity (ref48, "48 kHz, block 960");
	printf ("48 kHz, block 960: %.1f ns per sample (%.2f %% of real time)\n", 1e9 * ref48->cpuSeconds / ref48->frames,
		100 * ref48->cpuSeconds / kit.seconds);
	const int blocks48[] = { 1, 64, 480, 4096, 333, 1001 };
	for (unsigned i = 0; i < sizeof blocks48 / sizeof blocks48[0]; i++)
	{
		Output *o = render (&kit, 48000, blocks48[i]);
		char what[64];
		snprintf (what, sizeof what, "48 kHz, block %d", blocks48[i]);
		sanity (o, what);
		CHECK (th_identical (o, ref48), "%s: differs from block 960 (first difference at frame %d)", what,
		       th_first_difference (o, ref48));
		release (o);
	}
	// the level of the kit as at 44.1 kHz
	const double l44 = rms (ref, 0, ref->frames), l48 = rms (ref48, 0, ref48->frames);
	printf ("kit level: 44.1 kHz %.2f dBFS, 48 kHz %.2f dBFS\n", 20 * log10 (l44), 20 * log10 (l48));
	CHECK (fabs (20 * log10 (l48 / l44)) < 0.5, "48 kHz level differs: %.2f dB", 20 * log10 (l48 / l44));
	release (ref48);
	release (ref);

	// the bass drum's pitch and level at 48 kHz
	Output *bd44 = render (&bd, 44100, 512);
	Output *bd48 = render (&bd, 48000, 512);
	const double f44 = th_frequency (bd44, 44100, 0.05, 0.6), f48 = th_frequency (bd48, 48000, 0.05, 0.6);
	printf ("bass drum: %.2f Hz at 44.1 kHz, %.2f Hz at 48 kHz\n", f44, f48);
	CHECK (f44 > 20 && fabs (f48 / f44 - 1) < 0.01, "bass drum pitch at 48 kHz: %.2f Hz, at 44.1 kHz %.2f Hz", f48, f44);
	const double r44 = rms (bd44, 0, (int) (0.5 * 44100)), r48 = rms (bd48, 0, (int) (0.5 * 48000));
	CHECK (fabs (20 * log10 (r48 / r44)) < 0.5, "bass drum level at 48 kHz: %.2f dB", 20 * log10 (r48 / r44));

	// other rates
	const double rates[] = { 22050, 32000, 88200, 96000, 192000, 47999 };
	for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++)
	{
		Output *o = render (&bd, rates[i], 777);
		char what[64];
		snprintf (what, sizeof what, "%.0f Hz", rates[i]);
		sanity (o, what);
		const double f = th_frequency (o, rates[i], 0.05, 0.6);
		const double r = rms (o, 0, (int) (0.5 * rates[i]));
		printf ("bass drum at %.0f Hz: %.2f Hz, level %+.2f dB\n", rates[i], f, 20 * log10 (r / r44));
		CHECK (fabs (f / f44 - 1) < 0.01, "%s: pitch %.2f Hz", what, f);
		CHECK (fabs (20 * log10 (r / r44)) < 0.5, "%s: level %.2f dB", what, 20 * log10 (r / r44));
		release (o);
	}
	release (bd44);
	release (bd48);
}

int main (int argc, char **argv)
{
	const char *converter = NULL, *work = NULL;
	int args = argc;
	th_verbose = argc > 2 && !strcmp (argv[argc - 1], "-v");
	args -= th_verbose;
	if (args == 5 && !strcmp (argv[2], "--presets"))
	{
		converter = argv[3];
		work = argv[4];
	}
	else if (args != 2)
	{
		fprintf (stderr, "usage: render_test PATH/edrumsynth.so [--presets CONVERTER WORKDIR] [-v]\n");
		return 2;
	}
	pluginPath = argv[1];
	// the control port symbols from the bundle's TTL (next to the binary)
	char ttl[4096];
	snprintf (ttl, sizeof ttl, "%s", pluginPath);
	char *slash = strrchr (ttl, '/');
	snprintf (slash ? slash + 1 : ttl, sizeof ttl - (size_t) (slash ? slash + 1 - ttl : 0), "edrumsynth.ttl");
	th_load_symbols (ttl, EDS_PORT_CONTROL0, EDS_NUM_CONTROLS);

	if (converter)
	{
		checkFactoryPresets ();
		mkdirs (work);
		checkConverter (converter, work);
	}
	else
	{
		checkRates ();
		checkEngines ();
		checkInactive ();
		checkMidiModes ();
		checkTuningFold ();
		checkLxrCc ();
	}

	printf ("%s\n", th_failures ? "FAILED" : converter ? "all preset checks passed" : "all render checks passed");
	return th_failures ? 1 : 0;
}
