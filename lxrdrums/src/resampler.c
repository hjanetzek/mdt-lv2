/*
 * resampler.c - the stereo FIR resampler (resampler.h).
 *
 * y(t) = sum_j x[j] g(t - j), g a Kaiser windowed sinc of taps input samples
 * (cut off at the lower Nyquist frequency), evaluated per output at its
 * input time t = n * inRate / outRate. The phases (the fractional parts of
 * t) are tabulated: exactly, for a ratio L/M (outRate / inRate reduced)
 * with L <= EDS_RS_MAX_PHASES, else EDS_RS_INTERP_PHASES phases with linear
 * interpolation between them. Each phase is normalized to a gain of 1.
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define KAISER_BETA	9.0	// about 90 dB stop band
#define PI		3.14159265358979323846

static double besselI0 (double x)
{
	double sum = 1, term = 1;
	for (int k = 1; k < 64; k++)
	{
		term *= (x / (2 * k)) * (x / (2 * k));
		sum += term;
		if (term < sum * 1e-17)
		{
			break;
		}
	}
	return sum;
}

static uint64_t gcd (uint64_t a, uint64_t b)
{
	while (b)
	{
		const uint64_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

int eds_rs_init (eds_rs *rs, double inRate, double outRate, int maxPush)
{
	memset (rs, 0, sizeof *rs);
	if (!(inRate > 0) || !(outRate > 0) || maxPush <= 0)
	{
		return -1;
	}

	const double ratio = outRate / inRate;
	// cut off at the lower Nyquist frequency (cycles per input sample);
	// downsampling needs a longer filter for the same transition
	const double fc = 0.5 * (ratio < 1 ? ratio : 1);
	int taps = EDS_RS_TAPS;
	if (ratio < 1)
	{
		taps = (int) ceil (EDS_RS_TAPS / ratio);
		taps += taps & 1;
	}
	rs->taps = taps;

	// exact ratio?
	rs->exact = 0;
	if (inRate == floor (inRate) && outRate == floor (outRate) && inRate < 1e9 && outRate < 1e9)
	{
		const uint64_t g = gcd ((uint64_t) inRate, (uint64_t) outRate);
		const uint64_t L = (uint64_t) outRate / g, M = (uint64_t) inRate / g;
		if (L <= EDS_RS_MAX_PHASES)
		{
			rs->exact = 1;
			rs->L = (uint32_t) L;
			rs->phases = (int) L;
			rs->stepInt = (uint32_t) (M / L);
			rs->stepRem = (uint32_t) (M % L);
		}
	}
	if (!rs->exact)
	{
		const double step = inRate / outRate;
		rs->phases = EDS_RS_INTERP_PHASES;
		rs->stepInt = (uint32_t) floor (step);
		rs->stepRem = (uint32_t) ((step - floor (step)) * 4294967296.0);
	}

	// the phases (+1: the interpolation's upper neighbour)
	rs->coef = malloc (sizeof (float) * (size_t) (rs->phases + 1) * (size_t) taps);
	rs->cap = 2 * (taps + maxPush) + (int) rs->stepInt + 8;
	rs->buf[0] = calloc ((size_t) rs->cap, sizeof (float));
	rs->buf[1] = calloc ((size_t) rs->cap, sizeof (float));
	if (!rs->coef || !rs->buf[0] || !rs->buf[1])
	{
		eds_rs_free (rs);
		return -1;
	}
	const double half = taps / 2;
	const double i0beta = besselI0 (KAISER_BETA);
	for (int p = 0; p <= rs->phases; p++)
	{
		float *c = rs->coef + (size_t) p * (size_t) taps;
		double sum = 0;
		for (int k = 0; k < taps; k++)
		{
			// t - j for tap k: the output at phase p / phases after
			// input pos + half - 1
			const double tau = (double) p / rs->phases + half - 1 - k;
			const double x = 2 * fc * tau;
			const double sinc = x == 0 ? 1 : sin (PI * x) / (PI * x);
			const double w = tau / half;
			const double win = fabs (w) >= 1 ? 0 : besselI0 (KAISER_BETA * sqrt (1 - w * w)) / i0beta;
			const double v = 2 * fc * sinc * win;
			c[k] = (float) v;
			sum += v;
		}
		for (int k = 0; k < taps; k++)
		{
			c[k] = (float) (c[k] / sum);
		}
	}

	// the first output at input time 0: taps / 2 - 1 zeros of history
	rs->fill = taps / 2 - 1;
	rs->pos = 0;
	rs->frac = 0;
	return 0;
}

void eds_rs_free (eds_rs *rs)
{
	free (rs->coef);
	free (rs->buf[0]);
	free (rs->buf[1]);
	memset (rs, 0, sizeof *rs);
}

int eds_rs_latency (const eds_rs *rs)
{
	return rs->taps / 2;
}

double eds_rs_pending (const eds_rs *rs)
{
	const double step = rs->exact ? rs->stepInt + (double) rs->stepRem / rs->L : rs->stepInt + rs->stepRem / 4294967296.0;
	const double frac = rs->exact ? (double) rs->frac / rs->L : rs->frac / 4294967296.0;
	const double k = (rs->fill - (rs->pos + frac) - rs->taps / 2) / step;
	return k > 0 ? k : 0;
}

void eds_rs_push (eds_rs *rs, const float *l, const float *r, int n)
{
	if (rs->fill + n > rs->cap)
	{
		// keep what the next outputs read (fill - pos: about taps frames)
		const int keep = rs->fill - rs->pos;
		memmove (rs->buf[0], rs->buf[0] + rs->pos, sizeof (float) * (size_t) keep);
		memmove (rs->buf[1], rs->buf[1] + rs->pos, sizeof (float) * (size_t) keep);
		rs->fill = keep;
		rs->pos = 0;
	}
	memcpy (rs->buf[0] + rs->fill, l, sizeof (float) * (size_t) n);
	memcpy (rs->buf[1] + rs->fill, r, sizeof (float) * (size_t) n);
	rs->fill += n;
}

int eds_rs_process (eds_rs *rs, float *l, float *r, int n)
{
	const int taps = rs->taps;
	int done = 0;
	if (rs->exact)
	{
		while (done < n && rs->pos + taps <= rs->fill)
		{
			const float *restrict c = rs->coef + (size_t) rs->frac * (size_t) taps;
			const float *restrict x0 = rs->buf[0] + rs->pos;
			const float *restrict x1 = rs->buf[1] + rs->pos;
			float a = 0, b = 0;
			for (int k = 0; k < taps; k++)
			{
				a += c[k] * x0[k];
				b += c[k] * x1[k];
			}
			l[done] = a;
			r[done] = b;
			done++;

			rs->pos += (int) rs->stepInt;
			rs->frac += rs->stepRem;
			if (rs->frac >= rs->L)
			{
				rs->frac -= rs->L;
				rs->pos++;
			}
		}
	}
	else
	{
		// EDS_RS_INTERP_PHASES = 256: the phase in the top 8 bits
		_Static_assert (EDS_RS_INTERP_PHASES == 256, "phase bits");
		while (done < n && rs->pos + taps <= rs->fill)
		{
			const float *restrict c0 = rs->coef + (size_t) (rs->frac >> 24) * (size_t) taps;
			const float *restrict c1 = c0 + taps;
			const float w = (float) (rs->frac & 0xFFFFFF) * (1.f / 16777216.f);
			const float *restrict x0 = rs->buf[0] + rs->pos;
			const float *restrict x1 = rs->buf[1] + rs->pos;
			float a = 0, b = 0;
			for (int k = 0; k < taps; k++)
			{
				const float c = c0[k] + w * (c1[k] - c0[k]);
				a += c * x0[k];
				b += c * x1[k];
			}
			l[done] = a;
			r[done] = b;
			done++;

			const uint32_t f = rs->frac + rs->stepRem;
			rs->pos += (int) rs->stepInt + (f < rs->frac);
			rs->frac = f;
		}
	}
	return done;
}
