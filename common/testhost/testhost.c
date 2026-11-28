/*
 * testhost.c - the render tests' minimal LV2 host (testhost.h).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "testhost.h"

#include <lv2/atom/util.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>

#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int th_failures;
int th_log_messages;
int th_verbose;

// ---- urid:map ----

#define MAX_URIS 128
static char *uris[MAX_URIS];
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
	if (numUris == MAX_URIS)
	{
		return 0;
	}
	uris[numUris] = strdup (uri);
	return (LV2_URID) ++numUris;
}

LV2_URID th_map (const char *uri)
{
	return mapUri (NULL, uri);
}

static LV2_URID_Map map = { NULL, mapUri };
static const LV2_Feature mapFeature = { LV2_URID__map, &map };

// ---- log:log ----

static int logVPrintf (LV2_Log_Handle handle, LV2_URID type, const char *fmt, va_list ap)
{
	(void) handle;
	(void) type;
	th_log_messages++;
	return th_verbose ? vfprintf (stderr, fmt, ap) : 0;
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

const LV2_Feature *const *th_features (void)
{
	return features;
}

// ---- the plugin ----

const LV2_Descriptor *th_load (const char *path, const char *uri)
{
	void *lib = dlopen (path, RTLD_NOW | RTLD_LOCAL);
	if (!lib)
	{
		fprintf (stderr, "dlopen: %s\n", dlerror ());
		exit (2);
	}
	const LV2_Descriptor *(*descriptor) (uint32_t);
	*(void **) &descriptor = dlsym (lib, "lv2_descriptor");
	if (!descriptor || !descriptor (0) || strcmp (descriptor (0)->URI, uri) || descriptor (1))
	{
		fprintf (stderr, "%s: no or a wrong lv2_descriptor\n", path);
		exit (2);
	}
	return descriptor (0);
}

// ---- control port symbols ----

static char (*symbols)[64];
static int numSymbols;

void th_load_symbols (const char *ttlPath, int first, int count)
{
	free (symbols);
	symbols = calloc ((size_t) count, sizeof *symbols);
	numSymbols = count;
	FILE *f = fopen (ttlPath, "r");
	if (!f || !symbols)
	{
		perror (ttlPath);
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
		if (sscanf (line, " lv2:symbol \"%63[a-z0-9_]\"", sym) == 1 && index >= first && index < first + count)
		{
			snprintf (symbols[index - first], sizeof symbols[0], "%s", sym);
			index = -1;
		}
	}
	fclose (f);
}

int th_control_index (const char *symbol)
{
	for (int i = 0; i < numSymbols; i++)
	{
		if (!strcmp (symbols[i], symbol))
		{
			return i;
		}
	}
	fprintf (stderr, "no control port %s\n", symbol);
	exit (2);
}

const char *th_control_symbol (int index)
{
	return index >= 0 && index < numSymbols ? symbols[index] : "";
}

// ---- atom sequences ----

void th_seq_clear (LV2_Atom_Sequence *seq)
{
	seq->atom.type = th_map (LV2_ATOM__Sequence);
	seq->atom.size = sizeof (LV2_Atom_Sequence_Body);
	seq->body.unit = 0;
	seq->body.pad = 0;
}

void th_seq_midi (LV2_Atom_Sequence *seq, uint32_t capacity, int64_t frame, const uint8_t *msg, uint32_t size)
{
	if (sizeof (LV2_Atom) + seq->atom.size + sizeof (LV2_Atom_Event) + lv2_atom_pad_size (size) > capacity)
	{
		return;
	}
	LV2_Atom_Event *ev = lv2_atom_sequence_end (&seq->body, seq->atom.size);
	ev->time.frames = frame;
	ev->body.type = th_map (LV2_MIDI__MidiEvent);
	ev->body.size = size;
	memcpy (ev + 1, msg, size);
	seq->atom.size += lv2_atom_pad_size ((uint32_t) sizeof (LV2_Atom_Event) + size);
}

void th_seq_tempo (LV2_Atom_Sequence *seq, uint32_t capacity, int64_t frame, float bpm)
{
	struct
	{
		LV2_Atom_Event ev;
		LV2_Atom_Object_Body body;
		LV2_Atom_Property_Body prop;
		float bpm;
	} obj;
	if (sizeof (LV2_Atom) + seq->atom.size + sizeof obj > capacity)
	{
		return;
	}
	memset (&obj, 0, sizeof obj);
	obj.ev.time.frames = frame;
	obj.ev.body.type = th_map (LV2_ATOM__Object);
	obj.ev.body.size = sizeof obj - sizeof obj.ev;
	obj.body.otype = th_map (LV2_TIME__Position);
	obj.prop.key = th_map (LV2_TIME__beatsPerMinute);
	obj.prop.value.type = th_map (LV2_ATOM__Float);
	obj.prop.value.size = sizeof (float);
	obj.bpm = bpm;
	memcpy (lv2_atom_sequence_end (&seq->body, seq->atom.size), &obj, sizeof obj);
	seq->atom.size += lv2_atom_pad_size (sizeof obj);
}

void th_seq_position (LV2_Atom_Sequence *seq, uint32_t capacity, int64_t frame, int64_t hostFrame, float speed,
		      int64_t bar, float barBeat, float bpm)
{
	// properties: key, then a Long, Float or Int body (padded to 8 bytes)
	struct
	{
		LV2_Atom_Event ev;
		LV2_Atom_Object_Body body;
		struct
		{
			LV2_Atom_Property_Body prop;
			union
			{
				int64_t l;
				float f;
				int32_t i;
			} v;
		} props[7];
	} obj;
	memset (&obj, 0, sizeof obj);
	int n = 0;
#define TH_PROP(KEY, TYPE, FIELD, SIZE, VALUE) \
	do { \
		obj.props[n].prop.key = th_map (KEY); \
		obj.props[n].prop.value.type = th_map (TYPE); \
		obj.props[n].prop.value.size = SIZE; \
		obj.props[n].v.FIELD = VALUE; \
		n++; \
	} while (0)
	TH_PROP (LV2_TIME__frame, LV2_ATOM__Long, l, 8, hostFrame);
	TH_PROP (LV2_TIME__speed, LV2_ATOM__Float, f, 4, speed);
	if (bar >= 0)
	{
		TH_PROP (LV2_TIME__barBeat, LV2_ATOM__Float, f, 4, barBeat);
		TH_PROP (LV2_TIME__bar, LV2_ATOM__Long, l, 8, bar);
		TH_PROP (LV2_TIME__beatUnit, LV2_ATOM__Int, i, 4, 4);
		TH_PROP (LV2_TIME__beatsPerBar, LV2_ATOM__Float, f, 4, 4.f);
		TH_PROP (LV2_TIME__beatsPerMinute, LV2_ATOM__Float, f, 4, bpm);
	}
#undef TH_PROP
	_Static_assert (sizeof obj.props[0] == 24, "property size");
	const uint32_t size = (uint32_t) (sizeof obj.ev + sizeof obj.body + (size_t) n * sizeof obj.props[0]);
	if (sizeof (LV2_Atom) + seq->atom.size + size > capacity)
	{
		return;
	}
	obj.ev.time.frames = frame;
	obj.ev.body.type = th_map (LV2_ATOM__Object);
	obj.ev.body.size = size - (uint32_t) sizeof obj.ev;
	obj.body.otype = th_map (LV2_TIME__Position);
	memcpy (lv2_atom_sequence_end (&seq->body, seq->atom.size), &obj, size);
	seq->atom.size += lv2_atom_pad_size (size);
}

// ---- renders ----

static size_t outputSize (int frames)
{
	return sizeof (th_output) + sizeof (float) * 2 * (size_t) frames;
}

th_output *th_output_new (int frames)
{
	th_output *o = mmap (NULL, outputSize (frames), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (o == MAP_FAILED)
	{
		perror ("mmap");
		exit (2);
	}
	o->frames = frames;
	return o;
}

void th_output_free (th_output *o)
{
	munmap (o, outputSize (o->frames));
}

void th_fork (void (*fn) (void *arg, th_output *out), void *arg, th_output *out, const char *what)
{
	fflush (stdout);
	fflush (stderr);
	const pid_t pid = fork ();
	if (pid == 0)
	{
		fn (arg, out);
		fflush (stdout);	// the child's output too
		_exit (0);
	}
	int status;
	waitpid (pid, &status, 0);
	if (!WIFEXITED (status) || WEXITSTATUS (status))
	{
		fprintf (stderr, "render %s failed\n", what);
		exit (2);
	}
}

double th_now (void)
{
	struct timespec t;
	clock_gettime (CLOCK_MONOTONIC, &t);
	return (double) t.tv_sec + 1e-9 * (double) t.tv_nsec;
}

// ---- measurements ----

double th_rms_channel (const th_output *o, int channel, int from, int to)
{
	double sum = 0;
	for (int i = from; i < to; i++)
	{
		const double x = o->data[channel * o->frames + i];
		sum += x * x;
	}
	return to > from ? sqrt (sum / (to - from)) : 0;
}

double th_rms (const th_output *o, int from, int to)
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

float th_peak (const th_output *o)
{
	float peak = 0;
	for (int i = 0; i < 2 * o->frames; i++)
	{
		const float x = fabsf (o->data[i]);
		peak = x > peak ? x : peak;
	}
	return peak;
}

int th_count_nan (const th_output *o)
{
	int n = 0;
	for (int i = 0; i < 2 * o->frames; i++)
	{
		const float x = o->data[i];
		n += !(x == x) || x > 1e30f || x < -1e30f;
	}
	return n;
}

int th_count_clipped (const th_output *o)
{
	int n = 0;
	for (int i = 0; i < 2 * o->frames; i++)
	{
		n += fabsf (o->data[i]) >= 0.999f;
	}
	return n;
}

double th_frequency (const th_output *o, double rate, double from, double to)
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

int th_identical (const th_output *a, const th_output *b)
{
	return a->frames == b->frames && !memcmp (a->data, b->data, sizeof (float) * 2 * (size_t) a->frames);
}

int th_first_difference (const th_output *a, const th_output *b)
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
