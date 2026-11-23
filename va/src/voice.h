/*
 * voice.h - the voice layer of mTrio and mAnalog: eight voices of one
 * engine (engine/trio_engine.cpp or analog_engine.cpp), MDT's voice
 * allocation, envelope, block update and mixer, at the host's sample rate,
 * plus pitch bend and velocity (not in MDT).
 *
 * What it follows in MicroDexed-touch (ca6518a8, engine/ORIGIN.md):
 * - the per-voice envelope and block update of the AudioStream wrappers
 *   (synth_trio.cpp, synth_analog.cpp: set_env, env_step_block, noteOn,
 *   noteOff, update), with the sample rate a parameter (MDT: 44100 and 44.1f);
 * - the voice allocation of handleNoteOn / handleNoteOff
 *   (MicroDexed-touch.ino 5052-5083, 5455-5463): the voice holding the same
 *   note, else a free idle voice, else a free one still releasing, else the
 *   next one round robin (stolen without a release);
 * - the mixer of the 8 voices (trio.cpp / analog.cpp: gain
 *   mapfloat (level, 0, 100, 0, 3.5 resp. 2.5) / sqrt (8); template_mixer.hpp:
 *   the Q16 multiplier), but without its int16 saturation (LV2: the sum
 *   may exceed full scale, the host's volume follows; MDT clips chords at
 *   level 100), and the panorama
 *   (effect_stereo_panorama.cpp: one side at full level, the other
 *   attenuated by 1 - |pan|).
 * LV2 additions: pitch bend (all voices, +-range semitones, in 1/16
 * semitone steps of the engine's pitch plus a fine ratio), velocity amount
 * (velocity to level and cutoff; 0: MDT's sound), MIDI note 0 is a normal
 * note (MDT's note buffer uses 0 for "free"), all notes / sound off.
 *
 * Everything per block is integer except the output scaling; no
 * allocations, no locks, no libm calls (sqrtf only when the level port
 * changes).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VA_VOICE_H_
#define VA_VOICE_H_

#include "dsp_basics.h"
#include "params.h"
#include "synth_shared.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define VA_VOICES	8		// MDT's NUM_TRIO, NUM_ANALOG
#define VA_BLOCK	CTL		// 128 frames, the engines' block (AUDIO_BLOCK_SAMPLES)

// what differs between the engines
typedef struct
{
	int analog;
	void (*note_on) (track_t *, voice_t *);
	void (*render) (track_t *, voice_t *, int32_t *, uint32_t, const vmod_t *);
	uint32_t phase[3];	// the wrappers' start phases
	float gainMax;		// the mixer gain at level 100 before / sqrt (8)
} va_engine;

enum
{
	VA_ENV_OFF, VA_ENV_ATTACK, VA_ENV_DECAY, VA_ENV_SUSTAIN, VA_ENV_RELEASE
};

typedef struct
{
	track_t track;		// the engine's state (one track per voice, as MDT)
	uint8_t envStage;
	int32_t envLevel;	// Q16.16, 0..32767 << 16
	int32_t envAttInc, envDecInc, envRelInc, envSusLvl;
	uint8_t key;		// the MIDI note it plays (before transpose)
	uint8_t held;		// key down (MDT: note_buffer[slot] != 0)
	int32_t velGain;	// Q15, 32768: 1
	int32_t velCut;		// cutoff offset (filter index << 8)
} va_voice;

typedef struct
{
	const va_engine *engine;
	double rate;
	int rateInt;
	float msRate;			// samples per millisecond
	va_voice voice[VA_VOICES];
	uint8_t slot;			// MDT's trio_slot / analog_slot (stealing)
	int value[VA_NUM_PARAMS];	// the parameters as set
	int32_t mult;			// mixer gain, Q16
	float gain[2];			// panorama: left, right
	int bend;			// pitch bend -8192..8191
	int32_t bendQ;			// in 1/16 semitones, Q12
} va_synth;

// (2^(f / 256 / 192) - 1) * 2^32: the fraction of a 1/16 semitone step
static uint32_t va_bendFrac[256];

// ---- the envelope (synth_trio.cpp / synth_analog.cpp) ----

#define VA_ENV_MAX	(32767 << 16)

static inline void va_setEnv (va_synth *s, va_voice *v)
{
	const int atk = s->value[VA_ATTACK], dec = s->value[VA_DECAY];
	const int sus = s->value[VA_SUSTAIN], rel = s->value[VA_RELEASE];
	const int32_t ENV_MAX = VA_ENV_MAX;

	// attack = atk * 4 ms, decay = (dec / 127)^2 * 3048 ms, release =
	// (rel / 127)^3 * 13000 ms
	const float DECAY_MS = 3048.0f;
	const float RELEASE_MS = 13000.0f;

	int64_t sus_q16 = (int64_t) sus * ENV_MAX / 50;
	if (sus_q16 > ENV_MAX)
		sus_q16 = ENV_MAX;
	v->envSusLvl = (int32_t) sus_q16;

	// LV2: the host rate (MDT: 44100 and 44.1f)
	const int32_t a_samp = ((int32_t) atk * 4 * s->rateInt) / 1000 + 1;

	const float dec_norm = (float) dec / 127.0f;
	const int32_t d_samp = (int32_t) (dec_norm * dec_norm * DECAY_MS * s->msRate) + 1;

	const float rel_norm = (float) rel / 127.0f;
	const int32_t r_samp = (int32_t) (rel_norm * rel_norm * rel_norm * RELEASE_MS * s->msRate) + 1;

	v->envAttInc = ENV_MAX / a_samp;
	if (v->envAttInc < 1)
		v->envAttInc = 1;
	v->envDecInc = (ENV_MAX - v->envSusLvl) / d_samp;
	if (v->envDecInc < 1)
		v->envDecInc = 1;
	v->envRelInc = ENV_MAX / r_samp;
	if (v->envRelInc < 1)
		v->envRelInc = 1;
}

// n samples of the envelope; its level at the end, Q15
static inline int32_t va_envStep (va_voice *v, uint32_t n)
{
	const int32_t ENV_MAX = VA_ENV_MAX;
	int32_t level = v->envLevel;
	uint8_t stage = v->envStage;
	for (uint32_t i = 0; i < n; i++)
	{
		switch (stage)
		{
		case VA_ENV_ATTACK:
			if (v->envAttInc >= ENV_MAX - level)
			{
				level = ENV_MAX;
				stage = VA_ENV_DECAY;
			}
			else
			{
				level += v->envAttInc;
			}
			break;
		case VA_ENV_DECAY:
			if (v->envDecInc >= level - v->envSusLvl)
			{
				level = v->envSusLvl;
				stage = VA_ENV_SUSTAIN;
			}
			else
			{
				level -= v->envDecInc;
			}
			break;
		case VA_ENV_SUSTAIN:
			level = v->envSusLvl;
			break;
		case VA_ENV_RELEASE:
			if (v->envRelInc >= level)
			{
				level = 0;
				stage = VA_ENV_OFF;
			}
			else
			{
				level -= v->envRelInc;
			}
			break;
		default:
			level = 0;
			break;
		}
	}
	v->envLevel = level;
	v->envStage = stage;
	return level >> 16;
}

// ---- parameters ----

static inline void va_applyGain (va_synth *s)
{
	// trio.cpp / analog.cpp: mapfloat (level, 0, 100, 0, max) / sqrt (8);
	// template_mixer.hpp: gain * 65536 (truncated)
	const float norm = sqrtf ((float) VA_VOICES);
	const float level = (float) s->value[VA_LEVEL];
	float gain = (level - 0.f) * (s->engine->gainMax - 0.0f) / (100.f - 0.f) + 0.0f;
	gain /= norm;
	if (gain > 32767.0f)
		gain = 32767.0f;
	s->mult = (int32_t) (gain * 65536.0f);
}

static inline void va_applyPan (va_synth *s)
{
	// mapfloat (pan, PANORAMA_MIN, PANORAMA_MAX, -1, 1); MDT's panorama
	// output 0 (to master_mixer_r, which feeds I2S channel 0) is the left
	const float pan = ((float) s->value[VA_PAN] - 0.f) * (1.0f - -1.0f) / (40.f - 0.f) + -1.0f;
	s->gain[0] = pan > 0.f ? 1.f - pan : 1.f;
	s->gain[1] = pan < 0.f ? 1.f + pan : 1.f;
}

static inline void va_applyBend (va_synth *s)
{
	// bend / 8192 * range semitones, in 1/16 semitones Q12
	s->bendQ = (int32_t) s->bend * s->value[VA_BEND_RANGE] * 8;
}

static inline void va_set (va_synth *s, int param, int value)
{
	s->value[param] = value;
	if (param <= VA_ENV_TO_PW)
	{
		for (int i = 0; i < VA_VOICES; i++)
		{
			s->voice[i].track.p[param] = (int16_t) value;
		}
	}
	else if (param <= VA_RELEASE)
	{
		for (int i = 0; i < VA_VOICES; i++)
		{
			va_setEnv (s, &s->voice[i]);
		}
	}
	else if (param == VA_LEVEL)
	{
		va_applyGain (s);
	}
	else if (param == VA_PAN)
	{
		va_applyPan (s);
	}
	else if (param == VA_BEND_RANGE)
	{
		va_applyBend (s);
	}
}

// ---- setup ----

// the engine's tables (dsp_basics_init) are global: built by the caller
static inline void va_init (va_synth *s, const va_engine *engine, double rate)
{
	memset (s, 0, sizeof *s);
	s->engine = engine;
	s->rate = rate;
	s->rateInt = (int) (rate + 0.5);
	s->msRate = (float) (rate / 1000.0);
	for (int i = 0; i < VA_VOICES; i++)
	{
		voice_t *v = &s->voice[i].track.v[0];
		v->ph[0] = engine->phase[0];
		v->ph[1] = engine->phase[1];
		v->ph[2] = engine->phase[2];
		s->voice[i].velGain = 32768;
	}
	for (int f = 0; f < 256; f++)
	{
		va_bendFrac[f] = (uint32_t) ((pow (2.0, f / 256.0 / 192.0) - 1.0) * 4294967296.0);
	}
}

// ---- notes ----

static inline void va_noteOn (va_synth *s, uint8_t key, uint8_t velocity)
{
	int slot = -1;
	// pass 0: the voice already playing this note
	for (int i = 0; i < VA_VOICES && slot < 0; i++)
	{
		if (s->voice[i].held && s->voice[i].key == key)
			slot = i;
	}
	// pass 1: free and fully idle
	for (int i = 0; i < VA_VOICES && slot < 0; i++)
	{
		if (!s->voice[i].held && s->voice[i].envStage == VA_ENV_OFF)
			slot = i;
	}
	// pass 2: free, still releasing
	for (int i = 0; i < VA_VOICES && slot < 0; i++)
	{
		if (!s->voice[i].held)
			slot = i;
	}
	// pass 3: all held, round robin steal
	if (slot < 0)
	{
		if (++s->slot >= VA_VOICES)
			s->slot = 0;
		slot = s->slot;
	}

	va_voice *v = &s->voice[slot];
	int note = (int) key + s->value[VA_TRANSPOSE];
	note = note < 0 ? 0 : note > 127 ? 127 : note;
	// the wrapper's noteOn
	v->track.v[0].note = (uint8_t) note;
	v->track.v[0].active = 1;
	v->track.v[0].age += 1;
	s->engine->note_on (&v->track, &v->track.v[0]);
	v->envStage = VA_ENV_ATTACK;
	v->key = key;
	v->held = 1;

	// LV2: velocity to level ((velocity / 127)^2) and cutoff (down to 32
	// filter steps, about 2.3 octaves, closed at velocity 0), by amount;
	// velocity 127 or amount 0: MDT's sound
	const int32_t amount = s->value[VA_VELOCITY];
	const int32_t curve = (int32_t) velocity * velocity * 32768 / (127 * 127);
	v->velGain = 32768 - (int32_t) ((int64_t) amount * (32768 - curve) / 100);
	v->velCut = -(amount * (127 - velocity) * (32 << 8)) / (100 * 127);
}

static inline void va_noteOff (va_synth *s, uint8_t key)
{
	for (int i = 0; i < VA_VOICES; i++)
	{
		va_voice *v = &s->voice[i];
		if (v->held && v->key == key)
		{
			v->track.v[0].active = 0;
			v->envStage = VA_ENV_RELEASE;
			v->held = 0;
			break;
		}
	}
}

// all notes off: every held voice released
static inline void va_allNotesOff (va_synth *s)
{
	for (int i = 0; i < VA_VOICES; i++)
	{
		if (s->voice[i].held)
		{
			va_noteOff (s, s->voice[i].key);
		}
	}
}

// all sound off: every voice silent and idle at once
static inline void va_allSoundOff (va_synth *s)
{
	for (int i = 0; i < VA_VOICES; i++)
	{
		va_voice *v = &s->voice[i];
		v->held = 0;
		v->track.v[0].active = 0;
		v->envStage = VA_ENV_OFF;
		v->envLevel = 0;
	}
}

static inline void va_pitchBend (va_synth *s, int bend)
{
	s->bend = bend;
	va_applyBend (s);
}

// ---- audio ----

// the voice wrapper's int16 output
static inline int32_t va_sat16 (int32_t x)
{
	return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

// one block of the 8 voices mixed: MDT's mixer output, unsaturated (16 bit
// full scale = 32768)
static inline void va_render (va_synth *s, int32_t *mix)
{
	int32_t acc[VA_BLOCK];
	memset (mix, 0, sizeof (int32_t) * VA_BLOCK);
	const int analog = s->engine->analog;

	for (int k = 0; k < VA_VOICES; k++)
	{
		va_voice *vc = &s->voice[k];
		track_t *t = &vc->track;
		voice_t *v = &t->v[0];

		// the wrapper's update (): an idle voice resets its filter (and
		// Trio its previous sample and drift) and adds silence
		if (!v->active && vc->envStage == VA_ENV_OFF)
		{
			v->s[0] = 0;
			v->s[1] = 0;
			if (!analog)
			{
				v->s[4] = 0;
				v->s[5] = 0;
			}
			vc->envLevel = 0;
			continue;
		}

		const int32_t env_start = vc->envLevel >> 16;
		const int32_t env_end = va_envStep (vc, VA_BLOCK);

		const int32_t cut_amt = (int32_t) t->p[P_E8_ENV_TO_CUT] << 8;
		const int32_t pw_amt = (int32_t) t->p[P_E9_ENV_TO_PW] << 8;
		const int32_t cut_start = mulq15 (env_start, cut_amt);
		const int32_t pw_mod = mulq15 (env_start, pw_amt);

		vmod_t m;
		memset (&m, 0, sizeof m);
		m.pitch16 = ((int32_t) v->note) << 4;
		m.inc = pitch_inc (m.pitch16);
		m.amp0 = env_start;
		m.amp1 = env_end;
		m.cutoff = cut_start;
		m.shape = (64 << 8) + pw_mod;
		m.envq15 = env_end;

		// LV2: pitch bend: whole 1/16 semitones in the engine's pitch, the
		// rest as a ratio of the increment (the engines derive their
		// other oscillators' fine offset from it)
		if (s->bendQ)
		{
			int32_t p16 = m.pitch16 + (s->bendQ >> 12);
			p16 = p16 < 0 ? 0 : p16 > 2047 ? 2047 : p16;
			m.pitch16 = p16;
			m.inc = pitch_inc (p16);
			const uint32_t frac = va_bendFrac[(s->bendQ & 4095) >> 4];
			m.inc += (uint32_t) (((uint64_t) m.inc * frac) >> 32);
		}
		// LV2: velocity
		if (vc->velGain != 32768)
		{
			m.amp0 = (int32_t) (((int64_t) m.amp0 * vc->velGain) >> 15);
			m.amp1 = (int32_t) (((int64_t) m.amp1 * vc->velGain) >> 15);
		}
		m.cutoff += vc->velCut;

		memset (acc, 0, sizeof acc);
		s->engine->render (t, v, acc, VA_BLOCK, &m);

		// the voice's int16 block (the wrapper) into the mixer:
		// template_mixer.hpp's (mult * x) >> 16, LV2: not saturated
		const int32_t mult = s->mult;
		for (int i = 0; i < VA_BLOCK; i++)
		{
			const int32_t x = va_sat16 (acc[i]);
			mix[i] += mult == 65536 ? x : (int32_t) (((int64_t) mult * x) >> 16);
		}
	}
}

#endif
