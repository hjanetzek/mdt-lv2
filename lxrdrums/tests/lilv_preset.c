/*
 * lilv_preset.c - a converted LXR project preset (tools/lxr2preset.py)
 * loaded with lilv, as jalv / Zynthian load presets:
 *
 *   lilv_preset PLUGIN_BUNDLE PRESET_FILE EXPECTED_CHUNK
 *
 * lilv_state_new_from_file(), the plugin instantiated by lilv,
 * lilv_state_restore() (port values and the state), one run(), then the
 * plugin's state saved: its project chunk must be the one in the preset
 * (EXPECTED_CHUNK: decoded by seq_test --project). Built when lilv is
 * found (pkg-config lilv-0).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "ports.h"
#include "testhost.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/state/state.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float controls[EDS_NUM_PORTS];
static int valuesSet, modeValue = -1;

static void setValue (const char *symbol, void *data, const void *value, uint32_t size, uint32_t type)
{
	(void) data;
	(void) size;
	if (type == th_map (LV2_ATOM__Float))
	{
		valuesSet++;
		if (!strcmp (symbol, "seq_mode"))
		{
			modeValue = (int) *(const float *) value;
		}
	}
}

static uint8_t *saved;
static size_t savedSize;

static LV2_State_Status store (LV2_State_Handle handle, uint32_t key, const void *value, size_t size, uint32_t type,
			       uint32_t flags)
{
	(void) handle;
	(void) flags;
	if (key == th_map (EDS_URI "#project") && type == th_map (LV2_ATOM__Chunk))
	{
		saved = malloc (size);
		memcpy (saved, value, size);
		savedSize = size;
	}
	return LV2_STATE_SUCCESS;
}

int main (int argc, char **argv)
{
	if (argc != 4)
	{
		fprintf (stderr, "usage: lilv_preset PLUGIN_BUNDLE PRESET_FILE EXPECTED_CHUNK\n");
		return 2;
	}
	LilvWorld *world = lilv_world_new ();
	char uri[8192];
	snprintf (uri, sizeof uri, "file://%s/", argv[1]);
	LilvNode *bundle = lilv_new_uri (world, uri);
	lilv_world_load_bundle (world, bundle);
	LilvNode *puri = lilv_new_uri (world, EDS_URI);
	const LilvPlugin *plugin = lilv_plugins_get_by_uri (lilv_world_get_all_plugins (world), puri);
	if (!plugin)
	{
		fprintf (stderr, "%s: no eDrumSynth\n", argv[1]);
		return 2;
	}
	LV2_URID_Map *map = th_features ()[0]->data;
	LilvState *state = lilv_state_new_from_file (world, map, NULL, argv[2]);
	if (!state)
	{
		printf ("FAIL: lilv: %s: no state\n", argv[2]);
		return 1;
	}
	LilvInstance *inst = lilv_plugin_instantiate (plugin, 44100, th_features ());
	if (!inst)
	{
		fprintf (stderr, "instantiate failed\n");
		return 2;
	}
	static uint64_t seq[64];
	LV2_Atom_Sequence *s = (LV2_Atom_Sequence *) seq;
	th_seq_clear (s);
	static float out[2][256];
	lilv_instance_connect_port (inst, EDS_PORT_MIDI_IN, s);
	lilv_instance_connect_port (inst, EDS_PORT_OUT_L, out[0]);
	lilv_instance_connect_port (inst, EDS_PORT_OUT_R, out[1]);
	for (uint32_t i = EDS_PORT_CONTROL0; i < EDS_NUM_PORTS; i++)
	{
		lilv_instance_connect_port (inst, i, &controls[i]);
	}
	lilv_instance_activate (inst);
	lilv_state_restore (state, inst, setValue, NULL, 0, th_features ());
	lilv_instance_run (inst, 256);
	const LV2_State_Interface *si = lilv_instance_get_extension_data (inst, LV2_STATE__interface);
	si->save (lilv_instance_get_handle (inst), store, NULL, 0, th_features ());

	size_t size = 0;
	uint8_t *expected = NULL;
	FILE *f = fopen (argv[3], "rb");
	if (f)
	{
		fseek (f, 0, SEEK_END);
		size = (size_t) ftell (f);
		fseek (f, 0, SEEK_SET);
		expected = malloc (size);
		size = fread (expected, 1, size, f);
		fclose (f);
	}
	TH_CHECK (expected && saved && savedSize == size && !memcmp (saved, expected, size),
		  "lilv: the restored project (%zu bytes) is not the preset's (%zu bytes)", savedSize, size);
	TH_CHECK (valuesSet > 300 && modeValue == 1, "lilv: %d port values set, seq_mode %d", valuesSet, modeValue);
	if (!th_failures)
	{
		printf ("lilv: the preset's state restored (%zu bytes, the preset's project), %d port values\n", savedSize, valuesSet);
	}
	lilv_instance_deactivate (inst);
	lilv_instance_free (inst);
	lilv_state_free (state);
	lilv_node_free (puri);
	lilv_node_free (bundle);
	lilv_world_free (world);
	free (saved);
	free (expected);
	return th_failures ? 1 : 0;
}
