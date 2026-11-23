/*
 * render_test.c - a minimal LV2 host for eDrumSynth (dlopen +
 * lv2_descriptor, a urid:map, no lilv) and checks against the engine:
 *
 * - 44.1 kHz: the plugin's output is the engine's (lxr_engine_render() with
 *   the same triggers and parameters), bit exact after the int16 -> float
 *   scale, for several host block sizes;
 * - 48 kHz: no NaN, no clipping storm; the same signal for block sizes 1,
 *   64, 480, 960, 4096 and odd ones; the bass drum's pitch within 1 % of
 *   the 44.1 kHz render, its level within 0.5 dB;
 * - other rates (22.05, 32, 88.2, 96, 192 kHz and 47999 Hz: interpolated
 *   phases): sane output;
 * - a second instance in the same process is refused.
 * It also prints the CPU time per output sample (this machine, this build).
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

static const char *pluginPath;

#define CHECK TH_CHECK

// ---- the scenario ----

typedef struct
{
	int frame44;	// at 44.1 kHz; other rates: scaled
	uint8_t note, velocity;
} Note;

typedef struct
{
	const char *symbol;
	float value;
} Control;

typedef struct
{
	const Note *notes;
	int numNotes;
	const Control *controls;
	int numControls;
	double seconds;
	int tempoEvent;		// send a time:Position (120.0 bpm) with the first note
} Scenario;

static const Note kitNotes[] = {
	{ 0, 36, 127 }, { 1000, 42, 90 }, { 5513, 38, 110 }, { 11025, 46, 100 }, { 11030, 36, 64 },
	{ 16537, 45, 120 }, { 22050, 41, 80 }, { 22051, 49, 127 }, { 27563, 42, 30 }, { 33075, 39, 100 },
	{ 38587, 47, 70 }, { 44100, 36, 127 }, { 44164, 44, 50 }, { 50000, 51, 100 }, { 60000, 37, 127 },
	{ 64000, 60, 100 },	// not mapped
};
// some parameters away from the init kit: filter, pan, a velocity
// modulation target, the FX bus (delay) with the cymbal on it
static const Control kitControls[] = {
	{ "bd_filter_cutoff", 90 }, { "bd_filter_resonance", 40 }, { "sd_mix_pan", -30 },
	{ "bd_mod_vel_dest", 2 }, { "bd_mod_vel_amount", 60 }, { "fx_type", 4 }, { "fx_dry_wet", 50 },
	{ "fx_p1", 40 }, { "fx_p4", 70 }, { "cy_mix_out", 6 }, { "hh_ampeg_decay_open", 60 },
	{ "t1_osc_coarse", -10 },
};
static const Scenario kit = { kitNotes, sizeof kitNotes / sizeof kitNotes[0], kitControls,
			      sizeof kitControls / sizeof kitControls[0], 2.0, 1 };

// the bass drum alone, a steady pitch (no pitch modulation), long decay
static const Note bdNotes[] = { { 441, 36, 127 } };
static const Control bdControls[] = { { "bd_mod_pitch_amount", 0 }, { "bd_ampeg_decay", 100 }, { "bd_osc_coarse", 0 } };
static const Scenario bd = { bdNotes, 1, bdControls, 3, 1.0, 0 };

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
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		controls[i] = eds_controls[i].def;
		d->connect_port (h, (uint32_t) (EDS_PORT_CONTROL0 + i), &controls[i]);
	}
	for (int i = 0; i < sc->numControls; i++)
	{
		controls[controlIndex (sc->controls[i].symbol)] = sc->controls[i].value;
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
		th_seq_clear (seq);
		for (int i = 0; i < sc->numNotes; i++)
		{
			const int f = scaledFrame (sc->notes[i].frame44, rate);
			if (f >= pos && f < pos + n)
			{
				const uint8_t msg[3] = { LV2_MIDI_MSG_NOTE_ON | 9, sc->notes[i].note, sc->notes[i].velocity };
				if (sc->tempoEvent && i == 0)
				{
					th_seq_tempo (seq, sizeof seqBuf, f - pos, 120.f);
				}
				th_seq_midi (seq, sizeof seqBuf, f - pos, msg, 3);
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

// the reference: the engine driven directly, 44.1 kHz. The plugin starts a
// trigger at the next engine block: the block ceil (frame / 64)
static void renderEngine (const Scenario *sc, Output *out)
{
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	for (int pass = 0; pass <= 1; pass++)
	{
		for (int i = 0; i < sc->numControls; i++)
		{
			const int c = controlIndex (sc->controls[i].symbol);
			if (eds_controls[c].target == pass)
			{
				const int v = (int) lrintf (sc->controls[i].value) + eds_controls[c].offset;
				if (v != lxr_engine_get_parameter (eds_controls[c].param))
				{
					lxr_engine_set_parameter (eds_controls[c].param, (uint8_t) v);
				}
			}
		}
	}
	const int frames = out->frames;
	for (int b = 0; b * LXR_ENGINE_BLOCK < frames; b++)
	{
		for (int i = 0; i < sc->numNotes; i++)
		{
			const int f = sc->notes[i].frame44;
			if (f <= b * LXR_ENGINE_BLOCK && f > (b - 1) * LXR_ENGINE_BLOCK)
			{
				int track = -1;
				switch (sc->notes[i].note)
				{
				case 35: case 36: track = 0; break;
				case 45: case 47: case 48: case 50: track = 1; break;
				case 41: case 43: track = 2; break;
				case 37: case 38: case 39: case 40: track = 3; break;
				case 49: case 51: case 52: case 55: case 57: case 59: track = 4; break;
				case 42: case 44: track = 5; break;
				case 46: track = 6; break;
				}
				if (track >= 0)
				{
					lxr_engine_trigger ((uint8_t) track, sc->notes[i].velocity, LXR_ENGINE_NOTE);
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

int main (int argc, char **argv)
{
	th_verbose = argc == 3 && !strcmp (argv[2], "-v");
	if (argc != 2 && !th_verbose)
	{
		fprintf (stderr, "usage: render_test PATH/edrumsynth.so [-v]\n");
		return 2;
	}
	pluginPath = argv[1];
	// the control port symbols from the bundle's TTL (next to the binary)
	char ttl[4096];
	snprintf (ttl, sizeof ttl, "%s", pluginPath);
	char *slash = strrchr (ttl, '/');
	snprintf (slash ? slash + 1 : ttl, sizeof ttl - (size_t) (slash ? slash + 1 - ttl : 0), "edrumsynth.ttl");
	th_load_symbols (ttl, EDS_PORT_CONTROL0, EDS_NUM_CONTROLS);

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

	printf ("%s\n", th_failures ? "FAILED" : "all render checks passed");
	return th_failures ? 1 : 0;
}
