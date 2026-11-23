/*
 * mdt_tables.h - MicroDexed-touch's parameter tables for Trio and Analog,
 * transcribed from its code (at ca6518a8, see engine/ORIGIN.md), for the
 * TTL generator and the render test:
 *
 * - the editors of the Analog Synths page (UI.cpp 8502-8610,
 *   UI_func_analog_synths): label, the trio_t / analog_t field (config.h
 *   1444-1478) as a voice layer parameter (src/params.h), range, in the
 *   editors' order;
 * - the boot defaults (MicroDexed-touch.ino 1818-1838: level 100, pan
 *   centre, transpose 0; then preset 0 when the envelope is empty, 2170-2180);
 * - the presets as MDT applies them (UI.cpp 8311-8359, as_apply_*_preset():
 *   clamped to the editor ranges; Trio's env_to_pw 0, Analog's fenv / 2).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VA_MDT_TABLES_H_
#define VA_MDT_TABLES_H_

#include "params.h"
#include "synth_shared.h"

#define MDT_NONE	(-1)	// an editor the plugin doesn't expose

typedef struct
{
	const char *label;	// the editor's label
	int param;		// VA_..., or MDT_NONE
	int min, max;
} mdt_editor;

// UI.cpp 8566-8608 (TRIO, bottom half), in order
static const mdt_editor MDT_TRIO_EDITORS[] = {
	{ "WAVE", VA_P_E0, 0, 15 },
	{ "INT2", VA_P_E1, -24, 24 },
	{ "INT3", VA_P_E2, -24, 24 },
	{ "DTN", VA_P_E3, 0, 50 },
	{ "PW", VA_P_E7, 0, 127 },
	{ "ENV>PW", VA_ENV_TO_PW, 0, 127 },
	{ "ENV>CUT", VA_ENV_TO_CUT, 0, 127 },
	{ "MODE", VA_P_E4, 0, 3 },
	{ "CUT", VA_P_E5, 0, 127 },
	{ "RES", VA_P_E6, 0, 127 },
	{ "ATK", VA_ATTACK, 0, 127 },
	{ "DEC", VA_DECAY, 0, 127 },
	{ "SUS", VA_SUSTAIN, 0, 127 },
	{ "REL", VA_RELEASE, 0, 127 },
	{ "TRANSP", VA_TRANSPOSE, -24, 24 },
	{ "LEVEL", VA_LEVEL, 0, 100 },
	{ "REV", MDT_NONE, 0, 100 },		// MDT's send effects: Zynthian has its own
	{ "DLY1", MDT_NONE, 0, 100 },
	{ "DLY2", MDT_NONE, 0, 100 },
	{ "PAN", VA_PAN, 0, 40 },
	{ "CHANNEL", MDT_NONE, 0, 17 },		// the host routes MIDI
};

// UI.cpp 8502-8535 (ANALOG, top half), in order
static const mdt_editor MDT_ANALOG_EDITORS[] = {
	{ "WAVE", VA_P_E0, 0, 4 },
	{ "DTN", VA_P_E1, 0, 127 },
	{ "MIX", VA_P_E2, 0, 127 },
	{ "NOIS", VA_P_E3, 0, 127 },
	{ "ENV>PW", VA_ENV_TO_PW, 0, 127 },
	{ "ENV>CUT", VA_ENV_TO_CUT, 0, 127 },
	{ "CUT", VA_P_E4, 0, 127 },
	{ "RES", VA_P_E5, 0, 127 },
	{ "DRV", VA_P_E6, 0, 127 },
	{ "KTR", VA_P_E7, 0, 127 },
	{ "ATK", VA_ATTACK, 0, 127 },
	{ "DEC", VA_DECAY, 0, 127 },
	{ "SUS", VA_SUSTAIN, 0, 127 },
	{ "REL", VA_RELEASE, 0, 127 },
	{ "TRANSP", VA_TRANSPOSE, -24, 24 },
	{ "LEVEL", VA_LEVEL, 0, 100 },
	{ "REV", MDT_NONE, 0, 100 },
	{ "DLY1", MDT_NONE, 0, 100 },
	{ "DLY2", MDT_NONE, 0, 100 },
	{ "PAN", VA_PAN, 0, 40 },
	{ "CHANNEL", MDT_NONE, 0, 17 },
};

static inline int mdt_constrain (int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

// a preset as as_apply_trio_preset() / as_apply_analog_preset() set it:
// values[param] for the parameters it sets (set[param] = 1)
static inline void mdt_apply_preset (int analog, const preset_t *pr, int values[VA_NUM_PARAMS],
				     int set[VA_NUM_PARAMS])
{
	for (int i = 0; i < VA_NUM_PARAMS; i++)
	{
		set[i] = 0;
	}
	const mdt_editor *ed = analog ? MDT_ANALOG_EDITORS : MDT_TRIO_EDITORS;
	const int n = analog ? (int) NELEM (MDT_ANALOG_EDITORS) : (int) NELEM (MDT_TRIO_EDITORS);
	for (int i = 0; i < n; i++)
	{
		const int p = ed[i].param;
		int v;
		if (p >= VA_P_E0 && p <= VA_P_E7)
			v = pr->e[p - VA_P_E0];
		else if (p >= VA_ATTACK && p <= VA_RELEASE)
			v = pr->env[p - VA_ATTACK];
		else if (p == VA_ENV_TO_CUT)
			v = pr->fenv;
		else if (p == VA_ENV_TO_PW)
			v = analog ? pr->fenv / 2 : 0;	// Trio: "no preset yet uses it"
		else
			continue;	// level, pan, transpose, sends: not in presets
		values[p] = mdt_constrain (v, ed[i].min, ed[i].max);
		set[p] = 1;
	}
}

// the state after MDT's boot: level 100, pan centre, transpose 0, then
// preset 0 (the envelope is empty); the plugin's own: bend range 2, velocity 0
static inline void mdt_boot_defaults (int analog, const preset_t *preset0, int values[VA_NUM_PARAMS])
{
	int set[VA_NUM_PARAMS];
	for (int i = 0; i < VA_NUM_PARAMS; i++)
	{
		values[i] = 0;
	}
	mdt_apply_preset (analog, preset0, values, set);
	values[VA_LEVEL] = 100;
	values[VA_PAN] = 20;		// PANORAMA_DEFAULT
	values[VA_TRANSPOSE] = 0;
	values[VA_BEND_RANGE] = 2;	// LV2
	values[VA_VELOCITY] = 0;	// LV2: MDT ignores velocity
}

#endif
