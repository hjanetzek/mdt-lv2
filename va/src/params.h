/*
 * params.h - the voice layer's parameters (mTrio and mAnalog): what a
 * control port sets. 0..9 are the engine's track parameters (track_t.p[],
 * P_E0..P_E9 in engine/synth_shared.h: their meaning depends on the engine),
 * then the wrapper's envelope, MDT's mixer and panorama, the transpose and
 * the plugin's own (pitch bend range, velocity amount).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VA_PARAMS_H_
#define VA_PARAMS_H_

enum
{
	VA_P_E0, VA_P_E1, VA_P_E2, VA_P_E3, VA_P_E4, VA_P_E5, VA_P_E6, VA_P_E7,
	VA_ENV_TO_CUT,		// P_E8_ENV_TO_CUT
	VA_ENV_TO_PW,		// P_E9_ENV_TO_PW
	VA_ATTACK, VA_DECAY, VA_SUSTAIN, VA_RELEASE,	// 0..127 (MDT's set_env)
	VA_LEVEL,		// 0..100 (MDT's sound_intensity: the mixer gain)
	VA_PAN,			// 0..40, 20 = centre (MDT's PANORAMA_MIN..MAX)
	VA_TRANSPOSE,		// -24..24 semitones
	VA_BEND_RANGE,		// 0..24 semitones (LV2: not in MDT)
	VA_VELOCITY,		// 0..100 % velocity to level and cutoff (LV2: not in MDT)
	VA_NUM_PARAMS
};

#endif
