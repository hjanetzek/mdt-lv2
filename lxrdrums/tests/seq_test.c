/*
 * seq_test.c - the LXR's sequencer in eDrumSynth (0.3.0):
 *
 *   seq_test PATH/edrumsynth.so [--project LXR2PRESET WORKDIR] [-v]
 *
 * 1. The ported sequencer on the engine (this program's own copy of it, in
 *    forked children): synthetic patterns give the triggers at the blocks
 *    expected from the LXR's timing (24 ticks per step, a tick at the first
 *    engine block at or after its time, one per block): steps, velocities
 *    and notes, flams, shift (both ways, with a flam), probabilities (0,
 *    100, 50 with the fixed seed: repeatable), track lengths (polymeter),
 *    shuffle (also over a flam), mutes, the pattern switch at the pattern's
 *    end, songs (end, loop), locating (into a pattern, a song, past its
 *    end); parameter locks set and restored as the LXR (the render equals
 *    the engine with the parameter set and set back at those blocks); a kit
 *    per pattern.
 * 2. The plugin against the engine driven the same way (bit exact at 44.1
 *    kHz): the host's transport (time:Position: start, stop, locate, tempo
 *    change, a start inside the song), MIDI notes alongside, the Internal
 *    mode; at 48 kHz the steps heard on time (onsets); the panel ports
 *    (step buttons, the step's fields, selection, track length, shuffle,
 *    kit, Euclid, clear, copy / paste, store kit, mutes, the outputs);
 *    the state (save, restore into a new instance, save: the same chunk;
 *    a 0.2.0 state without a project; restore while running: swapped at a
 *    block boundary, the position kept); CPU with the sequencer playing.
 * 3. With --project: tools/lxr2preset.py on synthetic LXR projects
 *    (WORKDIR), the preset's state renders as the engine with the project.
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "lxr_engine.h"
#include "lxr_sequencer.h"
#include "ports.h"
#include "testhost.h"

#include <lv2/atom/atom.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK TH_CHECK
#define NELEM(a) (int) (sizeof (a) / sizeof ((a)[0]))
#define BLOCK LXR_ENGINE_BLOCK
#define TPS LXR_SEQ_TICKS_PER_STEP

static const char *pluginPath;

// ---- engine level: triggers ----

typedef struct
{
	int block, track, velocity, note;
} Trig;

#define MAX_TRIGS 2048
static Trig *trigs;	// in the output's data
static int *numTrigs;

static void onTrigger (uint64_t block, uint8_t track, uint8_t velocity, uint8_t note)
{
	if (*numTrigs < MAX_TRIGS)
	{
		trigs[*numTrigs] = (Trig) { (int) block, track, velocity, note };
		(*numTrigs)++;
	}
}

// the trigger log in a render's output (ints in the float array)
static void logInto (th_output *out)
{
	numTrigs = (int *) out->data;
	trigs = (Trig *) (out->data + 4);
	*numTrigs = 0;
	lxr_seq_set_trigger_fn (onTrigger);
}

static th_output *newLog (void)
{
	return th_output_new ((int) (4 + sizeof (Trig) * MAX_TRIGS / sizeof (float)) / 2 + 4);
}

static int logCount (const th_output *o)
{
	int n;
	memcpy (&n, o->data, sizeof n);
	return n;
}

static const Trig *logTrig (const th_output *o, int i)
{
	return (const Trig *) (o->data + 4) + i;
}

// the block a tick plays in at bpm (the engine's clock: base 0, a block's
// position n * ticks per block): the first block at or after it
static int tickBlock (double tick, double bpm)
{
	const double tpb = bpm / 60.0 * LXR_SEQ_TICKS_PER_QUARTER * BLOCK / 44100.0;
	int b = (int) floor (tick / tpb) - 1;
	if (b < 0)
		b = 0;
	while ((double) b * tpb + 1e-6 < tick)
		b++;
	return b;
}

// a step (on the current project, through the front panel as the LXR's)
typedef struct
{
	int pattern, track, step;
	int velocity, note, prob, flam, shift;	// shift -7..7
	int lock1, lock1Value;			// a modulation target index, 0: none
} StepDef;

static void putSteps (const StepDef *steps, int n)
{
	for (int i = 0; i < n; i++)
	{
		const StepDef *d = &steps[i];
		lxr_seq_select ((uint8_t) d->pattern, (uint8_t) d->track, (uint8_t) d->step);
		lxr_seq_toggle_step ((uint8_t) d->step);
		lxr_seq_set_step_field (LXR_STEP_VELOCITY, (uint8_t) (d->velocity ? d->velocity : 100));
		lxr_seq_set_step_field (LXR_STEP_NOTE, (uint8_t) (d->note ? d->note : 60));
		lxr_seq_set_step_field (LXR_STEP_PROB, (uint8_t) (d->prob ? d->prob - 1 : 100));	// prob: % + 1
		lxr_seq_set_step_field (LXR_STEP_FLAM, (uint8_t) d->flam);
		lxr_seq_set_step_field (LXR_STEP_SHIFT, (uint8_t) (d->shift + 7));
		if (d->lock1)
		{
			lxr_seq_set_step_field (LXR_STEP_LOCK1_TARGET, (uint8_t) d->lock1);
			lxr_seq_set_step_field (LXR_STEP_LOCK1_VALUE, (uint8_t) d->lock1Value);
		}
	}
}

static void setLength (int pattern, int track, int length)
{
	lxr_seq_select ((uint8_t) pattern, (uint8_t) track, 0);
	lxr_seq_set_track_length ((uint8_t) length);
}

// a scenario of the engine level tests
typedef struct
{
	const char *what;
	const StepDef *steps;
	int numSteps;
	double bpm;
	int blocks;
	int shuffle;		// pattern 0's
	int lengths[7];		// pattern 0's (0: 16)
	int muted;		// tracks muted (bits)
	uint32_t seed;		// 0: the default
	int songMode;
	uint8_t song[8];	// song 0 (with songMode)
	int numSong;
	double startTick;
	int switchAt, switchTo;	// lxr_seq_set_pattern (switchTo) at block switchAt (0: none)
	int pattern1Length;	// pattern 1's tracks' length (0: 16)
} Scen;

static void runScen (void *arg, th_output *out)
{
	const Scen *sc = arg;
	lxr_engine_init ();
	if (sc->seed)
	{
		lxr_seq_seed (sc->seed);
	}
	putSteps (sc->steps, sc->numSteps);
	for (int t = 0; t < 7; t++)
	{
		if (sc->lengths[t])
		{
			setLength (0, t, sc->lengths[t]);
		}
		if (sc->pattern1Length)
		{
			setLength (1, t, sc->pattern1Length);
		}
		lxr_seq_set_mute ((uint8_t) t, sc->muted >> t & 1);
	}
	lxr_seq_select (0, 0, 0);
	lxr_seq_set_shuffle ((uint8_t) sc->shuffle);
	if (sc->songMode)
	{
		lxr_project *p = lxr_seq_project_current ();
		uint8_t file[LXR_SEQ_SONG_FILE];
		memset (file, 0, sizeof file);
		memset (file + 8, LXR_SEQ_SONG_END, 64);
		memcpy (file + 8, sc->song, (size_t) sc->numSong);
		lxr_seq_project_set_song (p, 0, file, sizeof file);
		lxr_seq_set_song (0);
		lxr_seq_set_song_mode (1);
	}
	logInto (out);
	lxr_seq_set_tempo (sc->bpm);
	lxr_seq_start (sc->startTick);
	int16_t l[BLOCK], r[BLOCK];
	for (int b = 0; b < sc->blocks; b++)
	{
		if (sc->switchAt && b == sc->switchAt)
		{
			lxr_seq_set_pattern ((uint8_t) sc->switchTo);
		}
		lxr_seq_block ();
		memset (l, 0, sizeof l);
		memset (r, 0, sizeof r);
		lxr_engine_render (l, r);
	}
	out->ok = lxr_seq_running ();
}

// expected triggers: (tick, track, velocity, note); the blocks at bpm
typedef struct
{
	double tick;
	int track, velocity, note;
} Exp;

static th_output *runLog (const Scen *sc)
{
	th_output *o = newLog ();
	th_fork (runScen, (void *) sc, o, sc->what);
	return o;
}

static void printLog (const th_output *o, int max)
{
	for (int i = 0; i < logCount (o) && i < max; i++)
	{
		const Trig *t = logTrig (o, i);
		printf ("    block %d track %d velocity %d note %d\n", t->block, t->track, t->velocity, t->note);
	}
}

// the log is the expected triggers (blocks from ticks, or given as blocks
// when block >= 0), in order
static void checkLog (const char *what, const th_output *o, const Exp *exp, int n, double bpm, const int *blocks)
{
	int ok = logCount (o) == n;
	for (int i = 0; ok && i < n; i++)
	{
		const Trig *t = logTrig (o, i);
		const int b = blocks ? blocks[i] : tickBlock (exp[i].tick, bpm);
		ok = t->block == b && t->track == exp[i].track && t->velocity == exp[i].velocity && t->note == exp[i].note;
		if (!ok)
		{
			printf ("  %s: trigger %d: block %d track %d velocity %d note %d, expected block %d (tick %.1f) track %d "
				"velocity %d note %d\n",
				what, i, t->block, t->track, t->velocity, t->note, b, exp[i].tick, exp[i].track, exp[i].velocity,
				exp[i].note);
		}
	}
	CHECK (ok, "%s: %d triggers, expected %d", what, logCount (o), n);
	if (!ok && th_verbose)
	{
		printLog (o, 64);
	}
	if (ok)
	{
		printf ("%s: %d triggers as expected\n", what, n);
	}
}

static void checkSteps (void)
{
	// one bar at 120 bpm: bass drum on the quarters, velocities; the snare
	// on 4 and 12 with a note; the open hi-hat on 15; two bars
	static const StepDef steps[] = {
		{ 0, 0, 0, 127, 0, 0, 0, 0, 0, 0 }, { 0, 0, 4, 90, 0, 0, 0, 0, 0, 0 }, { 0, 0, 8, 60, 0, 0, 0, 0, 0, 0 },
		{ 0, 0, 12, 30, 0, 0, 0, 0, 0, 0 }, { 0, 3, 4, 100, 67, 0, 0, 0, 0, 0 }, { 0, 3, 12, 100, 0, 0, 0, 0, 0, 0 },
		{ 0, 6, 15, 80, 0, 0, 0, 0, 0, 0 },
	};
	Scen sc = { "steps", steps, NELEM (steps), 120, 2800, 0, { 0 }, 0, 0, 0, { 0 }, 0, 0, 0, 0, 0 };
	th_output *o = runLog (&sc);
	Exp exp[16];
	int n = 0;
	for (int bar = 0; bar < 2; bar++)
	{
		const double t0 = bar * 16 * TPS;
		exp[n++] = (Exp) { t0, 0, 127, 60 };
		exp[n++] = (Exp) { t0 + 4 * TPS, 0, 90, 60 };
		exp[n++] = (Exp) { t0 + 4 * TPS, 3, 100, 67 };
		exp[n++] = (Exp) { t0 + 8 * TPS, 0, 60, 60 };
		exp[n++] = (Exp) { t0 + 12 * TPS, 0, 30, 60 };
		exp[n++] = (Exp) { t0 + 12 * TPS, 3, 100, 60 };
		exp[n++] = (Exp) { t0 + 15 * TPS, 6, 80, 60 };
	}
	// 2800 blocks: 4.06 s, past the start of bar 3 (block 2757)
	exp[n++] = (Exp) { 32 * TPS, 0, 127, 60 };
	checkLog ("steps (120 bpm)", o, exp, n, 120, NULL);
	CHECK (o->ok, "steps: not running");
	th_output_free (o);

	// another tempo
	sc.bpm = 97.5;
	sc.blocks = 1696;	// bar 1 (bar 2 at 1696.5)
	sc.what = "steps 97.5";
	o = runLog (&sc);
	n = 7;
	checkLog ("steps (97.5 bpm)", o, exp, n, 97.5, NULL);
	th_output_free (o);
}

static void checkFlamShift (void)
{
	// flam x3 on the bass drum's step 4: ticks 0, 8, 16 of the step (the
	// LXR's flam patterns); x8 on step 8: every 3 ticks; snare shifted +2
	// (2 x 3 ticks) on step 8, the hi-hat -3 on step 12 (9 ticks into step
	// 11); a tom with x2 shifted +4: tick 12 of step 2 and tick 0 of step 3
	// (the second hit shifted into the next step)
	static const StepDef steps[] = {
		{ 0, 0, 4, 100, 0, 0, 2, 0, 0, 0 }, { 0, 0, 8, 100, 0, 0, 5, 0, 0, 0 }, { 0, 3, 8, 110, 0, 0, 0, 2, 0, 0 },
		{ 0, 5, 12, 90, 0, 0, 0, -3, 0, 0 }, { 0, 1, 2, 70, 0, 0, 1, 4, 0, 0 },
	};
	const Scen sc = { "flam / shift", steps, NELEM (steps), 120, 1100, 0, { 0 }, 0, 0, 0, { 0 }, 0, 0, 0, 0, 0 };
	th_output *o = runLog (&sc);
	Exp exp[32];
	int n = 0;
	exp[n++] = (Exp) { 2 * TPS + 12, 1, 70, 60 };
	exp[n++] = (Exp) { 3 * TPS, 1, 70, 60 };
	for (int k = 0; k < 3; k++)
		exp[n++] = (Exp) { 4 * TPS + 8 * k, 0, 100, 60 };
	for (int k = 0; k < 8; k++)
	{
		exp[n++] = (Exp) { 8 * TPS + 3 * k, 0, 100, 60 };
		if (k == 2)
			exp[n++] = (Exp) { 8 * TPS + 6, 3, 110, 60 };
	}
	exp[n++] = (Exp) { 11 * TPS + 15, 5, 90, 60 };
	checkLog ("flams and shift", o, exp, n, 120, NULL);
	th_output_free (o);
}

static void checkProbability (void)
{
	// every step of the bass drum at 0 %, of the snare at 100 %, of the
	// cymbal at 50 %: 4 bars
	StepDef steps[48];
	int n = 0;
	for (int i = 0; i < 16; i++)
	{
		steps[n++] = (StepDef) { 0, 0, i, 100, 0, 0 + 1, 0, 0, 0, 0 };
		steps[n++] = (StepDef) { 0, 3, i, 100, 0, 100 + 1, 0, 0, 0, 0 };
		steps[n++] = (StepDef) { 0, 4, i, 100, 0, 50 + 1, 0, 0, 0, 0 };
	}
	Scen sc = { "probability", steps, n, 120, 64 * 345 / 4, 0, { 0 }, 0, 0, 0, { 0 }, 0, 0, 0, 0, 0 };
	th_output *a = runLog (&sc), *b = runLog (&sc);
	sc.seed = 12345;
	th_output *c = runLog (&sc);
	int bd = 0, sd = 0, cy = 0, cyC = 0, same = logCount (a) == logCount (b);
	for (int i = 0; i < logCount (a); i++)
	{
		const Trig *t = logTrig (a, i);
		bd += t->track == 0;
		sd += t->track == 3;
		cy += t->track == 4;
		same = same && !memcmp (t, logTrig (b, i), sizeof *t);
	}
	for (int i = 0; i < logCount (c); i++)
	{
		cyC += logTrig (c, i)->track == 4;
	}
	const int steps64 = (int) ceil (sc.blocks / (5512.5 / 64));
	printf ("probability: 0 %%: %d of %d steps, 100 %%: %d, 50 %%: %d (another seed: %d); repeated: %s\n", bd, steps64, sd,
		cy, cyC, same ? "the same" : "different");
	// the LXR compares a step with the random value drawn at the track's
	// active step before (prev / current / next for shifted steps), 0 at
	// first: the first step plays whatever its probability
	CHECK (bd == 1, "probability 0: %d triggers (the first step only)", bd);
	CHECK (sd == steps64, "probability 100: %d of %d", sd, steps64);
	CHECK (cy > steps64 / 4 && cy < steps64 * 3 / 4, "probability 50: %d of %d", cy, steps64);
	CHECK (same, "probability: a second render differs");
	th_output_free (a);
	th_output_free (b);
	th_output_free (c);
}

static void checkPolymeter (void)
{
	// the bass drum's track 3 steps long (step 0), the snare 5 (step 1),
	// the hi-hat 16 (step 0: the master): they run against each other over
	// the pattern's repeats
	static const StepDef steps[] = { { 0, 0, 0, 100, 0, 0, 0, 0, 0, 0 }, { 0, 3, 1, 100, 0, 0, 0, 0, 0, 0 },
					 { 0, 5, 0, 100, 0, 0, 0, 0, 0, 0 } };
	const Scen sc = { "polymeter", steps, NELEM (steps), 120, 2700, 0, { 3, 0, 0, 5, 0, 0, 0 }, 0, 0, 0, { 0 }, 0, 0, 0, 0, 0 };
	th_output *o = runLog (&sc);
	Exp exp[64];
	int n = 0;
	for (int g = 0; g < 32 && tickBlock (g * TPS, 120) < sc.blocks; g++)
	{
		if (g % 3 == 0)
			exp[n++] = (Exp) { g * TPS, 0, 100, 60 };
		if (g % 5 == 1)
			exp[n++] = (Exp) { g * TPS, 3, 100, 60 };
		if (g % 16 == 0)
			exp[n++] = (Exp) { g * TPS, 5, 100, 60 };
	}
	checkLog ("polymeter (3, 5, 16 steps)", o, exp, n, 120, NULL);
	th_output_free (o);
}

static void checkShuffle (void)
{
	// shuffle 127 (16 ticks): the odd steps' first tick 16 ticks late; the
	// bass drum on 0, 1, 2, 3; a flam x2 on the snare's step 3: its second
	// hit (tick 12) is overdue when the step's first tick plays, the ticks
	// after it follow one per block (the LXR's seq_tick())
	static const StepDef steps[] = { { 0, 0, 0, 100, 0, 0, 0, 0, 0, 0 }, { 0, 0, 1, 100, 0, 0, 0, 0, 0, 0 },
					 { 0, 0, 2, 100, 0, 0, 0, 0, 0, 0 }, { 0, 0, 3, 100, 0, 0, 0, 0, 0, 0 },
					 { 0, 3, 3, 100, 0, 0, 1, 0, 0, 0 } };
	const Scen sc = { "shuffle", steps, NELEM (steps), 120, 345, 127, { 0 }, 0, 0, 0, { 0 }, 0, 0, 0, 0, 0 };
	th_output *o = runLog (&sc);
	const Exp exp[] = { { 0, 0, 100, 60 }, { TPS + 16, 0, 100, 60 }, { 2 * TPS, 0, 100, 60 }, { 3 * TPS + 16, 0, 100, 60 },
			    { 3 * TPS + 16, 3, 100, 60 }, { 0, 3, 100, 60 } };
	const int b3 = tickBlock (3 * TPS + 16, 120);
	const int blocks[] = { 0, tickBlock (TPS + 16, 120), tickBlock (2 * TPS, 120), b3, b3, b3 + 12 };
	checkLog ("shuffle 127 (with a flam)", o, exp, NELEM (exp), 120, blocks);
	th_output_free (o);

	// shuffle 64: 64 / 127 x 16 ticks
	const Scen sc2 = { "shuffle 64", steps, 4, 120, 345, 64, { 0 }, 0, 0, 0, { 0 }, 0, 0, 0, 0, 0 };
	o = runLog (&sc2);
	const double d = 16.0 * 64 / 127;
	const Exp exp2[] = { { 0, 0, 100, 60 }, { TPS + d, 0, 100, 60 }, { 2 * TPS, 0, 100, 60 }, { 3 * TPS + d, 0, 100, 60 } };
	checkLog ("shuffle 64", o, exp2, NELEM (exp2), 120, NULL);
	th_output_free (o);
}

static void checkMute (void)
{
	static const StepDef steps[] = { { 0, 0, 0, 100, 0, 0, 0, 0, 0, 0 }, { 0, 3, 4, 100, 0, 0, 0, 0, 0, 0 } };
	const Scen sc = { "mute", steps, NELEM (steps), 120, 345 * 2, 0, { 0 }, 1 << 3, 0, 0, { 0 }, 0, 0, 0, 0, 0 };
	th_output *o = runLog (&sc);
	const Exp exp[] = { { 0, 0, 100, 60 } };
	checkLog ("mute (the snare)", o, exp, NELEM (exp), 120, NULL);
	th_output_free (o);
}

static void checkPatternSwitch (void)
{
	// pattern 0: the bass drum on 0 and 8; pattern 1 (8 steps): the snare
	// on 0; pattern 1 asked for in step 5: it starts at step 16
	static const StepDef steps[] = { { 0, 0, 0, 100, 0, 0, 0, 0, 0, 0 }, { 0, 0, 8, 100, 0, 0, 0, 0, 0, 0 },
					 { 1, 3, 0, 100, 0, 0, 0, 0, 0, 0 } };
	const Scen sc = { "pattern switch", steps, NELEM (steps), 120, tickBlock (40 * TPS, 120) + 1, 0, { 0 }, 0, 0, 0, { 0 }, 0,
			  0, tickBlock (5 * TPS, 120) + 1, 1, 8 };
	th_output *o = runLog (&sc);
	const Exp exp[] = { { 0, 0, 100, 60 }, { 8 * TPS, 0, 100, 60 }, { 16 * TPS, 3, 100, 60 }, { 24 * TPS, 3, 100, 60 },
			    { 32 * TPS, 3, 100, 60 }, { 40 * TPS, 3, 100, 60 } };
	checkLog ("pattern switch at the pattern's end", o, exp, NELEM (exp), 120, NULL);
	th_output_free (o);
}

static void checkSongs (void)
{
	// song: 0, 1, 1, end (pattern 0: 16 steps, the bass drum on 0;
	// pattern 1: 8 steps, the snare on 0 and 4): bass drum 0, snare 16,
	// 20, 24, 28, then stopped
	static const StepDef steps[] = { { 0, 0, 0, 100, 0, 0, 0, 0, 0, 0 }, { 1, 3, 0, 100, 0, 0, 0, 0, 0, 0 },
					 { 1, 3, 4, 90, 0, 0, 0, 0, 0, 0 } };
	Scen sc = { "song end", steps, NELEM (steps), 120, tickBlock (48 * TPS, 120), 0, { 0 }, 0, 0, 1, { 0, 1, 1, LXR_SEQ_SONG_END }, 4,
		    0, 0, 0, 8 };
	th_output *o = runLog (&sc);
	const Exp exp[] = { { 0, 0, 100, 60 }, { 16 * TPS, 3, 100, 60 }, { 20 * TPS, 3, 90, 60 }, { 24 * TPS, 3, 100, 60 },
			    { 28 * TPS, 3, 90, 60 } };
	checkLog ("song (end)", o, exp, NELEM (exp), 120, NULL);
	CHECK (!o->ok, "song end: still running");
	th_output_free (o);

	// with a loop mark: 0, 1, loop: 0 (0), 1 (16, 20), 0 (24), 1 (40, 44)
	const uint8_t loop[] = { 0, 1, LXR_SEQ_SONG_LOOP };
	memcpy (sc.song, loop, sizeof loop);
	sc.numSong = 3;
	sc.what = "song loop";
	sc.blocks = tickBlock (48 * TPS, 120) + 1;
	o = runLog (&sc);
	const Exp exp2[] = { { 0, 0, 100, 60 }, { 16 * TPS, 3, 100, 60 }, { 20 * TPS, 3, 90, 60 }, { 24 * TPS, 0, 100, 60 },
			     { 40 * TPS, 3, 100, 60 }, { 44 * TPS, 3, 90, 60 }, { 48 * TPS, 0, 100, 60 } };
	checkLog ("song (loop)", o, exp2, NELEM (exp2), 120, NULL);
	CHECK (o->ok, "song loop: stopped");
	th_output_free (o);

	// started inside the song: step 21 (pattern 1, its step 5): the next
	// snare at 24 (the repeat of pattern 1: its step 0), 28; then the end
	sc.song[2] = 1;
	sc.song[3] = LXR_SEQ_SONG_END;
	sc.numSong = 4;
	sc.startTick = 21 * TPS;
	sc.what = "song locate";
	const Exp exp3[] = { { 24 * TPS - 21 * TPS, 3, 100, 60 }, { 28 * TPS - 21 * TPS, 3, 90, 60 } };
	o = runLog (&sc);
	checkLog ("song, started at step 21", o, exp3, NELEM (exp3), 120, NULL);
	th_output_free (o);

	// a start past the end: stopped
	sc.startTick = 40 * TPS;
	sc.what = "song past its end";
	o = runLog (&sc);
	CHECK (logCount (o) == 0 && !o->ok, "song, started past its end: %d triggers, running %d", logCount (o), o->ok);
	if (logCount (o) == 0 && !o->ok)
	{
		printf ("song, started past its end: stopped\n");
	}
	th_output_free (o);

	// a pattern started in step 5 of the bar (no song): the tracks as if
	// played from the start (3-step bass drum: its step 2 at 5 -> next 0 at 6)
	static const StepDef poly[] = { { 0, 0, 0, 100, 0, 0, 0, 0, 0, 0 } };
	Scen sp = { "locate", poly, 1, 120, tickBlock (8 * TPS, 120), 0, { 3 }, 0, 0, 0, { 0 }, 0, 5 * TPS, 0, 0, 0 };
	o = runLog (&sp);
	const Exp exp4[] = { { TPS, 0, 100, 60 }, { 4 * TPS, 0, 100, 60 }, { 7 * TPS, 0, 100, 60 } };
	checkLog ("pattern, started at step 5 (3-step track)", o, exp4, NELEM (exp4), 120, NULL);
	th_output_free (o);
}

// ---- engine level: renders (locks, kits) ----

static int blockOfStep (int step)
{
	return tickBlock (step * TPS, 120);
}

static void renderBlocks (th_output *out, int from, int to)
{
	int16_t l[BLOCK], r[BLOCK];
	for (int b = from; b < to; b++)
	{
		lxr_seq_block ();
		memset (l, 0, sizeof l);
		memset (r, 0, sizeof r);
		lxr_engine_render (l, r);
		for (int i = 0; i < BLOCK && b * BLOCK + i < out->frames; i++)
		{
			out->data[b * BLOCK + i] = l[i] / 32768.f;
			out->data[out->frames + b * BLOCK + i] = r[i] / 32768.f;
		}
	}
}

// the locks: the bass drum's step 0 locks its coarse tune (parameter 8,
// modulation target "1 coa") to 90 and its decay to 40 (lock 2); step 4
// plays without locks (the parameters back to the kit's), step 8 locks
// the coarse tune again
static int targetOf (uint16_t param)
{
	// the modulation target index of a parameter (as the panel shows it)
	for (int i = 1; i < 256; i++)
	{
		lxr_seq_select (63, 6, 63);
		lxr_seq_set_step_field (LXR_STEP_LOCK1_TARGET, (uint8_t) i);
		if (lxr_seq_get_step_field (63, 6, 63, LXR_STEP_LOCK1_TARGET) != i)
		{
			break;
		}
		// the stored number: the parameter + 1 below 128 (the front panel's)
		uint8_t file[LXR_SEQ_PATTERN_FILE];
		lxr_seq_project_get_pattern (lxr_seq_project_current (), 63, file);
		const uint8_t stored = file[14 + 6 * 512 + 63 * 7 + 3];
		if ((stored < 128 ? stored - 1 : stored) == param)
		{
			lxr_seq_clear_pattern ();
			return i;
		}
	}
	return -1;
}

static void lockSeq (void *arg, th_output *out)
{
	(void) arg;
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	const int coarse = targetOf (8), decay = targetOf (50);
	out->ok = coarse > 0 && decay > 0;
	const StepDef steps[] = { { 0, 0, 0, 120, 0, 0, 0, 0, coarse, 90 }, { 0, 0, 4, 120, 0, 0, 0, 0, 0, 0 },
				  { 0, 0, 8, 120, 0, 0, 0, 0, coarse, 30 } };
	putSteps (steps, NELEM (steps));
	lxr_seq_select (0, 0, 0);
	lxr_seq_set_step_field (LXR_STEP_LOCK2_TARGET, (uint8_t) decay);
	lxr_seq_set_step_field (LXR_STEP_LOCK2_VALUE, 40);
	lxr_seq_set_tempo (120);
	lxr_seq_start (0);
	renderBlocks (out, 0, out->frames / BLOCK);
}

static void lockRef (void *arg, th_output *out)
{
	(void) arg;
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	const uint8_t coarse0 = lxr_engine_get_parameter (8), decay0 = lxr_engine_get_parameter (50);
	int16_t l[BLOCK], r[BLOCK];
	for (int b = 0; b < out->frames / BLOCK; b++)
	{
		if (b == blockOfStep (0))
		{
			lxr_engine_set_parameter (8, 90);
			lxr_engine_set_parameter (50, 40);
			lxr_engine_trigger (0, 120, 60);
		}
		if (b == blockOfStep (4))
		{
			lxr_engine_set_parameter (8, coarse0);
			lxr_engine_set_parameter (50, decay0);
			lxr_engine_trigger (0, 120, 60);
		}
		if (b == blockOfStep (8))
		{
			lxr_engine_set_parameter (8, 30);
			lxr_engine_trigger (0, 120, 60);
		}
		memset (l, 0, sizeof l);
		memset (r, 0, sizeof r);
		lxr_engine_render (l, r);
		for (int i = 0; i < BLOCK; i++)
		{
			out->data[b * BLOCK + i] = l[i] / 32768.f;
			out->data[out->frames + b * BLOCK + i] = r[i] / 32768.f;
		}
	}
	out->ok = 1;
}

static void checkLocks (void)
{
	const int frames = blockOfStep (12) * BLOCK;
	th_output *a = th_output_new (frames), *b = th_output_new (frames);
	th_fork (lockSeq, NULL, a, "locks");
	th_fork (lockRef, NULL, b, "locks reference");
	CHECK (a->ok, "locks: no modulation target for parameters 8 / 50");
	CHECK (th_peak (a) > 0.05f, "locks: silent");
	CHECK (th_identical (a, b), "locks: not the engine with the parameters set and set back (first difference at %d)",
	       th_first_difference (a, b));
	if (a->ok && th_identical (a, b))
	{
		printf ("locks: set at the step, back to the kit's value at the track's next step (bit exact)\n");
	}
	th_output_free (a);
	th_output_free (b);
}

// a kit per pattern: pattern 0 with kit 1 (the bass drum's coarse tune
// 40), pattern 1 with kit 2 (70), song 0, 1, 0: the kit changes at the
// pattern's first step
static uint8_t kitFile[3][LXR_SEQ_KIT_FILE];

static void makeKits (void)
{
	static int made;
	if (made)
	{
		return;
	}
	made = 1;
	lxr_engine_init ();
	for (int k = 0; k < 3; k++)
	{
		uint8_t params[256];
		lxr_engine_get_kit (params, lxr_engine_kit_size ());
		params[8] = (uint8_t) (k == 1 ? 40 : k == 2 ? 70 : params[8]);
		params[50] = (uint8_t) (20 + 30 * k);	// the bass drum's decay
		memset (kitFile[k], 0, LXR_SEQ_KIT_FILE);
		snprintf ((char *) kitFile[k], 8, "KIT%d", k);
		memcpy (kitFile[k] + 8, params, lxr_engine_kit_size ());
		uint8_t engines[6];
		lxr_engine_get_engines (engines);
		if (k == 2)
		{
			engines[2] = 1;	// tom 2 a snare
		}
		lxr_engine_encode_engines (engines, kitFile[k] + 8 + lxr_engine_kit_size ());
	}
}

typedef struct
{
	int blocks;
	uint8_t coarse[4000];	// the bass drum's coarse tune after each block
	uint8_t engine2[4000];
	int loads;
} KitLog;

static void kitSeq (void *arg, th_output *out)
{
	(void) arg;
	KitLog *log = (KitLog *) out->data;
	lxr_engine_init ();
	lxr_project *p = lxr_seq_project_new ();
	lxr_seq_project_set_kit (p, 1, kitFile[1], LXR_SEQ_KIT_FILE);
	lxr_seq_project_set_kit (p, 2, kitFile[2], LXR_SEQ_KIT_FILE);
	uint8_t file[LXR_SEQ_PATTERN_FILE];
	lxr_seq_project_get_pattern (p, 0, file);
	file[14 + 7 * 512 + 56 + 1 + 7] = 1;	// pattern 0: kit 1
	lxr_seq_project_set_pattern (p, 0, file, sizeof file);
	file[14 + 7 * 512 + 56 + 1 + 7] = 2;	// pattern 1: kit 2
	lxr_seq_project_set_pattern (p, 1, file, sizeof file);
	uint8_t song[LXR_SEQ_SONG_FILE] = { 'S' };
	memset (song + 8, LXR_SEQ_SONG_END, 64);
	song[8] = 0;
	song[9] = 1;
	song[10] = 0;
	lxr_seq_project_set_song (p, 0, song, sizeof song);
	lxr_seq_project_set_start (p, 0, 0, 1);
	lxr_seq_project_free (lxr_seq_project_install (p));
	lxr_seq_set_tempo (120);
	lxr_seq_start (0);
	log->blocks = blockOfStep (40);
	int16_t l[BLOCK], r[BLOCK];
	for (int b = 0; b < log->blocks; b++)
	{
		lxr_seq_block ();
		lxr_engine_render (l, r);
		log->coarse[b] = lxr_engine_get_parameter (8);
		log->engine2[b] = lxr_engine_get_engine (2);
	}
	lxr_seq_status st;
	lxr_seq_get_status (&st);
	log->loads = (int) st.kitLoads;
}

static void checkKits (void)
{
	makeKits ();
	th_output *o = th_output_new ((int) sizeof (KitLog));
	th_fork (kitSeq, NULL, o, "kits");
	const KitLog *log = (const KitLog *) o->data;
	// install: kit 1 (pattern 0's, kit autoload); step 16: kit 2; step 32: kit 1
	const int b16 = blockOfStep (16), b32 = blockOfStep (32);
	int ok = log->coarse[0] == 40 && log->coarse[b16 - 1] == 40 && log->coarse[b16] == 70 && log->engine2[b16] == 1
		 && log->coarse[b32 - 1] == 70 && log->coarse[b32] == 40 && log->engine2[b32] == 0 && log->loads == 3;
	CHECK (ok, "kits per pattern: coarse %d %d %d %d %d, tom 2 engine %d %d, %d loads", log->coarse[0], log->coarse[b16 - 1],
	       log->coarse[b16], log->coarse[b32 - 1], log->coarse[b32], log->engine2[b16], log->engine2[b32], log->loads);
	if (ok)
	{
		printf ("kits per pattern: loaded at the patterns' first steps (blocks %d, %d), engines included\n", b16, b32);
	}
	th_output_free (o);
}

// ---- the plugin ----

typedef struct
{
	const LV2_Descriptor *d;
	LV2_Handle h;
	float controls[EDS_NUM_CONTROLS];
	float panel[EDS_NUM_PANEL];
	float out[2][4096];
	uint64_t seq[8192];
} Plugin;

static int panelIndex (const char *symbol)
{
	return th_control_index (symbol) - EDS_NUM_CONTROLS;
}

static Plugin *pluginNew (double rate)
{
	Plugin *p = calloc (1, sizeof *p);
	p->d = th_load (pluginPath, EDS_URI);
	p->h = p->d->instantiate (p->d, rate, "", th_features ());
	if (!p->h)
	{
		fprintf (stderr, "instantiate failed\n");
		exit (2);
	}
	for (int i = 0; i < EDS_NUM_CONTROLS; i++)
	{
		p->controls[i] = eds_controls[i].def;
		p->d->connect_port (p->h, (uint32_t) (EDS_PORT_CONTROL0 + i), &p->controls[i]);
	}
	for (int i = 0; i < EDS_NUM_PANEL; i++)
	{
		p->panel[i] = eds_panel[i].output ? -999.f : eds_panel[i].def;
		p->d->connect_port (p->h, (uint32_t) (EDS_PORT_PANEL0 + i), &p->panel[i]);
	}
	p->d->connect_port (p->h, EDS_PORT_MIDI_IN, p->seq);
	p->d->connect_port (p->h, EDS_PORT_OUT_L, p->out[0]);
	p->d->connect_port (p->h, EDS_PORT_OUT_R, p->out[1]);
	p->d->activate (p->h);
	th_seq_clear ((LV2_Atom_Sequence *) p->seq);
	return p;
}

static void pluginFree (Plugin *p)
{
	p->d->deactivate (p->h);
	p->d->cleanup (p->h);
	free (p);
}

static LV2_Atom_Sequence *events (Plugin *p)
{
	return (LV2_Atom_Sequence *) p->seq;
}

// one run of n frames, the output at pos of out (if any); the events
// cleared after
static void pluginRun (Plugin *p, int n, th_output *out, int pos)
{
	p->d->run (p->h, (uint32_t) n);
	if (out)
	{
		for (int i = 0; i < n && pos + i < out->frames; i++)
		{
			out->data[pos + i] = p->out[0][i];
			out->data[out->frames + pos + i] = p->out[1][i];
		}
	}
	th_seq_clear (events (p));
}

static float panelOut (const Plugin *p, const char *symbol)
{
	return p->panel[panelIndex (symbol)];
}

static void setPanel (Plugin *p, const char *symbol, float v)
{
	p->panel[panelIndex (symbol)] = v;
}

// a trigger: up for one run, down for the next
static void press (Plugin *p, const char *symbol)
{
	setPanel (p, symbol, 1);
	pluginRun (p, 64, NULL, 0);
	setPanel (p, symbol, 0);
	pluginRun (p, 64, NULL, 0);
}

// the state: store / retrieve one property

typedef struct
{
	LV2_URID key, type;
	uint32_t flags;
	size_t size;
	uint8_t *data;
} Stored;

static LV2_State_Status storeFn (LV2_State_Handle handle, uint32_t key, const void *value, size_t size, uint32_t type,
				 uint32_t flags)
{
	Stored *s = handle;
	free (s->data);
	s->data = malloc (size);
	memcpy (s->data, value, size);
	s->size = size;
	s->key = key;
	s->type = type;
	s->flags = flags;
	return LV2_STATE_SUCCESS;
}

static const void *retrieveFn (LV2_State_Handle handle, uint32_t key, size_t *size, uint32_t *type, uint32_t *flags)
{
	Stored *s = handle;
	if (!s->data || key != s->key)
	{
		return NULL;
	}
	*size = s->size;
	*type = s->type;
	*flags = s->flags;
	return s->data;
}

static const LV2_State_Interface *stateOf (const Plugin *p)
{
	return p->d->extension_data (LV2_STATE__interface);
}

static int pluginSave (Plugin *p, Stored *st)
{
	memset (st, 0, sizeof *st);
	return stateOf (p)->save (p->h, storeFn, st, 0, th_features ()) == LV2_STATE_SUCCESS && st->data
	       && st->key == th_map (EDS_URI "#project") && st->type == th_map (LV2_ATOM__Chunk);
}

static int pluginRestore (Plugin *p, const uint8_t *chunk, size_t size)
{
	Stored st = { th_map (EDS_URI "#project"), th_map (LV2_ATOM__Chunk), LV2_STATE_IS_POD, size, (uint8_t *) chunk };
	return stateOf (p)->restore (p->h, retrieveFn, &st, 0, th_features ()) == LV2_STATE_SUCCESS;
}

// a project for the plugin tests (built with this program's engine): the
// bass drum on the quarters (step 12 with a flam), the snare on 4 and 12,
// the closed hi-hat on the eighths (2 bars long, the open one on its last
// step), shuffle 30; pattern 1: the toms
static size_t buildProject (uint8_t *chunk, size_t cap)
{
	lxr_engine_init ();
	static const StepDef steps[] = {
		{ 0, 0, 0, 127, 0, 0, 0, 0, 0, 0 }, { 0, 0, 4, 100, 0, 0, 0, 0, 0, 0 }, { 0, 0, 8, 110, 0, 0, 0, 0, 0, 0 },
		{ 0, 0, 12, 100, 0, 0, 1, 0, 0, 0 }, { 0, 3, 4, 120, 0, 0, 0, 0, 0, 0 }, { 0, 3, 12, 120, 62, 0, 0, 0, 0, 0 },
		{ 0, 5, 0, 80, 0, 0, 0, 0, 0, 0 }, { 0, 5, 2, 60, 0, 0, 0, 0, 0, 0 }, { 0, 5, 6, 60, 0, 0, 0, 1, 0, 0 },
		{ 0, 6, 31, 70, 0, 0, 0, 0, 0, 0 }, { 1, 1, 0, 100, 0, 0, 0, 0, 0, 0 }, { 1, 2, 2, 100, 0, 0, 0, 0, 0, 0 },
	};
	putSteps (steps, NELEM (steps));
	setLength (0, 5, 32);
	setLength (0, 6, 32);
	lxr_seq_select (0, 0, 0);
	lxr_seq_set_shuffle (30);
	lxr_seq_select (64, 0, 0);
	return lxr_seq_chunk_write (lxr_seq_project_current (), chunk, cap);
}

static uint8_t projectChunk[300000];
static size_t projectSize;

// the engine driven as the plugin: the project installed, then actions at
// blocks
typedef struct
{
	int block;
	enum { A_START, A_STOP, A_LOCATE, A_TEMPO_POS, A_NOTE, A_INSTALL } action;
	double tick, bpm;
	int track;
} Action;

typedef struct
{
	const Action *actions;
	int numActions;
	const uint8_t *chunk2;	// A_INSTALL: this project
	size_t size2;
} Script;

static void installChunk (const uint8_t *chunk, size_t size)
{
	lxr_project *p = lxr_seq_project_new ();
	if (lxr_seq_chunk_read (p, chunk, size))
	{
		exit (3);
	}
	lxr_seq_project_free (lxr_seq_project_install (p));
}

static void scriptRef (void *arg, th_output *out)
{
	const Script *sc = arg;
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	installChunk (projectChunk, projectSize);
	int16_t l[BLOCK], r[BLOCK];
	for (int b = 0; b * BLOCK < out->frames; b++)
	{
		for (int i = 0; i < sc->numActions; i++)
		{
			const Action *a = &sc->actions[i];
			if (a->block != b)
				continue;
			switch (a->action)
			{
			case A_START:
				lxr_seq_set_tempo (a->bpm);
				lxr_seq_start (a->tick);
				break;
			case A_STOP:
				lxr_seq_stop ();
				break;
			case A_LOCATE:
				lxr_seq_set_tempo (a->bpm);
				lxr_seq_locate (a->tick);
				break;
			case A_TEMPO_POS:
				lxr_seq_set_tempo (a->bpm);
				lxr_seq_set_position (a->tick);
				break;
			case A_NOTE:
				lxr_engine_trigger ((uint8_t) a->track, 100, 60);
				break;
			case A_INSTALL:
				// installed (located there when running), the position from
				// the host's (as the plugin: syncHost())
				installChunk (sc->chunk2, sc->size2);
				lxr_seq_set_tempo (a->bpm);
				lxr_seq_set_position (a->tick);
				break;
			}
		}
		lxr_seq_block ();
		memset (l, 0, sizeof l);
		memset (r, 0, sizeof r);
		lxr_engine_render (l, r);
		for (int i = 0; i < BLOCK && b * BLOCK + i < out->frames; i++)
		{
			out->data[b * BLOCK + i] = l[i] / 32768.f;
			out->data[out->frames + b * BLOCK + i] = r[i] / 32768.f;
		}
	}
	out->ok = 1;
}

// the host's events of a transport test, at the start of a run
typedef struct
{
	int frame;		// a multiple of the host block
	float speed;
	int bar;		// -1: no bar / beat
	float barBeat, bpm;
	int note;		// a MIDI note (GM) instead of a position, 0: none
} HostEv;

typedef struct
{
	const HostEv *evs;
	int numEvs;
	int block;
	const uint8_t *chunk2;	// restored before the run at frame restoreAt
	size_t size2;
	int restoreAt;
	const char *panelSym[4];	// panel settings from the start
	float panelVal[4];
} HostScen;

static void hostRender (void *arg, th_output *out)
{
	const HostScen *sc = arg;
	Plugin *p = pluginNew (44100);
	for (int i = 0; i < 4 && sc->panelSym[i]; i++)
	{
		setPanel (p, sc->panelSym[i], sc->panelVal[i]);
	}
	CHECK (pluginRestore (p, projectChunk, projectSize), "restore failed");
	for (int pos = 0; pos < out->frames; pos += sc->block)
	{
		for (int i = 0; i < sc->numEvs; i++)
		{
			const HostEv *e = &sc->evs[i];
			if (e->frame == pos)
			{
				if (e->note)
				{
					const uint8_t msg[3] = { 0x99, (uint8_t) e->note, 100 };
					th_seq_midi (events (p), sizeof p->seq, 0, msg, 3);
				}
				else
				{
					th_seq_position (events (p), sizeof p->seq, 0, pos, e->speed, e->bar, e->barBeat, e->bpm);
				}
			}
		}
		if (sc->chunk2 && pos == sc->restoreAt)
		{
			CHECK (pluginRestore (p, sc->chunk2, sc->size2), "restore while running failed");
		}
		pluginRun (p, sc->block, out, pos);
	}
	out->ok = th_failures == 0;
	pluginFree (p);
}

static void checkHostTransport (void)
{
	projectSize = buildProject (projectChunk, sizeof projectChunk);
	const int block = 256, frames = 44100 * 7;
	// 120 bpm: a quarter is 22050 frames; positions at run starts
	// (multiples of 256 frames = 4 engine blocks)
	const int f1 = 256 * 300;	// 76800: tempo change to 140 there
	const double q1 = f1 / 22050.0;	// quarters at f1
	const int f2 = 256 * 500;	// a locate to bar 2 (quarter 8)
	const double q2at = q1 + (f2 - f1) / 44100.0 * 140 / 60;
	(void) q2at;
	const int f3 = 256 * 700;	// stop
	const int f4 = 256 * 1000;	// start at bar 1, beat 2
	const int f5 = 256 * 900;	// a MIDI note (the snare) while stopped
	const int f6 = 256 * 150;	// a MIDI note (tom 1) while playing
	const HostEv evs[] = {
		{ 0, 1, 0, 0, 120, 0 },
		{ f6, 0, 0, 0, 0, 45 },
		{ f1, 1, (int) (q1 / 4), (float) fmod (q1, 4), 140, 0 },
		{ f2, 1, 2, 0, 140, 0 },
		{ f3, 0, 2, 3, 140, 0 },
		{ f5, 0, -1, 0, 0, 38 },
		{ f4, 1, 1, 2, 140, 0 },
	};
	const HostScen hs = { evs, NELEM (evs), block, NULL, 0, 0, { NULL }, { 0 } };
	const Action acts[] = {
		{ 0, A_START, 0, 120, 0 },
		{ f6 / BLOCK, A_NOTE, 0, 0, 1 },
		{ f1 / BLOCK, A_TEMPO_POS, ((int) (q1 / 4) * 4 + (float) fmod (q1, 4)) * 96.0, 140, 0 },
		{ f2 / BLOCK, A_LOCATE, 8 * 96.0, 140, 0 },
		{ f3 / BLOCK, A_STOP, 0, 0, 0 },
		{ f5 / BLOCK, A_NOTE, 0, 0, 3 },
		{ f4 / BLOCK, A_START, 6 * 96.0, 140, 0 },
	};
	const Script ref = { acts, NELEM (acts), NULL, 0 };
	th_output *a = th_output_new (frames), *b = th_output_new (frames);
	th_fork (hostRender, (void *) &hs, a, "host transport");
	th_fork (scriptRef, (void *) &ref, b, "host transport reference");
	CHECK (th_peak (a) > 0.05f, "host transport: silent");
	CHECK (th_identical (a, b), "host transport: not the engine's sequencer driven the same way (first difference at %d)",
	       th_first_difference (a, b));
	if (th_identical (a, b) && th_peak (a) > 0.05f)
	{
		printf ("host transport (start, a MIDI note, tempo 120 -> 140, locate to bar 2, stop, a MIDI note, start at bar 1 beat 2): "
			"bit exact against the engine's sequencer\n");
	}
	// silence after the stop (the last notes decay: after a second, quiet)
	CHECK (th_rms (a, f3 + 44100, f5) < 1e-4, "host transport: sound after the stop (%g)", th_rms (a, f3 + 44100, f5));
	th_output_free (a);
	th_output_free (b);

	// Internal mode: Play from the start at 100 bpm, the host's transport
	// ignored
	const HostEv evs2[] = { { 0, 1, 3, 0, 150, 0 } };
	const HostScen hs2 = { evs2, 1, block, NULL, 0, 0, { "seq_mode", "seq_play", "seq_tempo", NULL }, { 2, 1, 100, 0 } };
	const Action acts2[] = { { 0, A_START, 0, 100, 0 } };
	const Script ref2 = { acts2, 1, NULL, 0 };
	a = th_output_new (44100 * 3);
	b = th_output_new (44100 * 3);
	th_fork (hostRender, (void *) &hs2, a, "internal");
	th_fork (scriptRef, (void *) &ref2, b, "internal reference");
	CHECK (th_peak (a) > 0.05f && th_identical (a, b), "internal mode: not the engine's sequencer at 100 bpm (first difference %d)",
	       th_first_difference (a, b));
	if (th_peak (a) > 0.05f && th_identical (a, b))
	{
		printf ("internal mode (100 bpm, the host's transport ignored): bit exact\n");
	}
	th_output_free (a);
	th_output_free (b);

	// Off: the sequencer doesn't play (silence)
	const HostScen hs3 = { evs, 1, block, NULL, 0, 0, { "seq_mode", NULL }, { 0 } };
	a = th_output_new (44100);
	th_fork (hostRender, (void *) &hs3, a, "off");
	CHECK (th_peak (a) == 0, "mode off: the sequencer played");
	th_output_free (a);

	// restore while running: at 1.5 s another project (pattern 1 of the
	// project as its pattern 0's), swapped at the run's block boundary, the
	// position kept
	static uint8_t chunk2[300000];
	lxr_engine_init ();
	static const StepDef other[] = { { 0, 4, 0, 127, 0, 0, 0, 0, 0, 0 }, { 0, 4, 6, 90, 0, 0, 0, 0, 0, 0 },
					 { 0, 1, 3, 100, 0, 0, 0, 0, 0, 0 } };
	putSteps (other, NELEM (other));
	const size_t size2 = lxr_seq_chunk_write (lxr_seq_project_current (), chunk2, sizeof chunk2);
	const int fr = 256 * 260;
	const HostScen hs4 = { evs, 1, block, chunk2, size2, fr, { NULL }, { 0 } };
	double q = 0;
	for (int pos = 0; pos < fr; pos += block)
	{
		q += block * (1.0 * 120 / 60.0 / 44100);	// the plugin's host position, run by run
	}
	const Action acts4[] = { { 0, A_START, 0, 120, 0 }, { fr / BLOCK, A_INSTALL, q * LXR_SEQ_TICKS_PER_QUARTER, 120, 0 } };
	const Script ref4 = { acts4, 2, chunk2, size2 };
	a = th_output_new (44100 * 3);
	b = th_output_new (44100 * 3);
	th_fork (hostRender, (void *) &hs4, a, "restore while running");
	th_fork (scriptRef, (void *) &ref4, b, "restore while running reference");
	CHECK (th_peak (a) > 0.05f && th_identical (a, b), "restore while running: not the engine with the project swapped at block %d "
	       "(first difference %d)", fr / BLOCK, th_first_difference (a, b));
	if (th_peak (a) > 0.05f && th_identical (a, b))
	{
		printf ("restore while running: swapped at block %d, the position kept (bit exact)\n", fr / BLOCK);
	}
	th_output_free (a);
	th_output_free (b);
}

// 48 kHz: the quarters heard on time (onsets in the output)
static void onsets48 (void *arg, th_output *out)
{
	(void) arg;
	Plugin *p = pluginNew (48000);
	lxr_engine_init ();
	// the closed hi-hat (short) on the quarters
	static const StepDef q[] = { { 0, 5, 0, 127, 0, 0, 0, 0, 0, 0 }, { 0, 5, 4, 127, 0, 0, 0, 0, 0, 0 },
				     { 0, 5, 8, 127, 0, 0, 0, 0, 0, 0 }, { 0, 5, 12, 127, 0, 0, 0, 0, 0, 0 } };
	putSteps (q, NELEM (q));
	static uint8_t chunk[300000];
	const size_t size = lxr_seq_chunk_write (lxr_seq_project_current (), chunk, sizeof chunk);
	pluginRestore (p, chunk, size);
	// the transport starts at frame 333 of the first run, at bar 0
	for (int pos = 0; pos < out->frames; pos += 960)
	{
		if (pos == 0)
		{
			th_seq_position (events (p), sizeof p->seq, 333, 0, 1, 0, 0, 120);
		}
		pluginRun (p, 960, out, pos);
	}
	pluginFree (p);
}

static void checkTiming48 (void)
{
	th_output *o = th_output_new (48000 * 2);
	th_fork (onsets48, NULL, o, "48 kHz");
	// onsets: the first sample above -60 dBFS after 10 ms below
	double worst = 0, sum = 0, first = 0;
	int n = 0;
	for (int i = 0, last = -100000; i < o->frames; i++)
	{
		if (fabsf (o->data[i]) > 0.001f)
		{
			if (i - last > 480 && n < 8)
			{
				const double expected = 333 + n * 24000.0;	// a quarter at 120 bpm
				const double err = (i - expected) / 48.0;	// ms
				if (n == 0)
				{
					first = err;	// after the start: the next block
				}
				else
				{
					worst = fabs (err) > fabs (worst) ? err : worst;
					sum += err;
				}
				n++;
			}
			last = i;
		}
	}
	printf ("48 kHz: %d onsets, %.2f ms late on average, worst %+.2f ms (an engine block is 1.45 ms; the first after "
		"the start %.2f ms)\n", n, n > 1 ? sum / (n - 1) : 0, worst, first);
	CHECK (n == 4, "48 kHz: %d onsets of 4", n);
	CHECK (worst > -0.3 && worst < 1.6, "48 kHz: an onset %.2f ms off", worst);
	CHECK (first > -0.3 && first < 2.3, "48 kHz: the first onset %.2f ms off", first);
	th_output_free (o);
}

// ---- the panel ----

typedef struct
{
	int ok;
	float out[EDS_NUM_PANEL];
	size_t size;
	uint8_t chunk[300000];
} PanelResult;

static void panelJob (void *arg, th_output *out)
{
	(void) arg;
	PanelResult *res = (PanelResult *) out->data;
	Plugin *p = pluginNew (44100);
	pluginRun (p, 64, NULL, 0);
	int ok = 1;
#define EXPECT(cond, ...) do { if (!(cond)) { printf ("FAIL: panel: " __VA_ARGS__); printf ("\n"); ok = 0; } } while (0)
	// the outputs at the start
	EXPECT (panelOut (p, "seq_out_running") == 0 && panelOut (p, "seq_out_playing") == 0 && panelOut (p, "seq_out_edit_pattern") == 0,
		"start outputs %g %g %g", panelOut (p, "seq_out_running"), panelOut (p, "seq_out_playing"),
		panelOut (p, "seq_out_edit_pattern"));
	EXPECT (panelOut (p, "seq_out_mask") == 0 && panelOut (p, "seq_out_step_velocity") == 127 && panelOut (p, "seq_out_step_note") == 60
		&& panelOut (p, "seq_out_step_prob") == 100 && panelOut (p, "seq_out_track_length") == 16,
		"start step outputs");
	// step buttons 1, 5, 9 on the bass drum: the mask; button 5 selects step 4
	press (p, "seq_button1");
	press (p, "seq_button5");
	press (p, "seq_button9");
	EXPECT (panelOut (p, "seq_out_mask") == (1 | 16 | 256), "mask %g", panelOut (p, "seq_out_mask"));
	EXPECT (panelOut (p, "seq_out_grid_bd") == (1 | 16 | 256) && panelOut (p, "seq_out_grid_sd") == 0, "grid");
	EXPECT (panelOut (p, "seq_out_selected") == 8 && panelOut (p, "seq_out_step_active") == 1, "selected %g",
		panelOut (p, "seq_out_selected"));
	press (p, "seq_button9");	// off again
	EXPECT (panelOut (p, "seq_out_mask") == (1 | 16), "mask after toggling off %g", panelOut (p, "seq_out_mask"));
	// the selected step (step 9, off): its fields
	setPanel (p, "seq_step_velocity", 77);
	setPanel (p, "seq_step_note", 64);
	setPanel (p, "seq_step_prob", 40);
	setPanel (p, "seq_step_flam", 3);
	setPanel (p, "seq_step_shift", -2);
	setPanel (p, "seq_lock1_target", 5);
	setPanel (p, "seq_lock1_value", 33);
	pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_step_velocity") == 77 && panelOut (p, "seq_out_step_note") == 64
		&& panelOut (p, "seq_out_step_prob") == 40 && panelOut (p, "seq_out_step_flam") == 3
		&& panelOut (p, "seq_out_step_shift") == -2 && panelOut (p, "seq_out_lock1_target") == 5
		&& panelOut (p, "seq_out_lock1_value") == 33 && panelOut (p, "seq_out_vel9") == 77,
		"step fields read back %g %g %g %g %g %g %g", panelOut (p, "seq_out_step_velocity"), panelOut (p, "seq_out_step_note"),
		panelOut (p, "seq_out_step_prob"), panelOut (p, "seq_out_step_flam"), panelOut (p, "seq_out_step_shift"),
		panelOut (p, "seq_out_lock1_target"), panelOut (p, "seq_out_lock1_value"));
	// select by page and step: page 2 (17-32), step 3 -> step 19
	setPanel (p, "seq_page", 1);
	setPanel (p, "seq_step", 3);
	pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_selected") == 19 && panelOut (p, "seq_out_step_velocity") == 127 && panelOut (p, "seq_out_mask") == 0,
		"page 2 step 4: selected %g", panelOut (p, "seq_out_selected"));
	press (p, "seq_button4");	// step 20 (page 2, button 4)
	EXPECT (panelOut (p, "seq_out_mask") == 8 && panelOut (p, "seq_out_selected") == 19, "page 2 button 4: mask %g selected %g",
		panelOut (p, "seq_out_mask"), panelOut (p, "seq_out_selected"));
	setPanel (p, "seq_page", 0);
	// the snare: track 4; length 12; mute
	setPanel (p, "seq_track", 3);
	setPanel (p, "seq_track_length", 12);
	setPanel (p, "seq_mute_sd", 1);
	pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_track_length") == 12 && panelOut (p, "seq_out_pattern_length") == 16 && panelOut (p, "seq_out_mutes") == 8,
		"length / mutes %g %g %g", panelOut (p, "seq_out_track_length"), panelOut (p, "seq_out_pattern_length"),
		panelOut (p, "seq_out_mutes"));
	// Euclid on the snare: 5 over 12, rotated 0: E(5, 12)
	setPanel (p, "seq_euclid_fill", 5);
	pluginRun (p, 64, NULL, 0);
	press (p, "seq_euclid");
	const unsigned e = (unsigned) panelOut (p, "seq_out_mask");
	int bits = 0;
	for (unsigned x = e; x; x &= x - 1)
		bits++;
	EXPECT (bits == 5 && !(e >> 12), "euclid 5 / 12: mask 0x%x", e);
	press (p, "seq_clear_track");
	EXPECT (panelOut (p, "seq_out_mask") == 0 && panelOut (p, "seq_out_track_length") == 16, "clear track");
	// shuffle, kit of pattern 0; store kit (none: slot 0); copy to pattern 5
	setPanel (p, "seq_shuffle", 50);
	setPanel (p, "seq_kit", 64);
	pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_shuffle") == 50 && panelOut (p, "seq_out_kit") == 64 && panelOut (p, "seq_out_kit_present") == 0,
		"shuffle / kit %g %g", panelOut (p, "seq_out_shuffle"), panelOut (p, "seq_out_kit"));
	press (p, "seq_store_kit");
	EXPECT (panelOut (p, "seq_out_kit") == 0 && panelOut (p, "seq_out_kit_present") == 1, "store kit: kit %g present %g",
		panelOut (p, "seq_out_kit"), panelOut (p, "seq_out_kit_present"));
	press (p, "seq_copy_pattern");
	setPanel (p, "seq_edit_pattern", 5);
	setPanel (p, "seq_track", 0);
	pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_edit_pattern") == 5 && panelOut (p, "seq_out_mask") == 0, "pattern 5 empty");
	press (p, "seq_paste_pattern");
	EXPECT (panelOut (p, "seq_out_mask") == (1 | 16) && panelOut (p, "seq_out_shuffle") == 50 && panelOut (p, "seq_out_kit") == 0,
		"paste: mask %g", panelOut (p, "seq_out_mask"));
	press (p, "seq_clear_pattern");
	EXPECT (panelOut (p, "seq_out_mask") == 0, "clear pattern");
	setPanel (p, "seq_edit_pattern", 64);
	pluginRun (p, 64, NULL, 0);
	// play (Internal): running, positions, flashes; pattern 1 asked for:
	// next 1, playing 1 after the bar
	setPanel (p, "seq_mute_sd", 0);
	setPanel (p, "seq_mode", 2);
	setPanel (p, "seq_play", 1);
	for (int i = 0; i < 10; i++)
		pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_running") == 1 && panelOut (p, "seq_out_pos_bd") == 0 && ((int) panelOut (p, "seq_out_flash") & 1)
		&& panelOut (p, "seq_out_bpm") == 120, "playing: running %g pos %g flash %g", panelOut (p, "seq_out_running"),
		panelOut (p, "seq_out_pos_bd"), panelOut (p, "seq_out_flash"));
	setPanel (p, "seq_pattern", 1);
	pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_playing") == 0 && panelOut (p, "seq_out_next") == 1, "next pattern %g %g",
		panelOut (p, "seq_out_playing"), panelOut (p, "seq_out_next"));
	for (int i = 0; i < 1380 / 1 && panelOut (p, "seq_out_playing") == 0; i++)
		pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_playing") == 1 && panelOut (p, "seq_out_pos_bd") == 0, "pattern 1 playing %g",
		panelOut (p, "seq_out_playing"));
	setPanel (p, "seq_play", 0);
	pluginRun (p, 64, NULL, 0);
	EXPECT (panelOut (p, "seq_out_running") == 0 && panelOut (p, "seq_out_pos_bd") == -1, "stopped");
	// song mode outputs
	setPanel (p, "seq_song_mode", 1);
	pluginRun (p, 64, NULL, 0);
	// the state with the edits
	Stored st;
	EXPECT (pluginSave (p, &st), "save");
	res->size = st.size < sizeof res->chunk ? st.size : 0;
	memcpy (res->chunk, st.data, res->size);
	free (st.data);
	for (int i = 0; i < EDS_NUM_PANEL; i++)
		res->out[i] = p->panel[i];
	pluginFree (p);
	res->ok = ok;
#undef EXPECT
}

static void checkPanel (void)
{
	th_output *o = th_output_new ((int) (sizeof (PanelResult) / sizeof (float) / 2 + 2));
	th_fork (panelJob, NULL, o, "panel");
	PanelResult *res = (PanelResult *) o->data;
	CHECK (res->ok, "panel ports");
	// the saved project has the edits: pattern 0 bass drum steps 0, 4 (and
	// 19 off, 20 on... on page 2: step 20 of the bass drum), shuffle 50,
	// kit 0; pattern 1 empty (switched to, not edited); song mode on
	lxr_engine_init ();
	lxr_project *p = lxr_seq_project_new ();
	const char *err = res->size ? lxr_seq_chunk_read (p, res->chunk, res->size) : "no chunk";
	CHECK (!err, "panel: the saved chunk: %s", err ? err : "");
	if (!err)
	{
		lxr_seq_project_free (lxr_seq_project_install (p));
		const uint16_t m0 = lxr_seq_step_mask (0, 0, 0), m1 = lxr_seq_step_mask (0, 0, 16);
		CHECK (m0 == (1 | 16) && m1 == 8 && lxr_seq_shuffle (0) == 50 && lxr_seq_kit (0) == 0 && lxr_seq_has_kit (0)
		       && lxr_seq_get_step_field (0, 0, 8, LXR_STEP_VELOCITY) == 77 && lxr_seq_get_step_field (0, 0, 8, LXR_STEP_FLAM) == 3,
		       "panel: the saved project: masks %x %x shuffle %d kit %d", m0, m1, lxr_seq_shuffle (0), lxr_seq_kit (0));
		lxr_seq_status s;
		lxr_seq_get_status (&s);
		CHECK (s.songMode == 1 && s.playing == 1, "panel: the saved project's song mode %d, pattern %d", s.songMode, s.playing);
	}
	if (res->ok && !err)
	{
		printf ("panel: step buttons, step fields, selection, length, mutes, Euclid, clear, shuffle, kit, store kit, copy / paste, "
			"play, pattern switch, song mode, the outputs; the saved project has the edits\n");
	}
	th_output_free (o);
}

// ---- the state ----

typedef struct
{
	int ok;
	size_t size[3];
	uint8_t chunk[3][300000];
} StateResult;

static void stateJob (void *arg, th_output *out)
{
	(void) arg;
	StateResult *res = (StateResult *) out->data;
	Plugin *p = pluginNew (44100);
	int ok = pluginRestore (p, projectChunk, projectSize);
	pluginRun (p, 256, NULL, 0);
	Stored st;
	ok = ok && pluginSave (p, &st);
	res->size[0] = st.size;
	memcpy (res->chunk[0], st.data, st.size);
	pluginFree (p);
	// into a new instance, saved again
	p = pluginNew (44100);
	ok = ok && pluginRestore (p, st.data, st.size);
	free (st.data);
	pluginRun (p, 256, NULL, 0);
	ok = ok && pluginSave (p, &st);
	res->size[1] = st.size;
	memcpy (res->chunk[1], st.data, st.size);
	free (st.data);
	// a state without a project (0.2.0): nothing restored, the project stays
	Stored none = { 0 };
	ok = ok && stateOf (p)->restore (p->h, retrieveFn, &none, 0, th_features ()) == LV2_STATE_SUCCESS;
	pluginRun (p, 256, NULL, 0);
	ok = ok && pluginSave (p, &st);
	res->size[2] = st.size;
	memcpy (res->chunk[2], st.data, st.size);
	free (st.data);
	// garbage: refused
	const uint8_t junk[64] = "EDSQ";
	ok = ok && !pluginRestore (p, junk, sizeof junk);
	pluginFree (p);
	res->ok = ok;
}

static void checkState (void)
{
	th_output *o = th_output_new ((int) (sizeof (StateResult) / sizeof (float) / 2 + 2));
	th_fork (stateJob, NULL, o, "state");
	const StateResult *res = (const StateResult *) o->data;
	CHECK (res->ok, "state: save / restore failed");
	CHECK (res->size[0] == projectSize && !memcmp (res->chunk[0], projectChunk, projectSize),
	       "state: restored and saved: not the project's chunk (%zu bytes, %zu)", res->size[0], projectSize);
	CHECK (res->size[1] == res->size[0] && !memcmp (res->chunk[1], res->chunk[0], res->size[0]),
	       "state: a second round trip differs");
	CHECK (res->size[2] == res->size[0] && !memcmp (res->chunk[2], res->chunk[0], res->size[0]),
	       "state: a state without a project changed the project");
	if (res->ok && res->size[0] == projectSize && !memcmp (res->chunk[0], projectChunk, projectSize)
	    && !memcmp (res->chunk[1], res->chunk[0], res->size[0]))
	{
		printf ("state: save -> restore -> save, %zu bytes, bit exact; a state without a project keeps it; garbage refused\n",
			res->size[0]);
	}
	th_output_free (o);
}

// the kit ports and the kits the sequencer loads: a restored project's kit
// (pattern 0's, kit autoload) is the engine's, the ports as they are don't
// change it; a port changed: its parameter (the others stay the kit's); an
// engine selector changed: the slot switched, its ports sent again; a
// pattern's kit loaded later: the engine's again
typedef struct
{
	int ok;
	uint8_t kit[4][256], engines[4][6];
} KitPortResult;

static void currentKit (const Stored *st, uint8_t *params, uint8_t *engines)
{
	const uint8_t *k = st->data + st->size - LXR_SEQ_KIT_FILE;
	memcpy (params, k + 8, lxr_engine_kit_size ());
	memcpy (engines, k + LXR_SEQ_KIT_FILE - 6, 6);
}

static void kitPortJob (void *arg, th_output *out)
{
	(void) arg;
	KitPortResult *res = (KitPortResult *) out->data;
	makeKits ();
	lxr_engine_init ();
	lxr_project *pr = lxr_seq_project_new ();
	lxr_seq_project_set_kit (pr, 1, kitFile[1], LXR_SEQ_KIT_FILE);
	lxr_seq_project_set_kit (pr, 2, kitFile[2], LXR_SEQ_KIT_FILE);
	uint8_t file[LXR_SEQ_PATTERN_FILE];
	lxr_seq_project_get_pattern (pr, 0, file);
	file[14 + 7 * 512] = 1;		// the bass drum on step 0
	file[14 + 7 * 512 + 56 + 1 + 7] = 1;	// pattern 0: kit 1
	lxr_seq_project_set_pattern (pr, 0, file, sizeof file);
	file[14 + 7 * 512 + 56 + 1 + 7] = 2;	// pattern 1: kit 2
	lxr_seq_project_set_pattern (pr, 1, file, sizeof file);
	static uint8_t chunk[300000];
	const size_t size = lxr_seq_chunk_write (pr, chunk, sizeof chunk);
	lxr_seq_project_free (pr);

	Plugin *p = pluginNew (44100);
	int ok = pluginRestore (p, chunk, size);
	Stored st;
	pluginRun (p, 256, NULL, 0);
	ok = ok && pluginSave (p, &st);
	currentKit (&st, res->kit[0], res->engines[0]);
	free (st.data);
	p->controls[th_control_index ("bd_osc_coarse")] = 0;	// engine value 60
	pluginRun (p, 256, NULL, 0);
	ok = ok && pluginSave (p, &st);
	currentKit (&st, res->kit[1], res->engines[1]);
	free (st.data);
	p->controls[th_control_index ("bd_mix_engine")] = 1;	// the snare engine
	pluginRun (p, 256, NULL, 0);
	ok = ok && pluginSave (p, &st);
	currentKit (&st, res->kit[2], res->engines[2]);
	free (st.data);
	// play: pattern 1 (kit 2) at the end of pattern 0
	setPanel (p, "seq_mode", 2);
	setPanel (p, "seq_play", 1);
	setPanel (p, "seq_pattern", 1);
	for (int i = 0; i < 6; i++)
		pluginRun (p, 256, NULL, 0);
	for (int i = 0; i < 400 && panelOut (p, "seq_out_playing") != 1; i++)
		pluginRun (p, 256, NULL, 0);
	ok = ok && panelOut (p, "seq_out_playing") == 1 && pluginSave (p, &st);
	currentKit (&st, res->kit[3], res->engines[3]);
	free (st.data);
	pluginFree (p);
	res->ok = ok;
}

static void checkKitPorts (void)
{
	th_output *o = th_output_new ((int) (sizeof (KitPortResult) / sizeof (float) / 2 + 2));
	th_fork (kitPortJob, NULL, o, "kit ports");
	const KitPortResult *r = (const KitPortResult *) o->data;
	CHECK (r->ok, "kit ports: the plugin job failed");
	CHECK (r->kit[0][8] == 40 && r->kit[0][50] == 50, "kit ports: the restored project's kit 1 not the engine's (coarse %d decay %d)",
	       r->kit[0][8], r->kit[0][50]);
	CHECK (r->kit[1][8] == 60 && r->kit[1][50] == 50, "kit ports: a port changed: coarse %d decay %d (60, the kit's 50)",
	       r->kit[1][8], r->kit[1][50]);
	CHECK (r->engines[2][0] == 1, "kit ports: the engine selector: the bass drum's engine %d", r->engines[2][0]);
	CHECK (r->kit[3][8] == 70 && r->kit[3][50] == 80 && r->engines[3][0] == 0 && r->engines[3][2] == 1,
	       "kit ports: pattern 1's kit 2 not loaded (coarse %d decay %d, engines %d %d)", r->kit[3][8], r->kit[3][50],
	       r->engines[3][0], r->engines[3][2]);
	if (!th_failures)
	{
		printf ("kit ports: a loaded kit is the engine's until a port changes; a changed port wins for its parameter, an "
			"engine selector switches the slot; the next pattern's kit is the engine's again\n");
	}
	th_output_free (o);
}

// CPU: the plugin with the sequencer playing the project (48 kHz, 960)
static void cpuJob (void *arg, th_output *out)
{
	const int playing = *(const int *) arg;
	Plugin *p = pluginNew (48000);
	pluginRestore (p, projectChunk, projectSize);
	if (!playing)
	{
		setPanel (p, "seq_mode", 0);
	}
	double cpu = 0;
	const int frames = 48000 * 10;
	for (int pos = 0; pos < frames; pos += 960)
	{
		if (pos == 0 && playing)
		{
			th_seq_position (events (p), sizeof p->seq, 0, 0, 1, 0, 0, 120);
		}
		if (!playing && (pos / 960) % 6 == 0)
		{
			// the same density of hits from MIDI (eighths)
			const uint8_t msg[3] = { 0x99, (uint8_t) (pos % 24000 ? 42 : 36), 100 };
			th_seq_midi (events (p), sizeof p->seq, 0, msg, 3);
		}
		const double t0 = th_now ();
		p->d->run (p->h, 960);
		cpu += th_now () - t0;
		th_seq_clear (events (p));
	}
	out->cpuSeconds = cpu / frames;
	pluginFree (p);
}

static void checkCpu (void)
{
	double ns[2] = { 0, 0 };
	for (int k = 0; k < 2; k++)
	{
		for (int rep = 0; rep < 3; rep++)
		{
			th_output *o = th_output_new (1);
			th_fork (cpuJob, &k, o, "cpu");
			ns[k] += 1e9 * o->cpuSeconds / 3;
			th_output_free (o);
		}
	}
	printf ("CPU (48 kHz, block 960, this machine and build): %.1f ns per sample with MIDI hits, %.1f with the sequencer "
		"playing (%+.1f %%)\n", ns[0], ns[1], 100 * (ns[1] / ns[0] - 1));
	CHECK (ns[1] < 1.5 * ns[0], "the sequencer costs more than half the plugin");
}

// ---- the LXR project converter (tools/lxr2preset.py) ----

static void checkConverted (const char *tool, const char *work);

int main (int argc, char **argv)
{
	int args = argc;
	th_verbose = argc > 2 && !strcmp (argv[argc - 1], "-v");
	args -= th_verbose;
	const char *tool = NULL, *work = NULL;
	if (args == 5 && !strcmp (argv[2], "--project"))
	{
		tool = argv[3];
		work = argv[4];
	}
	else if (args != 2)
	{
		fprintf (stderr, "usage: seq_test PATH/edrumsynth.so [--project LXR2PRESET WORKDIR] [-v]\n");
		return 2;
	}
	pluginPath = argv[1];
	char ttl[4096];
	snprintf (ttl, sizeof ttl, "%s", pluginPath);
	char *slash = strrchr (ttl, '/');
	snprintf (slash ? slash + 1 : ttl, sizeof ttl - (size_t) (slash ? slash + 1 - ttl : 0), "edrumsynth.ttl");
	th_load_symbols (ttl, EDS_PORT_CONTROL0, EDS_NUM_CONTROLS + EDS_NUM_PANEL);

	if (tool)
	{
		checkConverted (tool, work);
	}
	else
	{
		checkSteps ();
		checkFlamShift ();
		checkProbability ();
		checkPolymeter ();
		checkShuffle ();
		checkMute ();
		checkPatternSwitch ();
		checkSongs ();
		checkLocks ();
		checkKits ();
		checkHostTransport ();
		checkTiming48 ();
		checkPanel ();
		checkState ();
		checkKitPorts ();
		checkCpu ();
	}
	printf ("%s\n", th_failures ? "FAILED" : "all sequencer checks passed");
	return th_failures ? 1 : 0;
}

// ---- converted LXR projects ----

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

static uint8_t *readFile (const char *path, size_t *size)
{
	FILE *f = fopen (path, "rb");
	if (!f)
	{
		return NULL;
	}
	fseek (f, 0, SEEK_END);
	const long n = ftell (f);
	fseek (f, 0, SEEK_SET);
	uint8_t *data = malloc ((size_t) n + 1);
	*size = fread (data, 1, (size_t) n, f);
	data[*size] = 0;
	fclose (f);
	return data;
}

// a pattern file of the given version from a V3 one
static size_t patternVersion (const uint8_t *v3, int version, const char *name, uint8_t *out)
{
	size_t n = 0;
	memset (out, 0, 8);
	memcpy (out, name, strlen (name) < 8 ? strlen (name) : 8);
	n = 8;
	if (version > 1)
	{
		memcpy (out + n, version == 3 ? "PatV03" : "PatV02", 6);
		n += 6;
	}
	const uint8_t *o = v3 + 14;
	for (int t = 0; t < 7; t++)
	{
		memcpy (out + n, o, 64 * 7);
		n += 64 * 7;
		o += 64 * 7;
		if (version > 2)
		{
			memcpy (out + n, o, 64);
			n += 64;
		}
		o += 64;
	}
	memcpy (out + n, o, 56 + 1 + 7);
	n += 64;
	o += 64;
	if (version > 1)
	{
		out[n++] = *o;
	}
	return n;
}

static int b64 (int c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
	       : c == '+' ? 62 : c == '/' ? 63 : -1;
}

// a preset file: the port values and the project chunk (base64)
typedef struct
{
	char symbol[512][48];
	float value[512];
	int n;
	uint8_t *chunk;
	size_t size;
} PresetFile;

static int parsePreset (const char *path, PresetFile *pf)
{
	size_t size;
	char *text = (char *) readFile (path, &size);
	if (!text)
	{
		return 0;
	}
	pf->n = 0;
	for (char *p = text; (p = strstr (p, "lv2:symbol \"")) && pf->n < 512; p++)
	{
		float v;
		char sym[48];
		if (sscanf (p, "lv2:symbol \"%47[a-z0-9_]\" ; pset:value %f", sym, &v) == 2
		    || sscanf (p, "lv2:symbol \"%47[a-z0-9_]\" ;\n\t\tpset:value %f", sym, &v) == 2)
		{
			snprintf (pf->symbol[pf->n], sizeof pf->symbol[0], "%s", sym);
			pf->value[pf->n++] = v;
		}
	}
	char *end = strstr (text, "\"^^xsd:base64Binary");
	char *start = end;
	while (start && start > text && start[-1] != '"')
		start--;
	if (!end || start == text)
	{
		free (text);
		return 0;
	}
	pf->chunk = malloc ((size_t) (end - start));
	pf->size = 0;
	unsigned acc = 0;
	int bits = 0;
	for (char *c = start; c < end; c++)
	{
		const int x = b64 (*c);
		if (x < 0)
			continue;
		acc = acc << 6 | (unsigned) x;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			pf->chunk[pf->size++] = (uint8_t) (acc >> bits);
		}
	}
	free (text);
	return 1;
}

static char projectDir[2048];

// the synthetic project: pattern 0 (V3: steps on five tracks, a flam, a
// shift, locks, a short track, shuffle; kit 1), 1 (V2, kit 2), 2 (V1, its
// kit 0: no such file), song 0: 0 1 2 1 loop, kits 1 (engine block), 2
// (engines changed), 3 (an older 255 byte kit); GLO.CFG 110 bpm; PRJ.NFO
static void writeProject (const char *dir)
{
	makeKits ();
	lxr_engine_init ();
	const int coarse = targetOf (8);
	static const StepDef steps[] = {
		{ 0, 0, 0, 127, 0, 0, 0, 0, 0, 0 }, { 0, 0, 6, 110, 0, 0, 0, 0, 0, 0 }, { 0, 0, 8, 120, 0, 0, 0, 0, 0, 0 },
		{ 0, 3, 4, 100, 0, 0, 0, 0, 0, 0 }, { 0, 3, 12, 100, 63, 0, 2, 0, 0, 0 }, { 0, 5, 0, 70, 0, 0, 0, 0, 0, 0 },
		{ 0, 5, 2, 50, 0, 0, 0, 0, 0, 0 }, { 0, 5, 3, 50, 0, 0, 0, -2, 0, 0 }, { 0, 1, 10, 90, 0, 0, 0, 0, 0, 0 },
		{ 0, 2, 0, 90, 0, 0, 0, 0, 0, 0 }, { 0, 2, 3, 90, 0, 0, 0, 0, 0, 0 },
		{ 1, 0, 0, 127, 0, 0, 0, 0, 0, 0 }, { 1, 4, 4, 100, 0, 0, 0, 0, 0, 0 }, { 1, 6, 14, 100, 0, 0, 0, 0, 0, 0 },
		{ 2, 3, 0, 100, 0, 0, 0, 0, 0, 0 }, { 2, 3, 8, 100, 0, 0, 0, 0, 0, 0 },
	};
	putSteps (steps, NELEM (steps));
	lxr_seq_select (0, 0, 6);
	lxr_seq_set_step_field (LXR_STEP_LOCK1_TARGET, (uint8_t) coarse);
	lxr_seq_set_step_field (LXR_STEP_LOCK1_VALUE, 80);
	setLength (0, 2, 5);
	lxr_seq_select (0, 0, 0);
	lxr_seq_set_shuffle (40);
	lxr_seq_set_kit (1);
	lxr_seq_select (1, 0, 0);
	lxr_seq_set_kit (2);
	mkdir (dir, 0755);
	char path[4096];
	uint8_t v3[LXR_SEQ_PATTERN_FILE], file[LXR_SEQ_PATTERN_FILE];
	static const char *const names[3] = { "00-GROOVE.PAT", "01-FILL.PAT", "02-OLD.PAT" };
	for (int pat = 0; pat < 3; pat++)
	{
		lxr_seq_project_get_pattern (lxr_seq_project_current (), (uint8_t) pat, v3);
		const size_t n = patternVersion (v3, 3 - pat, pat == 0 ? "GROOVE" : pat == 1 ? "FILL" : "OLD", file);
		snprintf (path, sizeof path, "%s/%s", dir, names[pat]);
		writeFile (path, file, n);
	}
	snprintf (path, sizeof path, "%s/01-KITA.SND", dir);
	writeFile (path, kitFile[1], LXR_SEQ_KIT_FILE);
	snprintf (path, sizeof path, "%s/02-KITB.SND", dir);
	writeFile (path, kitFile[2], LXR_SEQ_KIT_FILE);
	snprintf (path, sizeof path, "%s/03-OLDKIT.SND", dir);
	writeFile (path, kitFile[0], 255);
	uint8_t song[LXR_SEQ_SONG_FILE] = "SONG";
	memset (song + 8, LXR_SEQ_SONG_END, 64);
	const uint8_t entries[] = { 0, 1, 2, 1, LXR_SEQ_SONG_LOOP };
	memcpy (song + 8, entries, sizeof entries);
	snprintf (path, sizeof path, "%s/00-SONG.SNG", dir);
	writeFile (path, song, sizeof song);
	uint8_t glo[31] = { 110 };
	glo[29] = 1;	// PAR_PATTERN_MODE: auto (kits per pattern)
	glo[30] = 1;
	snprintf (path, sizeof path, "%s/GLO.CFG", dir);
	writeFile (path, glo, sizeof glo);
	snprintf (path, sizeof path, "%s/PRJ.NFO", dir);
	writeFile (path, "TESTPRJ\0", 8);
}

// the engine with the project built from the files through the engine's
// API (its own parser), started at 110 bpm
typedef struct
{
	int songMode;
	int pattern;
} ConvRef;

static void convRef (void *arg, th_output *out)
{
	const ConvRef *cr = arg;
	lxr_engine_init ();
	lxr_engine_set_split (LXR_SPLIT_KIT, 0);
	lxr_project *p = lxr_seq_project_new ();
	static const char *const pats[3] = { "00-GROOVE.PAT", "01-FILL.PAT", "02-OLD.PAT" };
	static const char *const kits[3] = { "01-KITA.SND", "02-KITB.SND", "03-OLDKIT.SND" };
	char path[4096];
	size_t size = 0;
	int ok = 1;
	for (int i = 0; i < 3; i++)
	{
		snprintf (path, sizeof path, "%s/%s", projectDir, pats[i]);
		uint8_t *d = readFile (path, &size);
		ok = ok && d && lxr_seq_project_set_pattern (p, (uint8_t) i, d, size);
		free (d);
		snprintf (path, sizeof path, "%s/%s", projectDir, kits[i]);
		d = readFile (path, &size);
		ok = ok && d && lxr_seq_project_set_kit (p, (uint8_t) (i + 1), d, size);
		free (d);
	}
	snprintf (path, sizeof path, "%s/00-SONG.SNG", projectDir);
	uint8_t *d = readFile (path, &size);
	lxr_seq_project_set_song (p, 0, d, size);
	free (d);
	lxr_seq_project_set_start (p, (uint8_t) cr->pattern, 0, cr->songMode);
	lxr_seq_project_free (lxr_seq_project_install (p));	// kit autoload: the start pattern's kit
	lxr_seq_set_tempo (110);
	lxr_seq_start (0);
	renderBlocks (out, 0, out->frames / BLOCK);
	out->ok = ok;
}

typedef struct
{
	const PresetFile *pf;
} ConvPlug;

static void convPlugin (void *arg, th_output *out)
{
	const ConvPlug *cp = arg;
	Plugin *p = pluginNew (44100);
	for (int i = 0; i < cp->pf->n; i++)
	{
		const int c = th_control_index (cp->pf->symbol[i]);
		if (c < EDS_NUM_CONTROLS)
			p->controls[c] = cp->pf->value[i];
		else
			p->panel[c - EDS_NUM_CONTROLS] = cp->pf->value[i];
	}
	const int restored = pluginRestore (p, cp->pf->chunk, cp->pf->size);
	for (int pos = 0; pos < out->frames; pos += 512)
	{
		if (pos == 0)
		{
			th_seq_position (events (p), sizeof p->seq, 0, 0, 1, 0, 0, 110);
		}
		pluginRun (p, 512, out, pos);
	}
	out->ok = restored;
	pluginFree (p);
}

static void checkConvertedOne (const char *tool, const char *work, const char *bundleName, const char *args, int songMode,
			       int pattern)
{
	char cmd[16384], bundle[4096];
	snprintf (bundle, sizeof bundle, "%s/%s", work, bundleName);
	snprintf (cmd, sizeof cmd, "%s '%s' -o '%s' %s > /dev/null", tool, projectDir, bundle, args);
	CHECK (system (cmd) == 0, "%s failed", cmd);
	char path[8192];
	snprintf (path, sizeof path, "%s/testprj.ttl", bundle);
	static PresetFile pf;
	if (!parsePreset (path, &pf))
	{
		CHECK (0, "%s: no preset with a project", path);
		return;
	}
	// the chunk as the engine reads and writes it again: the same bytes
	lxr_engine_init ();
	lxr_project *p = lxr_seq_project_new ();
	const char *err = lxr_seq_chunk_read (p, pf.chunk, pf.size);
	static uint8_t again[300000];
	const size_t n = err ? 0 : lxr_seq_chunk_write (p, again, sizeof again);
	lxr_seq_project_free (p);
	CHECK (!err && n == pf.size && !memcmp (again, pf.chunk, n), "%s: the chunk (%zu bytes) read and written again: %s %zu bytes",
	       bundleName, pf.size, err ? err : "", n);
	snprintf (path, sizeof path, "%s/%s.chunk", work, bundleName);
	writeFile (path, pf.chunk, pf.size);	// for the lilv check
	// the preset's values: Host mode, the tempo, the start
	int mode = -1, tempo = -1, start = -1;
	for (int i = 0; i < pf.n; i++)
	{
		mode = !strcmp (pf.symbol[i], "seq_mode") ? (int) pf.value[i] : mode;
		tempo = !strcmp (pf.symbol[i], "seq_tempo") ? (int) pf.value[i] : tempo;
		start = !strcmp (pf.symbol[i], "seq_pattern") ? (int) pf.value[i] : start;
	}
	CHECK (mode == 1 && tempo == 110 && start == pattern, "%s: seq_mode %d, seq_tempo %d, seq_pattern %d", bundleName, mode, tempo,
	       start);
	// rendered: the plugin with the preset as the engine with the project
	const int frames = 512 * 775;	// 9 s: patterns 0, 1, 2 at 110 bpm and back to 1
	th_output *a = th_output_new (frames), *b = th_output_new (frames);
	const ConvPlug cp = { &pf };
	const ConvRef cr = { songMode, pattern };
	th_fork (convPlugin, (void *) &cp, a, "converted preset");
	th_fork (convRef, (void *) &cr, b, "converted reference");
	CHECK (a->ok && b->ok && th_peak (a) > 0.05f, "%s: failed or silent", bundleName);
	CHECK (th_identical (a, b), "%s: the preset doesn't play as the engine with the project (first difference at %d)", bundleName,
	       th_first_difference (a, b));
	if (a->ok && th_identical (a, b) && th_peak (a) > 0.05f && !err)
	{
		printf ("%s (%s): %zu bytes of state, read and written again the same; plays as the engine with the project "
			"(%d pf values, bit exact)\n", bundleName, args[0] ? args : "the first pattern", pf.size, pf.n);
	}
	th_output_free (a);
	th_output_free (b);
	free (pf.chunk);
}

static void checkConverted (const char *tool, const char *work)
{
	mkdir (work, 0755);
	snprintf (projectDir, sizeof projectDir, "%s/PROJ01", work);
	writeProject (projectDir);
	checkConvertedOne (tool, work, "song.lv2", "--song 0", 1, 0);
	checkConvertedOne (tool, work, "pattern.lv2", "", 0, 0);
	checkConvertedOne (tool, work, "pattern1.lv2", "--pattern 1", 0, 1);
}
