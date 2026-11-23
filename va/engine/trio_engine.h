/* trio_engine.h — three oscillators, ring mod, hard sync, trapezoidal SVF.
 * License: GPL-3.0-only. */

#pragma once
#include "synth_shared.h"

enum { TW_OFF, TW_TRI, TW_SAW, TW_PLS, TW_NOI, TW_ST, TW_PS };
#define TRIO_RING 1u
#define TRIO_SYNC 2u
#define TRIO_G3 11000
#define TRIO_G2 15000
#define TRIO_G1 24000

typedef struct {
    uint8_t w[3];
    uint8_t flags;
    int16_t g[3];
} trio_set_t;

extern const char *const   N_TRIO_WAVE[16];
extern const char *const   N_TRIO_MODE[4];
extern const trio_set_t    TRIO_SETS[16];
extern const int16_t       TRIO_MIX[4][3];
extern const preset_t      TRIO_PRESETS[];
extern const uint8_t       TRIO_NPRESETS;

void trio_note_on(track_t *t, voice_t *v);
void trio_render (track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);