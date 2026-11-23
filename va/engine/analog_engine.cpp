/* analog_engine.cpp — ANALOG DSP.
 * License: GPL-3.0-only. */

#include "analog_engine.h"
#include "dsp_basics.h"

const char *const N_ANALOG_WAVE[5] = {"SAW", "SQR", "TRI", "SIN", "PWM"};

void analog_note_on(track_t *t, voice_t *v)
{
    (void)t;
    /* Same policy as TRIO: do not reset filter or ph[]; only seed noise once. */
    if (!v->s[2])
        v->s[2] = 0x1234567 + (int32_t)v->age;
}

void analog_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t wave = (uint32_t)p[P_E0], i;
    int32_t det = p[P_E1], mix = p[P_E2], noise = p[P_E3];

    /* Bass-weighted cutoff: parameter → filter index, then add envelope
     * modulation and key-tracking (KTR) on top. */
    int32_t cut_base = CUT_PARAM_TO_IDX[clamp(p[P_E4], 0, 127)] << 8;
    int32_t cut = cut_base + m->cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);

    tsvf_t flt;
    int32_t drive = 32768 + p[P_E6] * 512;                  /* 1x .. 3x */
    uint32_t inc1 = m->inc;
    /* LV2: a fine offset of m->inc from the note's pitch (pitch bend
     * between 1/16 semitone steps) also for the detuned oscillator, as
     * trio_render() does; 0 without bend (MDT: cents_inc(..., det, 0)) */
    uint32_t base = pitch_inc(m->pitch16);
    int32_t fine = (base >> 12) ? (int32_t)(m->inc - base) / (int32_t)(base >> 12) : 0;
    uint32_t inc2 = det ? cents_inc(m->pitch16, det, fine) : inc1;

    /* Pulse width (PWM wave, #4): 50 % base + envelope sweep.
     * m.shape contains (64 << 8) + modulation from the wrapper. Adding
     * (m.shape - 64 << 8) to the 50 % reference and shifting into the
     * 32-bit phase domain gives the correct duty cycle. */
    uint32_t pw = 0x80000000u + (uint32_t)((m->shape - (64 << 8)) << 15);

    int32_t m2 = mix * 258, m1 = 32767 - m2;                /* osc mix Q15 */
    int32_t nz = noise * 200, drv = p[P_E6];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1];
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];

    tsvf_coef(&flt, cut, p[P_E5]);                          /* dsp.c: damping 8192 - reso*7600/127 */

    for (i = 0; i < n; i++) {
        int32_t a, b, s;
        switch (wave) {
        case 1:  a = osc_pulse(ph0, inc1, 0x80000000u); b = osc_pulse(ph1, inc2, 0x80000000u); break;
        case 2:  a = osc_tri(ph0);                      b = osc_tri(ph1);                      break;
        case 3:  a = osc_sine(ph0);                     b = osc_sine(ph1);                     break;
        case 4:  a = osc_pulse(ph0, inc1, pw);          b = osc_pulse(ph1, inc2, pw);          break;
        default: a = osc_saw(ph0, inc1);                b = osc_saw(ph1, inc2);                break;
        }
        ph0 += inc1;
        ph1 += inc2;
        s = mulq15(a, m1) + mulq15(b, m2);
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        {
            /* filter, linear up to half scale, then a soft knee */
            int32_t y = tsvf_lp(&flt, s >> 1, &ic1, &ic2), aa = y < 0 ? -y : y;
            if (aa > 16000) {
                aa = 16000 + (softclip((aa - 16000) * 2) >> 1);
                y = y < 0 ? -aa : aa;
            }
            s = y << 1;
        }
        out[i] += voice_amp(s, m, i);
    }

    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
}

const preset_t ANALOG_PRESETS[] = {
    {"SAW LEAD", {0, 12, 64, 0, 90, 30, 10, 64}, {4, 70, 100, 50}, 20, 1, FX(0, 10, 45, 30), PAT(4)},
    {"SOFT PAD", {0, 20, 64, 4, 60, 10, 0, 32},  {80, 90, 110, 95}, 10, 0, FX(0, 60, 20, 70), PAT(5)},
    {"SQR BASS", {1, 0, 0, 0, 50, 70, 40, 64},    {0, 60, 40, 30}, 30, 1, FX(5, 0, 10, 10), PAT(2)},
    {"PWM STR",  {4, 8, 40, 0, 75, 20, 0, 48},    {60, 80, 110, 85}, 8, 0, FX(0, 50, 20, 60), PAT(5)},
    {"ACID",     {0, 0, 0, 0, 50, 100, 25, 64},   {0, 55, 20, 30}, 48, 1, FX(20, 0, 45, 15), PAT(1)},
    {"SINE KEY", {3, 6, 50, 0, 127, 0, 0, 0},     {2, 80, 30, 70}, 0, 0, FX(0, 30, 25, 40), PAT(6)},
    {"RAVE",     {4, 30, 64, 0, 85, 20, 30, 64},  {20, 80, 110, 60}, 10, 1, FX(30, 40, 30, 30), PAT(13)},
    {"SUB BASS", {3, 0, 0, 0, 40, 0, 20, 0},      {0, 60, 100, 20}, 0, 1, FX(0, 0, 0, 10), PAT(8)},
    {"PLUCK",    {0, 8, 50, 0, 30, 40, 0, 64},    {0, 88, 0, 60}, 55, 0, FX(0, 20, 50, 30), PAT(3)},
    {"BRASS",    {0, 10, 64, 0, 45, 20, 10, 64},  {35, 70, 90, 45}, 40, 0, FX(0, 20, 20, 40), PAT(6)},
    {"WIND",     {0, 0, 0, 90, 30, 90, 0, 0},     {60, 90, 60, 80}, 50, 0, FX(0, 30, 30, 70), PAT(5)},
    {"STRINGS",  {0, 25, 64, 0, 70, 10, 0, 32},   {70, 90, 115, 90}, 5, 0, FX(0, 60, 20, 70), PAT(5)},
};
const uint8_t ANALOG_NPRESETS = NELEM(ANALOG_PRESETS);