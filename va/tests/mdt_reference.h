/*
 * mdt_reference.h - MicroDexed-touch's Trio / Analog as MDT runs them, at
 * 44.1 kHz, for the render test: an independent transcription (not the
 * plugin's voice layer) of
 * - AudioSynthTrio / AudioSynthAnalog (synth_trio.cpp, synth_analog.cpp:
 *   constructor phases, set_env with 44100 and 44.1f, env_step_block,
 *   noteOn, noteOff, update), driving the copied engine (engine/);
 * - trio_update_all_settings / analog_update_all_settings (trio.cpp,
 *   analog.cpp): every parameter to every voice, the mixer gain;
 * - handleNoteOn / handleNoteOff's voice allocation (MicroDexed-touch.ino
 *   5052-5117, 5455-5463), with its note_buffer (0 = free);
 * - AudioMixer<8> (template_mixer.hpp, the generic DSP path: Q16
 *   multiplier, signed_multiply_32x16b, saturation, saturating adds), and
 *   the same without its saturation (saturate = 0: the plugin's mixer);
 * - the panorama at the centre (q15 -> float -> q15: unchanged).
 * The output is the mixer's block (16 bit full scale; int16 when saturated).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VA_MDT_REFERENCE_H_
#define VA_MDT_REFERENCE_H_

#include "analog_engine.h"
#include "dsp_basics.h"
#include "params.h"
#include "synth_shared.h"
#include "trio_engine.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MDT_NUM 8	// NUM_TRIO, NUM_ANALOG

// AudioSynthTrio / AudioSynthAnalog
struct MdtVoice
{
	int analog;
	track_t track;
	enum EnvStage : uint8_t { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
	EnvStage env_stage;
	int32_t env_level, env_att_inc, env_dec_inc, env_rel_inc, env_sus_lvl;

	void init (int isAnalog)
	{
		analog = isAnalog;
		env_stage = ENV_OFF;
		env_level = 0;
		env_att_inc = env_dec_inc = env_rel_inc = isAnalog ? 0x7FFF0000 : 32767;
		env_sus_lvl = 0;
		memset (&track, 0, sizeof track);
		if (isAnalog)
		{
			track.v[0].ph[0] = 0;
			track.v[0].ph[1] = 0x40000000u;
		}
		else
		{
			track.v[0].ph[0] = 0;
			track.v[0].ph[1] = 0x1C000000u;
			track.v[0].ph[2] = 0x38000000u;
		}
	}

	void set_env (uint8_t atk, uint8_t dec, uint8_t sus, uint8_t rel)
	{
		const int32_t ENV_MAX = 32767 << 16;
		const float DECAY_MS = 3048.0f;
		const float RELEASE_MS = 13000.0f;

		int64_t sus_q16 = (int64_t) sus * ENV_MAX / 50;
		if (sus_q16 > ENV_MAX)
			sus_q16 = ENV_MAX;
		env_sus_lvl = (int32_t) sus_q16;

		const int32_t a_samp = ((int32_t) atk * 4 * 44100) / 1000 + 1;

		const float dec_norm = (float) dec / 127.0f;
		const int32_t d_samp = (int32_t) (dec_norm * dec_norm * DECAY_MS * 44.1f) + 1;

		const float rel_norm = (float) rel / 127.0f;
		const int32_t r_samp = (int32_t) (rel_norm * rel_norm * rel_norm * RELEASE_MS * 44.1f) + 1;

		env_att_inc = ENV_MAX / a_samp;
		if (env_att_inc < 1)
			env_att_inc = 1;
		env_dec_inc = (ENV_MAX - env_sus_lvl) / d_samp;
		if (env_dec_inc < 1)
			env_dec_inc = 1;
		env_rel_inc = ENV_MAX / r_samp;
		if (env_rel_inc < 1)
			env_rel_inc = 1;
	}

	int32_t env_step_block (uint32_t n)
	{
		const int32_t ENV_MAX = 32767 << 16;
		for (uint32_t i = 0; i < n; i++)
		{
			switch (env_stage)
			{
			case ENV_ATTACK:
				if (env_att_inc >= ENV_MAX - env_level)
				{
					env_level = ENV_MAX;
					env_stage = ENV_DECAY;
				}
				else
					env_level += env_att_inc;
				break;
			case ENV_DECAY:
				if (env_dec_inc >= env_level - env_sus_lvl)
				{
					env_level = env_sus_lvl;
					env_stage = ENV_SUSTAIN;
				}
				else
					env_level -= env_dec_inc;
				break;
			case ENV_SUSTAIN:
				env_level = env_sus_lvl;
				break;
			case ENV_RELEASE:
				if (env_rel_inc >= env_level)
				{
					env_level = 0;
					env_stage = ENV_OFF;
				}
				else
					env_level -= env_rel_inc;
				break;
			case ENV_OFF:
			default:
				env_level = 0;
				break;
			}
		}
		return env_level >> 16;
	}

	bool isBusy () const { return env_stage != ENV_OFF; }

	void noteOn (uint8_t note)
	{
		track.v[0].note = note;
		track.v[0].active = 1;
		track.v[0].age += 1;
		if (analog)
			analog_note_on (&track, &track.v[0]);
		else
			trio_note_on (&track, &track.v[0]);
		env_stage = ENV_ATTACK;
	}

	void noteOff ()
	{
		track.v[0].active = 0;
		env_stage = ENV_RELEASE;
	}

	void update (int16_t *data)
	{
		const bool was_busy = isBusy ();
		if (!track.v[0].active && !was_busy)
		{
			track.v[0].s[0] = 0;
			track.v[0].s[1] = 0;
			if (!analog)
			{
				track.v[0].s[4] = 0;
				track.v[0].s[5] = 0;
			}
			env_level = 0;
			memset (data, 0, CTL * sizeof (int16_t));
			return;
		}
		const int32_t env_start = env_level >> 16;
		const int32_t env_end = env_step_block (CTL);
		const int32_t cut_amt = (int32_t) track.p[P_E8_ENV_TO_CUT] << 8;
		const int32_t pw_amt = (int32_t) track.p[P_E9_ENV_TO_PW] << 8;
		const int32_t cut_start = mulq15 (env_start, cut_amt);
		const int32_t pw_mod = mulq15 (env_start, pw_amt);

		vmod_t m;
		memset (&m, 0, sizeof m);
		m.pitch16 = ((int32_t) track.v[0].note) << 4;
		m.inc = pitch_inc (m.pitch16);
		m.amp0 = env_start;
		m.amp1 = env_end;
		m.cutoff = cut_start;
		m.shape = (64 << 8) + pw_mod;
		m.envq15 = env_end;

		int32_t acc[CTL] = { 0 };
		if (analog)
			analog_render (&track, &track.v[0], acc, CTL, &m);
		else
			trio_render (&track, &track.v[0], acc, CTL, &m);
		for (int i = 0; i < CTL; i++)
		{
			int32_t s = acc[i];
			if (s > 32767)
				s = 32767;
			if (s < -32768)
				s = -32768;
			data[i] = (int16_t) s;
		}
	}
};

// template_mixer.hpp's helpers (the generic C versions of the Cortex-M
// instructions)
static inline int32_t mdt_signed_multiply_32x16b (int32_t a, uint32_t b)
{
	return (int32_t) (((int64_t) a * (int16_t) (b & 0xFFFF)) >> 16);
}

static inline int32_t mdt_signed_saturate_rshift (int32_t val, int bits, int rshift)
{
	const int32_t out = val >> rshift, max = (1 << (bits - 1)) - 1, min = -(1 << (bits - 1));
	return out > max ? max : out < min ? min : out;
}

static inline int16_t mdt_sat_add16 (int16_t a, int16_t b)
{
	return (int16_t) mdt_signed_saturate_rshift ((int32_t) a + b, 16, 0);
}

static inline float mdt_mapfloat (float val, float in_min, float in_max, float out_min, float out_max)
{
	return (val - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

struct MdtSynth
{
	int analog;
	MdtVoice voice[MDT_NUM];
	uint8_t note_buffer[MDT_NUM];
	uint8_t slot;
	int value[VA_NUM_PARAMS];	// trio_osc / analog_osc as voice layer parameters
	int32_t multiplier;		// AudioMixer<8>, the same for all channels
	int saturate;			// MDT's int16 mixer, else the plugin's sum

	void init (int isAnalog, const int values[VA_NUM_PARAMS], int saturated)
	{
		analog = isAnalog;
		saturate = saturated;
		for (int i = 0; i < MDT_NUM; i++)
		{
			voice[i].init (isAnalog);
			note_buffer[i] = 0;
		}
		slot = 0;
		memcpy (value, values, sizeof value);
		update_all_settings ();
	}

	// trio_update_all_settings / analog_update_all_settings
	void update_all_settings ()
	{
		const float norm = sqrtf ((float) MDT_NUM);
		const float gain = mdt_mapfloat ((float) value[VA_LEVEL], 0, 100, 0.0f, analog ? 2.5f : 3.5f) / norm;
		for (int i = 0; i < MDT_NUM; i++)
		{
			for (int p = VA_P_E0; p <= VA_P_E7; p++)
				voice[i].track.p[p] = (int16_t) value[p];
			voice[i].set_env ((uint8_t) value[VA_ATTACK], (uint8_t) value[VA_DECAY], (uint8_t) value[VA_SUSTAIN],
					  (uint8_t) value[VA_RELEASE]);
			voice[i].track.p[P_E8_ENV_TO_CUT] = (int16_t) value[VA_ENV_TO_CUT];
			voice[i].track.p[P_E9_ENV_TO_PW] = (int16_t) value[VA_ENV_TO_PW];
		}
		// AudioMixer::gain
		float g = gain;
		if (g > 32767.0f)
			g = 32767.0f;
		multiplier = (int32_t) (g * 65536.0f);
	}

	void handleNoteOn (uint8_t inNumber)
	{
		int8_t s_ = -1;
		for (uint8_t s = 0; s < MDT_NUM; s++)
			if (note_buffer[s] == inNumber)
			{
				s_ = (int8_t) s;
				break;
			}
		if (s_ < 0)
			for (uint8_t s = 0; s < MDT_NUM; s++)
				if (note_buffer[s] == 0 && !voice[s].isBusy ())
				{
					s_ = (int8_t) s;
					break;
				}
		if (s_ < 0)
			for (uint8_t s = 0; s < MDT_NUM; s++)
				if (note_buffer[s] == 0)
				{
					s_ = (int8_t) s;
					break;
				}
		if (s_ < 0)
		{
			slot++;
			if (slot >= MDT_NUM)
				slot = 0;
			s_ = (int8_t) slot;
		}
		int n = (int) inNumber + value[VA_TRANSPOSE];
		voice[s_].noteOn ((uint8_t) (n < 0 ? 0 : n > 127 ? 127 : n));
		note_buffer[s_] = inNumber;
	}

	void handleNoteOff (uint8_t inNumber)
	{
		for (uint8_t s = 0; s < MDT_NUM; s++)
			if (note_buffer[s] == inNumber)
			{
				voice[s].noteOff ();
				note_buffer[s] = 0;
				break;
			}
	}

	// the 8 voices' update, then the mixer's
	void render (int32_t *out)
	{
		int16_t blocks[MDT_NUM][CTL];
		for (int i = 0; i < MDT_NUM; i++)
			voice[i].update (blocks[i]);
		if (!saturate)
		{
			// the plugin's mixer: the gained voices summed
			for (int i = 0; i < CTL; i++)
			{
				int32_t o = 0;
				for (int c = 0; c < MDT_NUM; c++)
					o += multiplier == 65536 ? blocks[c][i] : mdt_signed_multiply_32x16b (multiplier, (uint16_t) blocks[c][i]);
				out[i] = o;
			}
			return;
		}
		for (int i = 0; i < CTL; i++)
		{
			int16_t o = 0;
			for (int c = 0; c < MDT_NUM; c++)
			{
				const int16_t x = blocks[c][i];
				int16_t g;
				if (c == 0)
				{
					// receiveWritable + applyGain (when not unity)
					o = multiplier == 65536
						? x
						: (int16_t) mdt_signed_saturate_rshift (mdt_signed_multiply_32x16b (multiplier, (uint16_t) x), 16, 0);
					continue;
				}
				// applyGainThenAdd
				if (multiplier == 65536)
					g = x;
				else
					g = (int16_t) mdt_signed_saturate_rshift (mdt_signed_multiply_32x16b (multiplier, (uint16_t) x), 16, 0);
				o = mdt_sat_add16 (g, o);
			}
			out[i] = o;
		}
	}
};

#endif
