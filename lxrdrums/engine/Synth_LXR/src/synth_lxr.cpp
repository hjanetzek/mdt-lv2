/*
 * synth_lxr.cpp
 *
 * Copyright (C) MicroDexed-touch contributors
 * SPDX-License-Identifier: GPL-3.0-only (see LICENSE and NOTICE.md)
 */
#include "synth_lxr.h"
#include <Audio.h>		// AudioNoInterrupts(), saturate16()

static_assert (AUDIO_BLOCK_SAMPLES % LXR_ENGINE_BLOCK == 0, "the engine renders 64 frames at a time");

AudioSynthLXR::AudioSynthLXR (void)
:	AudioStream (0, NULL)
{
	lxr_engine_init ();
}

void AudioSynthLXR::noteOn (uint8_t track, uint8_t velocity, uint8_t note)
{
	AudioNoInterrupts ();
	lxr_engine_trigger (track, velocity, note);
	AudioInterrupts ();
}

void AudioSynthLXR::setParameter (uint16_t param, uint8_t value)
{
	AudioNoInterrupts ();
	lxr_engine_set_parameter (param, value);
	AudioInterrupts ();
}

uint8_t AudioSynthLXR::getParameter (uint16_t param)
{
	return lxr_engine_get_parameter (param);
}

void AudioSynthLXR::loadKit (const uint8_t *params, uint16_t count, const uint8_t *engines)
{
	AudioNoInterrupts ();
	lxr_engine_load_kit_engines (params, count, engines);
	AudioInterrupts ();
}

void AudioSynthLXR::loadMorphKit (const uint8_t *params, uint16_t count, const uint8_t *engines)
{
	AudioNoInterrupts ();
	lxr_engine_load_morph_kit (params, count, engines);
	AudioInterrupts ();
}

void AudioSynthLXR::setEngine (uint8_t slot, uint8_t engine)
{
	AudioNoInterrupts ();
	lxr_engine_set_engine (slot, engine);
	AudioInterrupts ();
}

uint8_t AudioSynthLXR::getEngine (uint8_t slot)
{
	return lxr_engine_get_engine (slot);
}

void AudioSynthLXR::loadInitKit (void)
{
	AudioNoInterrupts ();
	lxr_engine_load_init_kit ();
	AudioInterrupts ();
}

void AudioSynthLXR::setTempo (uint16_t bpm)
{
	AudioNoInterrupts ();
	lxr_engine_set_tempo (bpm);
	AudioInterrupts ();
}

void AudioSynthLXR::midiCC (uint8_t cc, uint8_t value)
{
	AudioNoInterrupts ();
	lxr_engine_midi_cc (cc, value);
	AudioInterrupts ();
}

void AudioSynthLXR::update (void)
{
	// outputs 0, 1: both mixes; 2, 3: LO; 4, 5: HI
	audio_block_t *out[6];
	for (int o = 0; o < 6; o++)
	{
		out[o] = allocate ();
		if (!out[o])
		{
			while (o--)
			{
				release (out[o]);
			}
			return;
		}
	}

	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i += LXR_ENGINE_BLOCK)
	{
		lxr_engine_render_split (out[2]->data + i, out[3]->data + i, out[4]->data + i, out[5]->data + i);
	}
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
	{
		out[0]->data[i] = saturate16 (out[2]->data[i] + out[4]->data[i]);
		out[1]->data[i] = saturate16 (out[3]->data[i] + out[5]->data[i]);
	}

	for (int o = 0; o < 6; o++)
	{
		transmit (out[o], o);
		release (out[o]);
	}
}

void AudioSynthLXR::setSplit (uint8_t mode, uint8_t fx)
{
	AudioNoInterrupts ();
	lxr_engine_set_split (mode, fx);
	AudioInterrupts ();
}
