/*
 * lxr_sequencer.c - the LXR's step sequencer (OpenElixir-02's sequencer.c,
 * clockSync.c, EuklidGenerator.c and automationNode.c, ../lxr, compiled
 * here as one unit) on the engine, and what it uses from the rest of the
 * LXR firmware (lxr_glue.h: front panel, menu, LEDs, MIDI out, presets).
 * See lxr_sequencer.h.
 *
 * - The clock: the LXR runs seq_tick() once per engine block, which
 *   processes one tick of its 96 PPQ clock (24 per step) when due:
 *   seq_nextStep() on a step's first tick, seq_processFlam() on each
 *   (flams, shift, the triggers); an odd step's first tick is late by the
 *   shuffle (16 ticks x the pattern's shuffle / 127), the ticks after it
 *   follow one per block. lxr_seq_block() does the same with the time in
 *   ticks counted per block (the host's or an internal tempo) instead of
 *   the 4 kHz system tick: a tick plays at the first block that starts at
 *   or after its time.
 * - Locating (a host transport jump, a start inside the song): the
 *   sequencer's position computed as if the steps before had played
 *   (pattern, song position, each track's step); not chased.
 * - The project: the pattern set and songs the sequencer plays
 *   (seq_patternSet, seq_songData: pointers here, sequencer.h), the
 *   project's 64 kits (the LXR's kits in RAM, presetManager.c), globals and
 *   names; swapped as a whole (lxr_seq_project_install()).
 * - Probabilities: the LXR draws them from the hardware random number
 *   generator (GetRngValue()); here from their own seeded xorshift32 (the
 *   noise oscillators keep theirs), so a render repeats.
 *
 * Copyright (C) 2012-2026 Erica Synths, 2026 tunefish.at (the sequencer),
 * 2026 Hannes Janetzek (this glue)
 * SPDX-License-Identifier: GPL-3.0-only (see ../LICENSE and ../NOTICE.md)
 */

// needs -I ../lxr -I ../lxr/shim, as lxr_engine.c
#define GetRngValue lxr_seq_random	// the probabilities' own random numbers
#include "Soundengine/Sequencer/sequencer.c"
#undef GetRngValue
#include "Soundengine/Sequencer/clockSync.c"
#include "Soundengine/Sequencer/EuklidGenerator.c"
#include "Soundengine/DSPAudio/automationNode.c"

#include "Soundengine/DSPAudio/VoiceSlot.h"
#include "menu/CcNr2Text.h"
#include "lxr_engine.h"
#include "lxr_sequencer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert (LXR_SEQ_TRACKS == NUM_TRACKS && LXR_SEQ_STEPS == NUM_STEPS && LXR_SEQ_PATTERNS == NUM_PATTERN
		&& LXR_SEQ_SONGS == NUM_KIT_SLOTS && LXR_SEQ_SONG_LENGTH == MAX_SONG_LENGTH && LXR_SEQ_SONG_END == SONG_END
		&& LXR_SEQ_SONG_LOOP == SONG_LOOP, "sizes");

// ---- what the sequencer uses from the rest of the firmware (lxr_glue.h) ----

INDTCMZ volatile uint32_t systick_ticks;	// seq_tick(), clockSync.c: not used here
uint8_t midi_process_block;
uint8_t midiParser_txRxFilter;			// MIDI out filters: no MIDI out
uint8_t midi_MidiChannels[8];
uint8_t frontParser_shownPattern;
uint8_t frontParser_activeTrack;
uint8_t frontParser_activeStep;
uint8_t trigger_clockReceived;

uint8_t menu_playedPattern;
uint8_t menu_shownPattern;
uint8_t menu_activeBar;
uint8_t menu_songMenuPosition;
uint8_t menu_selectedPresetNr[NUM_PRESET_LOCATIONS];
uint8_t menu_loadedPresetNr[NUM_CURRENT];

void menu_setShownPattern (uint8_t patternNr)
{
	menu_shownPattern = patternNr;
}

uint8_t menu_getViewedPattern (void)
{
	return frontParser_shownPattern;
}

uint8_t menu_getActiveVoice (void)
{
	return frontParser_activeTrack;
}

void menu_repaintAll (void)
{
}

void midiParser_checkMtc (void)
{
}

void uart_sendMidi (MidiMsg msg)
{
	(void) msg;
}

void usb_sendMidi (MidiMsg msg)
{
	(void) msg;
}

void uart_sendFrontpanelByte (uint8_t data)
{
	(void) data;
}

void uart_sendFrontpanelSysExByte (uint8_t data)
{
	(void) data;
}

void trigger_reset (uint8_t value)
{
	(void) value;
}

void trigger_clockTick (uint8_t tickType, uint8_t prescaleCounter)
{
	(void) tickType;
	(void) prescaleCounter;
}

// MidiVoiceControl.c: a trigger plays the voice (slot_trigger(), through the
// engine's trigger: its mutes and counters); MIDI out and the front panel's
// LED pulse are not here, note offs only went to MIDI out
static lxr_seq_trigger_fn triggerFn;
static uint64_t blocks;

void voiceControl_noteOn (uint8_t voice, uint8_t note, uint8_t vel)
{
	if (triggerFn)
	{
		triggerFn (blocks, voice, vel, note);
	}
	lxr_engine_trigger (voice, vel, note);
}

void voiceControl_noteOff (uint8_t voice)
{
	(void) voice;
}

// the probabilities' random numbers
static uint32_t rngState = 2463534242u;

uint32_t lxr_seq_random (void)
{
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return rngState;
}

void lxr_seq_seed (uint32_t seed)
{
	rngState = seed ? seed : 2463534242u;
}

// ---- the project ----

#define NAME_SIZE	8
#define KIT_SIZE	END_OF_SOUND_PARAMETERS
#define NUM_GLOBALS	(NUM_PARAMS - PAR_BEGINNING_OF_GLOBALS)
#define ENGINE_BLOCK	11	// "OE02", 6, the engines (KitEngines.c)

struct lxr_project
{
	PatternSet patterns;
	uint8_t songs[NUM_KIT_SLOTS][MAX_SONG_LENGTH];
	uint8_t kits[NUM_KIT_SLOTS][KIT_SIZE];
	uint8_t kitEngines[NUM_KIT_SLOTS][NUM_VOICE_SLOTS];
	uint64_t kitMask;		// the kit slots that hold a kit
	char name[NAME_SIZE];
	char patternNames[NUM_PATTERN][NAME_SIZE];
	char kitNames[NUM_KIT_SLOTS][NAME_SIZE];
	char songNames[NUM_KIT_SLOTS][NAME_SIZE];
	uint8_t globals[NUM_GLOBALS];	// GLO.CFG (kept, not used: the plugin's ports set tempo and kit loading)
	// the start: the pattern and song, and the kit in the engine (a
	// project read from a chunk; the current project's are the sequencer's)
	uint8_t pattern, song, songMode;
	uint8_t hasKit;
	uint8_t kit[KIT_SIZE], engines[NUM_VOICE_SLOTS];
	char kitName[NAME_SIZE];
};

static lxr_project engineProject;	// the project after lxr_seq_init()
static lxr_project *project = &engineProject;
static uint8_t clearPattern[LXR_SEQ_PATTERN_FILE];	// a cleared pattern (seq_clearPattern())
static uint8_t defaultSongs[NUM_KIT_SLOTS][MAX_SONG_LENGTH];	// seq_init()'s songs
static uint8_t activeSongNr;
static uint8_t kitAutoload = 1;
static uint32_t kitLoads;

// PresetManager.c: the project's kits in RAM; a slot without a kit loads
// nothing (the plugin's project has no kits until one is stored)
void preset_loadDrumsetRam (uint8_t presetNr, uint8_t isMorph, uint8_t sendImmediate)
{
	(void) sendImmediate;
	if (isMorph || presetNr >= NUM_KIT_SLOTS || !(project->kitMask >> presetNr & 1))
	{
		return;
	}
	lxr_engine_load_kit_engines (project->kits[presetNr], KIT_SIZE, project->kitEngines[presetNr]);
	kitLoads++;
}

static void putName (uint8_t *out, const char *name)
{
	memcpy (out, name, NAME_SIZE);
}

static void getName (char *name, const uint8_t *in)
{
	memcpy (name, in, NAME_SIZE);
}

// a pattern of the project as an LXR V3 pattern file (preset_writePatternData())
static void writePattern (const lxr_project *p, uint8_t pat, uint8_t *o)
{
	const PatternSet *ps = &p->patterns;
	putName (o, p->patternNames[pat]);
	memcpy (o + 8, "PatV03", 6);
	o += 14;
	for (int t = 0; t < NUM_TRACKS; t++)
	{
		for (int i = 0; i < NUM_STEPS; i++)
		{
			const Step *s = &ps->seq_subStepPattern[pat][t][i];
			*o++ = s->volume;
			*o++ = s->prob;
			*o++ = s->note;
			*o++ = s->param1Nr;
			*o++ = s->param1Val;
			*o++ = s->param2Nr;
			*o++ = s->param2Val;
		}
		for (int i = 0; i < NUM_STEPS; i++)
		{
			const StepTiming *st = &ps->seq_stepTiming[pat][t][i];
			*o++ = (uint8_t) (st->flam | st->shift << 4);
		}
	}
	for (int t = 0; t < NUM_TRACKS; t++)
	{
		for (int b = 0; b < NUM_BARS; b++)
		{
			const uint16_t w = ps->seq_mainSteps[b][pat][t];
			*o++ = (uint8_t) w;
			*o++ = (uint8_t) (w >> 8);
		}
	}
	*o++ = ps->shuffle[pat];
	for (int t = 0; t < NUM_TRACKS; t++)
	{
		*o++ = ps->length[pat][t];
	}
	*o++ = ps->kitNr[pat];
}

// an LXR pattern file (V1, V2, V3; preset_readPatternData()): 0 if not one
static int readPattern (lxr_project *p, uint8_t pat, const uint8_t *in, size_t size)
{
	int version = 1;
	size_t o = 8;
	if (size >= 14 && !memcmp (in + 8, "PatV", 4) && in[12] == '0' && (in[13] == '2' || in[13] == '3'))
	{
		version = in[13] - '0';
		o = 14;
	}
	const size_t need = o + NUM_TRACKS * NUM_STEPS * (version > 2 ? 8 : 7) + NUM_TRACKS * NUM_BARS * 2 + 1 + NUM_TRACKS
			    + (version > 1);
	if (size < need)
	{
		return 0;
	}
	PatternSet *ps = &p->patterns;
	getName (p->patternNames[pat], in);
	for (int t = 0; t < NUM_TRACKS; t++)
	{
		for (int i = 0; i < NUM_STEPS; i++, o += 7)
		{
			Step *s = &ps->seq_subStepPattern[pat][t][i];
			s->volume = in[o];
			s->prob = in[o + 1];
			s->note = in[o + 2];
			s->param1Nr = in[o + 3];
			s->param1Val = in[o + 4];
			s->param2Nr = in[o + 5];
			s->param2Val = in[o + 6];
		}
		for (int i = 0; i < NUM_STEPS; i++)
		{
			StepTiming *st = &ps->seq_stepTiming[pat][t][i];
			const uint8_t x = version > 2 ? in[o++] : 7 << 4;
			st->flam = x & 0x0F;
			st->shift = x >> 4;
		}
	}
	for (int t = 0; t < NUM_TRACKS; t++)
	{
		for (int b = 0; b < NUM_BARS; b++, o += 2)
		{
			ps->seq_mainSteps[b][pat][t] = (uint16_t) (in[o] | in[o + 1] << 8);
		}
	}
	ps->shuffle[pat] = in[o++];
	for (int t = 0; t < NUM_TRACKS; t++)
	{
		ps->length[pat][t] = in[o++];
	}
	ps->kitNr[pat] = version > 1 ? in[o] : 0;
	return 1;
}

static void writeKit (uint8_t *o, const char *name, const uint8_t *params, const uint8_t *engines)
{
	putName (o, name);
	memcpy (o + NAME_SIZE, params, KIT_SIZE);
	lxr_engine_encode_engines (engines, o + NAME_SIZE + KIT_SIZE);
}

// an LXR kit file: name, up to 247 parameters (missing ones 0), the engine
// block (stock engines without one), as lxr_engine_load_kit_engines() reads it
static int readKit (const uint8_t *in, size_t size, char *name, uint8_t *params, uint8_t *engines)
{
	if (size < NAME_SIZE + 128)
	{
		return 0;
	}
	getName (name, in);
	const size_t n = size - NAME_SIZE < KIT_SIZE ? size - NAME_SIZE : KIT_SIZE;
	memset (params, 0, KIT_SIZE);
	memcpy (params, in + NAME_SIZE, n);
	const size_t rest = size - NAME_SIZE - n;
	lxr_engine_decode_engines (rest ? in + NAME_SIZE + n : NULL, (uint16_t) rest, engines);
	return 1;
}

static void initProject (lxr_project *p)
{
	memset (p, 0, sizeof *p);
	for (int pat = 0; pat < NUM_PATTERN; pat++)
	{
		readPattern (p, (uint8_t) pat, clearPattern, sizeof clearPattern);
	}
	memcpy (p->songs, defaultSongs, sizeof p->songs);
	p->globals[PAR_BPM - PAR_BEGINNING_OF_GLOBALS] = 120;
	p->globals[PAR_PATTERN_MODE - PAR_BEGINNING_OF_GLOBALS] = 1;
	p->globals[PAR_MIXER_LIMITER - PAR_BEGINNING_OF_GLOBALS] = 1;
}

lxr_project *lxr_seq_project_new (void)
{
	lxr_project *p = malloc (sizeof *p);
	if (p)
	{
		initProject (p);
	}
	return p;
}

void lxr_seq_project_free (lxr_project *p)
{
	if (p != &engineProject)
	{
		free (p);
	}
}

lxr_project *lxr_seq_project_current (void)
{
	return project;
}

// ---- the chunk (the LV2 state's project) ----
//
// Little endian, version 1:
//   0  "EDSQ"
//   4  u16 version (1), u16 header size (48)
//   8  u64 pattern mask: the patterns that follow (the others are cleared,
//      seq_clearPattern(), without a name)
//  16  u64 kit mask: the kit slots that follow (the others hold no kit)
//  24  u64 song mask: the songs that follow (the others as seq_init():
//      song 0 plays pattern 0, the others are empty)
//  32  u8 pattern playing, u8 song, u8 flags (1: song mode, 2: a current
//      kit follows), u8 globals (their count)
//  36  8 bytes the project's name, u32 0
//  48  the patterns (LXR V3 pattern files, 3663 bytes), the kits (LXR kit
//      files: name, 247 parameters, OpenElixir-02's engine block, 266
//      bytes), the songs (LXR song files: name, 64 entries, 72 bytes), the
//      globals (GLO.CFG: the LXR's globals from PAR_BPM on), the current
//      kit (a kit file: the engine's kit)
// Readers take later versions with a larger header (they skip the rest of
// the header) and stop on a smaller one.

#define CHUNK_MAGIC	"EDSQ"
#define CHUNK_VERSION	1
#define CHUNK_HEADER	48

static void put16 (uint8_t *o, uint16_t v)
{
	o[0] = (uint8_t) v;
	o[1] = (uint8_t) (v >> 8);
}

static void put64 (uint8_t *o, uint64_t v)
{
	for (int i = 0; i < 8; i++)
	{
		o[i] = (uint8_t) (v >> 8 * i);
	}
}

static uint16_t get16 (const uint8_t *in)
{
	return (uint16_t) (in[0] | in[1] << 8);
}

static uint64_t get64 (const uint8_t *in)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
	{
		v |= (uint64_t) in[i] << 8 * i;
	}
	return v;
}

static int popcount64 (uint64_t x)
{
	int n = 0;
	for (; x; x &= x - 1)
	{
		n++;
	}
	return n;
}

static uint64_t patternMask (const lxr_project *p)
{
	uint64_t mask = 0;
	uint8_t file[LXR_SEQ_PATTERN_FILE];
	for (int pat = 0; pat < NUM_PATTERN; pat++)
	{
		writePattern (p, (uint8_t) pat, file);
		if (memcmp (file, clearPattern, sizeof file))
		{
			mask |= (uint64_t) 1 << pat;
		}
	}
	return mask;
}

static uint64_t songMask (const lxr_project *p)
{
	uint64_t mask = 0;
	for (int s = 0; s < NUM_KIT_SLOTS; s++)
	{
		static const char noName[NAME_SIZE];
		if (memcmp (p->songs[s], defaultSongs[s], MAX_SONG_LENGTH) || memcmp (p->songNames[s], noName, NAME_SIZE))
		{
			mask |= (uint64_t) 1 << s;
		}
	}
	return mask;
}

size_t lxr_seq_chunk_size (const lxr_project *p)
{
	return CHUNK_HEADER + (size_t) popcount64 (patternMask (p)) * LXR_SEQ_PATTERN_FILE
	       + (size_t) popcount64 (p->kitMask) * LXR_SEQ_KIT_FILE + (size_t) popcount64 (songMask (p)) * LXR_SEQ_SONG_FILE
	       + NUM_GLOBALS + LXR_SEQ_KIT_FILE;
}

size_t lxr_seq_chunk_write (const lxr_project *p, uint8_t *buf, size_t size)
{
	const size_t need = lxr_seq_chunk_size (p);
	if (size < need)
	{
		return 0;
	}
	// the current project: the sequencer's pattern, song and the engine's kit
	const int current = p == project;
	const uint64_t pm = patternMask (p), sm = songMask (p);
	memset (buf, 0, CHUNK_HEADER);
	memcpy (buf, CHUNK_MAGIC, 4);
	put16 (buf + 4, CHUNK_VERSION);
	put16 (buf + 6, CHUNK_HEADER);
	put64 (buf + 8, pm);
	put64 (buf + 16, p->kitMask);
	put64 (buf + 24, sm);
	buf[32] = current ? seq_activePattern : p->pattern;
	buf[33] = current ? activeSongNr : p->song;
	buf[34] = (uint8_t) (((current ? seq_songModeEnabled : p->songMode) ? 1 : 0) | 2);
	buf[35] = NUM_GLOBALS;
	putName (buf + 36, p->name);
	uint8_t *o = buf + CHUNK_HEADER;
	for (int pat = 0; pat < NUM_PATTERN; pat++)
	{
		if (pm >> pat & 1)
		{
			writePattern (p, (uint8_t) pat, o);
			o += LXR_SEQ_PATTERN_FILE;
		}
	}
	for (int k = 0; k < NUM_KIT_SLOTS; k++)
	{
		if (p->kitMask >> k & 1)
		{
			writeKit (o, p->kitNames[k], p->kits[k], p->kitEngines[k]);
			o += LXR_SEQ_KIT_FILE;
		}
	}
	for (int s = 0; s < NUM_KIT_SLOTS; s++)
	{
		if (sm >> s & 1)
		{
			putName (o, p->songNames[s]);
			memcpy (o + NAME_SIZE, p->songs[s], MAX_SONG_LENGTH);
			o += LXR_SEQ_SONG_FILE;
		}
	}
	memcpy (o, p->globals, NUM_GLOBALS);
	o += NUM_GLOBALS;
	if (current)
	{
		uint8_t kit[KIT_SIZE], engines[NUM_VOICE_SLOTS];
		lxr_engine_get_kit (kit, KIT_SIZE);
		lxr_engine_get_engines (engines);
		writeKit (o, p->hasKit ? p->kitName : "", kit, engines);
	}
	else
	{
		writeKit (o, p->kitName, p->kit, p->engines);
	}
	o += LXR_SEQ_KIT_FILE;
	return (size_t) (o - buf);
}

const char *lxr_seq_chunk_read (lxr_project *p, const uint8_t *in, size_t size)
{
	if (size < 36 || memcmp (in, CHUNK_MAGIC, 4))
	{
		return "not an eDrumSynth project";
	}
	const uint16_t version = get16 (in + 4), header = get16 (in + 6);
	if (version < 1 || header < CHUNK_HEADER || header > size)
	{
		return "unknown project version";
	}
	const uint64_t pm = get64 (in + 8), km = get64 (in + 16), sm = get64 (in + 24);
	const uint8_t flags = in[34], numGlobals = in[35];
	const size_t need = header + (size_t) popcount64 (pm) * LXR_SEQ_PATTERN_FILE + (size_t) popcount64 (km) * LXR_SEQ_KIT_FILE
			    + (size_t) popcount64 (sm) * LXR_SEQ_SONG_FILE + numGlobals + (flags & 2 ? LXR_SEQ_KIT_FILE : 0);
	if (size < need)
	{
		return "the project is cut short";
	}
	initProject (p);
	p->pattern = in[32] < NUM_PATTERN ? in[32] : 0;
	p->song = in[33] < NUM_KIT_SLOTS ? in[33] : 0;
	p->songMode = flags & 1;
	getName (p->name, in + 36);
	const uint8_t *o = in + header;
	for (int pat = 0; pat < NUM_PATTERN; pat++)
	{
		if (pm >> pat & 1)
		{
			readPattern (p, (uint8_t) pat, o, LXR_SEQ_PATTERN_FILE);
			o += LXR_SEQ_PATTERN_FILE;
		}
	}
	p->kitMask = km;
	for (int k = 0; k < NUM_KIT_SLOTS; k++)
	{
		if (km >> k & 1)
		{
			readKit (o, LXR_SEQ_KIT_FILE, p->kitNames[k], p->kits[k], p->kitEngines[k]);
			o += LXR_SEQ_KIT_FILE;
		}
	}
	for (int s = 0; s < NUM_KIT_SLOTS; s++)
	{
		if (sm >> s & 1)
		{
			getName (p->songNames[s], o);
			memcpy (p->songs[s], o + NAME_SIZE, MAX_SONG_LENGTH);
			o += LXR_SEQ_SONG_FILE;
		}
	}
	memcpy (p->globals, o, numGlobals < NUM_GLOBALS ? numGlobals : NUM_GLOBALS);
	o += numGlobals;
	if (flags & 2)
	{
		p->hasKit = 1;
		readKit (o, LXR_SEQ_KIT_FILE, p->kitName, p->kit, p->engines);
	}
	return NULL;
}

void lxr_seq_project_get_pattern (const lxr_project *p, uint8_t pattern, uint8_t *file)
{
	writePattern (p, pattern < NUM_PATTERN ? pattern : 0, file);
}

int lxr_seq_project_set_pattern (lxr_project *p, uint8_t pattern, const uint8_t *file, size_t size)
{
	return pattern < NUM_PATTERN && readPattern (p, pattern, file, size);
}

int lxr_seq_project_set_kit (lxr_project *p, uint8_t kit, const uint8_t *file, size_t size)
{
	if (kit >= NUM_KIT_SLOTS || !readKit (file, size, p->kitNames[kit], p->kits[kit], p->kitEngines[kit]))
	{
		return 0;
	}
	p->kitMask |= (uint64_t) 1 << kit;
	return 1;
}

void lxr_seq_project_set_song (lxr_project *p, uint8_t song, const uint8_t *file, size_t size)
{
	if (song < NUM_KIT_SLOTS && size >= NAME_SIZE)
	{
		getName (p->songNames[song], file);
		memset (p->songs[song], SONG_END, MAX_SONG_LENGTH);
		memcpy (p->songs[song], file + NAME_SIZE, size - NAME_SIZE < MAX_SONG_LENGTH ? size - NAME_SIZE : MAX_SONG_LENGTH);
	}
}

void lxr_seq_project_set_start (lxr_project *p, uint8_t pattern, uint8_t song, int songMode)
{
	p->pattern = pattern < NUM_PATTERN ? pattern : 0;
	p->song = song < NUM_KIT_SLOTS ? song : 0;
	p->songMode = songMode != 0;
}

// ---- the clock ----

#define TICKS_PER_STEP	LXR_SEQ_TICKS_PER_STEP
#define EPSILON		1e-6	// ticks: a tick exactly at a block start plays in that block

static struct
{
	double bpm;
	double ticksPerBlock;
	double base;		// the position at block n = 0
	uint64_t n;		// blocks since base
	int64_t next;		// the next tick
	double due;		// its time
} clk;

static double clockPos (void)
{
	return clk.base + (double) clk.n * clk.ticksPerBlock;
}

static void rebase (double pos)
{
	clk.base = pos;
	clk.n = 0;
}

// the time of the next tick: an odd step's first tick late by the shuffle
// (seq_tick(): seq_calcDeltaT() with isStepShuffled from seq_masterStepCnt,
// the steps played)
static void computeDue (void)
{
	clk.due = (double) clk.next;
	if (clk.next % TICKS_PER_STEP == 0 && seq_masterStepCnt % 2 == 1 && seq_shuffle != 0)
	{
		clk.due += 16.0 * seq_shuffle;
	}
}

void lxr_seq_set_tempo (double bpm)
{
	if (!(bpm >= 0) || bpm > 1000)
	{
		bpm = 0;
	}
	if (bpm == clk.bpm)
	{
		return;
	}
	rebase (clockPos ());
	clk.bpm = bpm;
	clk.ticksPerBlock = bpm / 60.0 * LXR_SEQ_TICKS_PER_QUARTER * LXR_ENGINE_BLOCK / 44100.0;
	if (bpm >= 1)
	{
		lxr_engine_set_tempo ((uint16_t) (bpm + 0.5));
	}
}

double lxr_seq_get_tempo (void)
{
	return clk.bpm;
}

// the master track's length of a pattern (seq_nextStep(): the longest track
// by its length byte, 0 = 16)
static uint8_t masterLength (uint8_t pat)
{
	uint8_t longest = 0;
	for (uint8_t i = 1; i < NUM_TRACKS; i++)
	{
		if (seq_patternSet.length[pat][i] > seq_patternSet.length[pat][longest])
		{
			longest = i;
		}
	}
	const uint8_t len = seq_patternSet.length[pat][longest];
	return len ? len : 16;
}

static uint8_t trackLength (uint8_t pat, uint8_t track)
{
	const uint8_t len = seq_patternSet.length[pat][track];
	return len ? len : 16;
}

// where the song is after the steps 0..s-1 (seq_nextStep(),
// seq_determineNextPattern(), seq_setPendingPatternActive()): the pattern,
// the song position and the steps since the pattern's tracks started; 0
// when the song ended before
static int songWalk (int64_t s, uint8_t *pattern, uint8_t *pos, int64_t *offset)
{
	for (int pass = 0; pass < 2; pass++)
	{
		uint8_t ptr = 0xFF, cur = seq_activePattern;
		int64_t c = 0, runStart = 0;
		for (int entries = 0;; entries++)
		{
			ptr++;
			if (ptr == MAX_SONG_LENGTH)
			{
				ptr = 0;
			}
			uint8_t next = seq_activeSong[ptr];
			const int wrapped = next == SONG_LOOP || (ptr == 0 && entries > 0);
			if (next == SONG_LOOP)
			{
				next = seq_activeSong[0];
				ptr = 0;
			}
			if (next >= MAX_PATTERN)
			{
				return 0;
			}
			if (wrapped && pass == 0 && c > 0 && s - 1 >= c)
			{
				// once through: the same again (a track running on over the
				// loop's seam is restarted here)
				s = 1 + (s - 1) % c;
				break;
			}
			if (next != cur)
			{
				runStart = c;
				cur = next;
			}
			const uint8_t m = masterLength (cur);
			if (s - 1 < c + m)
			{
				*pattern = cur;
				*pos = ptr;
				*offset = s - 1 - runStart;
				return 1;
			}
			c += m;
		}
	}
	return 0;
}

static void setTrackSteps (int64_t offset)
{
	for (uint8_t t = 0; t < NUM_TRACKS; t++)
	{
		// seq_stepIndex: static in sequencer.c, this unit
		seq_stepIndex[t] = offset < 0 ? -1 : (int8_t) (offset % trackLength (seq_activePattern, t));
		seq_lastMasterStep[t] = 0;
	}
}

void lxr_seq_locate (double tick)
{
	if (!seq_running)
	{
		return;
	}
	int64_t s = (int64_t) ceil (tick / TICKS_PER_STEP - 1e-9);
	if (s < 0)
	{
		s = 0;
	}
	uint8_t pattern = seq_activePattern, pos = 0xFF;
	int64_t offset = s - 1;
	if (s > 0 && seq_songModeEnabled && !songWalk (s, &pattern, &pos, &offset))
	{
		// the song ended before: stopped, as at its end
		seq_setRunning (0);
		seq_resetPosition ();
		return;
	}
	if (s == 0)
	{
		pos = 0xFF;
	}
	if (pattern != seq_activePattern)
	{
		seq_pendingPattern = pattern;
		seq_setPendingPatternActive ();
	}
	seq_pendingPattern = seq_activePattern;
	seq_songPositionPointer = pos;
	setTrackSteps (offset);
	seq_masterStepCnt = (uint8_t) s;
	seq_lastShuffle = 0;
	seq_prescaleCounter = 0;
	clk.next = s * TICKS_PER_STEP;
	computeDue ();
	rebase (tick);
}

void lxr_seq_start (double tick)
{
	seq_resetPosition ();
	seq_setRunning (1);
	clk.next = 0;
	seq_prescaleCounter = 0;
	computeDue ();
	rebase (0);
	if (tick > EPSILON)
	{
		lxr_seq_locate (tick);
	}
}

void lxr_seq_stop (void)
{
	if (seq_running)
	{
		seq_resetPosition ();
		seq_setRunning (0);
	}
}

int lxr_seq_running (void)
{
	return seq_running;
}

double lxr_seq_position (void)
{
	return clockPos ();
}

void lxr_seq_set_position (double tick)
{
	rebase (tick);
}

// one tick of seq_tick()'s internal clock: the step on its first tick, the
// flams / shift / triggers on each (trigger outputs, MIDI clock: none here)
static void tick (void)
{
	if (seq_prescaleCounter == 0)
	{
		seq_nextStep ();
	}
	seq_processFlam (seq_prescaleCounter);
	seq_prescaleCounter++;
	if (seq_prescaleCounter >= TICKS_PER_STEP)
	{
		seq_prescaleCounter = 0;
	}
}

void lxr_seq_block (void)
{
	if (seq_running && clk.bpm > 0)
	{
		const double pos = clockPos ();
		if (clk.due <= pos + EPSILON)
		{
			tick ();
			clk.next++;
			computeDue ();
			// more than a step behind (a tempo above the one tick per block,
			// about 430 bpm): on from where the time is
			if (seq_running && pos - clk.due > TICKS_PER_STEP)
			{
				lxr_seq_locate (pos);
			}
		}
	}
	blocks++;
	clk.n++;
}

uint64_t lxr_seq_blocks (void)
{
	return blocks;
}

void lxr_seq_set_trigger_fn (lxr_seq_trigger_fn fn)
{
	triggerFn = fn;
}

void lxr_seq_get_status (lxr_seq_status *st)
{
	st->running = seq_running;
	st->playing = seq_activePattern;
	st->next = seq_songModeEnabled && seq_activePattern == seq_pendingPattern ? seq_determineNextPattern (0) : seq_pendingPattern;
	if (st->next >= MAX_PATTERN)
	{
		st->next = seq_activeSong[0] < MAX_PATTERN ? seq_activeSong[0] : seq_activePattern;
	}
	st->songMode = seq_songModeEnabled;
	st->song = activeSongNr;
	st->songPos = seq_songPositionPointer < MAX_SONG_LENGTH ? seq_songPositionPointer : 0;
	for (uint8_t t = 0; t < NUM_TRACKS; t++)
	{
		st->step[t] = seq_running ? (int8_t) seq_getStepIndex (t) : -1;
	}
	st->muted = 0;
	for (uint8_t t = 0; t < NUM_TRACKS; t++)
	{
		st->muted |= (uint8_t) (seq_isTrackMuted (t) << t);
	}
	st->kitAutoload = kitAutoload;
	st->kitLoads = kitLoads;
}

// ---- the front panel ----

static uint8_t editPattern = NUM_PATTERN;	// NUM_PATTERN: the playing one
static uint8_t copySource = 0xFF;

static uint8_t shown (void)
{
	return editPattern < NUM_PATTERN ? editPattern : seq_activePattern;
}

// the front panel's selection as CortexParser.c keeps it
static void selectFront (void)
{
	frontParser_shownPattern = shown ();
	seq_selectedStep = frontParser_activeStep;
}

void lxr_seq_set_pattern (uint8_t pattern)
{
	seq_setNextPattern (pattern);
}

void lxr_seq_set_song_mode (int on)
{
	if ((on != 0) != (seq_songModeEnabled != 0))
	{
		seq_setSongModeEnabled (on != 0);
	}
}

void lxr_seq_set_song (uint8_t song)
{
	if (song < NUM_KIT_SLOTS)
	{
		activeSongNr = song;
		seq_activeSong = seq_songData[song];
	}
}

void lxr_seq_set_mute (uint8_t track, int muted)
{
	if (track < NUM_TRACKS)
	{
		seq_setMute (track, muted != 0);
	}
}

void lxr_seq_set_kit_autoload (int on)
{
	kitAutoload = on != 0;
	activeParameters[PAR_PATTERN_MODE] = kitAutoload;
}

void lxr_seq_select (uint8_t pattern, uint8_t track, uint8_t step)
{
	editPattern = pattern < NUM_PATTERN ? pattern : NUM_PATTERN;
	frontParser_activeTrack = track < NUM_TRACKS ? track : 0;
	frontParser_activeStep = step < NUM_STEPS ? step : 0;
	selectFront ();
}

uint8_t lxr_seq_edit_pattern (void)
{
	return shown ();
}

// the step automation's target: stored as the front panel sends it
// (CortexParser.c FRONT_SET_P1_DEST: the parameter, +1 below 128), read
// back as FRONT_SEQ_REQUEST_STEP_PARAMS and frontPanelParser.c (a
// modulation target index)
static uint8_t lockStored (uint8_t target)
{
	if (target >= getNumModTargets ())
	{
		target = 0;
	}
	uint8_t val = (uint8_t) modTargets[target].param;
	if (val && val < 128)
	{
		val++;
	}
	return val;
}

static uint8_t lockTarget (uint8_t stored)
{
	uint8_t dest = stored;
	if (dest < 128 && dest)
	{
		dest--;
	}
	if (dest == 0xFF || dest >= END_OF_SOUND_PARAMETERS)
	{
		dest = 0;
	}
	return paramToModTarget[dest];
}

void lxr_seq_set_step_field (int field, uint8_t value)
{
	selectFront ();
	Step *s = &seq_patternSet.seq_subStepPattern[frontParser_shownPattern][frontParser_activeTrack][frontParser_activeStep];
	StepTiming *t = &seq_patternSet.seq_stepTiming[frontParser_shownPattern][frontParser_activeTrack][frontParser_activeStep];
	switch (field)
	{
	case LXR_STEP_VELOCITY:
		s->volume = (uint8_t) ((s->volume & ~0x7F) | (value & 0x7F));
		break;
	case LXR_STEP_NOTE:
		s->note = value & 0x7F;
		break;
	case LXR_STEP_PROB:
		s->prob = value > 100 ? 100 : value;
		break;
	case LXR_STEP_FLAM:
		t->flam = value > 5 ? 5 : value;
		break;
	case LXR_STEP_SHIFT:
		t->shift = value > 14 ? 14 : value;
		break;
	case LXR_STEP_LOCK1_TARGET:
		s->param1Nr = lockStored (value);
		break;
	case LXR_STEP_LOCK1_VALUE:
		s->param1Val = value & 0x7F;
		break;
	case LXR_STEP_LOCK2_TARGET:
		s->param2Nr = lockStored (value);
		break;
	case LXR_STEP_LOCK2_VALUE:
		s->param2Val = value & 0x7F;
		break;
	default:
		break;
	}
}

uint8_t lxr_seq_get_step_field (uint8_t pattern, uint8_t track, uint8_t step, int field)
{
	if (pattern >= NUM_PATTERN || track >= NUM_TRACKS || step >= NUM_STEPS)
	{
		return 0;
	}
	const Step *s = &seq_patternSet.seq_subStepPattern[pattern][track][step];
	const StepTiming *t = &seq_patternSet.seq_stepTiming[pattern][track][step];
	switch (field)
	{
	case LXR_STEP_VELOCITY:	return s->volume & STEP_VOLUME_MASK;
	case LXR_STEP_NOTE:	return s->note;
	case LXR_STEP_PROB:	return s->prob;
	case LXR_STEP_FLAM:	return t->flam;
	case LXR_STEP_SHIFT:	return t->shift;
	case LXR_STEP_LOCK1_TARGET:	return lockTarget (s->param1Nr);
	case LXR_STEP_LOCK1_VALUE:	return s->param1Val;
	case LXR_STEP_LOCK2_TARGET:	return lockTarget (s->param2Nr);
	case LXR_STEP_LOCK2_VALUE:	return s->param2Val;
	case LXR_STEP_ACTIVE:	return seq_isMainStepActive (track, step, pattern);
	default:		return 0;
	}
}

void lxr_seq_toggle_step (uint8_t step)
{
	selectFront ();
	if (step < NUM_STEPS)
	{
		// buttonHandler_setRemoveStep(): the main step, the step selected
		seq_toggleMainStep (frontParser_activeTrack, step, frontParser_shownPattern);
		frontParser_activeStep = step;
		seq_selectedStep = step;
	}
}

uint16_t lxr_seq_step_mask (uint8_t pattern, uint8_t track, uint8_t first)
{
	if (pattern >= NUM_PATTERN || track >= NUM_TRACKS || first >= NUM_STEPS)
	{
		return 0;
	}
	uint16_t mask = 0;
	for (uint8_t i = 0; i < 16 && first + i < NUM_STEPS; i++)
	{
		mask |= (uint16_t) (seq_isMainStepActive (track, (uint8_t) (first + i), pattern) << i);
	}
	return mask;
}

void lxr_seq_set_track_length (uint8_t length)
{
	selectFront ();
	seq_setTrackLength (frontParser_activeTrack, length < 1 ? 1 : length > NUM_STEPS ? NUM_STEPS : length);
}

uint8_t lxr_seq_track_length (uint8_t pattern, uint8_t track)
{
	return pattern < NUM_PATTERN && track < NUM_TRACKS ? trackLength (pattern, track) : 16;
}

uint8_t lxr_seq_pattern_length (uint8_t pattern)
{
	return pattern < NUM_PATTERN ? masterLength (pattern) : 16;
}

void lxr_seq_set_shuffle (uint8_t shuffle)
{
	// the pattern's shuffle (saved with it on the LXR); the playing one's
	// at once (FRONT_SEQ_SHUFFLE)
	shuffle &= 0x7F;
	seq_patternSet.shuffle[shown ()] = shuffle;
	if (shown () == seq_activePattern)
	{
		activeParameters[PAR_SHUFFLE] = shuffle;
		seq_setShuffle (shuffle / 127.f);
	}
}

uint8_t lxr_seq_shuffle (uint8_t pattern)
{
	return pattern < NUM_PATTERN ? seq_patternSet.shuffle[pattern] : 0;
}

void lxr_seq_set_kit (uint8_t kit)
{
	seq_patternSet.kitNr[shown ()] = kit < NUM_KIT_SLOTS ? kit : LXR_SEQ_NO_KIT;
}

uint8_t lxr_seq_kit (uint8_t pattern)
{
	if (pattern >= NUM_PATTERN)
	{
		return LXR_SEQ_NO_KIT;
	}
	const uint8_t kit = seq_patternSet.kitNr[pattern];
	return kit < NUM_KIT_SLOTS ? kit : LXR_SEQ_NO_KIT;
}

int lxr_seq_has_kit (uint8_t kit)
{
	return kit < NUM_KIT_SLOTS && (project->kitMask >> kit & 1);
}

void lxr_seq_euclid (uint8_t fill, uint8_t rotation)
{
	// the LXR's Euklid page: length (the track's), steps, rotation
	selectFront ();
	const uint8_t track = frontParser_activeTrack;
	euklid_setLength (track, trackLength (frontParser_shownPattern, track));
	euklid_setRotation (track, rotation, frontParser_shownPattern);
	euklid_setSteps (track, fill, frontParser_shownPattern);
}

void lxr_seq_clear_track (void)
{
	selectFront ();
	seq_clearTrack (frontParser_activeTrack, frontParser_shownPattern);
}

void lxr_seq_clear_pattern (void)
{
	selectFront ();
	seq_clearPattern (frontParser_shownPattern);
	memset (project->patternNames[frontParser_shownPattern], 0, NAME_SIZE);
}

void lxr_seq_copy_pattern (void)
{
	copySource = shown ();
}

void lxr_seq_paste_pattern (void)
{
	if (copySource < NUM_PATTERN && copySource != shown ())
	{
		seq_copyPattern (copySource, shown ());
	}
}

void lxr_seq_store_kit (void)
{
	// preset_saveDrumsetRam(): the engine's kit (the edit buffer) into the
	// pattern's kit slot
	const uint8_t pat = shown ();
	uint8_t kit = lxr_seq_kit (pat);
	if (kit == LXR_SEQ_NO_KIT)
	{
		kit = pat;
		for (uint8_t k = 0; k < NUM_KIT_SLOTS; k++)
		{
			if (!(project->kitMask >> k & 1))
			{
				kit = k;
				break;
			}
		}
		seq_patternSet.kitNr[pat] = kit;
	}
	lxr_engine_get_kit (project->kits[kit], KIT_SIZE);
	lxr_engine_get_engines (project->kitEngines[kit]);
	project->kitMask |= (uint64_t) 1 << kit;
}

// ---- installing a project ----

static void usePointers (lxr_project *p)
{
	project = p;
	seq_patternSetPtr = &p->patterns;
	seq_songDataPtr = &p->songs;
	seq_activeSong = seq_songData[activeSongNr];
}

lxr_project *lxr_seq_project_install (lxr_project *p)
{
	lxr_project *old = project;
	const int running = seq_running;
	const double pos = clockPos ();
	usePointers (p);
	activeSongNr = p->song;
	seq_activeSong = seq_songData[activeSongNr];
	seq_songModeEnabled = p->songMode;
	// the pattern without the switch's kit load (the project's own kit next)
	seq_activePattern = seq_pendingPattern = p->pattern;
	seq_copyPatternToBackupBuffer (seq_activePattern);
	activeParameters[PAR_SHUFFLE] = seq_patternSet.shuffle[seq_activePattern];
	seq_setShuffle (activeParameters[PAR_SHUFFLE] / 127.f);
	menu_playedPattern = seq_activePattern;
	if (p->hasKit)
	{
		lxr_engine_load_kit_engines (p->kit, KIT_SIZE, p->engines);
		kitLoads++;
	}
	else if (kitAutoload)
	{
		preset_loadDrumsetRam (seq_patternSet.kitNr[seq_activePattern], 0, 1);
	}
	copySource = 0xFF;
	if (running)
	{
		lxr_seq_locate (pos);
	}
	else
	{
		seq_resetPosition ();
	}
	return old;
}

// ---- init ----

void lxr_seq_init (void)
{
	// as soundengine_init(), main.c: the sequencer, the Euklid generator
	// (its patterns into the engine's project)
	usePointers (&engineProject);
	memset (&engineProject, 0, sizeof engineProject);
	activeSongNr = 0;
	seq_init ();
	euklid_init ();
	seq_setBpm (120);
	memcpy (defaultSongs, seq_songData, sizeof defaultSongs);
	writePattern (&engineProject, 0, clearPattern);
	initProject (&engineProject);
	seq_activePattern = seq_pendingPattern = 0;
	seq_copyPatternToBackupBuffer (0);
	seq_setShuffle (0);
	seq_songModeEnabled = 0;
	seq_mutedTracks = 0;
	kitAutoload = 1;
	activeParameters[PAR_PATTERN_MODE] = 1;
	kitLoads = 0;
	editPattern = NUM_PATTERN;
	copySource = 0xFF;
	frontParser_activeTrack = frontParser_activeStep = 0;
	selectFront ();
	memset (&clk, 0, sizeof clk);
	blocks = 0;
	triggerFn = NULL;
	lxr_seq_seed (0);
	lxr_seq_set_tempo (120);
	seq_resetPosition ();
	seq_running = 0;
}
