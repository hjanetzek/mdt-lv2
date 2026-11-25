/*
 * testhost.h - a minimal LV2 host for the plugins' render tests: dlopen +
 * lv2_descriptor, a urid:map, a counting log:log, no lilv. Plus a MIDI /
 * time:Position atom sequence writer, renders in a forked child (output in
 * shared memory: engines with global state start fresh each time) and a
 * few signal measurements.
 *
 * Used by lxrdrums/tests/render_test.c and va/tests/render_test.cpp.
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef MDT_LV2_TESTHOST_H_
#define MDT_LV2_TESTHOST_H_

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// failed checks so far; TH_CHECK prints "FAIL: ..." and counts
extern int th_failures;
#define TH_CHECK(cond, ...)                                                                       \
	do                                                                                        \
	{                                                                                         \
		if (!(cond))                                                                      \
		{                                                                                 \
			printf ("FAIL: ");                                                        \
			printf (__VA_ARGS__);                                                     \
			printf ("\n");                                                            \
			th_failures++;                                                            \
		}                                                                                 \
	} while (0)

// log:log messages so far (printed to stderr when th_verbose)
extern int th_log_messages;
extern int th_verbose;

// the host features: urid:map and log:log, NULL terminated
const LV2_Feature *const *th_features (void);
LV2_URID th_map (const char *uri);

// the plugin's descriptor 0 (checks its URI and that there is only one);
// exits on errors
const LV2_Descriptor *th_load (const char *path, const char *uri);

// control port symbols from a plugin TTL (the lv2:index / lv2:symbol
// lines as the generators write them): ports first..first+count-1;
// th_control_index() exits on an unknown symbol
void th_load_symbols (const char *ttlPath, int first, int count);
int th_control_index (const char *symbol);	// 0..count-1
const char *th_control_symbol (int index);	// of control 0..count-1

// atom sequences (capacity in bytes, including the sequence header)
void th_seq_clear (LV2_Atom_Sequence *seq);
void th_seq_midi (LV2_Atom_Sequence *seq, uint32_t capacity, int64_t frame, const uint8_t *msg, uint32_t size);
void th_seq_tempo (LV2_Atom_Sequence *seq, uint32_t capacity, int64_t frame, float bpm);

// a stereo render in shared memory: left data[0..frames-1], right
// data[frames..2*frames-1]
typedef struct
{
	int frames;
	double cpuSeconds;	// in run()
	int ok;			// test specific
	float data[];
} th_output;

th_output *th_output_new (int frames);
void th_output_free (th_output *o);
// runs fn (arg, out) in a child process; exits when the child fails
void th_fork (void (*fn) (void *arg, th_output *out), void *arg, th_output *out, const char *what);

// seconds of CPU time (monotonic clock) for timing run()
double th_now (void);

// measurements (both channels for rms, peak; left for frequency)
double th_rms (const th_output *o, int from, int to);
double th_rms_channel (const th_output *o, int channel, int from, int to);
float th_peak (const th_output *o);
// from the rising zero crossings (interpolated) of the left channel,
// from..to in seconds; 0 if fewer than two
double th_frequency (const th_output *o, double rate, double from, double to);
int th_identical (const th_output *a, const th_output *b);
int th_first_difference (const th_output *a, const th_output *b);	// frame, or -1
// NaN or inf samples, samples at or above full scale (0.999)
int th_count_nan (const th_output *o);
int th_count_clipped (const th_output *o);

#ifdef __cplusplus
}
#endif

#endif
