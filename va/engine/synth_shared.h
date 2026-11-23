/* synth_shared.h — types shared by the TRIO and ANALOG engines.
 * License: GPL-3.0-only. */

#pragma once
#include <stdint.h>

#ifndef CTL
#define CTL 128                       /* must equal AUDIO_BLOCK_SAMPLES */
#endif


enum {
    P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7,
    P_E8_ENV_TO_CUT, P_E9_ENV_TO_PW,
    P_COUNT
};

enum {
    F_INT, F_PCT, F_BIPCT, F_TIME, F_LFOHZ, F_CUTOFF, F_DB, F_SEMI, F_ENUM, F_BPM, F_NOTE,
    F_ONOFF, F_OCT, F_STEPS
};

typedef struct {
    const char *label;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;
    const char *unit;
} param_desc_t;

typedef struct {
    const char *name;
    int8_t e[8];
    uint8_t env[4];
    int8_t fenv;
    uint8_t mono;
    uint8_t fx[4];
    uint8_t pat;
} preset_t;

typedef struct track track_t;
typedef struct voice voice_t;
typedef struct vmod vmod_t;

struct voice {
    uint32_t ph[3];
    int32_t  s[8];
    uint8_t  note, active, gate, stage;
    int32_t  env, env_out;
    int32_t  pitch16, pitch_cur, gstep, fine;
    uint32_t age;
    uint8_t  mvel;
    int16_t  mrnd;
};

struct vmod {
    uint32_t inc;
    int32_t  pitch16;
    int32_t  amp0, amp1;      /* block-start / block-end amplitude (Q15) */
    int32_t  cutoff, shape;   /* block-start cutoff / pulse-width modulation */
    int32_t  envq15;
    int32_t  fine;
};

struct track {
    int16_t p[P_COUNT];
    voice_t v[1];
};

typedef struct {
    const char *name;
    const char *page_title[2];
    param_desc_t edit[8];
    const preset_t *presets;
    uint8_t npresets;
    uint8_t knob[4];
    uint8_t poly, sampled, keep, oneshot;
    void (*note_on)(track_t *, voice_t *);
    void (*render)(track_t *, voice_t *, int32_t *, uint32_t, const vmod_t *);
    const param_desc_t *(*desc)(const track_t *, uint32_t);
    void (*block)(track_t *);
    int32_t (*keys)(const track_t *, uint32_t);
    uint8_t ownenv;
    int (*done)(track_t *, voice_t *);
} engine_t;

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
/* Preset FX block. The four arguments map to fx[0..3]:
 *   d  = delay send (informational, currently unused by trio/analog)
 *   c  = chorus send (informational, currently unused)
 *   dl = delay send level (both delays use fx[2])
 *   r  = reverb send
 * These used to be discarded by a stub macro, so every preset had
 * rev_send = 0 and delay sends = 0. That was the primary reason reverb
 * "didn't work" for either engine from presets. */
#define FX(d, c, dl, r) {(uint8_t)(d), (uint8_t)(c), (uint8_t)(dl), (uint8_t)(r)}
#define PAT(n) 0