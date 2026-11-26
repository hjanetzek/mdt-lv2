/*
 * lxr_engine.c - the drum voices of OpenElixir-02 (the LXR V2 firmware with
 * voice engines per slot; ../lxr, see README.md) as an engine. The sources
 * are compiled here as one unit; a block is the LXR's
 * mixer_calcNextSampleBlock() (mixer.c: the 6 voice slots, the 32-bit mix
 * with its limiter, the DJ filter), its two DAC outputs summed to one stereo
 * output, without the FX bus. Kits are parameter values (0..127, the LXR's
 * activeParameters) and the engine of each slot; they are set as the LXR's
 * front panel does (preset_sendDrumsetParameters(), presetManager.c: the
 * engines first, then the parameters through its parameter setter
 * midiParser_ccHandler(), MidiParserCC.c, the velocity and LFO targets as
 * indices into its modTargets table, Cc2Text.c). Parameter names, ranges and
 * pages come from the LXR's menu tables and follow the engines (MenuData.c).
 *
 * Copyright (C) 2012-2026 Erica Synths, 2026 tunefish.at (the voices),
 * MicroDexed-touch contributors (this engine)
 * SPDX-License-Identifier: GPL-3.0-only (see LICENSE and NOTICE.md)
 */

// needs -I ../lxr -I ../lxr/shim (library.json, the port's build files)
#include "Soundengine/DSPAudio/1PoleLp.c"
#include "Soundengine/DSPAudio/BufferTools.c"
#include "Soundengine/DSPAudio/CymbalVoice.c"
#include "Soundengine/DSPAudio/Decay.c"
#include "Soundengine/DSPAudio/distortion.c"
#include "Soundengine/DSPAudio/dither.c"
#include "Soundengine/DSPAudio/DrumVoice.c"
#include "Soundengine/DSPAudio/HiHat.c"
#include "Soundengine/DSPAudio/lfo.c"
#include "Soundengine/DSPAudio/modulationNode.c"
#include "Soundengine/DSPAudio/Oscillator.c"
#include "Soundengine/DSPAudio/random.c"
#include "Soundengine/DSPAudio/ResonantFilter.c"
#include "Soundengine/DSPAudio/SlopeEg2.c"
#include "Soundengine/DSPAudio/snapEg.c"
#include "Soundengine/DSPAudio/Snare.c"
#include "Soundengine/DSPAudio/squareRootLut.c"
#include "Soundengine/DSPAudio/transientGenerator.c"
#include "Soundengine/DSPAudio/transientTables.c"
#include "Soundengine/DSPAudio/wavetable.c"
#include "Soundengine/DSPAudio/VoiceSlot.c"
#include "Soundengine/DSPAudio/DjFilter.c"
#include "Soundengine/DSPAudio/mixer.c"
#include "Soundengine/DSPAudio/FX/FxRamBuffer.c"
#include "Soundengine/DSPAudio/FX/Distortion.c"
#include "Soundengine/DSPAudio/FX/Ringmod.c"
#include "Soundengine/DSPAudio/FX/Compressor.c"
#include "Soundengine/DSPAudio/FX/Delay.c"
#include "Soundengine/FxBus.c"
#include "Soundengine/MIDI/ParameterArray.c"
#include "Soundengine/MIDI/MidiParserCC.c"
#include "Preset/initKit.c"
#include "Preset/KitEngines.c"
#include "menu/MenuData.c"
#include "menu/Cc2Text.c"

#include "lxr_engine.h"
#include <stdio.h>
#include <string.h>

// ---- what the voices use from the rest of the LXR firmware (lxr_glue.h) ----

uint8_t activeParameters[NUM_PARAMS];
uint8_t menu_activePage;
PatternSet seq_patternSet;
GPIO_TypeDef lxr_gpio_plugged = { 0xFFFFFFFF };
RNG_HandleTypeDef hrng;

static uint8_t mutedTracks;		// bit per track
static uint32_t triggers[LXR_ENGINE_TRACKS];
static uint8_t paramName[NUM_PARAMS];	// NamesEnum per parameter (0: none)

uint32_t HAL_RNG_GetRandomNumber (RNG_HandleTypeDef *rng)
{
	// xorshift32, for the noise oscillators
	static uint32_t state = 2463534242u;
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

static uint16_t tempo = 120;		// lxr_engine_set_tempo()

uint16_t seq_getBpm (void)
{
	return tempo;		// the LFO sync
}

void seq_setMute (uint8_t trackNr, uint8_t isMuted)
{
	if (trackNr < LXR_ENGINE_TRACKS)
	{
		mutedTracks = isMuted ? mutedTracks | 1 << trackNr : mutedTracks & ~(1 << trackNr);
	}
}

void preset_touchTimer (PresetTimer type)
{
}

void menu_repaint (void)
{
}

uint8_t menu_getSubPage (void)
{
	return 0;
}

void menu_switchSubPage (uint8_t subPageNr, uint8_t toggle)
{
}

float adc_getFaderVolume (uint8_t idx)
{
	return 1.f;
}

// the front panel's messages to the sound engine, as CortexParser.c
void frontPanel_sendData (uint8_t status, uint8_t data1, uint8_t data2)
{
	MidiMsg msg = {0};
	msg.status = status;
	msg.data1 = data1;
	msg.data2 = data2;
	switch (status)
	{
	case MIDI_CC:
		// the parameter number offset between the front panel and the
		// sound engine
		msg.data1 = (data1 + 1) & 0x7F;
		midiParser_ccHandler (msg, 1, 0, 0.f);
		break;

	case CC_2:
		midiParser_ccHandler (msg, 1, 0, 0.f);
		break;

	case CC_LFO_TARGET:
	case CC_VELO_TARGET:
		{
			const uint8_t value = ((data1 & 0x01) << 7) | data2;
			const uint8_t nr = (data1 & 0xFE) >> 1;
			if (nr < NUM_VOICE_SLOTS)
			{
				modNode_setDestination (status == CC_LFO_TARGET ? &voiceSlots[nr].u.c.lfo.modTarget
										: &velocityModulators[nr], value);
			}
		}
		break;
	}
}

// ---- the engine ----

static void initParamNames (void);
static void setGlobal (uint16_t param, uint8_t value);

void lxr_engine_init (void)
{
	// as soundengine_init() (Soundengine.c)
	mixer_init ();
	parameterArray_init ();
	for (int i = 0; i < NUM_VOICE_SLOTS; i++)
	{
		modNode_init (&velocityModulators[i]);
	}
	slot_init ();
	kitEngines_init ();
	paramToModTargetInit ();

	mutedTracks = 0;
	memset (triggers, 0, sizeof triggers);

	// as menu_init() (menu.c): the DJ filter off, the limiter on
	setGlobal (PAR_DJ_FILTER, DJ_FILTER_OFF);
	setGlobal (PAR_DJ_RESO, 0);
	setGlobal (PAR_MIXER_LIMITER, 1);

	lxr_engine_load_init_kit ();
}

// a velocity or LFO modulation target: value is an index into modTargets
static int setModTarget (uint16_t param, uint8_t value)
{
	uint8_t status;
	uint8_t nr;
	if (param >= PAR_VEL_DEST_1 && param < PAR_VEL_DEST_1 + NUM_VOICE_SLOTS)
	{
		status = CC_VELO_TARGET;
		nr = param - PAR_VEL_DEST_1;
	}
	else if (param >= PAR_TARGET_LFO1 && param < PAR_TARGET_LFO1 + NUM_VOICE_SLOTS)
	{
		status = CC_LFO_TARGET;
		nr = param - PAR_TARGET_LFO1;
	}
	else
	{
		return 0;
	}

	if (value >= getNumModTargets ())
	{
		value = 0;
	}
	activeParameters[param] = value;
	// as kitEngines_sendTarget() (KitEngines.c)
	const uint8_t target = (uint8_t) modTargets[value].param;
	frontPanel_sendData (status, (uint8_t) (((target & 0x80) >> 7) | (nr << 1)), target & 0x7F);

	return 1;
}

// the global parameters (not in kits) the engine has, as the LXR's
// menu_parseGlobalParam() (menu.c)
static int isGlobal (uint16_t param)
{
	return param == PAR_DJ_FILTER || param == PAR_DJ_RESO || param == PAR_MIXER_LIMITER || param == PAR_MORPH;
}

// kit morph, as the LXR's preset_morph_tick() (presetManager.c), all at once:
// the engine gets each sound parameter between the kit's value
// (activeParameters) and the morph kit's, PAR_MORPH (0..255) the amount.
// Not morphed: the modulation targets, the LFOs' voices, the audio outputs
// and the engine specific parameters of a slot whose engine differs in the
// morph kit
_Static_assert (LXR_ENGINE_PAR_MORPH == PAR_MORPH, "LXR_ENGINE_PAR_MORPH");
static uint8_t morphKit[END_OF_SOUND_PARAMETERS];
static uint8_t morphEngines[NUM_VOICE_SLOTS];
static uint8_t morphLoaded;

static int isModTarget (uint16_t param);

// the voices' mix (LO, HI), over the kit's audio outputs (the LXR's jacks
// mean nothing in MDT): LXR_SPLIT_ENGINE: the drum engine's voices LO, the
// others HI; LXR_SPLIT_KIT: the kit's ("St1" LO, "St2" HI); LXR_SPLIT_LO:
// all LO. Voices routed to the FX bus stay there; the FX bus to fxMix (but
// with LXR_SPLIT_KIT: the kit's FX routing)
static uint8_t splitMode = LXR_SPLIT_ENGINE;
static uint8_t fxMix = 0;

static void applySplit (void)
{
	for (uint8_t v = 0; v < NUM_VOICE_SLOTS; v++)
	{
		const uint8_t kit = activeParameters[PAR_AUDIO_OUT1 + v];
		if (kit == MIXER_ROUTING_FX_BUS || splitMode == LXR_SPLIT_KIT)
			mixer_audioRouting[v] = kit;
		else if (splitMode == LXR_SPLIT_ENGINE && kitEngines[v] != ENGINE_DRUM)
			mixer_audioRouting[v] = MIXER_ROUTING_DAC2_STEREO;
		else
			mixer_audioRouting[v] = MIXER_ROUTING_DAC1_STEREO;
	}
	fxBus.audioRouting = splitMode == LXR_SPLIT_KIT ? activeParameters[PAR_FX_ROUTING]
			   : fxMix ? MIXER_ROUTING_DAC2_STEREO : MIXER_ROUTING_DAC1_STEREO;
}

void lxr_engine_set_split (uint8_t mode, uint8_t fx)
{
	splitMode = mode < LXR_SPLIT_MODES ? mode : LXR_SPLIT_ENGINE;
	fxMix = fx ? 1 : 0;
	applySplit ();
}

void lxr_engine_get_split (uint8_t *mode, uint8_t *fx)
{
	*mode = splitMode;
	*fx = fxMix;
}

// the LXR's interpolate(): x 0..255, rounded
static uint8_t morphInterpolate (uint8_t a, uint8_t b, uint8_t x)
{
	uint16_t fixedPointValue = (uint16_t) ((a * 256) + (b - a) * x);
	uint8_t result = (uint8_t) (fixedPointValue / 256);
	return (uint8_t) ((fixedPointValue & 0xff) < 0x7f ? result : result + 1);
}

static uint8_t morphValue (uint16_t param)
{
	const uint8_t morph = activeParameters[PAR_MORPH];
	if (!morphLoaded || morph == 0 || isModTarget (param)
	    || (param >= PAR_VOICE_LFO1 && param <= PAR_VOICE_LFO6)
	    || (param >= PAR_AUDIO_OUT1 && param <= PAR_AUDIO_OUT6))
	{
		return activeParameters[param];
	}
	const uint8_t slot = kitEngines_rolePar2Slot (param);
	if (slot < NUM_VOICE_SLOTS && kitEngines[slot] != morphEngines[slot])
	{
		return activeParameters[param];
	}
	return morphInterpolate (activeParameters[param], morphKit[param], morph);
}

// the slots of the sound parameters that hold MIDI state (NRPN data entry,
// NRPN number, bank select), not sound: sent like the others they set an
// NRPN number and write a value to that parameter (from 0: voice 1's filter
// drive). Kept in the kit, not sent (as the LXR's morph, Erica Synths
// 39928e1)
static int isTransportParameter (uint16_t param)
{
	return    param == PAR_NRPN_DATA_ENTRY_COARSE || param == PAR_NRPN_FINE
	       || param == PAR_NRPN_COARSE || param == PAR_BANK_CHANGE;
}

static void sendParameter (uint16_t param)
{
	if (isTransportParameter (param))
	{
		return;
	}
	frontPanel_sendData (param < 128 ? MIDI_CC : CC_2, param < 128 ? param : param - 128, morphValue (param));
}

static void applyMorph (void)
{
	for (uint16_t i = 0; i < END_OF_SOUND_PARAMETERS; i++)
	{
		if (!isModTarget (i))
		{
			sendParameter (i);
		}
	}
	applySplit ();	// the audio outputs were sent again
}

static void setGlobal (uint16_t param, uint8_t value)
{
	activeParameters[param] = value;
	switch (param)
	{
	case PAR_DJ_FILTER:	djFilter_value = value;		break;
	case PAR_DJ_RESO:	djFilter_reso = value;		break;
	case PAR_MIXER_LIMITER:	mixer_limiterOn = value;	break;
	case PAR_MORPH:		applyMorph ();			break;
	}
}

void lxr_engine_set_parameter (uint16_t param, uint8_t value)
{
	uint8_t min, max;
	lxr_engine_param_range (param, &min, &max);
	if ((param >= END_OF_SOUND_PARAMETERS && !isGlobal (param)) || value < min || value > max)
	{
		return;
	}

	if (isGlobal (param))
	{
		setGlobal (param, value);
		return;
	}

	if (setModTarget (param, value))
	{
		return;
	}

	activeParameters[param] = value;
	sendParameter (param);
	if (param == PAR_FX_TYPE)
	{
		initParamNames ();	// P1-P4 mean other things
	}
	if ((param >= PAR_AUDIO_OUT1 && param <= PAR_AUDIO_OUT6) || param == PAR_FX_ROUTING)
	{
		applySplit ();
	}
}

uint16_t lxr_engine_fx_param (uint8_t slot)
{
	if (slot >= 8)
	{
		return 0;
	}
	const Page *p = menu_getPage (FX_MODE_PAGE, fxBus.fxType);
	const uint16_t bot[8] = {p->bot1, p->bot2, p->bot3, p->bot4, p->bot5, p->bot6, p->bot7, p->bot8};
	return bot[slot] != PAR_NONE ? bot[slot] : 0;
}

uint8_t lxr_engine_get_parameter (uint16_t param)
{
	return param < END_OF_SOUND_PARAMETERS || isGlobal (param) ? activeParameters[param] : 0;
}

static int isModTarget (uint16_t param)
{
	return    (param >= PAR_VEL_DEST_1 && param < PAR_VEL_DEST_1 + NUM_VOICE_SLOTS)
	       || (param >= PAR_TARGET_LFO1 && param < PAR_TARGET_LFO1 + NUM_VOICE_SLOTS);
}

void lxr_engine_load_kit_engines (const uint8_t *params, uint16_t count, const uint8_t *engines)
{
	// the kit's engines first: its parameters are for them
	for (uint8_t slot = 0; slot < NUM_VOICE_SLOTS; slot++)
	{
		kitEngines[slot] = engines && engines[slot] < NUM_ENGINES ? engines[slot] : slot_stockEngine (slot);
	}
	kitEngines_apply ();
	initParamNames ();

	// the targets last: a target takes the value its parameter has then
	// (modNode_setDestination()); missing parameters are 0 (as the LXR's
	// preset_readDrumsetData())
	for (int targets = 0; targets <= 1; targets++)
	{
		for (uint16_t i = 0; i < END_OF_SOUND_PARAMETERS; i++)
		{
			if (isModTarget (i) == targets)
			{
				lxr_engine_set_parameter (i, i < count ? params[i] : 0);
			}
		}
	}
}

void lxr_engine_load_kit (const uint8_t *params, uint16_t count)
{
	lxr_engine_load_kit_engines (params, count, NULL);
}

void lxr_engine_get_kit (uint8_t *params, uint16_t count)
{
	for (uint16_t i = 0; i < count; i++)
	{
		params[i] = i < END_OF_SOUND_PARAMETERS ? activeParameters[i] : 0;
	}
}

uint16_t lxr_engine_kit_size (void)
{
	return END_OF_SOUND_PARAMETERS;
}

const uint16_t lxr_engine_globals[LXR_ENGINE_GLOBALS] = { PAR_MORPH, PAR_DJ_FILTER, PAR_DJ_RESO, PAR_MIXER_LIMITER };

void lxr_engine_load_init_kit (void)
{
	// the LXR's init kit: the name (8 bytes), then the sound parameters
	lxr_engine_load_kit (initKit + 8, INIT_KIT_SIZE - 8);
}

uint8_t lxr_engine_get_engine (uint8_t slot)
{
	return slot < NUM_VOICE_SLOTS ? kitEngines[slot] : 0;
}

void lxr_engine_set_engine (uint8_t slot, uint8_t engine)
{
	// as the LXR's engine menu: the slot's parameters start from the
	// engine's defaults (kitEngines_assign())
	kitEngines_assign (slot, engine);
	initParamNames ();
	applyMorph ();
	applySplit ();
}

void lxr_engine_load_morph_kit (const uint8_t *params, uint16_t count, const uint8_t *engines)
{
	morphLoaded = params != NULL;
	for (uint16_t i = 0; i < END_OF_SOUND_PARAMETERS; i++)
	{
		morphKit[i] = params && i < count ? params[i] : 0;
	}
	for (uint8_t slot = 0; slot < NUM_VOICE_SLOTS; slot++)
	{
		morphEngines[slot] = engines && engines[slot] < NUM_ENGINES ? engines[slot] : slot_stockEngine (slot);
	}
	applyMorph ();
}

uint8_t lxr_engine_get_morphed_parameter (uint16_t param)
{
	return param < END_OF_SOUND_PARAMETERS ? morphValue (param) : lxr_engine_get_parameter (param);
}

int lxr_engine_has_morph_kit (void)
{
	return morphLoaded;
}

void lxr_engine_get_engines (uint8_t *engines)
{
	memcpy (engines, kitEngines, NUM_VOICE_SLOTS);
}

const char *lxr_engine_engine_name (uint8_t engine)
{
	static const char *const names[NUM_ENGINES] = { "drum", "snare", "cymbal", "hi-hat" };
	return engine < NUM_ENGINES ? names[engine] : "";
}

uint8_t lxr_engine_encode_engines (const uint8_t *engines, uint8_t *block)
{
	return kitEngines_encode (engines, block);
}

void lxr_engine_decode_engines (const uint8_t *block, uint16_t size, uint8_t *engines)
{
	kitEngines_decode (block, size, engines);
}

void lxr_engine_set_tempo (uint16_t bpm)
{
	if (bpm != tempo && bpm > 0)
	{
		tempo = bpm;
		lfo_recalcSync ();
	}
}

void lxr_engine_midi_cc (uint8_t cc, uint8_t value)
{
	// as the LXR's MIDI implementation (midiParser_ccHandler()): CC n is the
	// parameter n-1, NRPN n (CC 99, 98, then 6) the parameter 128+n; CC 0
	// (bank select) and the mod wheel do not set parameters. NRPN 98: an
	// engine (value = slot * 4 + engine, OpenElixir-02)
	static uint16_t nrpn;
	switch (cc)
	{
	case NRPN_COARSE:
		nrpn = (nrpn & 0x7F) | (value & 0x7F) << 7;
		break;

	case NRPN_FINE:
		nrpn = (nrpn & ~0x7F) | (value & 0x7F);
		break;

	case NRPN_DATA_ENTRY_COARSE:
		if (128 + nrpn == PAR_SLOT_ENGINE)
		{
			lxr_engine_set_engine (value / NUM_ENGINES, value % NUM_ENGINES);
		}
		else if (128 + nrpn == PAR_MORPH)
		{
			lxr_engine_set_parameter (PAR_MORPH, value * 2 + (value >> 6));	// 0..255
		}
		else
		{
			lxr_engine_set_parameter (128 + nrpn, value);
		}
		break;

	case CC_BANK_CHANGE:
	case CC_MOD_WHEEL:
		break;

	default:
		lxr_engine_set_parameter (cc - 1, value);
		break;
	}
}

// ---- parameter info, from the LXR's menu tables (following the engines) ----

// the name of each parameter: from the voice pages (menu_getPage(): the
// pages of the engines the slots run), then the mod targets
static void initParamNames (void)
{
	menu_updateVoicePages ();
	memset (paramName, 0, sizeof paramName);
	// the voices, the FX bus (its parameters P1-P4 named after the FX type's
	// sub page first), then the globals
	static const uint8_t pages[] = { VOICE1_PAGE, VOICE2_PAGE, VOICE3_PAGE, VOICE4_PAGE, VOICE5_PAGE, VOICE6_PAGE, VOICE7_PAGE,
					 FX_MODE_PAGE, PERF_PAGE, MENU_MIDI_PAGE };
	for (uint8_t i = 0; i < sizeof pages; i++)
	{
		const uint8_t page = pages[i];
		for (uint8_t s = 0; s < LXR_ENGINE_SUB_PAGES; s++)
		{
			const uint8_t sub = page == FX_MODE_PAGE ? (s + fxBus.fxType) % LXR_ENGINE_SUB_PAGES : s;
			const Page *p = menu_getPage (page, sub);
			const uint8_t top[8] = {p->top1, p->top2, p->top3, p->top4, p->top5, p->top6, p->top7, p->top8};
			const uint16_t bot[8] = {p->bot1, p->bot2, p->bot3, p->bot4, p->bot5, p->bot6, p->bot7, p->bot8};
			for (uint8_t slot = 0; slot < 8; slot++)
			{
				if (   bot[slot] != PAR_NONE && bot[slot] < NUM_PARAMS
				    && top[slot] != TEXT_EMPTY && top[slot] != TEXT_SKIP && !paramName[bot[slot]])
				{
					paramName[bot[slot]] = top[slot];
				}
			}
		}
	}
	for (uint8_t i = 1; i < getNumModTargets (); i++)
	{
		const uint8_t name = modTargetNameIdx (i);
		if (modTargets[i].param < END_OF_SOUND_PARAMETERS && !paramName[modTargets[i].param] && name != MOD_TARGET_HIDDEN)
		{
			paramName[modTargets[i].param] = name;
		}
	}
}

uint16_t lxr_engine_page_param (uint8_t page, uint8_t subPage, uint8_t slot)
{
	if (page >= LXR_ENGINE_PAGES || subPage >= LXR_ENGINE_SUB_PAGES || slot >= 8)
	{
		return 0;
	}
	const Page *p = menu_getPage (page, subPage);
	const uint16_t bot[8] = {p->bot1, p->bot2, p->bot3, p->bot4, p->bot5, p->bot6, p->bot7, p->bot8};
	const uint8_t top[8] = {p->top1, p->top2, p->top3, p->top4, p->top5, p->top6, p->top7, p->top8};

	return top[slot] == TEXT_SKIP || bot[slot] >= END_OF_SOUND_PARAMETERS ? 0 : bot[slot];
}

const char *lxr_engine_param_name (uint16_t param, uint8_t kind)
{
	if (param >= NUM_PARAMS || !paramName[param])
	{
		return "";
	}
	const Name *name = &valueNames[paramName[param]];
	switch (kind)
	{
	case LXR_NAME_SHORT:	return shortNames[name->shortName];
	case LXR_NAME_CATEGORY:	return catNames[name->category];
	default:		return longNames[name->longName];
	}
}

void lxr_engine_param_range (uint16_t param, uint8_t *min, uint8_t *max)
{
	*min = 0;
	*max = 127;
	if (param >= END_OF_SOUND_PARAMETERS && !isGlobal (param))
	{
		*max = 0;
		return;
	}

	const unsigned dtype = menu_paramDtype (param);
	switch (dtype & 0xFF)
	{
	case DTYPE_0B255:	*max = 255;	break;
	case DTYPE_ON_OFF:
	case DTYPE_0b1:		*max = 1;	break;
	case DTYPE_1B16:	*min = 1; *max = 16;	break;
	case DTYPE_0B15:	*max = 15;	break;
	case DTYPE_1B64:	*min = 1; *max = 64;	break;
	case DTYPE_0B100:	*max = 100;	break;
	case DTYPE_VOICE_LFO:	*min = 1; *max = 6;	break;
	case DTYPE_TARGET_SELECTION_VELO:
	case DTYPE_TARGET_SELECTION_LFO:
	case DTYPE_AUTOM_TARGET:
		*max = getNumModTargets () - 1;
		break;
	case DTYPE_MENU:
		*max = getMaxEntriesForMenu (dtype >> 8) - 1;
		break;
	default:
		break;
	}
}

uint8_t lxr_engine_param_kind (uint16_t param)
{
	if (param >= END_OF_SOUND_PARAMETERS && !isGlobal (param))
	{
		return LXR_KIND_NUMBER;
	}
	switch (menu_paramDtype (param) & 0xFF)
	{
	case DTYPE_PM63:		return LXR_KIND_SIGNED;
	case DTYPE_ON_OFF:
	case DTYPE_0b1:			return LXR_KIND_SWITCH;
	case DTYPE_MENU:
	case DTYPE_NOTE_NAME:
	case DTYPE_TARGET_SELECTION_VELO:
	case DTYPE_TARGET_SELECTION_LFO:
	case DTYPE_AUTOM_TARGET:	return LXR_KIND_TEXT;
	default:			return LXR_KIND_NUMBER;
	}
}

uint8_t lxr_engine_param_center (uint16_t param)
{
	// the tuning parameters show +-63 around 60 (menu_repaintGeneric())
	return menu_isTuningParam (param) ? 60 : 63;
}

void lxr_engine_param_text (uint16_t param, uint8_t value, char *text, unsigned size)
{
	if ((param >= END_OF_SOUND_PARAMETERS && !isGlobal (param)) || size < 2)
	{
		if (size)
		{
			*text = '\0';
		}
		return;
	}

	static const char *const notes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	const unsigned dtype = menu_paramDtype (param);
	switch (dtype & 0xFF)
	{
	case DTYPE_MENU:
		{
			char name[4] = {0};
			getMenuItemNameForValue (dtype >> 8, value, name);
			snprintf (text, size, "%.3s", name);
		}
		break;

	case DTYPE_PM63:
		snprintf (text, size, "%+d", value - lxr_engine_param_center (param));
		break;

	case DTYPE_ON_OFF:
	case DTYPE_0b1:
		snprintf (text, size, "%s", value ? "on" : "off");
		break;

	case DTYPE_NOTE_NAME:
		if (value == 0)
		{
			snprintf (text, size, "-");		// no note override
		}
		else
		{
			snprintf (text, size, "%s%d", notes[value % 12], value / 12 - 1);
		}
		break;

	case DTYPE_TARGET_SELECTION_VELO:
	case DTYPE_TARGET_SELECTION_LFO:
	case DTYPE_AUTOM_TARGET:
		{
			const uint8_t name = value < getNumModTargets () ? modTargetNameIdx (value) : MOD_TARGET_HIDDEN;
			if (value == 0 || name == MOD_TARGET_HIDDEN)
			{
				snprintf (text, size, "off");
			}
			else
			{
				snprintf (text, size, "%u %s", voiceFromModTargValue (value), shortNames[valueNames[name].shortName]);
			}
		}
		break;

	default:
		snprintf (text, size, "%u", value);
		break;
	}
}

void lxr_engine_trigger (uint8_t track, uint8_t velocity, uint8_t note)
{
	if (track >= LXR_ENGINE_TRACKS || (mutedTracks & 1 << track))
	{
		return;
	}
	triggers[track]++;

	// tracks 1..6: slots 1..6, track 7: the 2nd trigger of slot 6 (VoiceSlot.c)
	slot_trigger (track, velocity, note);
}

void lxr_engine_status (char *text, unsigned size)
{
	snprintf (text, size, "bd=%u tom1=%u tom2=%u snare=%u cymbal=%u hhc=%u hho=%u",
		  (unsigned) triggers[0], (unsigned) triggers[1], (unsigned) triggers[2], (unsigned) triggers[3],
		  (unsigned) triggers[4], (unsigned) triggers[5], (unsigned) triggers[6]);
}

// the two mixes: LO the LXR's DAC 1 (its routing "St1", the mixer's mix2),
// HI its DAC 2 ("St2", mix). The LXR's outputs are AC coupled: a DC blocker
// (one pole high pass, about 10 Hz) on each removes the DC some of its
// sounds have (the FX bus' tube and fold distortion, offsets)
void lxr_engine_render_split (int16_t *loL, int16_t *loR, int16_t *hiL, int16_t *hiR)
{
	int16_t mix[LXR_ENGINE_BLOCK * 2];	// interleaved stereo
	int16_t mix2[LXR_ENGINE_BLOCK * 2];
	mixer_calcNextSampleBlock (mix, mix2);

	static float x1[4], y1[4];
	const float r = 1.f - 2.f * (float) M_PI * 10.f / REAL_FS;
	int16_t *const out[4] = { loL, loR, hiL, hiR };
	for (int i = 0; i < LXR_ENGINE_BLOCK; i++)
	{
		for (int c = 0; c < 4; c++)
		{
			const float x = c < 2 ? mix2[2 * i + c] : mix[2 * i + c - 2];
			y1[c] = x - x1[c] + r * y1[c];
			x1[c] = x;
			out[c][i] = __SSAT ((int32_t) y1[c], 16);
		}
	}
}

void lxr_engine_render (int16_t *left, int16_t *right)
{
	int16_t loL[LXR_ENGINE_BLOCK], loR[LXR_ENGINE_BLOCK], hiL[LXR_ENGINE_BLOCK], hiR[LXR_ENGINE_BLOCK];
	lxr_engine_render_split (loL, loR, hiL, hiR);
	for (int i = 0; i < LXR_ENGINE_BLOCK; i++)
	{
		left[i] = __SSAT (left[i] + loL[i] + hiL[i], 16);
		right[i] = __SSAT (right[i] + loR[i] + hiR[i], 16);
	}
}
