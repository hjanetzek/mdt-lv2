/*
 * gen_ttl.c - writes the eDrumSynth LV2 bundle's TTL (manifest.ttl,
 * edrumsynth.ttl, presets.ttl) and the plugin's port table (src/ports.h)
 * from the drum engine's menu tables (engine/Synth_LXR), for every engine
 * on every voice slot.
 *
 *   gen_ttl BUNDLE_DIR PORTS_H PORTS_JSON   write the files
 *   gen_ttl --symbols              list the control port symbols
 *   gen_ttl --pages                list the groups (Zynthian pages)
 *   gen_ttl --engines              the engine specific parameters of every
 *                                  slot and engine, and their ports
 *
 * Ports: the parameters every engine of a slot has with the same meaning
 * (the shared ones: oscillator, amp EG, filter, transient, velocity, LFO,
 * mix) once per slot, as in v1; the engine specific ones (the LXR's 7
 * "roles" per slot, whose meaning depends on the engine) once per slot and
 * engine family (Mod EG: drum and snare; FM: drum; Noise: snare; Mod Osc:
 * cymbal and hi-hat), each port marked with the engines it applies to. Both
 * are found from the engine's pages with each engine set on the slot
 * (lxr_engine_set_engine(), lxr_engine_page_param()): a parameter that has
 * the same name, kind and range with all four engines is shared.
 *
 * PORTS_JSON (tools/edrumsynth_ports.json) is the port map for tools
 * (tools/snd2lv2.py, the LXR project converter): per control port its
 * symbol, index, engine parameter, slot, engines, range and the value
 * conversion; the engine selectors and the MIDI mode port.
 *
 * The outputs are committed (cross builds don't run this program); a test
 * regenerates them and compares. Port symbols are API (Zynthian presets,
 * snapshots and MIDI learn store them): they come from the naming table
 * below, not from the engine's display names, and must not change once
 * released (tests/symbols.txt).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "lxr_engine.h"
#include "ttl.h"	// common/

// the engine's modulation target table (index -> parameter), for the labels
// of the velocity and LFO destination ports
#include "menu/menu.h"
#include "menu/CcNr2Text.h"

#if defined (__GNUC__) && !defined (__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"	// names are short
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define URI		"https://github.com/hjanetzek/mdt-lv2/edrumsynth"
#define NAME		"eDrumSynth"
#define BINARY		"edrumsynth.so"
#define TTL		"edrumsynth.ttl"
#define MINOR_VERSION	3
#define MICRO_VERSION	0

#define MAX_PORTS	400
#define MAX_GROUPS	96
#define SLOTS		LXR_ENGINE_SLOTS
#define ENGINES		LXR_ENGINE_ENGINES
#define ALL_ENGINES	((1 << ENGINES) - 1)
#define GLOBAL		SLOTS		// the slot of the FX bus and master ports

// ---- naming ----

static const struct voice
{
	const char *name, *sym, *abbr;
} voices[SLOTS] = {
	{ "Bass Drum", "bd", "BD" },
	{ "Tom 1", "t1", "T1" },
	{ "Tom 2", "t2", "T2" },
	{ "Snare", "sd", "SD" },
	{ "Cymbal", "cy", "CY" },
	{ "Hi-Hat", "hh", "HH" },
};

static const char *const engineNames[ENGINES] = { "Drum", "Snare", "Cymbal", "Hi-Hat" };

// the groups of a voice in display order. Shared groups take the shared
// parameters of a sub page of the voice's menu page; engine groups (family
// set) the engine specific parameters of their engines
enum { G_MIX, G_OSC, G_AMPEG, G_FILTER, G_TRANS, G_MODEG, G_FM, G_NOISE, G_MODOSC, G_VEL, G_LFO, NUM_VGROUPS };
static const struct vgroup
{
	const char *sym, *name, *shortName, *cls;
	int sub;		// shared groups: the sub page; -1: an engine group
	uint8_t engines;	// engine groups: the engines (bits) it applies to
} vgroups[NUM_VGROUPS] = {
	{ "mix", "Mix", "Mix", NULL, 7, 0 },
	{ "osc", "Osc", "Osc", "param:OscillatorControls", 0, 0 },
	{ "ampeg", "Amp EG", "Amp EG", "param:EnvelopeControls", 1, 0 },
	{ "filter", "Filter", "Filter", "param:FilterControls", 5, 0 },
	{ "trans", "Transient", "Transient", NULL, 4, 0 },
	{ "modeg", "Mod EG (Drum/Snare)", "Mod EG", NULL, -1, 1 << 0 | 1 << 1 },
	{ "fm", "FM (Drum)", "FM", NULL, -1, 1 << 0 },
	{ "noise", "Noise (Snare)", "Noise", NULL, -1, 1 << 1 },
	{ "modosc", "Mod Osc (Cym/HH)", "Mod Osc", NULL, -1, 1 << 2 | 1 << 3 },
	{ "vel", "Velocity", "Velocity", NULL, 2, 0 },
	{ "lfo", "LFO", "LFO", NULL, 6, 0 },
};

// port symbol (after the voice's), name and modulation target label of a
// voice parameter: by group, the engine's category (NULL: any) and long
// name. Duplicates within a group are numbered ("wave1", "Wave 1", ...).
// The order of an engine group's entries is its ports' order.
static const struct naming
{
	int group;
	const char *category, *longName, *sym, *name, *label, *designation;
} namings[] = {
	{ G_OSC, NULL, "Waveform", "osc_wave", "Waveform", NULL, NULL },
	{ G_OSC, NULL, "Coarse", "osc_coarse", "Coarse", NULL, NULL },
	{ G_OSC, NULL, "Fine", "osc_fine", "Fine", NULL, NULL },
	{ G_OSC, NULL, "PulsWdth", "osc_pwm", "Pulse Width", NULL, NULL },

	{ G_AMPEG, NULL, "Attack", "ampeg_attack", "Attack", NULL, "param:attack" },
	{ G_AMPEG, NULL, "Decay", "ampeg_decay", "Decay", NULL, "param:decay" },
	{ G_AMPEG, NULL, "Dcy Clsd", "ampeg_decay_closed", "Decay Closed", NULL, "param:decay" },
	{ G_AMPEG, NULL, "Dcy Open", "ampeg_decay_open", "Decay Open", NULL, NULL },
	{ G_AMPEG, NULL, "Slope", "ampeg_slope", "Slope", NULL, NULL },

	{ G_VEL, "Velocity", "DstParam", "mod_vel_dest", "Velo Dest", NULL, NULL },
	{ G_VEL, "Velocity", "Amount", "mod_vel_amount", "Velo Amount", NULL, NULL },
	{ G_VEL, "Velocity", "Vol mod", "mod_vel_vol", "Velo Vol Mod", NULL, NULL },

	{ G_TRANS, NULL, "Waveform", "trans_wave", "Waveform", NULL, NULL },
	{ G_TRANS, NULL, "Volume", "trans_volume", "Volume", NULL, NULL },
	{ G_TRANS, NULL, "Frequncy", "trans_freq", "Frequency", NULL, NULL },

	{ G_FILTER, NULL, "Frequncy", "filter_cutoff", "Cutoff", NULL, "param:cutoffFrequency" },
	{ G_FILTER, NULL, "Resnance", "filter_resonance", "Resonance", NULL, "param:resonance" },
	{ G_FILTER, NULL, "Type", "filter_type", "Type", NULL, NULL },
	{ G_FILTER, NULL, "Overdriv", "filter_drive", "Drive", NULL, NULL },

	{ G_LFO, NULL, "Frequncy", "lfo_freq", "Frequency", NULL, NULL },
	{ G_LFO, NULL, "ClockSnc", "lfo_sync", "Clock Sync", NULL, NULL },
	{ G_LFO, NULL, "Amount", "lfo_amount", "Amount", NULL, NULL },
	{ G_LFO, NULL, "Waveform", "lfo_wave", "Waveform", NULL, NULL },
	{ G_LFO, NULL, "Retriggr", "lfo_retrigger", "Retrigger", NULL, NULL },
	{ G_LFO, NULL, "Offset", "lfo_offset", "Offset", NULL, NULL },
	{ G_LFO, NULL, "DstVoice", "lfo_dest_voice", "Dest Voice", NULL, NULL },
	{ G_LFO, NULL, "DstParam", "lfo_dest_param", "Dest Param", NULL, NULL },

	{ G_MIX, NULL, "Volume", "mix_volume", "Volume", NULL, NULL },
	{ G_MIX, NULL, "Panning", "mix_pan", "Pan", NULL, NULL },
	{ G_MIX, NULL, "SampleRt", "mix_sample_rate", "Sample Rate", NULL, NULL },
	{ G_MIX, NULL, "Overdriv", "mix_drive", "Drive", NULL, NULL },
	{ G_MIX, NULL, "AudioOut", "mix_out", "Audio Out", NULL, NULL },

	// the engine groups
	{ G_MODEG, "Mod EG", "Decay", "modeg_decay", "Mod EG Decay", "Mod EG Decay", NULL },
	{ G_MODEG, "Veloc EG", "Slope", "modeg_slope", "Mod EG Slope", "Mod EG Slope", NULL },
	{ G_MODEG, "PitchMod", "Amount", "modeg_pitch", "Pitch Mod", "Pitch Mod", NULL },

	{ G_FM, "FM", "Amount", "fm_amount", "FM Amount", "FM Amount", NULL },
	{ G_FM, "FM", "Frequncy", "fm_freq", "FM Freq", "FM Freq", NULL },
	{ G_FM, "Oscilltr", "Waveform", "fm_wave", "FM Wave", "FM Wave", NULL },
	{ G_FM, "Oscilltr", "Mode", "fm_mode", "FM Mode", "FM Mode", NULL },

	{ G_NOISE, "Noise", "Frequncy", "noise_freq", "Noise Freq", "Noise Freq", NULL },
	{ G_NOISE, "Nois/Osc", "Mix", "noise_mix", "Noise Mix", "Noise Mix", NULL },
	{ G_NOISE, "Veloc EG", "RepeatCt", "noise_repeat", "Repeat", "Repeat", NULL },

	{ G_MODOSC, "Mod Osc", "Freqcy 1", "modosc_freq1", "Freq 1", "Mod Freq 1", NULL },
	{ G_MODOSC, "Mod Osc", "Freqcy 2", "modosc_freq2", "Freq 2", "Mod Freq 2", NULL },
	{ G_MODOSC, "Mod Osc", "Gain 1", "modosc_gain1", "Gain 1", "Mod Gain 1", NULL },
	{ G_MODOSC, "Mod Osc", "Gain 2", "modosc_gain2", "Gain 2", "Mod Gain 2", NULL },
	{ G_MODOSC, "Oscilltr", "Waveform", "modosc_wave", "Wave", "Mod Wave", NULL },
	{ G_MODOSC, "Veloc EG", "RepeatCt", "modosc_repeat", "Repeat", "Repeat", NULL },
};
#define NUM_NAMINGS (int) (sizeof namings / sizeof namings[0])

// the FX bus (its page depends on the FX type: all its parameters, the
// type specific ones after the common ones; P1-P4 mean something else per
// type) and the master parameters (the DJ filter and the limiter are
// globals, not in kits; the overall sample rate is in kits)
static const struct fixed
{
	uint16_t param;
	const char *sym, *name, *comment;
} fxParams[] = {
	{ 234, "type", "FX Type", NULL },
	{ 236, "dry_wet", "Dry/Wet", NULL },
	{ 235, "out", "Audio Out", NULL },
	{ 237, "dist_type", "Dist Type", "distortion type (FX type drv)" },
	{ 238, "p1", "FX Param 1", "drv: drive; rng: frequency; cmp: attack; del: time" },
	{ 239, "p2", "FX Param 2", "drv: color; cmp: decay; del: range" },
	{ 240, "p3", "FX Param 3", "drv: tone; cmp: threshold; del: tone" },
	{ 241, "p4", "FX Param 4", "drv: volume; cmp: gain; del: feedback" },
	{ 242, "comp_ratio", "Comp Ratio", "compressor ratio (FX type cmp)" },
	{ 243, "ring_wave", "Ring Wave", "ring modulator waveform (FX type rng)" },
	{ 245, "delay_type", "Delay Mode", "delay or ping pong (FX type del)" },
}, masterParams[] = {
	{ 266, "dj_filter", "DJ Filter", "negative: low pass, positive: high pass, 0: off" },
	{ 267, "dj_reso", "DJ Reso", NULL },
	{ 302, "limiter", "Limiter", NULL },
	{ 114, "sample_rate", "Sample Rate", "the sample rate of all voices (the kit's, with the voices' own)" },
};
#define PAR_DECIMATION_ALL	114

// ---- the engine's pages, per slot and engine ----

typedef struct
{
	uint16_t param;
	uint8_t page, sub, pos;		// where (pos: in page order)
	char category[16], longName[16];
	uint8_t kind, min, max, center, def;
} Info;

#define MAX_INFO 64
static Info infos[SLOTS][ENGINES][MAX_INFO];
static int numInfos[SLOTS][ENGINES];

static const Info *findInfo (int slot, int engine, uint16_t param)
{
	for (int i = 0; i < numInfos[slot][engine]; i++)
	{
		if (infos[slot][engine][i].param == param)
		{
			return &infos[slot][engine][i];
		}
	}
	return NULL;
}

// the parameters on the slot's pages (slot 6: and page 7, the open hi-hat)
// with the engine set, their names, kinds, ranges and the values the engine
// gives them (the init kit's, or the engine's defaults after the switch)
static void collectInfo (void)
{
	for (int slot = 0; slot < SLOTS; slot++)
	{
		for (int e = 0; e < ENGINES; e++)
		{
			lxr_engine_init ();
			lxr_engine_set_engine ((uint8_t) slot, (uint8_t) e);
			if (lxr_engine_get_engine ((uint8_t) slot) != e)
			{
				fprintf (stderr, "gen_ttl: slot %d: engine %d not set\n", slot + 1, e);
				exit (1);
			}
			int pos = 0;
			const int pages[2] = { slot, LXR_ENGINE_PAGES - 1 };
			for (int pi = 0; pi < (slot == SLOTS - 1 ? 2 : 1); pi++)
			{
				for (uint8_t sub = 0; sub < LXR_ENGINE_SUB_PAGES; sub++)
				{
					for (uint8_t k = 0; k < 8; k++)
					{
						const uint16_t par = lxr_engine_page_param ((uint8_t) pages[pi], sub, k);
						if (!par || !*lxr_engine_param_name (par, LXR_NAME_LONG) || findInfo (slot, e, par))
						{
							continue;
						}
						if (numInfos[slot][e] == MAX_INFO)
						{
							fprintf (stderr, "gen_ttl: too many parameters on slot %d\n", slot + 1);
							exit (1);
						}
						Info *in = &infos[slot][e][numInfos[slot][e]++];
						in->param = par;
						in->page = (uint8_t) pages[pi];
						in->sub = sub;
						in->pos = (uint8_t) pos++;
						snprintf (in->category, sizeof in->category, "%s", lxr_engine_param_name (par, LXR_NAME_CATEGORY));
						snprintf (in->longName, sizeof in->longName, "%s", lxr_engine_param_name (par, LXR_NAME_LONG));
						in->kind = lxr_engine_param_kind (par);
						lxr_engine_param_range (par, &in->min, &in->max);
						in->center = in->kind == LXR_KIND_SIGNED ? lxr_engine_param_center (par) : 0;
						in->def = lxr_engine_get_parameter (par);
					}
				}
			}
		}
	}
	lxr_engine_init ();
}

static int sameInfo (const Info *a, const Info *b)
{
	return a && b && !strcmp (a->category, b->category) && !strcmp (a->longName, b->longName) && a->kind == b->kind
	       && a->min == b->min && a->max == b->max && a->center == b->center && a->sub == b->sub && a->page == b->page;
}

// a parameter the slot has with the same meaning whatever its engine
static int isShared (int slot, uint16_t param)
{
	const Info *a = findInfo (slot, 0, param);
	for (int e = 1; e < ENGINES; e++)
	{
		if (!sameInfo (a, findInfo (slot, e, param)))
		{
			return 0;
		}
	}
	return a != NULL;
}

static int stockEngine (int slot)
{
	return slot == 3 ? 1 : slot == 4 ? 2 : slot == 5 ? 3 : 0;
}

// ---- the ports ----

enum { CTL_PARAM, CTL_ENGINE, CTL_MIDI_MODE, CTL_LXR_CC };	// as EDS_CTL_* in ports.h

typedef struct
{
	char sym[64], name[64], label[64];	// label: in modulation target names (engine groups)
	const char *designation, *comment;
	uint16_t param;
	uint8_t type, kind, min, max, center, def;
	uint8_t slot, engines;			// the port is sent while its slot runs one of these engines
	int naming;				// voice parameters: the naming entry
	uint8_t pos;				// the engine's page order (numbering duplicates)
	int group;
	int index;				// in the group
} Port;

typedef struct
{
	char sym[32], name[64], shortName[32];
	const char *cls;
	int voice;				// -1: FX bus, master
} Group;

static Port ports[MAX_PORTS];
static int numPorts;
static Group groups[MAX_GROUPS];
static int numGroups;

static int addGroup (const char *sym, const char *name, const char *shortName, const char *cls, int voice)
{
	if (numGroups >= MAX_GROUPS)
	{
		fprintf (stderr, "gen_ttl: too many groups\n");
		exit (1);
	}
	Group *g = &groups[numGroups];
	snprintf (g->sym, sizeof g->sym, "%s", sym);
	snprintf (g->name, sizeof g->name, "%s", name);
	snprintf (g->shortName, sizeof g->shortName, "%s", shortName);
	g->cls = cls;
	g->voice = voice;
	return numGroups++;
}

static Port *newPort (int group)
{
	if (numPorts >= MAX_PORTS)
	{
		fprintf (stderr, "gen_ttl: too many ports\n");
		exit (1);
	}
	Port *p = &ports[numPorts++];
	memset (p, 0, sizeof *p);
	p->group = group;
	p->naming = -1;
	return p;
}

// a parameter port from the engine's current state (FX bus, master)
static Port *addEnginePort (int group, uint16_t param, const char *sym, const char *name)
{
	Port *p = newPort (group);
	snprintf (p->sym, sizeof p->sym, "%s", sym);
	snprintf (p->name, sizeof p->name, "%s", name);
	p->param = param;
	p->type = CTL_PARAM;
	p->kind = lxr_engine_param_kind (param);
	lxr_engine_param_range (param, &p->min, &p->max);
	p->center = p->kind == LXR_KIND_SIGNED ? lxr_engine_param_center (param) : 0;
	p->def = lxr_engine_get_parameter (param);
	p->slot = GLOBAL;
	p->engines = ALL_ENGINES;
	return p;
}

static int findNaming (int group, const Info *in)
{
	for (int i = 0; i < NUM_NAMINGS; i++)
	{
		const struct naming *n = &namings[i];
		if (n->group == group && !strcmp (n->longName, in->longName) && (!n->category || !strcmp (n->category, in->category)))
		{
			return i;
		}
	}
	return -1;
}

static int portOfVoiceParam (int slot, uint16_t param, int naming)
{
	for (int i = 0; i < numPorts; i++)
	{
		if (ports[i].type == CTL_PARAM && ports[i].slot == slot && ports[i].param == param && ports[i].naming == naming)
		{
			return i;
		}
	}
	return -1;
}

static Port *addVoicePort (int group, int slot, const Info *in, int naming)
{
	const struct naming *nm = &namings[naming];
	Port *p = newPort (group);
	snprintf (p->sym, sizeof p->sym, "%s_%s", voices[slot].sym, nm->sym);
	snprintf (p->name, sizeof p->name, "%s", nm->name);
	snprintf (p->label, sizeof p->label, "%s", nm->label ? nm->label : nm->name);
	p->designation = nm->designation;
	p->param = in->param;
	p->type = CTL_PARAM;
	p->kind = in->kind;
	p->min = in->min;
	p->max = in->max;
	p->center = in->center;
	p->def = in->def;
	p->slot = (uint8_t) slot;
	p->naming = naming;
	p->pos = in->pos;
	return p;
}

// numbers the ports of a group whose symbols repeat ("wave1", "wave2"), in
// the engine's page order
static void numberDuplicates (int first)
{
	for (int i = first; i < numPorts; i++)
	{
		int count = 0;
		for (int j = first; j < numPorts; j++)
		{
			count += !strcmp (ports[i].sym, ports[j].sym);
		}
		if (count < 2)
		{
			continue;
		}
		char base[64], bname[64], blabel[64];
		snprintf (base, sizeof base, "%s", ports[i].sym);
		snprintf (bname, sizeof bname, "%s", ports[i].name);
		snprintf (blabel, sizeof blabel, "%s", ports[i].label);
		for (int k = 1; k <= count; k++)
		{
			// the k-th in page order
			int best = -1;
			for (int j = first; j < numPorts; j++)
			{
				if (!strcmp (ports[j].sym, base) && (best < 0 || ports[j].pos < ports[best].pos))
				{
					best = j;
				}
			}
			snprintf (ports[best].sym, sizeof ports[best].sym, "%s%d", base, k);
			snprintf (ports[best].name, sizeof ports[best].name, "%s %d", bname, k);
			snprintf (ports[best].label, sizeof ports[best].label, "%s %d", blabel, k);
		}
	}
}

// a shared group: the slot's shared parameters of its sub page, in the
// stock engine's page order (slot 6: page 7's own after page 6's)
static void collectShared (int slot, int vg, int group)
{
	const int e = stockEngine (slot);
	const int first = numPorts;
	for (int pi = 0; pi < 2; pi++)
	{
		for (int i = 0; i < numInfos[slot][e]; i++)
		{
			const Info *in = &infos[slot][e][i];
			if (in->sub != vgroups[vg].sub || (pi == 0) != (in->page == slot) || !isShared (slot, in->param))
			{
				continue;
			}
			const int nm = findNaming (vg, in);
			if (nm < 0)
			{
				fprintf (stderr, "gen_ttl: no name for parameter %u (%s / %s) in group %s_%s\n", in->param,
					 in->category, in->longName, voices[slot].sym, vgroups[vg].sym);
				exit (1);
			}
			Port *p = addVoicePort (group, slot, in, nm);
			p->engines = ALL_ENGINES;
		}
	}
	numberDuplicates (first);
}

// an engine group: the engine specific parameters of its engines with a
// name of this group; one port per parameter and name (the engines that
// have it so share it), in the naming table's order; the default from the
// slot's stock engine if the group applies to it, else from the group's
// first engine (the value the engine gives after switching to it)
static void collectEngineGroup (int slot, int vg, int group)
{
	const int first = numPorts;
	const int stock = stockEngine (slot);
	int order[ENGINES], n = 0;
	if (vgroups[vg].engines & 1 << stock)
	{
		order[n++] = stock;
	}
	for (int e = 0; e < ENGINES; e++)
	{
		if (e != stock && vgroups[vg].engines & 1 << e)
		{
			order[n++] = e;
		}
	}
	for (int nm = 0; nm < NUM_NAMINGS; nm++)
	{
		if (namings[nm].group != vg)
		{
			continue;
		}
		for (int k = 0; k < n; k++)
		{
			const int e = order[k];
			for (int i = 0; i < numInfos[slot][e]; i++)
			{
				const Info *in = &infos[slot][e][i];
				if (isShared (slot, in->param) || findNaming (vg, in) != nm)
				{
					continue;
				}
				int pi = portOfVoiceParam (slot, in->param, nm);
				if (pi < 0)
				{
					addVoicePort (group, slot, in, nm);
					pi = numPorts - 1;
				}
				Port *p = &ports[pi];
				if (p->kind != in->kind || p->min != in->min || p->max != in->max || p->center != in->center)
				{
					fprintf (stderr, "gen_ttl: %s: engines disagree on the range\n", p->sym);
					exit (1);
				}
				p->engines |= (uint8_t) (1 << e);
			}
		}
	}
	numberDuplicates (first);
}

static void collect (void)
{
	collectInfo ();
	for (int v = 0; v < SLOTS; v++)
	{
		const struct voice *voice = &voices[v];
		for (int vg = 0; vg < NUM_VGROUPS; vg++)
		{
			char sym[32], name[64];
			snprintf (sym, sizeof sym, "%s_%s", voice->sym, vgroups[vg].sym);
			snprintf (name, sizeof name, "%s %s", voice->name, vgroups[vg].name);
			const int g = addGroup (sym, name, vgroups[vg].shortName, vgroups[vg].cls, v);
			const int first = numPorts;
			if (vg == G_MIX)
			{
				// the engine selector first
				Port *p = newPort (g);
				snprintf (p->sym, sizeof p->sym, "%s_mix_engine", voice->sym);
				snprintf (p->name, sizeof p->name, "Engine");
				p->comment = "the voice engine of this slot: switching resets the slot in the engine, then the "
					     "slot's controls (the shared ones and the new engine's groups) are sent again";
				p->type = CTL_ENGINE;
				p->kind = LXR_KIND_TEXT;
				p->min = 0;
				p->max = ENGINES - 1;
				p->def = (uint8_t) stockEngine (v);
				p->slot = (uint8_t) v;
				p->engines = ALL_ENGINES;
			}
			if (vgroups[vg].sub >= 0)
			{
				collectShared (v, vg, g);
			}
			else
			{
				collectEngineGroup (v, vg, g);
			}
			if (numPorts == first)
			{
				fprintf (stderr, "gen_ttl: group %s is empty\n", sym);
				exit (1);
			}
			for (int i = first; i < numPorts; i++)
			{
				ports[i].index = i - first;
			}
		}

		// every parameter of every engine on this slot has exactly one
		// port sent while the slot runs that engine
		for (int e = 0; e < ENGINES; e++)
		{
			for (int i = 0; i < numInfos[v][e]; i++)
			{
				const uint16_t par = infos[v][e][i].param;
				int count = 0;
				for (int j = 0; j < numPorts; j++)
				{
					count += ports[j].type == CTL_PARAM && ports[j].param == par && ports[j].engines & 1 << e;
				}
				if (count != 1)
				{
					fprintf (stderr, "gen_ttl: slot %d, engine %s: parameter %u (%s / %s) has %d ports\n", v + 1,
						 engineNames[e], par, infos[v][e][i].category, infos[v][e][i].longName, count);
					exit (1);
				}
			}
		}
	}

	int g = addGroup ("fx", "FX Bus", "FX Bus", NULL, -1);
	for (unsigned i = 0; i < sizeof fxParams / sizeof fxParams[0]; i++)
	{
		Port *p = addEnginePort (g, fxParams[i].param, fxParams[i].sym, fxParams[i].name);
		p->comment = fxParams[i].comment;
		p->index = (int) i;
	}
	g = addGroup ("master", "Master", "Master", NULL, -1);
	for (unsigned i = 0; i < sizeof masterParams / sizeof masterParams[0]; i++)
	{
		Port *p = addEnginePort (g, masterParams[i].param, masterParams[i].sym, masterParams[i].name);
		p->comment = masterParams[i].comment;
		p->index = (int) i;
	}
	Port *p = newPort (g);
	snprintf (p->sym, sizeof p->sym, "midi_mode");
	snprintf (p->name, sizeof p->name, "MIDI Mode");
	p->comment = "Kit: every channel plays the General MIDI drum map; Multi: channel 10 the drum map, channels 1-7 "
		     "tracks 1-7 chromatically (note 60: the tuned pitch), other channels ignored";
	p->type = CTL_MIDI_MODE;
	p->kind = LXR_KIND_TEXT;
	p->min = 0;
	p->max = 1;
	p->slot = GLOBAL;
	p->engines = ALL_ENGINES;
	p->index = (int) (sizeof masterParams / sizeof masterParams[0]);
	// v2.1 (0.2.0): the engine's own CC / NRPN map, off by default (the
	// pedal, volume and panic CCs a Zynthian chain passes would change drum
	// parameters behind the ports' backs)
	p = newPort (g);
	snprintf (p->sym, sizeof p->sym, "lxr_cc");
	snprintf (p->name, sizeof p->name, "LXR CC");
	p->comment = "On: CCs and NRPNs on any channel set engine parameters as the LXR's MIDI implementation (CC n: "
		     "parameter n-1, NRPN n (CC 99, 98, 6): parameter 128+n; not the engine selection NRPN 98), without "
		     "changing the controls; off: CCs are ignored";
	p->type = CTL_LXR_CC;
	p->kind = LXR_KIND_SWITCH;
	p->min = 0;
	p->max = 1;
	p->slot = GLOBAL;
	p->engines = ALL_ENGINES;
	p->index = (int) (sizeof masterParams / sizeof masterParams[0]) + 1;

	// full symbols of the FX bus and master ports (group prefix), checks
	for (int i = 0; i < numPorts; i++)
	{
		Port *q = &ports[i];
		if (q->slot == GLOBAL)
		{
			char sym[64];
			snprintf (sym, sizeof sym, "%s_%s", groups[q->group].sym, q->sym);
			snprintf (q->sym, sizeof q->sym, "%s", sym);
		}
		if (q->max <= q->min)
		{
			fprintf (stderr, "gen_ttl: %s has no range\n", q->sym);
			exit (1);
		}
		if (q->designation && (q->kind != LXR_KIND_NUMBER || q->min != 0))
		{
			fprintf (stderr, "gen_ttl: %s: a designation needs a plain number from 0\n", q->sym);
			exit (1);
		}
		for (const char *c = q->sym; *c; c++)
		{
			if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
			{
				fprintf (stderr, "gen_ttl: bad symbol %s\n", q->sym);
				exit (1);
			}
		}
		for (int j = 0; j < i; j++)
		{
			if (!strcmp (ports[j].sym, q->sym))
			{
				fprintf (stderr, "gen_ttl: %s: duplicate symbol\n", q->sym);
				exit (1);
			}
			if (q->type == CTL_PARAM && ports[j].type == CTL_PARAM && ports[j].param == q->param
			    && ports[j].engines & q->engines)
			{
				fprintf (stderr, "gen_ttl: %s and %s: the same parameter for the same engine\n", q->sym, ports[j].sym);
				exit (1);
			}
		}
	}
}

// ---- output ----

#define PORT_CONTROL0	3	// after the MIDI input and the two audio outputs

static float portValue (const Port *p, int value)
{
	return (float) (value - p->center);
}

static int isModTargetParam (uint16_t param)
{
	return (param >= 149 && param <= 154) || (param >= 167 && param <= 172);
}

static int isKitPort (const Port *p)
{
	return p->type == CTL_ENGINE || (p->type == CTL_PARAM && p->param < lxr_engine_kit_size ());
}

// the label of a modulation target (an index into modTargets): voice and
// the port's group and name ("BD Osc Coarse"); an engine specific
// parameter: what it is in each engine group ("BD FM Amount / Noise Mix /
// Mod Gain 1"); numbered when a label repeats
static void targetLabel (uint8_t target, char *text, unsigned size)
{
	if (target == 0)
	{
		snprintf (text, size, "Off");
		return;
	}
	const uint16_t param = modTargets[target].param;
	const uint8_t voice = voiceFromModTargValue (target);
	const char *vabbr = voice ? voices[voice - 1].abbr : "All";
	if (param == PAR_DECIMATION_ALL)
	{
		snprintf (text, size, "All Sample Rate");
		return;
	}
	int found[ENGINES + 1], n = 0;
	for (int i = 0; i < numPorts && n <= ENGINES; i++)
	{
		if (ports[i].type == CTL_PARAM && ports[i].param == param)
		{
			found[n++] = i;
		}
	}
	if (n == 1 && ports[found[0]].engines == ALL_ENGINES)
	{
		// the group left out where the name says it ("FM Amount", "Pitch Mod")
		const Port *p = &ports[found[0]];
		const char *gname = groups[p->group].shortName;
		if (!strcmp (gname, "Velocity") || !strcmp (gname, "FX Bus") || !strncmp (p->name, gname, strlen (gname)))
		{
			snprintf (text, size, "%s %s", vabbr, p->name);
		}
		else
		{
			snprintf (text, size, "%s %s %s", vabbr, gname, p->name);
		}
		return;
	}
	if (n > 0)
	{
		int len = snprintf (text, size, "%s ", vabbr);
		for (int k = 0; k < n; k++)
		{
			int dup = 0;
			for (int j = 0; j < k; j++)
			{
				dup |= !strcmp (ports[found[j]].label, ports[found[k]].label);
			}
			if (!dup && len < (int) size)
			{
				len += snprintf (text + len, size - (unsigned) len, "%s%s", k ? " / " : "", ports[found[k]].label);
			}
		}
		return;
	}
	char t[32];
	lxr_engine_param_text (149, target, t, sizeof t);	// a velocity destination: the engine's text
	snprintf (text, size, "%s %s", vabbr, t);
}


// ---- the sequencer's panel (0.3.0): control ports after the others ----
//
// A window into the LXR's sequencer modelled on its front panel
// (SEQUENCER_PLAN.md §3): inputs select and edit (the plugin applies an
// input when its value changes; triggers on a rise above 0.5), outputs show
// the state (Zynthian's monitors: ≤ 24 bits per float). Symbols are API.

enum { PK_INT, PK_TOGGLE, PK_TRIGGER, PK_ENUM, PK_FLOAT };

typedef struct
{
	const char *sym, *name;
	int group;		// index into panelGroups
	uint8_t kind;
	float min, max, def;
	const char *labels;	// PK_ENUM: "a|b|c" for min..max, or "@targets" (the modulation targets)
	const char *comment;
} PanelPort;

static const struct
{
	const char *sym, *name;
	int output;
} panelGroups[] = {
	{ "seq_transport", "Seq Transport", 0 },
	{ "seq_pattern", "Seq Pattern", 0 },
	{ "seq_select", "Seq Select", 0 },
	{ "seq_step", "Seq Step", 0 },
	{ "seq_locks", "Seq Step Locks", 0 },
	{ "seq_track", "Seq Track", 0 },
	{ "seq_mutes", "Seq Mutes", 0 },
	{ "seq_buttons", "Seq Step Buttons", 0 },
	{ "seq_actions", "Seq Actions", 0 },
	{ "seq_status", "Seq Status", 1 },
	{ "seq_positions", "Seq Positions", 1 },
	{ "seq_page", "Seq Page", 1 },
	{ "seq_read", "Seq Step Read", 1 },
};
#define NUM_PANEL_GROUPS (int) (sizeof panelGroups / sizeof panelGroups[0])
enum { PG_TRANSPORT, PG_PATTERN, PG_SELECT, PG_STEP, PG_LOCKS, PG_TRACK, PG_MUTES, PG_BUTTONS, PG_ACTIONS, PG_STATUS,
       PG_POSITIONS, PG_PAGE, PG_READ };

static const char *const trackLabels = "Bass Drum|Tom 1|Tom 2|Snare|Cymbal|Hi-Hat Closed|Hi-Hat Open";
static const char *const trackAbbr[7] = { "bd", "t1", "t2", "sd", "cy", "hhc", "hho" };
static const char *const trackNames[7] = { "BD", "T1", "T2", "SD", "CY", "HHC", "HHO" };

#define MAX_PANEL 128
static PanelPort panel[MAX_PANEL];
static int numPanel;
static char panelStrings[MAX_PANEL][2][64];	// generated symbols and names

static PanelPort *addPanel (const char *sym, const char *name, int group, uint8_t kind, float min, float max, float def,
			    const char *labels, const char *comment)
{
	if (numPanel >= MAX_PANEL)
	{
		fprintf (stderr, "gen_ttl: too many panel ports\n");
		exit (1);
	}
	PanelPort *p = &panel[numPanel++];
	p->sym = sym;
	p->name = name;
	p->group = group;
	p->kind = kind;
	p->min = min;
	p->max = max;
	p->def = def;
	p->labels = labels;
	p->comment = comment;
	return p;
}

// a numbered port: symbol and name built from the formats and n
static PanelPort *addPanelN (const char *symFmt, const char *nameFmt, const char *arg, int n, int group, uint8_t kind,
			     float min, float max, float def, const char *comment)
{
	char *sym = panelStrings[numPanel][0], *name = panelStrings[numPanel][1];
	if (arg)
	{
		snprintf (sym, 64, symFmt, arg);
		snprintf (name, 64, nameFmt, n >= 0 ? trackNames[n] : arg);
	}
	else
	{
		snprintf (sym, 64, symFmt, n);
		snprintf (name, 64, nameFmt, n);
	}
	return addPanel (sym, name, group, kind, min, max, def, NULL, comment);
}

static void collectPanel (void)
{
	// inputs
	addPanel ("seq_mode", "Seq Mode", PG_TRANSPORT, PK_ENUM, 0, 2, 1, "Off|Host|Internal",
		  "Off: the sequencer does not play (MIDI only, as 0.2.0); Host: it follows the host's transport "
		  "(time:Position: rolling, bar / beat, tempo); Internal: its own tempo, started by Play");
	addPanel ("seq_play", "Play", PG_TRANSPORT, PK_TOGGLE, 0, 1, 0, NULL,
		  "Internal mode: on starts the sequencer at the start, off stops it (as the LXR's start / stop button)");
	addPanel ("seq_tempo", "Tempo", PG_TRANSPORT, PK_INT, 20, 300, 120, NULL, "Internal mode: the tempo in BPM");
	addPanel ("seq_shuffle", "Shuffle", PG_TRANSPORT, PK_INT, 0, 127, 0, NULL,
		  "the edited pattern's shuffle (odd steps late by up to 2/3 of a step); the playing pattern's at once");
	addPanel ("seq_pattern", "Pattern", PG_PATTERN, PK_INT, 0, 63, 0, NULL,
		  "the pattern to play: it starts when the playing one ends (stopped: at once), as the LXR's pattern buttons");
	addPanel ("seq_song_mode", "Song Mode", PG_PATTERN, PK_TOGGLE, 0, 1, 0, NULL,
		  "on: the song plays its patterns in turn (from its start at the next start)");
	addPanel ("seq_song", "Song", PG_PATTERN, PK_INT, 0, 63, 0, NULL, "the song song mode plays");
	addPanel ("seq_kit_autoload", "Kit Autoload", PG_PATTERN, PK_TOGGLE, 0, 1, 1, NULL,
		  "on: a pattern loads its kit when it starts (the LXR's pattern change mode 'auto'); patterns without "
		  "a kit in the project keep the sound");
	addPanel ("seq_edit_pattern", "Edit Pattern", PG_SELECT, PK_INT, 0, 64, 64, NULL,
		  "the pattern the panel edits: 0-63, 64 the playing one");
	addPanel ("seq_track", "Edit Track", PG_SELECT, PK_ENUM, 0, 6, 0, trackLabels, "the track the panel edits");
	addPanel ("seq_page", "Edit Page", PG_SELECT, PK_ENUM, 0, 3, 0, "1-16|17-32|33-48|49-64",
		  "the 16 steps the step buttons and the page outputs show");
	addPanel ("seq_step", "Edit Step", PG_SELECT, PK_INT, 0, 15, 0, NULL,
		  "the step of the page the step fields edit (a step button selects it too)");
	addPanel ("seq_step_velocity", "Velocity", PG_STEP, PK_INT, 0, 127, 127, NULL, "the selected step's velocity");
	addPanel ("seq_step_note", "Note", PG_STEP, PK_INT, 0, 127, 60, NULL,
		  "the selected step's note: 60 plays the voice as tuned, others shift its oscillators in semitones");
	addPanel ("seq_step_prob", "Probability", PG_STEP, PK_INT, 0, 100, 100, NULL, "the selected step's probability (%)");
	addPanel ("seq_step_flam", "Flam", PG_STEP, PK_ENUM, 0, 5, 0, "Off|x2|x3|x4|x6|x8", "the selected step's hits");
	addPanel ("seq_step_shift", "Shift", PG_STEP, PK_INT, -7, 7, 0, NULL,
		  "the selected step's time shift in eighths of a step (negative: earlier)");
	addPanel ("seq_lock1_target", "Lock 1", PG_LOCKS, PK_ENUM, 0, 0, 0, "@targets",
		  "the parameter the selected step sets (a parameter lock: back to the kit's value at the track's next step)");
	addPanel ("seq_lock1_value", "Lock 1 Value", PG_LOCKS, PK_INT, 0, 127, 0, NULL, "the value lock 1 sets");
	addPanel ("seq_lock2_target", "Lock 2", PG_LOCKS, PK_ENUM, 0, 0, 0, "@targets", "the second parameter lock");
	addPanel ("seq_lock2_value", "Lock 2 Value", PG_LOCKS, PK_INT, 0, 127, 0, NULL, "the value lock 2 sets");
	addPanel ("seq_track_length", "Length", PG_TRACK, PK_INT, 1, 64, 16, NULL,
		  "the edited track's length in steps (tracks of different lengths run against each other)");
	addPanel ("seq_euclid_fill", "Euclid Steps", PG_TRACK, PK_INT, 1, 64, 4, NULL,
		  "the Euclid generator: steps spread evenly over the track's length");
	addPanel ("seq_euclid_rotation", "Euclid Rotation", PG_TRACK, PK_INT, 0, 63, 0, NULL, "the Euclid generator's rotation");
	addPanel ("seq_euclid", "Euclid", PG_TRACK, PK_TRIGGER, 0, 1, 0, NULL,
		  "writes the Euclid pattern into the edited track (its steps replaced)");
	for (int t = 0; t < 7; t++)
	{
		addPanelN ("seq_mute_%s", "Mute %s", trackAbbr[t], t, PG_MUTES, PK_TOGGLE, 0, 1, 0, NULL);
	}
	for (int i = 1; i <= 16; i++)
	{
		addPanelN ("seq_button%d", "Step %d", NULL, i, PG_BUTTONS, PK_TRIGGER, 0, 1, 0,
			   i == 1 ? "toggles the step of the page on the edited track, and selects it (the LXR's step buttons)" : NULL);
	}
	addPanel ("seq_kit", "Kit", PG_ACTIONS, PK_INT, 0, 64, 0, NULL, "the edited pattern's kit (0-63, 64 none)");
	addPanel ("seq_store_kit", "Store Kit", PG_ACTIONS, PK_TRIGGER, 0, 1, 0, NULL,
		  "stores the sound (the kit as it is now) as the edited pattern's kit (none: into a free kit slot)");
	addPanel ("seq_clear_track", "Clear Track", PG_ACTIONS, PK_TRIGGER, 0, 1, 0, NULL, "clears the edited track");
	addPanel ("seq_clear_pattern", "Clear Pattern", PG_ACTIONS, PK_TRIGGER, 0, 1, 0, NULL, "clears the edited pattern");
	addPanel ("seq_copy_pattern", "Copy Pattern", PG_ACTIONS, PK_TRIGGER, 0, 1, 0, NULL, "remembers the edited pattern");
	addPanel ("seq_paste_pattern", "Paste Pattern", PG_ACTIONS, PK_TRIGGER, 0, 1, 0, NULL,
		  "copies the remembered pattern into the edited one");

	// outputs
	addPanel ("seq_out_running", "Running", PG_STATUS, PK_TOGGLE, 0, 1, 0, NULL, "the sequencer plays");
	addPanel ("seq_out_playing", "Playing", PG_STATUS, PK_INT, 0, 63, 0, NULL, "the pattern playing");
	addPanel ("seq_out_next", "Next", PG_STATUS, PK_INT, 0, 63, 0, NULL, "the pattern after it");
	addPanel ("seq_out_song_pos", "Song Position", PG_STATUS, PK_INT, 0, 63, 0, NULL, "song mode: the song's entry playing");
	addPanel ("seq_out_edit_pattern", "Edited Pattern", PG_STATUS, PK_INT, 0, 63, 0, NULL, "the pattern the panel edits");
	addPanel ("seq_out_bpm", "BPM", PG_STATUS, PK_FLOAT, 0, 1000, 0, NULL, "the sequencer's tempo");
	addPanel ("seq_out_flash", "Triggers", PG_STATUS, PK_INT, 0, 127, 0, NULL,
		  "a bit per track (1 bass drum .. 64 open hi-hat): triggered in the last 80 ms (sequencer and MIDI)");
	addPanel ("seq_out_mutes", "Mutes", PG_STATUS, PK_INT, 0, 127, 0, NULL, "a bit per track: muted");
	addPanel ("seq_out_pattern_length", "Pattern Length", PG_STATUS, PK_INT, 1, 64, 16, NULL,
		  "the edited pattern's length (its longest track)");
	addPanel ("seq_out_track_length", "Track Length", PG_STATUS, PK_INT, 1, 64, 16, NULL, "the edited track's length");
	addPanel ("seq_out_shuffle", "Pattern Shuffle", PG_STATUS, PK_INT, 0, 127, 0, NULL, "the edited pattern's shuffle");
	addPanel ("seq_out_kit", "Pattern Kit", PG_STATUS, PK_INT, 0, 64, 64, NULL, "the edited pattern's kit (64 none)");
	addPanel ("seq_out_kit_present", "Kit Present", PG_STATUS, PK_TOGGLE, 0, 1, 0, NULL,
		  "the project holds the edited pattern's kit");
	addPanel ("seq_out_selected", "Selected Step", PG_STATUS, PK_INT, 0, 63, 0, NULL,
		  "the step the step fields edit (Edit Page and Edit Step, or the step button pressed last)");
	for (int t = 0; t < 7; t++)
	{
		addPanelN ("seq_out_pos_%s", "Position %s", trackAbbr[t], t, PG_POSITIONS, PK_INT, -1, 63, -1,
			   t == 0 ? "the track's step playing (-1: stopped)" : NULL);
	}
	addPanel ("seq_out_mask", "Page Steps", PG_PAGE, PK_INT, 0, 65535, 0, NULL,
		  "the edited track's steps on the page: bit i (value 2^i) = step i + 1 of the page on");
	for (int t = 0; t < 7; t++)
	{
		addPanelN ("seq_out_grid_%s", "Page %s", trackAbbr[t], t, PG_PAGE, PK_INT, 0, 65535, 0,
			   t == 0 ? "the track's steps on the page, as Page Steps" : NULL);
	}
	for (int i = 1; i <= 16; i++)
	{
		addPanelN ("seq_out_vel%d", "Velocity %d", NULL, i, PG_PAGE, PK_INT, 0, 127, 0,
			   i == 1 ? "the velocity of the edited track's step i of the page" : NULL);
	}
	addPanel ("seq_out_step_active", "Step On", PG_READ, PK_TOGGLE, 0, 1, 0, NULL, "the selected step plays");
	addPanel ("seq_out_step_velocity", "Step Velocity", PG_READ, PK_INT, 0, 127, 0, NULL, "the selected step's fields as they are");
	addPanel ("seq_out_step_note", "Step Note", PG_READ, PK_INT, 0, 127, 60, NULL, NULL);
	addPanel ("seq_out_step_prob", "Step Probability", PG_READ, PK_INT, 0, 100, 100, NULL, NULL);
	addPanel ("seq_out_step_flam", "Step Flam", PG_READ, PK_ENUM, 0, 5, 0, "Off|x2|x3|x4|x6|x8", NULL);
	addPanel ("seq_out_step_shift", "Step Shift", PG_READ, PK_INT, -7, 7, 0, NULL, NULL);
	addPanel ("seq_out_lock1_target", "Step Lock 1", PG_READ, PK_ENUM, 0, 0, 0, "@targets", NULL);
	addPanel ("seq_out_lock1_value", "Step Lock 1 Value", PG_READ, PK_INT, 0, 127, 0, NULL, NULL);
	addPanel ("seq_out_lock2_target", "Step Lock 2", PG_READ, PK_ENUM, 0, 0, 0, "@targets", NULL);
	addPanel ("seq_out_lock2_value", "Step Lock 2 Value", PG_READ, PK_INT, 0, 127, 0, NULL, NULL);

	for (int i = 0; i < numPanel; i++)
	{
		PanelPort *p = &panel[i];
		if (p->labels && !strcmp (p->labels, "@targets"))
		{
			p->max = (float) (getNumModTargets () - 1);
		}
		for (int j = 0; j < numPorts; j++)
		{
			if (!strcmp (ports[j].sym, p->sym))
			{
				fprintf (stderr, "gen_ttl: %s: duplicate symbol\n", p->sym);
				exit (1);
			}
		}
		for (int j = 0; j < i; j++)
		{
			if (!strcmp (panel[j].sym, p->sym))
			{
				fprintf (stderr, "gen_ttl: %s: duplicate symbol\n", p->sym);
				exit (1);
			}
		}
		if (strncmp (p->sym, "seq_", 4) || strlen (p->sym) > 40)
		{
			fprintf (stderr, "gen_ttl: %s: panel symbols start with seq_\n", p->sym);
			exit (1);
		}
		const int output = panelGroups[p->group].output;
		if (output != !strncmp (p->sym, "seq_out_", 8) || (output && p->kind == PK_TRIGGER))
		{
			fprintf (stderr, "gen_ttl: %s: outputs are seq_out_*, not triggers\n", p->sym);
			exit (1);
		}
	}
}

static int panelOutput (const PanelPort *p)
{
	return panelGroups[p->group].output;
}

// the label of an enumeration value
static void panelLabel (const PanelPort *p, int v, char *text, unsigned size)
{
	if (!strcmp (p->labels, "@targets"))
	{
		targetLabel ((uint8_t) v, text, size);
		return;
	}
	const char *s = p->labels;
	for (int i = (int) p->min; i < v && s; i++)
	{
		s = strchr (s, '|');
		s = s ? s + 1 : NULL;
	}
	const char *e = s ? strchr (s, '|') : NULL;
	snprintf (text, size, "%.*s", s ? (int) (e ? e - s : (int) strlen (s)) : 0, s ? s : "");
}

static void writePanelPort (FILE *f, const PanelPort *p, int index, int prio)
{
	const int output = panelOutput (p);
	fprintf (f, " , [\n\t\ta lv2:%s , lv2:ControlPort ;\n\t\tlv2:index %d ;\n\t\tlv2:symbol \"%s\" ;\n\t\tlv2:name ",
		 output ? "OutputPort" : "InputPort", index, p->sym);
	ttl_string (f, p->name);
	if (p->comment)
	{
		fprintf (f, " ;\n\t\trdfs:comment ");
		ttl_string (f, p->comment);
	}
	if (p->kind == PK_FLOAT)
	{
		if (!output)
		{
			fprintf (f, " ;\n\t\tlv2:default %.1f", p->def);
		}
		fprintf (f, " ;\n\t\tlv2:minimum %.1f ;\n\t\tlv2:maximum %.1f", p->min, p->max);
	}
	else
	{
		if (!output)
		{
			fprintf (f, " ;\n\t\tlv2:default %.0f", p->def);
		}
		fprintf (f, " ;\n\t\tlv2:minimum %.0f ;\n\t\tlv2:maximum %.0f", p->min, p->max);
	}
	switch (p->kind)
	{
	case PK_TOGGLE:
		fprintf (f, " ;\n\t\tlv2:portProperty lv2:toggled");
		break;
	case PK_TRIGGER:
		fprintf (f, " ;\n\t\tlv2:portProperty lv2:toggled , pprops:trigger");
		break;
	case PK_INT:
		fprintf (f, " ;\n\t\tlv2:portProperty lv2:integer");
		break;
	case PK_ENUM:
		fprintf (f, " ;\n\t\tlv2:portProperty lv2:integer , lv2:enumeration ;\n\t\tlv2:scalePoint ");
		for (int v = (int) p->min; v <= (int) p->max; v++)
		{
			char label[96];
			panelLabel (p, v, label, sizeof label);
			fprintf (f, "%s[ rdfs:label ", v > (int) p->min ? " , " : "");
			ttl_string (f, label);
			fprintf (f, " ; rdf:value %d ]", v);
		}
		break;
	default:
		break;
	}
	fprintf (f, " ;\n\t\tpg:group <" URI "#%s> ;\n\t\tlv2:displayPriority %d\n\t]", panelGroups[p->group].sym, prio);
}

static void valueLabel (const Port *p, int v, char *text, unsigned size)
{
	switch (p->type)
	{
	case CTL_ENGINE:
		snprintf (text, size, "%s", engineNames[v]);
		break;
	case CTL_MIDI_MODE:
		snprintf (text, size, "%s", v ? "Multi" : "Kit");
		break;
	default:
		if (isModTargetParam (p->param))
		{
			targetLabel ((uint8_t) v, text, size);
		}
		else
		{
			lxr_engine_param_text (p->param, (uint8_t) v, text, size);
		}
		break;
	}
}

static void writeScalePoints (FILE *f, const Port *p)
{
	char labels[256][80];
	const int n = p->max - p->min + 1;
	for (int v = p->min; v <= p->max; v++)
	{
		valueLabel (p, v, labels[v - p->min], sizeof labels[0]);
	}
	fprintf (f, " ;\n\t\tlv2:scalePoint ");
	for (int i = 0; i < n; i++)
	{
		int dup = 0;
		for (int j = 0; j < n; j++)
		{
			dup |= j != i && !strcmp (labels[i], labels[j]);
		}
		char label[96];
		if (dup)
		{
			snprintf (label, sizeof label, "%s (%d)", labels[i], p->min + i);
		}
		else
		{
			snprintf (label, sizeof label, "%s", labels[i]);
		}
		fprintf (f, "%s[ rdfs:label ", i ? " , " : "");
		ttl_string (f, label);
		fprintf (f, " ; rdf:value %d ]", (int) portValue (p, p->min + i));
	}
}

static const char *engineList (uint8_t engines, char *text, unsigned size)
{
	int len = 0;
	text[0] = '\0';
	for (int e = 0; e < ENGINES; e++)
	{
		if (engines & 1 << e && len < (int) size)
		{
			len += snprintf (text + len, size - (unsigned) len, "%s%s", len ? ", " : "", engineNames[e]);
		}
	}
	return text;
}

static void writePlugin (FILE *f)
{
	fprintf (f, "# generated by tools/gen_ttl.c from the drum engine's tables: do not edit\n");
	ttl_prefixes (f, "atom doap foaf lv2 midi param pg pprops rdf rdfs state time urid");

	// the groups, lv2:displayPriority as Zynthian reads it (lv2core
	// namespace): the first group highest; the sequencer's panel last
	ttl_output_group (f, URI);
	const int allGroups = numGroups + NUM_PANEL_GROUPS;
	for (int g = 0; g < numGroups; g++)
	{
		ttl_group (f, URI, groups[g].sym, groups[g].name, groups[g].cls, allGroups - g);
	}
	for (int g = 0; g < NUM_PANEL_GROUPS; g++)
	{
		if (panelGroups[g].output)
			ttl_output_control_group (f, URI, panelGroups[g].sym, panelGroups[g].name, NUM_PANEL_GROUPS - g);
		else
			ttl_group (f, URI, panelGroups[g].sym, panelGroups[g].name, NULL, NUM_PANEL_GROUPS - g);
	}

	fprintf (f,
		 "<" URI ">\n"
		 "\ta lv2:Plugin , lv2:InstrumentPlugin , doap:Project ;\n"
		 "\tdoap:name \"" NAME "\" ;\n"
		 "\tlv2:project <" URI "> ;\n");
	ttl_project (f);
	fprintf (f,
		 "\tdoap:shortdesc \"Synthesized drums\" ;\n"
		 "\tlv2:minorVersion %d ;\n"
		 "\tlv2:microVersion %d ;\n"
		 "\trdfs:comment \"Synthesized drums: six voices (bass drum, two toms, snare, cymbal, hi-hat closed and open) "
		 "from MicroDexed-touch's drum engine, the voices of the OpenElixir-02 firmware (based on the open source "
		 "LXR V2 firmware by Erica Synths). Each voice runs a drum, snare, cymbal or hi-hat engine. General MIDI "
		 "drum notes, or channels 1-7 playing the voices chromatically; every voice parameter as a control. The "
		 "LXR's step sequencer (patterns with parameter locks, flams, probabilities, kits per pattern, songs) "
		 "following the host's transport, edited through the Seq controls; the project is the plugin's state.\" ;\n"
		 "\tlv2:requiredFeature urid:map ;\n"
		 "\tlv2:optionalFeature lv2:hardRTCapable , lv2:isLive ;\n"
		 "\tlv2:extensionData state:interface ;\n"
		 "\tpg:mainOutput <" URI "#out> ;\n",
		 MINOR_VERSION, MICRO_VERSION);

	ttl_io_ports (f, URI, "midi:MidiEvent , time:Position");

	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		fprintf (f, " , [\n\t\ta lv2:InputPort , lv2:ControlPort ;\n\t\tlv2:index %d ;\n\t\tlv2:symbol \"%s\" ;\n\t\tlv2:name ",
			 PORT_CONTROL0 + i, p->sym);
		ttl_string (f, p->name);
		char comment[256];
		if (p->comment)
		{
			snprintf (comment, sizeof comment, "%s", p->comment);
		}
		else if (p->engines != ALL_ENGINES)
		{
			char list[64];
			engineList (p->engines, list, sizeof list);
			snprintf (comment, sizeof comment, "used while the voice runs the %s engine%s", list,
				 strchr (list, ',') ? "s" : "");
		}
		if (p->comment || p->engines != ALL_ENGINES)
		{
			fprintf (f, " ;\n\t\trdfs:comment ");
			ttl_string (f, comment);
		}
		// integer ports with integer literals, the widget ports' (plain floats) with decimals
		fprintf (f, p->designation ? " ;\n\t\tlv2:default %.1f ;\n\t\tlv2:minimum %.1f ;\n\t\tlv2:maximum %.1f"
					   : " ;\n\t\tlv2:default %.0f ;\n\t\tlv2:minimum %.0f ;\n\t\tlv2:maximum %.0f",
			 portValue (p, p->def), portValue (p, p->min), portValue (p, p->max));
		switch (p->kind)
		{
		case LXR_KIND_SWITCH:
			fprintf (f, " ;\n\t\tlv2:portProperty lv2:toggled");
			break;
		case LXR_KIND_TEXT:
			fprintf (f, " ;\n\t\tlv2:portProperty lv2:integer , lv2:enumeration");
			writeScalePoints (f, p);
			break;
		default:
			// widget ports stay plain floats (Zynthian's envelope and
			// filter widgets), the others step in integers
			if (!p->designation)
			{
				fprintf (f, " ;\n\t\tlv2:portProperty lv2:integer");
			}
			break;
		}
		if (p->designation)
		{
			fprintf (f, " ;\n\t\tlv2:designation %s", p->designation);
		}
		fprintf (f, " ;\n\t\tpg:group <" URI "#%s> ;\n\t\tlv2:displayPriority %d\n\t]", groups[p->group].sym,
			 100 - p->index);
	}
	for (int i = 0, inGroup = 0; i < numPanel; i++)
	{
		inGroup = i > 0 && panel[i - 1].group == panel[i].group ? inGroup + 1 : 0;
		writePanelPort (f, &panel[i], PORT_CONTROL0 + numPorts + i, 100 - inGroup);
	}
	fprintf (f, " .\n");
}

// ---- factory presets (kits made here; no third party kits) ----

typedef struct
{
	const char *symbol;
	float value;		// port value
} Setting;

static const Setting twoSnares[] = {
	// tom 2 as a clap: the snare engine (its defaults from the engine
	// switch), mostly noise through a band pass, EG repeats
	{ "t2_noise_mix", 110 }, { "t2_noise_freq", 100 }, { "t2_noise_repeat", 3 }, { "t2_ampeg_decay", 18 },
	{ "t2_ampeg_slope", 30 }, { "t2_modeg_pitch", 20 }, { "t2_osc_coarse", 0 }, { "t2_filter_type", 2 },
	{ "t2_filter_cutoff", 80 }, { "t2_filter_resonance", 30 }, { "t2_mix_pan", 15 }, { "t2_mix_volume", 110 },
};
static const Setting allDrums[] = {
	// the snare slot as a high tom with a rim click, the cymbal as an FM
	// bell, the hi-hat as a short and a long FM noise click
	{ "t1_mix_pan", -20 }, { "t2_mix_pan", 20 },
	{ "sd_osc_coarse", -5 }, { "sd_ampeg_decay", 30 }, { "sd_modeg_pitch", 30 }, { "sd_trans_wave", 6 },
	{ "sd_trans_volume", 60 },
	{ "cy_osc_coarse", 12 }, { "cy_osc_wave", 0 }, { "cy_fm_amount", 70 }, { "cy_fm_freq", 7 }, { "cy_fm_wave", 0 },
	{ "cy_ampeg_decay", 70 }, { "cy_modeg_pitch", 0 }, { "cy_mix_pan", 25 }, { "cy_mix_volume", 100 },
	{ "hh_osc_coarse", 24 }, { "hh_osc_wave", 4 }, { "hh_fm_amount", 100 }, { "hh_fm_freq", 19 },
	{ "hh_ampeg_decay_closed", 8 }, { "hh_ampeg_decay_open", 35 }, { "hh_modeg_pitch", 0 }, { "hh_filter_type", 1 },
	{ "hh_filter_cutoff", 90 }, { "hh_mix_pan", -25 }, { "hh_mix_volume", 100 },
};

static const struct preset
{
	const char *sym, *label;
	uint8_t engines[SLOTS];
	const Setting *settings;
	int numSettings;
} presets[] = {
	{ "init", "Init", { 0, 0, 0, 1, 2, 3 }, NULL, 0 },
	{ "two_snares", "Two Snares", { 0, 0, 1, 1, 2, 3 }, twoSnares, sizeof twoSnares / sizeof twoSnares[0] },
	{ "all_drums", "All Drums", { 0, 0, 0, 0, 0, 0 }, allDrums, sizeof allDrums / sizeof allDrums[0] },
};
#define NUM_PRESETS (int) (sizeof presets / sizeof presets[0])

static int portOfSymbol (const char *sym)
{
	for (int i = 0; i < numPorts; i++)
	{
		if (!strcmp (ports[i].sym, sym))
		{
			return i;
		}
	}
	fprintf (stderr, "gen_ttl: no port %s\n", sym);
	exit (1);
}

static int isActive (const Port *p, const uint8_t *engines)
{
	return p->slot == GLOBAL || (p->engines >> engines[p->slot] & 1);
}

// the kit ports' values of a preset: the init kit, the slots' engines
// switched (their defaults), the settings; the ports of the engine groups
// the slots don't run keep their defaults
static void presetValues (const struct preset *pr, float *values)
{
	lxr_engine_init ();
	for (int s = 0; s < SLOTS; s++)
	{
		lxr_engine_set_engine ((uint8_t) s, pr->engines[s]);
	}
	for (int i = 0; i < pr->numSettings; i++)
	{
		const Port *p = &ports[portOfSymbol (pr->settings[i].symbol)];
		const int v = (int) pr->settings[i].value + p->center;
		if (p->type != CTL_PARAM || !isActive (p, pr->engines) || v < p->min || v > p->max)
		{
			fprintf (stderr, "gen_ttl: preset %s: %s is not active or out of range\n", pr->label, p->sym);
			exit (1);
		}
		lxr_engine_set_parameter (p->param, (uint8_t) v);
	}
	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		if (p->type == CTL_ENGINE)
			values[i] = pr->engines[p->slot];
		else if (p->type == CTL_PARAM && isActive (p, pr->engines))
			values[i] = portValue (p, lxr_engine_get_parameter (p->param));
		else
			values[i] = portValue (p, p->def);
	}
	lxr_engine_init ();
}

static void writePresets (FILE *f)
{
	fprintf (f, "# generated by tools/gen_ttl.c: do not edit\n");
	ttl_prefixes (f, "lv2 pset rdfs");
	fprintf (f, "<" URI "#bank_factory>\n\ta pset:Bank ;\n\tlv2:appliesTo <" URI "> ;\n\trdfs:label \"Factory\" .\n");
	for (int k = 0; k < NUM_PRESETS; k++)
	{
		float values[MAX_PORTS];
		presetValues (&presets[k], values);
		fprintf (f, "\n<" URI "#preset_%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <" URI "> ;\n\trdfs:label ", presets[k].sym);
		ttl_string (f, presets[k].label);
		fprintf (f, " ;\n\tpset:bank <" URI "#bank_factory> ;\n\tlv2:port ");
		int n = 0;
		for (int i = 0; i < numPorts; i++)
		{
			if (isKitPort (&ports[i]))
			{
				fprintf (f, "%s[\n\t\tlv2:symbol \"%s\" ;\n\t\tpset:value %.1f\n\t]", n++ ? " , " : "", ports[i].sym,
					 values[i]);
			}
		}
		fprintf (f, " .\n");
	}
}

static void writeManifest (FILE *f)
{
	fprintf (f,
		 "# generated by tools/gen_ttl.c: do not edit\n"
		 "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
		 "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
		 "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
		 "\n");
	ttl_manifest_plugin (f, URI, BINARY, TTL);
	// the presets (and their bank) are found through the manifest
	// (lv2:appliesTo the plugin), their port values in presets.ttl
	fprintf (f, "\n<" URI "#bank_factory>\n\ta pset:Bank ;\n\tlv2:appliesTo <" URI "> ;\n\trdfs:label \"Factory\" ;\n"
		    "\trdfs:seeAlso <presets.ttl> .\n");
	for (int k = 0; k < NUM_PRESETS; k++)
	{
		fprintf (f, "\n<" URI "#preset_%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <" URI "> ;\n\trdfs:label ", presets[k].sym);
		ttl_string (f, presets[k].label);
		fprintf (f, " ;\n\trdfs:seeAlso <presets.ttl> .\n");
	}
}

static void writePortsH (FILE *f)
{
	fprintf (f,
		 "/* generated by tools/gen_ttl.c from the drum engine's tables: do not edit */\n"
		 "#ifndef EDS_PORTS_H_\n"
		 "#define EDS_PORTS_H_\n"
		 "\n"
		 "#include <stdint.h>\n"
		 "\n"
		 "#define EDS_URI\t\t\t\"" URI "\"\n"
		 "#define EDS_PORT_MIDI_IN\t0\n"
		 "#define EDS_PORT_OUT_L\t\t1\n"
		 "#define EDS_PORT_OUT_R\t\t2\n"
		 "#define EDS_PORT_CONTROL0\t%d\n"
		 "#define EDS_NUM_CONTROLS\t%d\n"
		 "#define EDS_PORT_PANEL0\t\t(EDS_PORT_CONTROL0 + EDS_NUM_CONTROLS)\t/* the sequencer's panel */\n"
		 "#define EDS_NUM_PANEL\t\t%d\n"
		 "#define EDS_NUM_PORTS\t\t(EDS_PORT_PANEL0 + EDS_NUM_PANEL)\n"
		 "\n"
		 "#define EDS_SLOTS\t\t%d\n"
		 "#define EDS_GLOBAL\t\tEDS_SLOTS\t/* the slot of the FX bus and master controls */\n"
		 "#define EDS_ALL_ENGINES\t\t0x%x\n"
		 "\n"
		 "/* control types */\n"
		 "enum { EDS_CTL_PARAM, EDS_CTL_ENGINE, EDS_CTL_MIDI_MODE, EDS_CTL_LXR_CC };\n"
		 "\n"
		 "/* control port i (index EDS_PORT_CONTROL0 + i): type; EDS_CTL_PARAM:\n"
		 "   engine parameter param, value = round (port value) + offset, in\n"
		 "   min..max, sent while slot runs one of engines (bits: 1 drum, 2 snare,\n"
		 "   4 cymbal, 8 hi-hat); target: a modulation target (set after the other\n"
		 "   parameters); kit: in kit presets; def: the port's default */\n"
		 "typedef struct\n"
		 "{\n"
		 "\tuint16_t param;\n"
		 "\tuint8_t min, max;\n"
		 "\tint8_t offset;\n"
		 "\tuint8_t type, target, slot, engines, kit;\n"
		 "\tfloat def;\n"
		 "} eds_control;\n"
		 "\n"
		 "static const eds_control eds_controls[EDS_NUM_CONTROLS] = {\n",
		 PORT_CONTROL0, numPorts, numPanel, SLOTS, ALL_ENGINES);
	int engineControls[SLOTS] = { 0 }, midiMode = 0, lxrCc = 0;
	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		if (p->type == CTL_ENGINE)
			engineControls[p->slot] = i;
		else if (p->type == CTL_MIDI_MODE)
			midiMode = i;
		else if (p->type == CTL_LXR_CC)
			lxrCc = i;
		fprintf (f, "\t{ %3u, %u, %3u, %2d, %d, %d, %d, 0x%x, %d, %6.1ff },\t/* %3d %s */\n", p->param, p->min, p->max,
			 p->center, p->type, p->type == CTL_PARAM && isModTargetParam (p->param), p->slot, p->engines,
			 isKitPort (p), portValue (p, p->def), PORT_CONTROL0 + i, p->sym);
	}
	fprintf (f, "};\n\n/* the engine selector of each slot, the MIDI mode, the LXR CC switch (control indices) */\n"
		    "static const uint16_t eds_engine_controls[EDS_SLOTS] = {");
	for (int s = 0; s < SLOTS; s++)
	{
		fprintf (f, "%s%d", s ? ", " : " ", engineControls[s]);
	}
	fprintf (f, " };\n#define EDS_MIDI_MODE_CONTROL\t%d\n#define EDS_LXR_CC_CONTROL\t%d\n\n", midiMode, lxrCc);

	fprintf (f, "/* the sequencer's panel: port EDS_PORT_PANEL0 + i, an input or an output\n"
		    "   (output); kind: an integer, a toggle, a trigger, an enumeration or\n"
		    "   a float; min..max, def: the port's range and default */\n"
		    "enum { EDS_PK_INT, EDS_PK_TOGGLE, EDS_PK_TRIGGER, EDS_PK_ENUM, EDS_PK_FLOAT };\n"
		    "typedef struct\n{\n\tfloat min, max, def;\n\tuint8_t output, kind;\n} eds_panel_port;\n\n"
		    "enum\n{\n");
	for (int i = 0; i < numPanel; i++)
	{
		char name[64];
		snprintf (name, sizeof name, "%s", panel[i].sym + 4);
		for (char *c = name; *c; c++)
		{
			*c = (char) (*c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c);
		}
		fprintf (f, "\tEDS_P_%s,%s/* %3d %s */\n", name, strlen (name) < 12 ? "\t\t\t" : strlen (name) < 20 ? "\t\t" : "\t",
			 PORT_CONTROL0 + numPorts + i, panel[i].sym);
	}
	fprintf (f, "};\n\nstatic const eds_panel_port eds_panel[EDS_NUM_PANEL] = {\n");
	for (int i = 0; i < numPanel; i++)
	{
		const PanelPort *p = &panel[i];
		fprintf (f, "\t{ %6.1ff, %7.1ff, %6.1ff, %d, %d },\t/* %s */\n", p->min, p->max, p->def, panelOutput (p), p->kind,
			 p->sym);
	}
	fprintf (f, "};\n\n#endif\n");
}

static void jsonString (FILE *f, const char *s)
{
	ttl_string (f, s);	// the same escapes (the names have no control characters)
}

static const char *const engineKeys[ENGINES] = { "drum", "snare", "cymbal", "hihat" };

static void writePortsJson (FILE *f)
{
	static const char *const types[] = { "param", "engine", "midi_mode", "lxr_cc" };
	fprintf (f, "{\n\t\"comment\": \"generated by tools/gen_ttl.c from the drum engine's tables: do not edit. "
		    "A port's value = the engine's value - offset; param: the engine (LXR kit) parameter number, null for "
		    "the plugin's own ports; engines: the slot engines the port is sent for; kit: in kit presets; "
		    "kit_init: the init kit's value (engine value)\",\n");
	fprintf (f, "\t\"uri\": \"" URI "\",\n\t\"version\": \"0.%d.%d\",\n", MINOR_VERSION, MICRO_VERSION);
	fprintf (f, "\t\"first_control_index\": %d,\n\t\"kit_size\": %u,\n\t\"kit_name_size\": 8,\n", PORT_CONTROL0,
		 lxr_engine_kit_size ());
	fprintf (f, "\t\"engines\": [");
	for (int e = 0; e < ENGINES; e++)
	{
		fprintf (f, "%s\"%s\"", e ? ", " : "", engineKeys[e]);
	}
	fprintf (f, "],\n\t\"slots\": [\n");
	for (int v = 0; v < SLOTS; v++)
	{
		int engPort = 0;
		for (int i = 0; i < numPorts; i++)
		{
			if (ports[i].type == CTL_ENGINE && ports[i].slot == v)
				engPort = i;
		}
		fprintf (f, "\t\t{ \"slot\": %d, \"name\": \"%s\", \"prefix\": \"%s\", \"stock_engine\": \"%s\", "
			    "\"engine_port\": \"%s\", \"tracks\": [%d%s] }%s\n",
			 v, voices[v].name, voices[v].sym, engineKeys[stockEngine (v)], ports[engPort].sym, v + 1,
			 v == SLOTS - 1 ? ", 7" : "", v < SLOTS - 1 ? "," : "");
	}
	int midiMode = 0, lxrCc = 0;
	for (int i = 0; i < numPorts; i++)
	{
		if (ports[i].type == CTL_MIDI_MODE)
			midiMode = i;
		else if (ports[i].type == CTL_LXR_CC)
			lxrCc = i;
	}
	fprintf (f, "\t],\n\t\"midi_mode_port\": { \"symbol\": \"%s\", \"values\": { \"kit\": 0, \"multi\": 1 } },\n",
		 ports[midiMode].sym);
	fprintf (f, "\t\"lxr_cc_port\": { \"symbol\": \"%s\", \"values\": { \"off\": 0, \"on\": 1 } },\n", ports[lxrCc].sym);

	uint8_t init[256];
	lxr_engine_init ();
	lxr_engine_get_kit (init, lxr_engine_kit_size ());
	fprintf (f, "\t\"ports\": [\n");
	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		fprintf (f, "\t\t{ \"symbol\": \"%s\", \"index\": %d, \"name\": ", p->sym, PORT_CONTROL0 + i);
		jsonString (f, p->name);
		fprintf (f, ", \"group\": \"%s\", \"type\": \"%s\", ", groups[p->group].sym, types[p->type]);
		if (p->type == CTL_PARAM)
			fprintf (f, "\"param\": %u, ", p->param);
		else
			fprintf (f, "\"param\": null, ");
		if (p->slot < SLOTS)
			fprintf (f, "\"slot\": %d, ", p->slot);
		else
			fprintf (f, "\"slot\": null, ");
		fprintf (f, "\"engines\": [");
		for (int e = 0, n = 0; e < ENGINES; e++)
		{
			if (p->engines >> e & 1)
				fprintf (f, "%s\"%s\"", n++ ? ", " : "", engineKeys[e]);
		}
		fprintf (f, "], \"min\": %d, \"max\": %d, \"offset\": %d, \"default\": %d, ", (int) portValue (p, p->min),
			 (int) portValue (p, p->max), p->center, (int) portValue (p, p->def));
		fprintf (f, "\"kind\": \"%s\", \"target\": %s, \"kit\": %s",
			 p->kind == LXR_KIND_SWITCH ? "toggle" : p->kind == LXR_KIND_TEXT ? "enum" : p->designation ? "float" : "integer",
			 p->type == CTL_PARAM && isModTargetParam (p->param) ? "true" : "false", isKitPort (p) ? "true" : "false");
		if (p->type == CTL_PARAM && p->param < lxr_engine_kit_size ())
			fprintf (f, ", \"kit_init\": %u", init[p->param]);
		fprintf (f, " }%s\n", i < numPorts - 1 ? "," : "");
	}
	// the sequencer's panel
	static const char *const kinds[] = { "integer", "toggle", "trigger", "enum", "float" };
	fprintf (f, "\t],\n\t\"panel_ports\": [\n");
	for (int i = 0; i < numPanel; i++)
	{
		const PanelPort *p = &panel[i];
		fprintf (f, "\t\t{ \"symbol\": \"%s\", \"index\": %d, \"name\": ", p->sym, PORT_CONTROL0 + numPorts + i);
		jsonString (f, p->name);
		fprintf (f, ", \"group\": \"%s\", \"direction\": \"%s\", \"kind\": \"%s\", \"min\": %g, \"max\": %g",
			 panelGroups[p->group].sym, panelOutput (p) ? "output" : "input", kinds[p->kind], p->min, p->max);
		if (!panelOutput (p))
		{
			fprintf (f, ", \"default\": %g", p->def);
		}
		fprintf (f, " }%s\n", i < numPanel - 1 ? "," : "");
	}
	// the kit parameters without a port (unused, or not exposed: their
	// init kit values)
	fprintf (f, "\t],\n\t\"unmapped_kit_params\": {");
	int n = 0;
	for (uint16_t par = 0; par < lxr_engine_kit_size (); par++)
	{
		int mapped = 0;
		for (int i = 0; i < numPorts && !mapped; i++)
		{
			mapped = ports[i].type == CTL_PARAM && ports[i].param == par;
		}
		if (!mapped)
		{
			fprintf (f, "%s\"%u\": %u", n++ ? ", " : " ", par, init[par]);
		}
	}
	fprintf (f, " }\n}\n");
}

// the engine specific parameters of each slot and engine and their ports
static void printEngines (void)
{
	for (int v = 0; v < SLOTS; v++)
	{
		printf ("%s (slot %d, stock engine %s)\n", voices[v].name, v + 1, engineNames[stockEngine (v)]);
		for (int e = 0; e < ENGINES; e++)
		{
			printf ("  %-6s:", engineNames[e]);
			for (int i = 0; i < numInfos[v][e]; i++)
			{
				const Info *in = &infos[v][e][i];
				if (isShared (v, in->param))
				{
					continue;
				}
				for (int j = 0; j < numPorts; j++)
				{
					if (ports[j].type == CTL_PARAM && ports[j].param == in->param && ports[j].engines & 1 << e)
					{
						printf (" %u=%s", in->param, ports[j].sym + strlen (voices[v].sym) + 1);
					}
				}
			}
			printf ("\n");
		}
	}
}

int main (int argc, char **argv)
{
	lxr_engine_init ();
	collect ();
	collectPanel ();

	if (argc == 2 && !strcmp (argv[1], "--symbols"))
	{
		for (int i = 0; i < numPorts; i++)
		{
			printf ("%s\n", ports[i].sym);
		}
		for (int i = 0; i < numPanel; i++)
		{
			printf ("%s\n", panel[i].sym);
		}
		return 0;
	}
	if (argc == 2 && !strcmp (argv[1], "--pages"))
	{
		// the Zynthian pages: 4 controls per page per group
		int pages = 0;
		for (int g = 0; g < numGroups; g++)
		{
			int n = 0;
			for (int i = 0; i < numPorts; i++)
			{
				n += ports[i].group == g;
			}
			printf ("%-32s %2d controls, %d page%s\n", groups[g].name, n, (n + 3) / 4, n > 4 ? "s" : "");
			pages += (n + 3) / 4;
		}
		int panelControls = 0;
		for (int g = 0; g < NUM_PANEL_GROUPS; g++)
		{
			int n = 0;
			for (int i = 0; i < numPanel; i++)
			{
				n += panel[i].group == g;
			}
			if (panelGroups[g].output)
			{
				printf ("%-32s %2d outputs\n", panelGroups[g].name, n);
				continue;
			}
			printf ("%-32s %2d controls, %d page%s\n", panelGroups[g].name, n, (n + 3) / 4, n > 4 ? "s" : "");
			pages += (n + 3) / 4;
			panelControls += n;
		}
		printf ("panel: %d inputs, %d outputs\n", panelControls, numPanel - panelControls);
		printf ("%d groups, %d controls, %d pages\n", numGroups, numPorts, pages);
		return 0;
	}
	if (argc == 2 && !strcmp (argv[1], "--engines"))
	{
		printEngines ();
		return 0;
	}
	if (argc != 4)
	{
		fprintf (stderr, "usage: gen_ttl BUNDLE_DIR PORTS_H PORTS_JSON | --symbols | --pages | --engines\n");
		return 2;
	}

	FILE *f = ttl_open (argv[1], "manifest.ttl");
	writeManifest (f);
	fclose (f);
	f = ttl_open (argv[1], TTL);
	writePlugin (f);
	fclose (f);
	f = ttl_open (argv[1], "presets.ttl");
	writePresets (f);
	fclose (f);
	f = ttl_open (NULL, argv[2]);
	writePortsH (f);
	fclose (f);
	f = ttl_open (NULL, argv[3]);
	writePortsJson (f);
	fclose (f);
	return 0;
}
