/* dsp_basics.cpp — shared DSP implementations. */

#include "dsp_basics.h"
#include <math.h>
#include <string.h>
#include "config.h"

// int32_t SINE[SINE_N + 1];
// int32_t SVF_G[SVF_G_N + 1];
// int16_t TANH_Q15[TANH_N + 1];
// static int32_t POW2_16[192];
// int32_t CUT_PARAM_TO_IDX[128];

int32_t SINE[SINE_N + 1]       DMAMEM;
int32_t SVF_G[SVF_G_N + 1]     DMAMEM;
int16_t TANH_Q15[TANH_N + 1]   DMAMEM;
static int32_t POW2_16[192]    DMAMEM;
int32_t CUT_PARAM_TO_IDX[128]  DMAMEM;

void dsp_basics_init(void) {
    for (int i = 0; i <= SINE_N; i++)
        SINE[i] = (int32_t)(sin(2.0 * M_PI * i / (double)SINE_N) * 32767.0 + 0.5);

    for (int i = 0; i <= SVF_G_N; i++) {
        double t  = (double)i / (double)SVF_G_N;
        double fc = 30.0 * pow(16000.0 / 30.0, t);
        double g  = tan(M_PI * fc / 44100.0);
        SVF_G[i]  = (int32_t)(g * 4096.0 + 0.5);
    }

    for (int i = 0; i <= TANH_N; i++) {
        double u = (double)i * 128.0 / 32768.0;
        TANH_Q15[i] = (int16_t)(tanh(u) * 32767.0 + 0.5);
    }

    for (int i = 0; i < 192; i++)
        POW2_16[i] = (int32_t)(pow(2.0, i / 192.0) * 65536.0 + 0.5);

    cutoff_map_init();
}

/* Bass-weighted cutoff parameter map.
 *
 *   param   0 → 30 Hz     (fully closed)
 *   param  32 → 44 Hz     (bass detail)
 *   param  64 → 145 Hz    (low-mid)
 *   param  96 → 1 kHz     (mid)
 *   param 127 → 16 kHz    (fully open, still reachable)
 *
 * The curve is param^EXP; EXP = 1.0 gives the previous linear behaviour,
 * EXP = 2.0 pushes more of the knob's resolution into the bass end.
 * CUT_MAX_IDX caps the top of the reachable range: 127 = 16 kHz,
 * 100 ≈ 4 kHz, 90 ≈ 2 kHz. */


void cutoff_map_init(void) {
    const double CUT_CURVE_EXP = 2.0;
    const double CUT_MAX_IDX   = 127.0;

    for (int i = 0; i < 128; i++) {
        double n = (double)i / 127.0;
        double m = pow(n, CUT_CURVE_EXP) * CUT_MAX_IDX;
        int32_t idx = (int32_t)(m + 0.5);
        if (idx < 0)   idx = 0;
        if (idx > 127) idx = 127;
        CUT_PARAM_TO_IDX[i] = idx;
    }
}

uint32_t pitch_inc(int32_t p16) {
    int32_t e   = p16 - 1104;
    int32_t oct = e >= 0 ? e / 192 : (e - 191) / 192;
    int32_t rem = e - oct * 192;
    uint64_t base = 440ULL * 4294967296ULL / 44100ULL;
    uint64_t inc  = (base * (uint64_t)POW2_16[rem]) >> 16;
    if (oct >= 0) return (uint32_t)(inc << oct);
    return (uint32_t)(inc >> (-oct));
}

uint32_t cents_inc(int32_t pitch16, int32_t ct, int32_t fine) {
    int32_t d16 = ct * 16 / 100, rem = ct * 16 - d16 * 100;
    uint32_t inc = pitch_inc(clamp(pitch16 + d16, 0, 2047));
    return inc + (uint32_t)((int32_t)(inc >> 12) * (rem * 2367 / 16000 + fine));
}

void tsvf_coef_k(tsvf_t *c, int32_t cut, int32_t k) {
    int32_t i, g, den;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = SVF_G[i];
    if (i < 127)
        g += ((SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    den = 4096 + ((g * (g + k)) >> 12);
    c->a1 = (int32_t)((4096u << 13) / (uint32_t)den);
    c->a2 = (c->a1 * g) >> 12;
    c->a3 = (c->a2 * g) >> 12;
}

void tsvf_coef(tsvf_t *c, int32_t cut, int32_t reso) {
    tsvf_coef_k(c, cut, 8192 - reso * 7600 / 127);
}