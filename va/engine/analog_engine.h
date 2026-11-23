/* analog_engine.h — two band-limited oscillators, noise, drive, trapezoidal LP SVF.
 * License: GPL-3.0-only. */

#pragma once
#include "synth_shared.h"

extern const char *const N_ANALOG_WAVE[5];
extern const preset_t    ANALOG_PRESETS[];
extern const uint8_t     ANALOG_NPRESETS;

void analog_note_on(track_t *t, voice_t *v);
void analog_render (track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);