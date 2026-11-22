/*
 * resampler_test.c - sines through the resampler (src/resampler.c): the
 * output is the same sine at the output rate (fitted: its amplitude, phase
 * and the rest), the rest (aliasing, images, noise) below -80 dB; at
 * 44.1 -> 48 kHz (exact polyphase), 47999 Hz (interpolated phases) and
 * down to 32 kHz. Pushes in 64-frame blocks as the plugin does.
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "resampler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define PI 3.14159265358979323846

static int failures;

// the sine through the resampler; the fit over the output after the
// filter's start (1 s): gain (dB), phase error (output samples), rest (dB
// below the sine)
static void run (double inRate, double outRate, double freq, double minRestDb, double maxGainDb)
{
	eds_rs rs;
	if (eds_rs_init (&rs, inRate, outRate, 64))
	{
		printf ("FAIL: no resampler for %.0f -> %.0f\n", inRate, outRate);
		failures++;
		return;
	}
	const int outFrames = (int) outRate;	// 1 s
	float *l = malloc (sizeof (float) * (size_t) outFrames), *r = malloc (sizeof (float) * (size_t) outFrames);
	long in = 0;
	int done = 0;
	const double amp = 0.5;
	while (done < outFrames)
	{
		done += eds_rs_process (&rs, l + done, r + done, outFrames - done);
		if (done < outFrames)
		{
			float bl[64], br[64];
			for (int i = 0; i < 64; i++, in++)
			{
				bl[i] = (float) (amp * sin (2 * PI * freq * (double) in / inRate));
				br[i] = -bl[i];
			}
			eds_rs_push (&rs, bl, br, 64);
		}
	}

	// least squares fit of a sin + b cos at freq, from 10 ms on
	const int from = (int) (0.01 * outRate);
	double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
	for (int n = from; n < outFrames; n++)
	{
		const double s = sin (2 * PI * freq * n / outRate), c = cos (2 * PI * freq * n / outRate);
		ss += s * s;
		sc += s * c;
		cc += c * c;
		ys += l[n] * s;
		yc += l[n] * c;
	}
	const double det = ss * cc - sc * sc;
	const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
	double rest = 0, mirror = 0;
	for (int n = from; n < outFrames; n++)
	{
		const double fit = a * sin (2 * PI * freq * n / outRate) + b * cos (2 * PI * freq * n / outRate);
		rest += (l[n] - fit) * (l[n] - fit);
		mirror += (l[n] + r[n]) * (l[n] + r[n]);	// right is -left
	}
	const double gainDb = 20 * log10 (sqrt (a * a + b * b) / amp);
	const double phase = atan2 (b, a) / (2 * PI * freq) * outRate;	// output samples
	const double restDb = 10 * log10 (rest / (outFrames - from) / (0.5 * (a * a + b * b)));
	printf ("%6.0f -> %6.0f Hz, %5.0f Hz sine: gain %+.4f dB, phase %+.4f samples, rest %.1f dB, %d taps, %s\n",
		inRate, outRate, freq, gainDb, phase, restDb, rs.taps, rs.exact ? "exact" : "interpolated");
	if (restDb > -minRestDb || fabs (gainDb) > maxGainDb || fabs (phase) > 0.01 || mirror > 1e-6)
	{
		printf ("FAIL: %.0f -> %.0f Hz at %.0f Hz\n", inRate, outRate, freq);
		failures++;
	}
	eds_rs_free (&rs);
	free (l);
	free (r);
}

int main (void)
{
	run (44100, 48000, 1000, 80, 0.01);
	run (44100, 48000, 100, 80, 0.01);
	run (44100, 48000, 10000, 80, 0.01);
	run (44100, 48000, 16000, 70, 0.1);
	run (44100, 47999, 1000, 80, 0.01);
	run (44100, 96000, 1000, 80, 0.01);
	run (44100, 32000, 1000, 80, 0.01);
	run (44100, 44100, 1000, 80, 0.01);
	printf ("%s\n", failures ? "FAILED" : "all resampler checks passed");
	return failures ? 1 : 0;
}
