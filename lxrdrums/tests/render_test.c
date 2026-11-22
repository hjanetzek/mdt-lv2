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

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *pluginPath;
static int failures;

#define CHECK(cond, ...)                                                                          \
	do                                                                                        \
	{                                                                                         \
		if (!(cond))                                                                      \
		{                                                                                 \
			printf ("FAIL: ");                                                        \
			printf (__VA_ARGS__);                                                     \
			printf ("\n");                                                            \
			failures++;                                                               \
		}                                                                                 \
	} while (0)

// ---- urid:map ----

static char *uris[64];
static int numUris;

static LV2_URID mapUri (LV2_URID_Map_Handle handle, const char *uri)
{
	(void) handle;
	for (int i = 0; i < numUris; i++)
	{
		if (!strcmp (uris[i], uri))
		{
			return (LV2_URID) (i + 1);
		}
	}
	if (numUris == 64)
	{
		return 0;
	}
	uris[numUris] = strdup (uri);
	return (LV2_URID) ++numUris;
}

static LV2_URID_Map map = { NULL, mapUri };
static const LV2_Feature mapFeature = { LV2_URID__map, &map };

// log:log: the messages counted (the refused second instance logs one),
// printed with -v
static int logMessages, verbose;

static int logVPrintf (LV2_Log_Handle handle, LV2_URID type, const char *fmt, va_list ap)
{
	(void) handle;
	(void) type;
	logMessages++;
	return verbose ? vfprintf (stderr, fmt, ap) : 0;
}

static int logPrintf (LV2_Log_Handle handle, LV2_URID type, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	const int n = logVPrintf (handle, type, fmt, ap);
	va_end (ap);
	return n;
}

static LV2_Log_Log logger = { NULL, logPrintf, logVPrintf };
static const LV2_Feature logFeature = { LV2_LOG__log, &logger };
static const LV2_Feature *const features[] = { &mapFeature, &logFeature, NULL };

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

static int controlIndex (const char *symbol);

// ---- the plugin ----

typedef struct
{
	int frames;		// total
	double cpuSeconds;	// in run()
	int ok;
	float data[];		// left[frames], right[frames]
} Output;

static int controlIndex (const char *symbol)
{
	// the symbols, from the generated TTL's ports.h comments: look them
	// up in the TTL instead (ports.h has no symbols); here a table built
	// once from the bundle
	static char syms[EDS_NUM_CONTROLS][64];
	static int loaded;
	if (!loaded)
	{
		char path[4096];
		snprintf (path, sizeof path, "%s", pluginPath);
		char *slash = strrchr (path, '/');
		snprintf (slash ? slash + 1 : path, sizeof path - (size_t) (slash ? slash + 1 - path : 0), "edrumsynth.ttl");
		FILE *f = fopen (path, "r");
		if (!f)
		{
			perror (path);
			exit (2);
		}
		char line[1024];
		int index = -1;
		while (fgets (line, sizeof line, f))
		{
			char sym[64];
			if (sscanf (line, " lv2:index %d", &index) == 1)
			{
				continue;
			}
			if (sscanf (line, " lv2:symbol \"%63[a-z0-9_]\"", sym) == 1 && index >= EDS_PORT_CONTROL0
			    && index < EDS_NUM_PORTS)
			{
				snprintf (syms[index - EDS_PORT_CONTROL0], sizeof syms[0], "%s", sym);
				index = -1;
			}
		}
		fclose (f);
		loaded = 1;
	}
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		if (!strcmp (syms[i], symbol))
		{
			return i;
		}
	}
	fprintf (stderr, "no control port %s\n", symbol);
	exit (2);
}

static const LV2_Descriptor *loadPlugin (void)
{
	void *lib = dlopen (pluginPath, RTLD_NOW | RTLD_LOCAL);
	if (!lib)
	{
		fprintf (stderr, "dlopen: %s\n", dlerror ());
		exit (2);
	}
	const LV2_Descriptor *(*descriptor) (uint32_t) = (const LV2_Descriptor *(*) (uint32_t)) dlsym (lib, "lv2_descriptor");
	if (!descriptor || !descriptor (0) || strcmp (descriptor (0)->URI, EDS_URI) || descriptor (1))
	{
		fprintf (stderr, "no or a wrong lv2_descriptor\n");
		exit (2);
	}
	return descriptor (0);
}

static int scaledFrame (int frame44, double rate)
{
	return (int) ((double) frame44 * rate / 44100.0 + 0.5);
}

// appends a MIDI note-on (and a time:Position) to the sequence
static void appendEvent (LV2_Atom_Sequence *seq, uint32_t capacity, int64_t frame, const uint8_t *msg, int tempo)
{
	LV2_Atom_Event *ev = lv2_atom_sequence_end (&seq->body, seq->atom.size);
	if (tempo)
	{
		struct
		{
			LV2_Atom_Event ev;
			LV2_Atom_Object_Body body;
			LV2_Atom_Property_Body prop;
			float bpm;
		} obj;
		memset (&obj, 0, sizeof obj);
		obj.ev.time.frames = frame;
		obj.ev.body.type = mapUri (NULL, LV2_ATOM__Object);
		obj.ev.body.size = sizeof obj - sizeof obj.ev;
		obj.body.otype = mapUri (NULL, LV2_TIME__Position);
		obj.prop.key = mapUri (NULL, LV2_TIME__beatsPerMinute);
		obj.prop.value.type = mapUri (NULL, LV2_ATOM__Float);
		obj.prop.value.size = sizeof (float);
		obj.bpm = 120.f;
		if (seq->atom.size + sizeof obj > capacity)
		{
			return;
		}
		memcpy (ev, &obj, sizeof obj);
		seq->atom.size += lv2_atom_pad_size (sizeof obj);
		ev = lv2_atom_sequence_end (&seq->body, seq->atom.size);
	}
	if (seq->atom.size + sizeof (LV2_Atom_Event) + 8 > capacity)
	{
		return;
	}
	ev->time.frames = frame;
	ev->body.type = mapUri (NULL, LV2_MIDI__MidiEvent);
	ev->body.size = 3;
	memcpy (ev + 1, msg, 3);
	seq->atom.size += lv2_atom_pad_size (sizeof (LV2_Atom_Event) + 3);
}

// renders the scenario with the plugin, in this process
static void renderPlugin (const Scenario *sc, double rate, int block, Output *out)
{
	const LV2_Descriptor *d = loadPlugin ();
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
		seq->atom.type = mapUri (NULL, LV2_ATOM__Sequence);
		seq->atom.size = sizeof (LV2_Atom_Sequence_Body);
		seq->body.unit = 0;
		seq->body.pad = 0;
		for (int i = 0; i < sc->numNotes; i++)
		{
			const int f = scaledFrame (sc->notes[i].frame44, rate);
			if (f >= pos && f < pos + n)
			{
				const uint8_t msg[3] = { LV2_MIDI_MSG_NOTE_ON | 9, sc->notes[i].note, sc->notes[i].velocity };
				appendEvent (seq, sizeof seqBuf, f - pos, msg, sc->tempoEvent && i == 0);
			}
		}
		struct timespec t0, t1;
		clock_gettime (CLOCK_MONOTONIC, &t0);
		d->run (h, (uint32_t) n);
		clock_gettime (CLOCK_MONOTONIC, &t1);
		cpu += (double) (t1.tv_sec - t0.tv_sec) + 1e-9 * (double) (t1.tv_nsec - t0.tv_nsec);
		memcpy (out->data + pos, outL, sizeof (float) * (size_t) n);
		memcpy (out->data + frames + pos, outR, sizeof (float) * (size_t) n);
	}
	out->cpuSeconds = cpu;

	// one instance per process
	const int messages = logMessages;
	LV2_Handle second = d->instantiate (d, rate, "", features);
	out->ok = second == NULL && logMessages == messages + 1;
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
static Output *render (const Scenario *sc, double rate, int block)
{
	const int frames = (int) (sc->seconds * rate);
	const size_t size = sizeof (Output) + sizeof (float) * 2 * (size_t) frames;
	Output *out = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (out == MAP_FAILED)
	{
		perror ("mmap");
		exit (2);
	}
	out->frames = frames;
	const pid_t pid = fork ();
	if (pid == 0)
	{
		if (block)
			renderPlugin (sc, rate, block, out);
		else
			renderEngine (sc, out);
		_exit (0);
	}
	int status;
	waitpid (pid, &status, 0);
	if (!WIFEXITED (status) || WEXITSTATUS (status))
	{
		fprintf (stderr, "render at %.0f Hz, block %d failed\n", rate, block);
		exit (2);
	}
	return out;
}

static void release (Output *o)
{
	munmap (o, sizeof (Output) + sizeof (float) * 2 * (size_t) o->frames);
}

// ---- analysis ----

static double rms (const Output *o, int from, int to)
{
	double sum = 0;
	for (int c = 0; c < 2; c++)
	{
		for (int i = from; i < to; i++)
		{
			const double x = o->data[c * o->frames + i];
			sum += x * x;
		}
	}
	return sqrt (sum / (2.0 * (to - from)));
}

static void sanity (const Output *o, const char *what)
{
	int nan = 0, clip = 0;
	float peak = 0;
	for (int i = 0; i < 2 * o->frames; i++)
	{
		const float x = o->data[i];
		nan += !(x == x) || x > 1e30f || x < -1e30f;
		clip += fabsf (x) >= 0.999f;
		peak = fabsf (x) > peak ? fabsf (x) : peak;
	}
	CHECK (!nan, "%s: %d NaN / inf samples", what, nan);
	CHECK (clip < o->frames / 1000, "%s: %d clipped samples", what, clip);
	CHECK (peak > 0.01f, "%s: silent (peak %f)", what, peak);
	CHECK (o->ok, "%s: a second instance was not refused, or a new one after cleanup failed", what);
}

// frequency from the rising zero crossings (interpolated) of the left channel
static double frequency (const Output *o, double rate, double from, double to)
{
	const float *x = o->data;
	double first = -1, last = -1;
	int count = 0;
	for (int i = (int) (from * rate); i < (int) (to * rate) && i + 1 < o->frames; i++)
	{
		if (x[i] < 0 && x[i + 1] >= 0)
		{
			const double t = i + x[i] / (x[i] - x[i + 1]);
			if (first < 0)
				first = t;
			last = t;
			count++;
		}
	}
	return count > 1 ? (count - 1) * rate / (last - first) : 0;
}

static int identical (const Output *a, const Output *b)
{
	return a->frames == b->frames && !memcmp (a->data, b->data, sizeof (float) * 2 * (size_t) a->frames);
}

static int firstDifference (const Output *a, const Output *b)
{
	for (int i = 0; i < 2 * a->frames; i++)
	{
		if (a->data[i] != b->data[i])
		{
			return i % a->frames;
		}
	}
	return -1;
}

int main (int argc, char **argv)
{
	verbose = argc == 3 && !strcmp (argv[2], "-v");
	if (argc != 2 && !verbose)
	{
		fprintf (stderr, "usage: render_test PATH/edrumsynth.so [-v]\n");
		return 2;
	}
	pluginPath = argv[1];
	controlIndex ("bd_mix_volume");	// load the symbols

	// 44.1 kHz: bit exact against the engine
	Output *ref = render (&kit, 44100, 0);
	const int blocks44[] = { 1, 63, 64, 256, 1000, 4096 };
	for (unsigned i = 0; i < sizeof blocks44 / sizeof blocks44[0]; i++)
	{
		Output *o = render (&kit, 44100, blocks44[i]);
		char what[64];
		snprintf (what, sizeof what, "44.1 kHz, block %d", blocks44[i]);
		sanity (o, what);
		CHECK (identical (o, ref), "%s: not the engine's output (first difference at frame %d)", what,
		       firstDifference (o, ref));
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
		CHECK (identical (o, ref48), "%s: differs from block 960 (first difference at frame %d)", what,
		       firstDifference (o, ref48));
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
	const double f44 = frequency (bd44, 44100, 0.05, 0.6), f48 = frequency (bd48, 48000, 0.05, 0.6);
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
		const double f = frequency (o, rates[i], 0.05, 0.6);
		const double r = rms (o, 0, (int) (0.5 * rates[i]));
		printf ("bass drum at %.0f Hz: %.2f Hz, level %+.2f dB\n", rates[i], f, 20 * log10 (r / r44));
		CHECK (fabs (f / f44 - 1) < 0.01, "%s: pitch %.2f Hz", what, f);
		CHECK (fabs (20 * log10 (r / r44)) < 0.5, "%s: level %.2f dB", what, 20 * log10 (r / r44));
		release (o);
	}
	release (bd44);
	release (bd48);

	printf ("%s\n", failures ? "FAILED" : "all render checks passed");
	return failures ? 1 : 0;
}
