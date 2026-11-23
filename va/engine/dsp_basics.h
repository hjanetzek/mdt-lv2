/* dsp_basics.h — shared DSP primitives, exact-match to Felucca dsp.c.
 * License: GPL-3.0-only. */

#pragma once
#include <stdint.h>

/* Bass-weighted parameter → filter-index map for Trio / Analog cutoff knobs.
 * Because the SVF_G table is 30 Hz .. 16 kHz log-distributed, a linear
 * parameter mapping wastes the top half of the knob (everything above
 * ~2 kHz already sounds "open"). This table applies a ^2 curve so the
 * lower half of the knob covers 30..145 Hz (bass detail), and 4 kHz+
 * only arrives at the very top.
 * Change CUT_CURVE_EXP / CUT_MAX_IDX in dsp_basics.cpp to taste. */
extern int32_t CUT_PARAM_TO_IDX[128];

#define SINE_BITS 10
#define SINE_N    (1 << SINE_BITS)
extern int32_t SINE[SINE_N + 1];

#define SVF_G_N   128
extern int32_t SVF_G[SVF_G_N + 1];

/* Felucca's tanh table is 257 entries (0..256 inclusive); index step is >> 8 */
#define TANH_N    256
extern int16_t TANH_Q15[TANH_N + 1];

/* Per-voice level: a full-scale oscillator at amp=1.0 contributes VOICE_FS.
 * 24000 = one voice peaks near -6 dBFS before the master mixer. */
#define VOICE_FS  24000

void dsp_basics_init(void);

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* a * b / 32768 without 64-bit overflow (a in ±2^31, b Q15) */
static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }

/* a * k / 65536 without 64-bit overflow (a up to 2^31, k Q16) */
static inline int32_t mulq16(int32_t a, uint32_t k) {
    return (int32_t)(((a >> 16) * (int32_t)k) +
                     (int32_t)(((uint32_t)(a & 0xFFFF) * k) >> 16));
}

/* Sine with linear interpolation between the 1024 table points */
static inline int32_t sine_i(uint32_t ph) {
    uint32_t i = ph >> 22;
    int32_t a = SINE[i], b = SINE[(i + 1u) & 1023u];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}
static inline int32_t osc_sine(uint32_t ph) { return sine_i(ph); }

/* polyBLEP residual (Q15) around a wrap of a phase accumulator (Felucca dsp.c) */
static inline int32_t blep(uint32_t ph, uint32_t inc) {
    uint32_t d = inc >> 15;
    int32_t x;
    if (!d)
        return 0;
    if (ph < inc) {
        x = (int32_t)(ph / d);                        /* 0..32767 */
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (ph > 0xFFFFFFFFu - inc) {
        x = -(int32_t)((0xFFFFFFFFu - ph) / d);       /* -32767..0 */
        return ((x * x) >> 15) + x + x + 32768;
    }
    return 0;
}

static inline int32_t osc_saw(uint32_t ph, uint32_t inc) {
    return ((int32_t)(ph >> 16) - 32768) - blep(ph, inc);
}

static inline int32_t osc_pulse(uint32_t ph, uint32_t inc, uint32_t pw) {
    return (osc_saw(ph, inc) - osc_saw(ph + pw, inc)) >> 1;
}

static inline int32_t osc_tri(uint32_t ph) {
    int32_t s = (int32_t)(ph >> 15);                  /* 0..131071 */
    return s < 65536 ? s - 32768 : 98303 - s;
}

/* tanh(x/32768)*32767, linearly interpolated; table only covers tanh(0..1) */
static inline int32_t softclip(int32_t x) {
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];                            /* continuous with the table */
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

/* Linear up to k, then a soft knee: only peaks above k saturate */
static inline int32_t soft_knee(int32_t y, int32_t k) {
    int32_t a = y < 0 ? -y : y;
    if (a <= k)
        return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}

/* xorshift32 */
static inline uint32_t noise32(int32_t *state) {
    uint32_t x = (uint32_t)*state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = (int32_t)x;
    return x;
}

/* Trapezoidal SVF (Simper), unconditionally stable */
typedef struct { int32_t a1, a2, a3; } tsvf_t;

void tsvf_coef_k(tsvf_t *c, int32_t cut, int32_t k);   /* k: damping Q12 */
void tsvf_coef  (tsvf_t *c, int32_t cut, int32_t reso);

/* Felucca returns v2, NOT v1 (v1 is band-pass, v2 is low-pass).
 * Both integrators clamp to ±150000. */
static inline int32_t tsvf_lp(const tsvf_t *c, int32_t in,
                              int32_t *ic1, int32_t *ic2) {
    int32_t v3 = in - *ic2;
    int32_t v1 = (c->a1 * *ic1 + c->a2 * v3) >> 13;
    int32_t v2 = *ic2 + ((c->a2 * *ic1 + c->a3 * v3) >> 13);
    *ic1 = clamp(2 * v1 - *ic1, -150000, 150000);
    *ic2 = clamp(2 * v2 - *ic2, -150000, 150000);
    return v2;
}

/* Per-sample amplitude from vmod_t; requires vmod_t from synth_shared.h */
#include "synth_shared.h"

static inline int32_t amp_at(const vmod_t *m, uint32_t i) {
    return m->amp0 + ((m->amp1 - m->amp0) * (int32_t)i) / (int32_t)CTL;
}

/* A voice's sample s (Q15) at sample i, scaled to VOICE_FS */
static inline int32_t voice_amp(int32_t s, const vmod_t *m, uint32_t i) {
    return mulq15(mulq15(s, amp_at(m, i)), VOICE_FS);
}

uint32_t pitch_inc (int32_t p16);
uint32_t cents_inc (int32_t p16, int32_t cents, int32_t fine);

void cutoff_map_init(void);
