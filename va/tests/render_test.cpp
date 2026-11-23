/*
 * render_test.cpp - render checks of mTrio / mAnalog (one binary per
 * plugin: VA_ANALOG, VA_PORTS) on common/testhost (dlopen + lv2_descriptor,
 * urid:map, no lilv):
 *
 * - 44.1 kHz: the plugin's output is MicroDexed-touch's (tests/mdt_reference.h:
 *   MDT's wrapper, allocation and mixer driving the copied engine), bit
 *   exact, for host blocks of 1 to 4096 frames: a polyphonic scenario
 *   (more than 8 notes: stealing, released voices reused, a note repeated
 *   while held, pitch bend at the centre, velocities with Velocity 0), every
 *   wave, every factory preset (its values read from the bundle's
 *   presets.ttl and compared with MDT's preset loader);
 * - 48 kHz: block sizes 1 to 4096 give the same signal;
 * - 22.05 to 192 kHz: the steady-state pitch of a note, the decay and release
 *   times of the envelope (until it reaches 0; at 44.1 kHz MDT's set_env
 *   times) and the level through the filter as at 44.1 kHz;
 * - pitch bend (+-range, range 12), velocity amount, CC 120 / 123;
 * - instances in one process: same rate allowed, another rate refused.
 * It prints the CPU time per output sample (8 voices, 48 kHz, 960 frames).
 *
 * Every render runs in a forked child (the engine's tables are global).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include VA_PORTS
#include "mdt_reference.h"
#include "mdt_tables.h"
#include "testhost.h"

#include <lv2/midi/midi.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK TH_CHECK

static const char *pluginPath;
static char bundleDir[4096];

// ---- scenarios ----

struct Event
{
	int frame44;		// at 44.1 kHz; other rates: scaled
	uint8_t msg[3];
};

struct Setting
{
	const char *symbol;
	float value;
};

struct Scenario
{
	const Event *events;
	int numEvents;
	const Setting *settings;
	int numSettings;
	double seconds;
	const Setting *extra;	// a second list (a preset's values), or NULL
	int numExtra;
};

#define ON(f, n, v)	{ f, { 0x90, n, v } }
#define OFF(f, n)	{ f, { 0x80, n, 0 } }
#define BEND(f, v)	{ f, { 0xE0, (uint8_t) ((v) & 127), (uint8_t) ((v) >> 7) } }
#define CC(f, c, v)	{ f, { 0xB0, c, v } }

// more than 8 notes: two stolen, a released voice reused, a held note
// repeated (its voice retriggered), velocities (ignored with Velocity 0),
// pitch bend at the centre (no change), a note-on with velocity 0
static const Event polyEvents[] = {
	ON (0, 48, 100), ON (1000, 52, 60), ON (2000, 55, 127), ON (5000, 60, 30), ON (5000, 64, 90),
	ON (5001, 67, 110), ON (5300, 71, 1), ON (6000, 72, 64), BEND (7000, 8192),
	ON (9000, 74, 100),	// the 9th: stolen
	ON (9500, 76, 80),	// the 10th: stolen
	OFF (12000, 48), ON (12500, 47, 99), OFF (20000, 52), { 20100, { 0x90, 55, 0 } },
	ON (22000, 60, 70),	// held: retriggered
	ON (26000, 36, 127), OFF (30000, 64), OFF (30000, 67), OFF (30001, 71), OFF (32000, 72),
	OFF (40000, 74), OFF (40000, 76), OFF (40000, 47), OFF (40000, 60), OFF (40000, 36),
	ON (60000, 62, 100), ON (60000, 69, 100), OFF (70000, 62), OFF (70000, 69),
};
static const Scenario poly = { polyEvents, (int) NELEM (polyEvents), NULL, 0, 2.0, NULL, 0 };

// the same with CC 123 (all notes off) at 40000 instead of the note-offs
static const Event poly123Events[] = {
	ON (0, 48, 100), ON (1000, 52, 60), ON (2000, 55, 127), ON (5000, 60, 30), ON (5000, 64, 90),
	ON (5001, 67, 110), ON (5300, 71, 1), ON (6000, 72, 64), BEND (7000, 8192),
	ON (9000, 74, 100), ON (9500, 76, 80),
	OFF (12000, 48), ON (12500, 47, 99), OFF (20000, 52), { 20100, { 0x90, 55, 0 } },
	ON (22000, 60, 70), ON (26000, 36, 127), OFF (30000, 64), OFF (30000, 67), OFF (30001, 71), OFF (32000, 72),
	CC (40000, 123, 0),
	ON (60000, 62, 100), ON (60000, 69, 100), OFF (70000, 62), OFF (70000, 69),
};
static const Scenario poly123 = { poly123Events, (int) NELEM (poly123Events), NULL, 0, 2.0, NULL, 0 };

// short: a chord, released
static const Event chordEvents[] = { ON (0, 57, 100), ON (300, 64, 100), ON (600, 69, 100), OFF (8000, 57),
				     OFF (8000, 64), OFF (8000, 69) };

// ---- the plugin ----

typedef th_output Output;

static int scaledFrame (int frame44, double rate)
{
	return (int) ((double) frame44 * rate / 44100.0 + 0.5);
}

static void portValues (const Scenario *sc, float *controls)
{
	for (int i = 0; i < VA_NUM_CONTROLS; i++)
	{
		controls[i] = va_controls[i].def;
	}
	for (int i = 0; i < sc->numExtra; i++)
	{
		controls[th_control_index (sc->extra[i].symbol)] = sc->extra[i].value;
	}
	for (int i = 0; i < sc->numSettings; i++)
	{
		controls[th_control_index (sc->settings[i].symbol)] = sc->settings[i].value;
	}
}

struct Job
{
	const Scenario *sc;
	double rate;
	int block;	// 0: the MDT reference
	int saturate;	// the reference: with MDT's int16 mixer
};

static void renderPlugin (const Job *job, Output *out)
{
	const Scenario *sc = job->sc;
	const double rate = job->rate;
	const int block = job->block;
	const LV2_Descriptor *d = th_load (pluginPath, VA_URI);
	const LV2_Feature *const *features = th_features ();
	LV2_Handle h = d->instantiate (d, rate, bundleDir, features);
	if (!h)
	{
		fprintf (stderr, "instantiate failed at %.0f Hz\n", rate);
		exit (2);
	}
	float controls[VA_NUM_CONTROLS];
	portValues (sc, controls);
	for (int i = 0; i < VA_NUM_CONTROLS; i++)
	{
		d->connect_port (h, (uint32_t) (VA_PORT_CONTROL0 + i), &controls[i]);
	}
	static uint64_t seqBuf[4096];
	LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *) seqBuf;
	d->connect_port (h, VA_PORT_MIDI_IN, seq);
	float *outL = (float *) malloc (sizeof (float) * (size_t) block);
	float *outR = (float *) malloc (sizeof (float) * (size_t) block);
	d->connect_port (h, VA_PORT_OUT_L, outL);
	d->connect_port (h, VA_PORT_OUT_R, outR);
	d->activate (h);

	const int frames = out->frames;
	double cpu = 0;
	for (int pos = 0; pos < frames; pos += block)
	{
		const int n = pos + block <= frames ? block : frames - pos;
		th_seq_clear (seq);
		for (int i = 0; i < sc->numEvents; i++)
		{
			const int f = scaledFrame (sc->events[i].frame44, rate);
			if (f >= pos && f < pos + n)
			{
				th_seq_midi (seq, sizeof seqBuf, f - pos, sc->events[i].msg, 3);
			}
		}
		const double t0 = th_now ();
		d->run (h, (uint32_t) n);
		cpu += th_now () - t0;
		memcpy (out->data + pos, outL, sizeof (float) * (size_t) n);
		memcpy (out->data + frames + pos, outR, sizeof (float) * (size_t) n);
	}
	out->cpuSeconds = cpu;
	d->deactivate (h);
	d->cleanup (h);
	free (outL);
	free (outR);
	out->ok = 1;
}

// the reference: MDT at 44.1 kHz. The plugin starts an event at the next
// block not yet rendered: block ceil (frame / 128)
static void renderReference (const Job *job, Output *out)
{
	const Scenario *sc = job->sc;
	dsp_basics_init (44100);
	float controls[VA_NUM_CONTROLS];
	portValues (sc, controls);
	int values[VA_NUM_PARAMS];
	mdt_boot_defaults (VA_ANALOG, VA_ANALOG ? &ANALOG_PRESETS[0] : &TRIO_PRESETS[0], values);
	for (int i = 0; i < VA_NUM_CONTROLS; i++)
	{
		values[va_controls[i].param] = (int) lrintf (controls[i]) + va_controls[i].offset;
	}
	static MdtSynth mdt;
	mdt.init (VA_ANALOG, values, job->saturate);
	const int frames = out->frames;
	for (int b = 0; b * CTL < frames; b++)
	{
		for (int i = 0; i < sc->numEvents; i++)
		{
			const Event *e = &sc->events[i];
			if (e->frame44 <= b * CTL && e->frame44 > (b - 1) * CTL)
			{
				const uint8_t type = e->msg[0] & 0xF0;
				if (type == 0x90 && e->msg[2])
					mdt.handleNoteOn (e->msg[1]);
				else if (type == 0x80 || type == 0x90)
					mdt.handleNoteOff (e->msg[1]);	// MDT: velocity 0 as a note-off
				// pitch bend, CCs: MDT's Trio and Analog ignore them
			}
		}
		int32_t mix[CTL];
		mdt.render (mix);
		for (int i = 0; i < CTL && b * CTL + i < frames; i++)
		{
			out->data[b * CTL + i] = (float) mix[i] / 32768.f;
			out->data[frames + b * CTL + i] = (float) mix[i] / 32768.f;
		}
	}
	out->ok = 1;
}

static void runJob (void *arg, th_output *out)
{
	const Job *job = (const Job *) arg;
	if (job->block)
		renderPlugin (job, out);
	else
		renderReference (job, out);
}

static Output *renderJob (const Scenario *sc, double rate, int block, int saturate)
{
	Output *out = th_output_new ((int) (sc->seconds * rate));
	Job job = { sc, rate, block, saturate };
	char what[64];
	snprintf (what, sizeof what, "at %.0f Hz, block %d", rate, block);
	th_fork (runJob, &job, out, what);
	return out;
}

// the plugin (block > 0) or the reference with the plugin's mixer (0)
static Output *render (const Scenario *sc, double rate, int block)
{
	return renderJob (sc, rate, block, 0);
}

// no NaN, not silent, no runaway (the mixer isn't saturated: 8 voices
// at level 100 reach about 8 x 1.24)
static void sanity (const Output *o, const char *what)
{
	const int nan = th_count_nan (o);
	const float peak = th_peak (o);
	CHECK (!nan, "%s: %d NaN / inf samples", what, nan);
	CHECK (peak > 0.01f && peak < 10.f, "%s: peak %f", what, peak);
}

static void checkExact (const Output *o, const Output *ref, const char *what)
{
	sanity (o, what);
	CHECK (th_identical (o, ref), "%s: not MDT's output (first difference at frame %d)", what,
	       th_first_difference (o, ref));
}

// a level where MDT's int16 mixer doesn't saturate: the plugin is MDT's
// output exactly (with the saturation in the reference)
static const Setting quietSettings[] = { { "master_level", 25 } };
static const Scenario polyQuiet = { polyEvents, (int) NELEM (polyEvents), quietSettings, 1, 2.0, NULL, 0 };

// ---- presets (from the bundle's presets.ttl) ----

#define MAX_PRESETS	16
#define MAX_VALUES	VA_NUM_CONTROLS

struct Preset
{
	char label[64];
	char syms[MAX_VALUES][64];
	Setting values[MAX_VALUES];
	int numValues;
};

static Preset presets[MAX_PRESETS];
static int numPresets;

static void loadPresets (void)
{
	char path[4200];
	snprintf (path, sizeof path, "%s/presets.ttl", bundleDir);
	FILE *f = fopen (path, "r");
	if (!f)
	{
		perror (path);
		exit (2);
	}
	char line[1024], sym[64] = "";
	Preset *p = NULL;
	while (fgets (line, sizeof line, f))
	{
		char label[64];
		float v;
		if (sscanf (line, " a pset:Preset %c", label) == 1 && numPresets < MAX_PRESETS)
		{
			p = &presets[numPresets++];
		}
		else if (p && sscanf (line, " rdfs:label \"%63[^\"]\"", label) == 1)
		{
			snprintf (p->label, sizeof p->label, "%s", label);
		}
		else if (sscanf (line, " lv2:symbol \"%63[a-z0-9_]\"", sym) == 1)
		{
		}
		else if (p && sscanf (line, " pset:value %f", &v) == 1 && p->numValues < MAX_VALUES)
		{
			snprintf (p->syms[p->numValues], sizeof p->syms[0], "%s", sym);
			p->values[p->numValues].symbol = p->syms[p->numValues];
			p->values[p->numValues].value = v;
			p->numValues++;
		}
	}
	fclose (f);
}

// the preset's TTL values are MDT's preset loader's
static void checkPresetValues (const Preset *p, const preset_t *mdt)
{
	int values[VA_NUM_PARAMS], set[VA_NUM_PARAMS], count = 0;
	mdt_apply_preset (VA_ANALOG, mdt, values, set);
	for (int i = 0; i < VA_NUM_PARAMS; i++)
	{
		count += set[i];
	}
	CHECK (p->numValues == count, "preset %s: %d values, MDT sets %d", p->label, p->numValues, count);
	for (int i = 0; i < p->numValues; i++)
	{
		const int c = th_control_index (p->values[i].symbol);
		const va_control *vc = &va_controls[c];
		CHECK (set[vc->param] && (int) p->values[i].value + vc->offset == values[vc->param],
		       "preset %s: %s = %g, MDT %d", p->label, p->values[i].symbol, p->values[i].value, values[vc->param]);
	}
}

// ---- measurements ----

// the time (s) after which the output is exactly zero: the envelope has
// reached 0 (sustain 0 after the decay, or the end of the release)
static double silentFrom (const Output *o, double rate)
{
	int last = -1;
	for (int i = 0; i < o->frames; i++)
	{
		if (o->data[i] != 0 || o->data[o->frames + i] != 0)
		{
			last = i;
		}
	}
	return (last + 1) / rate;
}

// ---- scenarios for the rate checks ----

// a steady tone: Analog sine / Trio three triangles in unison, filter open
#if VA_ANALOG
#define TONE_SETTINGS                                                                                  \
	{ "osc_wave", 3 }, { "osc_detune", 0 }, { "osc_noise", 0 }, { "filter_cutoff", 127 },          \
	{ "filter_resonance", 0 }, { "filter_drive", 0 }, { "filter_key_track", 0 }, { "mod_env_cutoff", 0 }
#define FILTER_SETTINGS                                                                                \
	{ "osc_wave", 0 }, { "osc_detune", 0 }, { "osc_noise", 0 }, { "filter_cutoff", 70 },           \
	{ "filter_resonance", 60 }, { "filter_drive", 0 }, { "filter_key_track", 0 }, { "mod_env_cutoff", 0 }
#else
#define TONE_SETTINGS                                                                                  \
	{ "osc_wave", 4 }, { "osc_interval2", 0 }, { "osc_interval3", 0 }, { "osc_detune", 0 },        \
	{ "filter_cutoff", 127 }, { "filter_resonance", 0 }, { "filter_mode", 0 }, { "mod_env_cutoff", 0 }
#define FILTER_SETTINGS                                                                                \
	{ "osc_wave", 0 }, { "osc_interval2", 0 }, { "osc_interval3", 0 }, { "osc_detune", 0 },        \
	{ "filter_cutoff", 70 }, { "filter_resonance", 60 }, { "filter_mode", 0 }, { "mod_env_cutoff", 0 }
#endif

static const Setting toneSettings[] = { TONE_SETTINGS, { "ampeg_attack", 0 }, { "ampeg_decay", 0 },
					{ "ampeg_sustain", 127 }, { "ampeg_release", 0 } };
static const Event toneEvents[] = { ON (0, 69, 100) };
static const Scenario tone = { toneEvents, 1, toneSettings, (int) NELEM (toneSettings), 1.0, NULL, 0 };

static const Event bendUpEvents[] = { BEND (0, 16383), ON (0, 69, 100) };
static const Event bendDownEvents[] = { BEND (0, 0), ON (0, 69, 100) };
static const Scenario bendUp = { bendUpEvents, 2, toneSettings, (int) NELEM (toneSettings), 1.0, NULL, 0 };
static const Scenario bendDown = { bendDownEvents, 2, toneSettings, (int) NELEM (toneSettings), 1.0, NULL, 0 };
static const Setting bend12Settings[] = { TONE_SETTINGS, { "ampeg_attack", 0 }, { "ampeg_decay", 0 },
					  { "ampeg_sustain", 127 }, { "ampeg_release", 0 }, { "master_bend_range", 12 } };
static const Scenario bend12 = { bendUpEvents, 2, bend12Settings, (int) NELEM (bend12Settings), 1.0, NULL, 0 };

static const Setting decaySettings[] = { TONE_SETTINGS, { "ampeg_attack", 0 }, { "ampeg_decay", 100 },
					 { "ampeg_sustain", 0 }, { "ampeg_release", 0 } };
static const Scenario decay = { toneEvents, 1, decaySettings, (int) NELEM (decaySettings), 2.5, NULL, 0 };

static const Setting releaseSettings[] = { TONE_SETTINGS, { "ampeg_attack", 0 }, { "ampeg_decay", 0 },
					   { "ampeg_sustain", 127 }, { "ampeg_release", 70 } };
static const Event releaseEvents[] = { ON (0, 69, 100), OFF (22050, 69) };
static const Scenario release = { releaseEvents, 2, releaseSettings, (int) NELEM (releaseSettings), 3.5, NULL, 0 };

static const Setting filterSettings[] = { FILTER_SETTINGS, { "ampeg_attack", 0 }, { "ampeg_decay", 0 },
					  { "ampeg_sustain", 127 }, { "ampeg_release", 0 } };
static const Event filterEvents[] = { ON (0, 45, 100) };
static const Scenario filter = { filterEvents, 1, filterSettings, (int) NELEM (filterSettings), 1.0, NULL, 0 };

// velocity: a note at 127 and one at 32, Velocity 100
static const Event vel127Events[] = { ON (0, 69, 127) };
static const Event vel32Events[] = { ON (0, 69, 32) };
static const Setting velSettings[] = { { "mod_velocity", 100 } };
static const Scenario vel127 = { vel127Events, 1, velSettings, 1, 0.5, NULL, 0 };
static const Scenario vel127off = { vel127Events, 1, NULL, 0, 0.5, NULL, 0 };
static const Scenario vel32 = { vel32Events, 1, velSettings, 1, 0.5, NULL, 0 };

// CC 120: all sound off at 0.25 s
static const Event cc120Events[] = { ON (0, 60, 100), ON (0, 64, 100), CC (11025, 120, 0) };
static const Scenario cc120 = { cc120Events, 3, NULL, 0, 0.5, NULL, 0 };

// CPU: 8 voices held (a full chord), 48 kHz
static const Event cpuEvents[] = { ON (0, 48, 100), ON (0, 52, 100), ON (0, 55, 100), ON (0, 59, 100),
				   ON (0, 60, 100), ON (0, 64, 100), ON (0, 67, 100), ON (0, 71, 100) };
static const Scenario cpu = { cpuEvents, 8, NULL, 0, 4.0, NULL, 0 };

// ---- instances ----

static void instancesJob (void *arg, th_output *out)
{
	(void) arg;
	const LV2_Descriptor *d = th_load (pluginPath, VA_URI);
	const LV2_Feature *const *features = th_features ();
	LV2_Handle a = d->instantiate (d, 48000, bundleDir, features);
	LV2_Handle b = d->instantiate (d, 48000, bundleDir, features);
	const int messages = th_log_messages;
	LV2_Handle c = d->instantiate (d, 44100, bundleDir, features);
	out->ok = a && b && !c && th_log_messages == messages + 1;
	if (c)
		d->cleanup (c);
	if (a)
		d->cleanup (a);
	if (b)
		d->cleanup (b);
	// all gone: another rate is fine
	LV2_Handle e = d->instantiate (d, 44100, bundleDir, features);
	out->ok = out->ok && e;
	if (e)
		d->cleanup (e);
}

int main (int argc, char **argv)
{
	th_verbose = argc == 3 && !strcmp (argv[2], "-v");
	if (argc != 2 && !th_verbose)
	{
		fprintf (stderr, "usage: render_test PATH/%s.so [-v]\n", VA_ANALOG ? "manalog" : "mtrio");
		return 2;
	}
	pluginPath = argv[1];
	snprintf (bundleDir, sizeof bundleDir, "%s", pluginPath);
	char *slash = strrchr (bundleDir, '/');
	if (slash)
		*slash = 0;
	else
		snprintf (bundleDir, sizeof bundleDir, ".");
	char ttl[4200];
	snprintf (ttl, sizeof ttl, "%s/%s.ttl", bundleDir, VA_ANALOG ? "manalog" : "mtrio");
	th_load_symbols (ttl, VA_PORT_CONTROL0, VA_NUM_CONTROLS);
	printf ("%s\n", VA_NAME);

	// ---- 44.1 kHz: MDT, bit exact ----
	Output *ref = render (&poly, 44100, 0);
	const int blocks44[] = { 1, 63, 128, 256, 1000, 4096 };
	for (unsigned i = 0; i < NELEM (blocks44); i++)
	{
		Output *o = render (&poly, 44100, blocks44[i]);
		char what[64];
		snprintf (what, sizeof what, "44.1 kHz, block %d", blocks44[i]);
		checkExact (o, ref, what);
		th_output_free (o);
	}
	// CC 123 == the note-offs it replaces
	Output *o123 = render (&poly123, 44100, 512);
	CHECK (th_identical (o123, ref), "CC 123: not the same as the note-offs (first difference at frame %d)",
	       th_first_difference (o123, ref));
	th_output_free (o123);
	th_output_free (ref);
	Output *refSat = renderJob (&polyQuiet, 44100, 0, 1), *refSum = render (&polyQuiet, 44100, 0);
	Output *quiet = render (&polyQuiet, 44100, 500);
	CHECK (th_identical (refSat, refSum), "level 25: MDT's mixer saturates");
	checkExact (quiet, refSat, "level 25, MDT's int16 mixer");
	th_output_free (refSat);
	th_output_free (refSum);
	th_output_free (quiet);
	printf ("44.1 kHz: identical to MDT (polyphony, stealing, blocks 1..4096), CC 123\n");

	// every wave
	const int waves = VA_ANALOG ? 5 : 16;
	for (int w = 0; w < waves; w++)
	{
		const Setting s[] = { { "osc_wave", (float) w } };
		const Scenario sc = { chordEvents, (int) NELEM (chordEvents), s, 1, 0.4, NULL, 0 };
		Output *r = render (&sc, 44100, 0);
		Output *o = render (&sc, 44100, 300);
		char what[64];
		snprintf (what, sizeof what, "wave %d", w);
		checkExact (o, r, what);
		th_output_free (o);
		th_output_free (r);
	}
	printf ("44.1 kHz: identical to MDT for all %d waves\n", waves);

	// every factory preset
	loadPresets ();
	const int mdtPresets = VA_ANALOG ? ANALOG_NPRESETS : TRIO_NPRESETS;
	CHECK (numPresets == mdtPresets, "presets.ttl: %d presets, MDT has %d", numPresets, mdtPresets);
	for (int i = 0; i < numPresets && i < mdtPresets; i++)
	{
		const Preset *p = &presets[i];
		checkPresetValues (p, VA_ANALOG ? &ANALOG_PRESETS[i] : &TRIO_PRESETS[i]);
		const Scenario sc = { polyEvents, (int) NELEM (polyEvents), NULL, 0, 2.0, p->values, p->numValues };
		Output *r = render (&sc, 44100, 0);
		Output *o = render (&sc, 44100, 256);
		char what[96];
		snprintf (what, sizeof what, "preset %.63s", p->label);
		checkExact (o, r, what);
		th_output_free (o);
		th_output_free (r);
	}
	printf ("44.1 kHz: identical to MDT for the %d presets (values as MDT's loader)\n", numPresets);

	// ---- 48 kHz: block sizes ----
	Output *ref48 = render (&poly, 48000, 960);
	sanity (ref48, "48 kHz, block 960");
	const int blocks48[] = { 1, 64, 333, 480, 1001, 4096 };
	for (unsigned i = 0; i < NELEM (blocks48); i++)
	{
		Output *o = render (&poly, 48000, blocks48[i]);
		char what[64];
		snprintf (what, sizeof what, "48 kHz, block %d", blocks48[i]);
		sanity (o, what);
		CHECK (th_identical (o, ref48), "%s: differs from block 960 (first difference at frame %d)", what,
		       th_first_difference (o, ref48));
		th_output_free (o);
	}
	th_output_free (ref48);
	printf ("48 kHz: blocks 1..4096 identical\n");

	// ---- rates ----
	Output *tone44 = render (&tone, 44100, 512);
	Output *decay44 = render (&decay, 44100, 512);
	Output *release44 = render (&release, 44100, 512);
	Output *filter44 = render (&filter, 44100, 512);
	const double f44 = th_frequency (tone44, 44100, 0.2, 0.9);
	const double d44 = silentFrom (decay44, 44100);
	const double r44 = silentFrom (release44, 44100) - 0.5;
	const double l44 = th_rms (filter44, (int) (0.2 * 44100), (int) (0.9 * 44100));
	// MDT's envelope times (synth_trio.cpp / synth_analog.cpp set_env)
	const double dMdt = pow (100 / 127.0, 2) * 3.048, rMdt = pow (70 / 127.0, 3) * 13.0;
	printf ("%9s %10s %10s %10s %10s\n", "rate", "A4 (Hz)", "decay (s)", "release", "filter dB");
	printf ("%9s %10.3f %10.4f %10.4f %10s  (MDT's set_env)\n", "MDT", 440.0, dMdt, rMdt, "");
	printf ("%9.0f %10.3f %10.4f %10.4f %10.2f\n", 44100.0, f44, d44, r44, 20 * log10 (l44));
	CHECK (fabs (f44 / 440 - 1) < 0.001, "A4 at 44.1 kHz: %.3f Hz", f44);
	// the envelope ends in the block it reaches 0; events start at a block
	CHECK (fabs (d44 - dMdt) < 2 * 128 / 44100.0, "decay at 44.1 kHz: %.4f s, MDT %.4f s", d44, dMdt);
	CHECK (fabs (r44 - rMdt) < 2 * 128 / 44100.0, "release at 44.1 kHz: %.4f s, MDT %.4f s", r44, rMdt);
	const double rates[] = { 22050, 32000, 48000, 88200, 96000, 192000 };
	for (unsigned i = 0; i < NELEM (rates); i++)
	{
		const double rate = rates[i];
		Output *t = render (&tone, rate, 512);
		Output *dc = render (&decay, rate, 512);
		Output *rl = render (&release, rate, 512);
		Output *fl = render (&filter, rate, 512);
		char what[64];
		snprintf (what, sizeof what, "%.0f Hz", rate);
		sanity (t, what);
		const double f = th_frequency (t, rate, 0.2, 0.9);
		const double dt = silentFrom (dc, rate);
		const double rt = silentFrom (rl, rate) - 0.5;
		const double l = th_rms (fl, (int) (0.2 * rate), (int) (0.9 * rate));
		printf ("%9.0f %10.3f %10.4f %10.4f %10.2f\n", rate, f, dt, rt, 20 * log10 (l));
		const double tol = 2 * 128 / 44100.0 + 2 * 128 / rate;	// blocks
		CHECK (fabs (f / f44 - 1) < 0.001, "%s: pitch %.3f Hz, 44.1 kHz %.3f Hz", what, f, f44);
		CHECK (fabs (dt - d44) < tol, "%s: decay %.4f s, 44.1 kHz %.4f s", what, dt, d44);
		CHECK (fabs (rt - r44) < tol, "%s: release %.4f s, 44.1 kHz %.4f s", what, rt, r44);
		// the filter's coefficients are coarser at higher rates (Q12)
		CHECK (fabs (20 * log10 (l / l44)) < 1.0, "%s: level through the filter %+.2f dB", what,
		       20 * log10 (l / l44));
		th_output_free (t);
		th_output_free (dc);
		th_output_free (rl);
		th_output_free (fl);
	}

	// ---- pitch bend ----
	Output *up = render (&bendUp, 48000, 480);
	Output *down = render (&bendDown, 48000, 480);
	Output *up12 = render (&bend12, 48000, 480);
	const double fu = th_frequency (up, 48000, 0.2, 0.9), fd = th_frequency (down, 48000, 0.2, 0.9);
	const double fu12 = th_frequency (up12, 48000, 0.2, 0.9);
	const double eu = 440 * pow (2, 2.0 * 8191 / 8192 / 12), ed = 440 * pow (2, -2.0 / 12);
	const double eu12 = 440 * pow (2, 12.0 * 8191 / 8192 / 12);
	printf ("pitch bend: +2: %.3f Hz (%.3f), -2: %.3f Hz (%.3f), +12: %.3f Hz (%.3f)\n", fu, eu, fd, ed, fu12, eu12);
	CHECK (fabs (fu / eu - 1) < 0.0005, "bend up: %.3f Hz, expected %.3f", fu, eu);
	CHECK (fabs (fd / ed - 1) < 0.0005, "bend down: %.3f Hz, expected %.3f", fd, ed);
	CHECK (fabs (fu12 / eu12 - 1) < 0.0005, "bend up, range 12: %.3f Hz, expected %.3f", fu12, eu12);
	th_output_free (up);
	th_output_free (down);
	th_output_free (up12);

	// ---- velocity ----
	Output *v127 = render (&vel127, 48000, 480);
	Output *v127off = render (&vel127off, 48000, 480);
	Output *v32 = render (&vel32, 48000, 480);
	const double lv127 = th_rms (v127, 0, v127->frames), lv32 = th_rms (v32, 0, v32->frames);
	printf ("velocity 100 %%: velocity 32 %+.1f dB below 127\n", 20 * log10 (lv32 / lv127));
	CHECK (th_identical (v127, v127off), "velocity 127 with Velocity 100 differs from Velocity 0");
	CHECK (20 * log10 (lv32 / lv127) < -12, "velocity 32: only %+.1f dB", 20 * log10 (lv32 / lv127));
	th_output_free (v127);
	th_output_free (v127off);
	th_output_free (v32);

	// ---- CC 120 ----
	Output *o120 = render (&cc120, 44100, 256);
	int after = 0;
	for (int i = 11025 + CTL; i < o120->frames; i++)
	{
		after += o120->data[i] != 0 || o120->data[o120->frames + i] != 0;
	}
	CHECK (th_rms (o120, 0, 11025) > 0.01 && !after, "CC 120: %d samples after it", after);
	th_output_free (o120);

	// ---- instances ----
	Output *inst = th_output_new (1);
	th_fork (instancesJob, NULL, inst, "instances");
	CHECK (inst->ok, "instances: two at 48 kHz, a third at 44.1 kHz refused, then 44.1 kHz alone");
	th_output_free (inst);

	// ---- CPU ----
	Output *c = render (&cpu, 48000, 960);
	printf ("CPU (this machine, this build): 8 voices at 48 kHz, block 960: %.1f ns per sample (%.2f %% of real time)\n",
		1e9 * c->cpuSeconds / c->frames, 100 * c->cpuSeconds / cpu.seconds);
	th_output_free (c);

	th_output_free (tone44);
	th_output_free (decay44);
	th_output_free (release44);
	th_output_free (filter44);
	printf ("%s\n", th_failures ? "FAILED" : "all render checks passed");
	return th_failures ? 1 : 0;
}
