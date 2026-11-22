/*
 * lxr_engine.h - the drum voices of OpenElixir-02 (the LXR V2 firmware of
 * Erica Synths with voice engines per slot) as a block based engine: 6
 * voice slots on 7 tracks, each slot running a drum, snare, cymbal or
 * hi-hat engine (stock: BD, Tom 1, Tom 2, snare/clap, cymbal, hi-hat; track
 * 7 is the second trigger of slot 6: the open hi-hat), mixed to stereo.
 * One engine per program (the LXR voices are globals).
 *
 * Copyright (C) 2012-2026 Erica Synths (the voices), MicroDexed-touch
 * contributors (this engine)
 * SPDX-License-Identifier: GPL-3.0-only (see LICENSE and NOTICE.md)
 */
#ifndef LXR_ENGINE_H_
#define LXR_ENGINE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXR_ENGINE_BLOCK	64	// frames per lxr_engine_render() (the LXR's OUTPUT_DMA_SIZE)
#define LXR_ENGINE_TRACKS	7
#define LXR_ENGINE_NOTE		60	// the note that plays a voice at its tuned pitch

#define LXR_ENGINE_SLOTS	6
#define LXR_ENGINE_ENGINES	4	// drum, snare, cymbal, hi-hat
#define LXR_ENGINE_BLOCK_SIZE	11	// the engine block of a kit file (after the parameters)

enum
{
	LXR_TRACK_DRUM1,	// bass drum
	LXR_TRACK_DRUM2,	// tom 1
	LXR_TRACK_DRUM3,	// tom 2
	LXR_TRACK_SNARE,	// snare / clap
	LXR_TRACK_CYMBAL,
	LXR_TRACK_HIHAT_CLOSED,
	LXR_TRACK_HIHAT_OPEN,	// the hi-hat voice; open and closed choke each other
};

// the voices with the LXR's init kit
void lxr_engine_init (void);

// a kit: the LXR's sound parameters (Parameters.h, PAR_*) in order, values
// 0..127, and the engine of each slot (NULL: the stock engines)
void lxr_engine_load_kit_engines (const uint8_t *params, uint16_t count, const uint8_t *engines);
void lxr_engine_load_kit (const uint8_t *params, uint16_t count);	// the stock engines
// the morph kit (as the LXR's: the kit's sound moves towards it with the
// morph amount; params NULL: none); the kit and its parameters stay as set
void lxr_engine_load_morph_kit (const uint8_t *params, uint16_t count, const uint8_t *engines);
int lxr_engine_has_morph_kit (void);
uint8_t lxr_engine_get_morphed_parameter (uint16_t param);	// the value the voice has

// the FX bus (as the LXR's: distortion, ring modulator, compressor or delay;
// the voices whose audio output is the FX bus go through it): the parameters
// of its page for the current FX type, slot 0..7 (0: none; slot 0 the type)
uint16_t lxr_engine_fx_param (uint8_t slot);

// the engine of a slot (0 drum, 1 snare, 2 cymbal, 3 hi-hat); setting one
// resets the slot's parameters to the engine's defaults (as the LXR)
uint8_t lxr_engine_get_engine (uint8_t slot);
void lxr_engine_set_engine (uint8_t slot, uint8_t engine);
void lxr_engine_get_engines (uint8_t *engines);			// LXR_ENGINE_SLOTS
const char *lxr_engine_engine_name (uint8_t engine);
// the engine block of LXR kit files (after the parameters): "OE02", size,
// the engines; decoding something else gives the stock engines
uint8_t lxr_engine_encode_engines (const uint8_t *engines, uint8_t *block);	// LXR_ENGINE_BLOCK_SIZE
void lxr_engine_decode_engines (const uint8_t *block, uint16_t size, uint8_t *engines);

// one sound parameter (PAR_*, below END_OF_SOUND_PARAMETERS), value in its range
// (lxr_engine_param_range(), mostly 0..127), or a global one (not in kits):
// lxr_engine_globals: the kit morph amount (0..255; NRPN 120: 0..127), the DJ
// filter (63 off), its resonance, the limiter (1 on)
void lxr_engine_set_parameter (uint16_t param, uint8_t value);
uint8_t lxr_engine_get_parameter (uint16_t param);
#define LXR_ENGINE_GLOBALS	4
#define LXR_ENGINE_PAR_MORPH	248	// PAR_MORPH
extern const uint16_t lxr_engine_globals[LXR_ENGINE_GLOBALS];

// the sound parameters as a kit (the LXR's .snd files without the 8 byte name)
uint16_t lxr_engine_kit_size (void);			// END_OF_SOUND_PARAMETERS
void lxr_engine_get_kit (uint8_t *params, uint16_t count);
void lxr_engine_load_init_kit (void);

// the tempo the LFOs synchronize to (default 120)
void lxr_engine_set_tempo (uint16_t bpm);

// a MIDI control change, as the LXR's MIDI implementation: CC n sets
// parameter n-1, NRPN n (CC 99, 98, then data entry CC 6) parameter 128+n;
// NRPN 98: an engine (value = slot * 4 + engine)
void lxr_engine_midi_cc (uint8_t cc, uint8_t value);

// parameter info, from the LXR's menu tables, following the engines the
// slots run. Pages: one per slot (0..5) and one for track 7 (6, slot 6's
// second trigger), 8 sub pages each (oscillator, amp EG, modulation, FM /
// noise, transient, filter, LFO, mix), 8 parameters per sub page (0: none)
#define LXR_ENGINE_PAGES	7
#define LXR_ENGINE_SUB_PAGES	8
enum { LXR_NAME_LONG, LXR_NAME_SHORT, LXR_NAME_CATEGORY };
uint16_t lxr_engine_page_param (uint8_t page, uint8_t subPage, uint8_t slot);
const char *lxr_engine_param_name (uint16_t param, uint8_t kind);
void lxr_engine_param_range (uint16_t param, uint8_t *min, uint8_t *max);
void lxr_engine_param_text (uint16_t param, uint8_t value, char *text, unsigned size);

// how a parameter is shown: a number (0..max), a signed number (value - 63),
// on/off, or a text (menu items, note names, modulation targets)
enum { LXR_KIND_NUMBER, LXR_KIND_SIGNED, LXR_KIND_SWITCH, LXR_KIND_TEXT };
uint8_t lxr_engine_param_kind (uint16_t param);
uint8_t lxr_engine_param_center (uint16_t param);	// LXR_KIND_SIGNED: the value shown as 0

// starts a voice: velocity 1..127, note LXR_ENGINE_NOTE for the tuned pitch
void lxr_engine_trigger (uint8_t track, uint8_t velocity, uint8_t note);

// the next LXR_ENGINE_BLOCK frames, added (saturating) to left and right
void lxr_engine_render (int16_t *left, int16_t *right);
// the two mixes (LO, HI: lxr_engine_set_split()), written
void lxr_engine_render_split (int16_t *loL, int16_t *loR, int16_t *hiL, int16_t *hiR);

// which voices play in which mix: LXR_SPLIT_ENGINE (the drum engine's voices
// LO, snare, cymbal and hi-hat HI), LXR_SPLIT_KIT (the kit's audio outputs:
// "St1" LO, "St2" HI), LXR_SPLIT_LO (all LO); voices routed to the FX bus
// stay there, the FX bus plays in fx (0 LO, 1 HI; LXR_SPLIT_KIT: the kit's)
enum { LXR_SPLIT_ENGINE, LXR_SPLIT_KIT, LXR_SPLIT_LO, LXR_SPLIT_MODES };
void lxr_engine_set_split (uint8_t mode, uint8_t fx);
void lxr_engine_get_split (uint8_t *mode, uint8_t *fx);

// test aid: the triggers per track since lxr_engine_init(), as text
// "bd=N tom1=N tom2=N snare=N cymbal=N hhc=N hho=N"
void lxr_engine_status (char *text, unsigned size);

#ifdef __cplusplus
}
#endif

#endif
