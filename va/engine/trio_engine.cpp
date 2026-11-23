/* trio_engine.cpp — TRIO DSP.
 * License: GPL-3.0-only. */

#include "trio_engine.h"
#include "dsp_basics.h"

const char *const N_TRIO_WAVE[16] = {
    "SAW3","PLS3","PPT","SST","TRI3","S&T","P&S","P+N",
    "NOIS","SYNC","SYNCP","SYNC3","RING","RING3","R+S","R+SP",
};
const char *const N_TRIO_MODE[4] = {"LP","BP","HP","NOT"};

const trio_set_t TRIO_SETS[16] = {
    {{TW_SAW, TW_SAW, TW_SAW}, 0, {TRIO_G3, TRIO_G3, TRIO_G3}},
    {{TW_PLS, TW_PLS, TW_PLS}, 0, {TRIO_G3, TRIO_G3, TRIO_G3}},
    {{TW_PLS, TW_PLS, TW_TRI}, 0, {TRIO_G3, TRIO_G3, TRIO_G3 + 3000}},
    {{TW_SAW, TW_SAW, TW_TRI}, 0, {TRIO_G3, TRIO_G3, TRIO_G3 + 3000}},
    {{TW_TRI, TW_TRI, TW_TRI}, 0, {TRIO_G3 + 2000, TRIO_G3 + 2000, TRIO_G3 + 2000}},
    {{TW_ST,  TW_ST,  TW_TRI}, 0, {TRIO_G3 + 3000, TRIO_G3 + 3000, TRIO_G3}},
    {{TW_PS,  TW_PS,  TW_TRI}, 0, {TRIO_G3 + 3000, TRIO_G3 + 3000, TRIO_G3}},
    {{TW_PLS, TW_OFF, TW_NOI}, 0, {TRIO_G2, 0, TRIO_G2 - 4000}},
    {{TW_NOI, TW_OFF, TW_OFF}, 0, {TRIO_G1, 0, 0}},
    {{TW_SAW, TW_SAW, TW_OFF}, TRIO_SYNC, {TRIO_G2 - 5000, TRIO_G2 + 5000, 0}},
    {{TW_PLS, TW_PLS, TW_OFF}, TRIO_SYNC, {TRIO_G2 - 5000, TRIO_G2 + 5000, 0}},
    {{TW_SAW, TW_SAW, TW_TRI}, TRIO_SYNC, {TRIO_G3 - 3000, TRIO_G3 + 4000, TRIO_G3}},
    {{TW_TRI, TW_OFF, TW_OFF}, TRIO_RING, {TRIO_G1, 0, 0}},
    {{TW_TRI, TW_OFF, TW_TRI}, TRIO_RING, {TRIO_G2 + 2000, 0, TRIO_G2 - 3000}},
    {{TW_TRI, TW_SAW, TW_OFF}, TRIO_RING | TRIO_SYNC, {TRIO_G2 + 2000, TRIO_G2, 0}},
    {{TW_TRI, TW_PLS, TW_OFF}, TRIO_RING | TRIO_SYNC, {TRIO_G2 + 2000, TRIO_G2, 0}},
};

const int16_t TRIO_MIX[4][3] = {
    {0, 0, 1024}, {0, 1400, 0}, {1024, 0, -1024}, {1024, 0, 0},
};

typedef struct {
    uint32_t pw;
    int32_t pdc, psm;
} trio_pw_t;

static inline int32_t trio_tri16(uint32_t ph) {
    return (int32_t)(((ph & 0x80000000u) ? ~ph : ph) >> 15) & 0xFFFF;
}
static inline int32_t trio_st(uint32_t ph) { return ((int32_t)(ph >> 16) & trio_tri16(ph)) - 16384; }
static inline int32_t trio_ps(uint32_t ph, const trio_pw_t *q) {
    return (ph >= q->pw ? (int32_t)(ph >> 16) : 0) - q->psm;
}

static int32_t trio_naive(uint32_t w, uint32_t ph, const trio_pw_t *q) {
    switch (w) {
    case TW_TRI: return osc_tri(ph);
    case TW_SAW: return (int32_t)(ph >> 16) - 32768;
    case TW_PLS: return (ph < q->pw ? 32767 : -32768) - q->pdc;
    case TW_ST:  return trio_st(ph);
    case TW_PS:  return trio_ps(ph, q);
    default: return 0;
    }
}

static inline void trio_blep(int32_t j, int32_t x, int32_t g, int32_t *cr) {
    int32_t h = ((j >> 1) * g) >> 15, y = 32767 - x;
    cr[0] += (h * ((x * x) >> 15)) >> 15;
    cr[1] -= (h * ((y * y) >> 15)) >> 15;
}

static inline int32_t trio_frac(uint32_t past, uint32_t inc) {
    uint32_t d = inc >> 15, x;
    if (!d) return 0;
    x = past / d;
    return x > 32767u ? 32767 : (int32_t)x;
}

static void trio_pass(uint32_t w, uint32_t *php, uint32_t inc, int32_t g,
                      const trio_pw_t *q, int32_t *nz, int32_t *nst,
                      int32_t *mix, int32_t *cr, int16_t *wx, uint32_t n)
{
    uint32_t ph = *php, o, i, pw = q->pw;
    int32_t pdc = q->pdc, z = *nz;
    switch (w) {
    case TW_TRI:
        for (i = 0; i < n; i++) { ph += inc; mix[i] += (osc_tri(ph) * g) >> 15; }
        break;
    case TW_SAW:
        for (i = 0; i < n; i++) {
            o = ph; ph += inc;
            if (ph < o) {
                int32_t x = trio_frac(ph, inc);
                trio_blep(-65536, x, g, cr + i);
                if (wx) wx[i] = (int16_t)x;
            }
            mix[i] += (((int32_t)(ph >> 16) - 32768) * g) >> 15;
        }
        break;
    case TW_PLS:
        for (i = 0; i < n; i++) {
            o = ph; ph += inc;
            if (ph < o) {
                int32_t x = trio_frac(ph, inc);
                trio_blep(65535, x, g, cr + i);
                if (wx) wx[i] = (int16_t)x;
            }
            if (ph - pw < o - pw)
                trio_blep(-65535, trio_frac(ph - pw, inc), g, cr + i);
            mix[i] += (((ph < pw ? 32767 : -32768) - pdc) * g) >> 15;
        }
        break;
    case TW_NOI:
        for (i = 0; i < n; i++) {
            o = ph; ph += inc;
            if ((o ^ ph) & 0x08000000u)
                z = (int32_t)(noise32(nst) >> 16) - 32768;
            mix[i] += (z * g) >> 15;
        }
        break;
    case TW_ST:
        for (i = 0; i < n; i++) { ph += inc; mix[i] += (trio_st(ph) * g) >> 15; }
        break;
    case TW_PS:
        for (i = 0; i < n; i++) { ph += inc; mix[i] += (trio_ps(ph, q) * g) >> 15; }
        break;
    default:
        ph += inc * n;
        break;
    }
    *php = ph;
    *nz = z;
}

static void trio_ring(uint32_t *php, uint32_t inc, int32_t g, uint32_t p3, uint32_t inc3,
                      int32_t *mix, int32_t *cr, int16_t *wx, uint32_t n)
{
    uint32_t ph = *php, o, o3, i;
    for (i = 0; i < n; i++) {
        int32_t s, sg;
        o = ph; ph += inc;
        o3 = p3; p3 += inc3;
        if (wx && ph < o) wx[i] = (int16_t)trio_frac(ph, inc);
        s = osc_tri(ph);
        sg = (p3 & 0x80000000u) ? -g : g;
        if ((o3 ^ p3) & 0x80000000u)
            trio_blep(2 * s, trio_frac(p3 & 0x7FFFFFFFu, inc3), sg, cr + i);
        mix[i] += (s * sg) >> 15;
    }
    *php = ph;
}

static void trio_sync(uint32_t w, uint32_t *php, uint32_t inc, int32_t g,
                      const trio_pw_t *q, const int16_t *wx,
                      int32_t *mix, int32_t *cr, uint32_t n)
{
    uint32_t ph = *php, o, i, pw = q->pw;
    int32_t pdc = q->pdc, pls = w == TW_PLS;
    for (i = 0; i < n; i++) {
        o = ph; ph += inc;
        if (wx[i] >= 0) {
            uint32_t back = (uint32_t)wx[i] * (inc >> 15);
            trio_blep(trio_naive(w, 0, q) - trio_naive(w, ph - back, q), wx[i], g, cr + i);
            ph = o = back;
        }
        if (pls) {
            if (ph < o) trio_blep(65535, trio_frac(ph, inc), g, cr + i);
            if (ph - pw < o - pw) trio_blep(-65535, trio_frac(ph - pw, inc), g, cr + i);
            mix[i] += (((ph < pw ? 32767 : -32768) - pdc) * g) >> 15;
        } else {
            if (ph < o) trio_blep(-65536, trio_frac(ph, inc), g, cr + i);
            mix[i] += (((int32_t)(ph >> 16) - 32768) * g) >> 15;
        }
    }
    *php = ph;
}

static uint32_t trio_inc(int32_t pitch16, int32_t semi, int32_t ct, int32_t fine) {
    return cents_inc(pitch16 + semi * 16, ct, fine);
}

void trio_note_on(track_t *t, voice_t *v)
{
    (void)t;
    /* Only seed the noise generator once. Do NOT touch ph[], s[0..1]
     * (filter), s[4] (prev sample) or s[5] (drift): the internal envelope
     * now handles amplitude and resetting the filter/prev would produce a
     * hard step on every note-on (the "crackle"). */
    if (!v->s[2])
        v->s[2] = 0x2468ACE + (int32_t)v->age;
}

void trio_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    const trio_set_t *ws = &TRIO_SETS[(uint32_t)p[P_E0] & 15u];
    uint32_t w1 = ws->w[0], w2 = ws->w[1], w3 = ws->w[2], i;
    uint32_t ring = ws->flags & TRIO_RING, sync = ws->flags & TRIO_SYNC;
    int32_t g1 = ws->g[0], g2 = ws->g[1], g3 = ws->g[2];
    uint32_t base = pitch_inc(m->pitch16);
    int32_t fine = (base >> 12) ? (int32_t)(m->inc - base) / (int32_t)(base >> 12) : 0;
    uint32_t inc1 = m->inc;
    uint32_t inc2 = trio_inc(m->pitch16, p[P_E1], p[P_E3], fine);
    uint32_t inc3 = trio_inc(m->pitch16, p[P_E2], -p[P_E3], fine);

    /* Pulse-width:
     *   duty = (PW << 8) + m.shape - (64 << 8), clamped to safe limits.
     *   m.shape is expected to contain a 64 << 8 base (see synth_trio.cpp),
     *   so a *freshly allocated voice with PW = 64 sits at ~50 % duty* and
     *   the envelope sweep modulates it up or down from there.
     *   Previously m.shape was 0, so *every* pulse wave sat at the 768
     *   clamp floor (~2 %) — thin click, which looked like "PWM is lost". */
    int32_t duty = clamp((p[P_E7] << 8) + m->shape - (64 << 8), 768, 32000);

    trio_pw_t q;
    uint32_t mode = (uint32_t)p[P_E4] & 3u;
    int32_t reso = p[P_E6], k = 8192 - reso * 7168 / 127;   /* Trio's own damping: no self-oscillation */
    int32_t ma = TRIO_MIX[mode][0], mb = TRIO_MIX[mode][1], mc = TRIO_MIX[mode][2];
    int32_t a1, a2, a3, cut, cur;
    tsvf_t flt;
    int32_t mix[CTL], cr[CTL + 1];
    int16_t wx[CTL];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], ph3s = ph2;
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2], nz = v->s[3], prev = v->s[4], drift = v->s[5];

    if (n > CTL)
        n = CTL;

    q.pw  = (uint32_t)duty << 17;
    q.pdc = 2 * duty - 32768;
    q.psm = 32768 - ((duty * duty) >> 15);

    if (mode >= 2u)
        mb = -(k >> 2);

    drift += (((int32_t)(noise32(&nst) >> 22) - 512) - drift) >> 3;

    /* Bass-weighted cutoff: map the raw 0..127 parameter through the
     * CUT_PARAM_TO_IDX table first, then add envelope + drift modulation
     * on top (modulation still reaches the full range even from low base). */
    int32_t cut_base = CUT_PARAM_TO_IDX[clamp(p[P_E5], 0, 127)] << 8;
    cut = clamp(cut_base + m->cutoff + drift, 0, 127 << 8);
    tsvf_coef_k(&flt, cut, k);
    a1 = flt.a1;
    a2 = flt.a2;
    a3 = flt.a3;

    for (i = 0; i < n; i++) {
        mix[i] = cr[i] = 0;
        wx[i] = -1;
    }
    cr[n] = 0;

    trio_pass(w3, &ph2, inc3, g3, &q, &nz, &nst, mix, cr, 0, n);
    if (ring)
        trio_ring(&ph0, inc1, g1, ph3s, inc3, mix, cr, sync ? wx : 0, n);
    else
        trio_pass(w1, &ph0, inc1, g1, &q, &nz, &nst, mix, cr, sync ? wx : 0, n);
    if (sync)
        trio_sync(w2, &ph1, inc2, g2, &q, wx, mix, cr, n);
    else
        trio_pass(w2, &ph1, inc2, g2, &q, &nz, &nst, mix, cr, 0, n);

    cur = prev;
    for (i = 0; i < n; i++) {
        int32_t x = cur + cr[i], y, v1, v2, v3, a;
        cur = mix[i];
        x = soft_knee(x, 16000) >> 1;

        /* SVF — matches Felucca eng_trio.c inline block */
        v3 = x - ic2;
        v1 = (a1 * ic1 + a2 * v3) >> 13;
        v2 = ic2 + ((a2 * ic1 + a3 * v3) >> 13);
        ic1 = clamp(2 * v1 - ic1, -150000, 150000);
        ic2 = clamp(2 * v2 - ic2, -150000, 150000);
        a = clamp(ic1, -49152, 49152);
        ic1 = a + ((ic1 - a) >> 2);

        y = soft_knee((x * ma + v1 * mb + v2 * mc) >> 10, 16000);
        out[i] += voice_amp(y << 1, m, i);
    }
    prev = cur + cr[n];

    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
    v->s[3] = nz;
    v->s[4] = prev;
    v->s[5] = drift;
}

const preset_t TRIO_PRESETS[] = {
    {"FAT BASS",  {0, 0, -12, 9, 0, 62, 45, 64}, {0, 62, 60, 25}, 40, 1, FX(10, 0, 10, 10), PAT(2)},
    {"ARP LEAD",  {2, 12, 0, 4, 0, 88, 25, 32}, {0, 60, 90, 30}, 8, 0, FX(0, 0, 35, 20), PAT(13)},
    {"SYNC LEAD", {9, 9, 0, 0, 0, 82, 30, 64}, {2, 70, 100, 40}, 18, 1, FX(0, 10, 40, 25), PAT(4)},
    {"RING BELL", {13, 0, 18, 6, 1, 96, 30, 64}, {0, 92, 0, 80}, 0, 0, FX(0, 20, 35, 55), PAT(7)},
    {"CHIP CHOIR",{1, 0, 12, 7, 1, 62, 95, 40}, {70, 90, 110, 85}, 30, 0, FX(0, 50, 20, 65), PAT(5)},
};
const uint8_t TRIO_NPRESETS = NELEM(TRIO_PRESETS);