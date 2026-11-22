/*
 * resampler.h - a stereo FIR resampler for eDrumSynth: the drum engine's
 * 44.1 kHz to the host rate. Polyphase (Kaiser windowed sinc); exact for
 * ratios with up to EDS_RS_MAX_PHASES phases (44.1 -> 48 kHz: 160), else
 * with linearly interpolated phases. All memory is allocated in
 * eds_rs_init(); eds_rs_push() and eds_rs_process() are real-time safe (no
 * allocations, no libm calls).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef EDS_RESAMPLER_H_
#define EDS_RESAMPLER_H_

#include <stdint.h>

#define EDS_RS_MAX_PHASES	1024	// exact polyphase up to this many phases
#define EDS_RS_INTERP_PHASES	256	// else: interpolated between this many
#define EDS_RS_TAPS		48	// taps per phase when upsampling (more when downsampling)

typedef struct
{
	int taps;		// per phase, even
	int phases;
	int exact;		// exact rational ratio (else interpolated phases)
	float *coef;		// [phases + 1][taps]
	// position of the next output: input index (into buf, of its first
	// tap) and phase: exact: frac in 0..phases-1 (phases = L), advanced by
	// stepInt + stepRem / L; interpolated: frac a 32 bit fraction
	uint32_t frac, stepInt, stepRem, L;
	int pos;
	float *buf[2];
	int fill, cap;
} eds_rs;

// 0 on success; maxPush: the most frames one eds_rs_push() brings
int eds_rs_init (eds_rs *rs, double inRate, double outRate, int maxPush);
void eds_rs_free (eds_rs *rs);
// input frames, at most maxPush at a time
void eds_rs_push (eds_rs *rs, const float *l, const float *r, int n);
// up to n output frames, as far as the input goes: the number written
int eds_rs_process (eds_rs *rs, float *l, float *r, int n);
// latency in input frames (the filter's half length)
int eds_rs_latency (const eds_rs *rs);

#endif
