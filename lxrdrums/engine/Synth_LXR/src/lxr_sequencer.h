/*
 * lxr_sequencer.h - the LXR's step sequencer (OpenElixir-02's sequencer.c,
 * ../lxr/Soundengine/Sequencer) on the engine (lxr_engine.h): its clock in
 * engine blocks, the project it plays (patterns, kits, songs: an LV2 state
 * chunk), and a front panel modelled on the LXR's (the edited pattern,
 * track and step, the step buttons, the step's fields, the track's
 * settings, the actions).
 *
 * Global state, like the engine: one per process. lxr_engine_init() calls
 * lxr_seq_init(). Not thread safe: everything but the project functions
 * (lxr_seq_project_*) runs in the audio thread, between engine blocks.
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LXR_SEQUENCER_H_
#define LXR_SEQUENCER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXR_SEQ_TRACKS		7
#define LXR_SEQ_STEPS		64	// per track (4 bars of 16)
#define LXR_SEQ_PATTERNS	64
#define LXR_SEQ_KITS		64
#define LXR_SEQ_SONGS		64
#define LXR_SEQ_SONG_LENGTH	64
#define LXR_SEQ_SONG_END	64	// song entries: a pattern, or the end / loop mark
#define LXR_SEQ_SONG_LOOP	65
#define LXR_SEQ_NO_KIT		64	// a pattern's kit number: none
#define LXR_SEQ_TICKS_PER_STEP	24	// the LXR's clock: 96 per quarter note
#define LXR_SEQ_TICKS_PER_QUARTER 96

// the sequencer stopped, an empty project, the tempo 120 (lxr_engine_init()
// calls it)
void lxr_seq_init (void);

// ---- the clock: engine blocks (LXR_ENGINE_BLOCK frames at 44.1 kHz) ----

// the tempo the ticks run at (quarter notes per minute; 0: stand still);
// also the LFOs' sync (rounded, as lxr_engine_set_tempo())
void lxr_seq_set_tempo (double bpm);
double lxr_seq_get_tempo (void);
// start at tick position tick (0: the start, as the LXR's start button; else
// as a locate); stop: as the LXR's stop button (running off, the position
// reset, the song to its start)
void lxr_seq_start (double tick);
void lxr_seq_stop (void);
int lxr_seq_running (void);
// jump to tick (while running): the steps before it are not played, the
// sequencer is where it would be after playing them (pattern, song
// position, each track's step); the next step plays at its start
void lxr_seq_locate (double tick);
// the tick position at the start of the next engine block; a small
// correction (no locate) from the host's position
double lxr_seq_position (void);
void lxr_seq_set_position (double tick);
// before rendering each engine block: the tick due (one per block, as the
// LXR's seq_tick() per block) and the position advanced by one block
void lxr_seq_block (void);
// engine blocks since lxr_seq_init() (the trigger log's time)
uint64_t lxr_seq_blocks (void);
// a function called on each trigger of the sequencer (tests): the block of
// lxr_seq_blocks() it is in, the track, velocity and note
typedef void (*lxr_seq_trigger_fn) (uint64_t block, uint8_t track, uint8_t velocity, uint8_t note);
void lxr_seq_set_trigger_fn (lxr_seq_trigger_fn fn);
// the seed of the probabilities' random numbers (xorshift32, not 0)
void lxr_seq_seed (uint32_t seed);

// ---- state ----

typedef struct
{
	int running;
	uint8_t playing, next;		// the pattern playing, the one after it
	uint8_t songMode, song, songPos;
	int8_t step[LXR_SEQ_TRACKS];	// each track's step, -1 before the first
	uint8_t muted;			// bit per track
	uint8_t kitAutoload;		// a pattern loads its kit when it starts
	uint32_t kitLoads;		// kits the sequencer loaded into the engine so far
} lxr_seq_status;
void lxr_seq_get_status (lxr_seq_status *st);

// ---- the front panel (as the LXR's step page, Euklid page, buttons) ----

// the pattern to play: at the end of the playing one (stopped: now), as the
// LXR's pattern buttons
void lxr_seq_set_pattern (uint8_t pattern);
void lxr_seq_set_song_mode (int on);
void lxr_seq_set_song (uint8_t song);
void lxr_seq_set_mute (uint8_t track, int muted);
void lxr_seq_set_kit_autoload (int on);

// the edited pattern (LXR_SEQ_PATTERNS: the playing one), track and step
// (0..63) the following act on
void lxr_seq_select (uint8_t pattern, uint8_t track, uint8_t step);
uint8_t lxr_seq_edit_pattern (void);

enum
{
	LXR_STEP_VELOCITY,	// 0..127
	LXR_STEP_NOTE,		// 0..127, 60: the voice's tuning
	LXR_STEP_PROB,		// 0..100 %
	LXR_STEP_FLAM,		// 0..5: off, x2, x3, x4, x6, x8
	LXR_STEP_SHIFT,		// 0..14: -7..+7 eighths of a step, 7: none
	LXR_STEP_LOCK1_TARGET,	// an index into the modulation targets (0: none)
	LXR_STEP_LOCK1_VALUE,	// 0..127
	LXR_STEP_LOCK2_TARGET,
	LXR_STEP_LOCK2_VALUE,
	LXR_STEP_ACTIVE,	// the main step bit (read only: lxr_seq_toggle_step())
	LXR_STEP_FIELDS
};
void lxr_seq_set_step_field (int field, uint8_t value);
uint8_t lxr_seq_get_step_field (uint8_t pattern, uint8_t track, uint8_t step, int field);
// toggles a step of the selected track in the edited pattern (a step button)
void lxr_seq_toggle_step (uint8_t step);
// 16 steps from first of a track: bit i = step first + i active
uint16_t lxr_seq_step_mask (uint8_t pattern, uint8_t track, uint8_t first);

// the edited pattern's settings: the selected track's length (1..64), the
// shuffle (0..127), the kit (0..63, LXR_SEQ_NO_KIT)
void lxr_seq_set_track_length (uint8_t length);
uint8_t lxr_seq_track_length (uint8_t pattern, uint8_t track);	// 1..64
uint8_t lxr_seq_pattern_length (uint8_t pattern);		// its longest track
void lxr_seq_set_shuffle (uint8_t shuffle);
uint8_t lxr_seq_shuffle (uint8_t pattern);
void lxr_seq_set_kit (uint8_t kit);
uint8_t lxr_seq_kit (uint8_t pattern);
int lxr_seq_has_kit (uint8_t kit);
// the Euklid generator on the selected track: fill steps over its length,
// rotated
void lxr_seq_euclid (uint8_t fill, uint8_t rotation);

// actions: clear the selected track / the edited pattern; copy the edited
// pattern, paste into the edited pattern; store the engine's kit as the
// edited pattern's kit (its slot; LXR_SEQ_NO_KIT: the first free slot)
void lxr_seq_clear_track (void);
void lxr_seq_clear_pattern (void);
void lxr_seq_copy_pattern (void);
void lxr_seq_paste_pattern (void);
void lxr_seq_store_kit (void);

// ---- the project (LV2 state) ----

typedef struct lxr_project lxr_project;

// a new empty project (calloc; NULL without memory); free: any but the
// engine's own (a no-op for that one)
lxr_project *lxr_seq_project_new (void);
void lxr_seq_project_free (lxr_project *p);
// the project being played (to save it; its pattern being played and the
// engine's kit are taken from the sequencer when serialised)
lxr_project *lxr_seq_project_current (void);
// the project as a chunk (lxr_sequencer.c: the format), written into buf
// (size: lxr_seq_chunk_size()); the size written
size_t lxr_seq_chunk_size (const lxr_project *p);
size_t lxr_seq_chunk_write (const lxr_project *p, uint8_t *buf, size_t size);
// a chunk read into p (from lxr_seq_project_new()): 0, or an error text
const char *lxr_seq_chunk_read (lxr_project *p, const uint8_t *buf, size_t size);
// makes p the project played (at a block boundary, in the audio thread):
// the pattern and song it says, its current kit into the engine, the
// position kept (located in the new project when running); returns the
// project replaced (to free outside the audio thread)
lxr_project *lxr_seq_project_install (lxr_project *p);

// project contents for tools and tests: a pattern as an LXR pattern file
// (V3, LXR_SEQ_PATTERN_FILE bytes) and back, a kit as an LXR kit file
// (name, the kit parameters, OpenElixir-02's engine block), a song as an
// LXR song file
#define LXR_SEQ_PATTERN_FILE	3663
#define LXR_SEQ_KIT_FILE	266
#define LXR_SEQ_SONG_FILE	72
void lxr_seq_project_get_pattern (const lxr_project *p, uint8_t pattern, uint8_t *file);
int lxr_seq_project_set_pattern (lxr_project *p, uint8_t pattern, const uint8_t *file, size_t size);
int lxr_seq_project_set_kit (lxr_project *p, uint8_t kit, const uint8_t *file, size_t size);
void lxr_seq_project_set_song (lxr_project *p, uint8_t song, const uint8_t *file, size_t size);
void lxr_seq_project_set_start (lxr_project *p, uint8_t pattern, uint8_t song, int songMode);

#ifdef __cplusplus
}
#endif

#endif
