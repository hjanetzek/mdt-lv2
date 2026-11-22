/*
 * gen_ttl.c - writes the eDrumSynth LV2 bundle's TTL (manifest.ttl,
 * edrumsynth.ttl) and the plugin's port table (src/ports.h) from the drum
 * engine's menu tables (engine/Synth_LXR), with the stock engines.
 *
 *   gen_ttl BUNDLE_DIR PORTS_H     write the files
 *   gen_ttl --symbols              list the control port symbols
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
#define MINOR_VERSION	0
#define MICRO_VERSION	1

#define MAX_PORTS	320
#define MAX_GROUPS	64

// ---- naming ----

static const struct voice
{
	const char *name, *sym, *abbr;
} voices[LXR_ENGINE_SLOTS] = {
	{ "Bass Drum", "bd", "BD" },
	{ "Tom 1", "t1", "T1" },
	{ "Tom 2", "t2", "T2" },
	{ "Snare", "sd", "SD" },
	{ "Cymbal", "cy", "CY" },
	{ "Hi-Hat", "hh", "HH" },
};

// the voice sub pages in display order: mix, oscillator, amp EG, filter,
// transient, FM / mod oscillators, modulation, LFO
static const uint8_t subOrder[LXR_ENGINE_SUB_PAGES] = { 7, 0, 1, 5, 4, 3, 2, 6 };

// group of a sub page; sub page 3 is FM for the drum engine, the mod
// oscillators for cymbal and hi-hat (named by its first parameter's category)
static void subGroup (uint8_t sub, uint16_t firstParam, const char **name, const char **sym, const char **cls)
{
	*cls = NULL;
	switch (sub)
	{
	case 0:	*name = "Osc";		*sym = "osc";	 *cls = "param:OscillatorControls"; break;
	case 1:	*name = "Amp EG";	*sym = "ampeg";	 *cls = "param:EnvelopeControls"; break;
	case 2:	*name = "Mod";		*sym = "mod";	 break;
	case 3:
		if (!strcmp (lxr_engine_param_name (firstParam, LXR_NAME_CATEGORY), "FM"))
		{
			*name = "FM";	*sym = "fm";
		}
		else
		{
			*name = "Mod Osc"; *sym = "modosc";
		}
		break;
	case 4:	*name = "Transient";	*sym = "trans";	 break;
	case 5:	*name = "Filter";	*sym = "filter"; *cls = "param:FilterControls"; break;
	case 6:	*name = "LFO";		*sym = "lfo";	 break;
	default: *name = "Mix";		*sym = "mix";	 break;
	}
}

// port symbol part and name of a voice parameter: by group symbol, the
// engine's category (NULL: any) and long name. Duplicates within a group
// are numbered ("wave1", "Wave 1", ...)
static const struct naming
{
	const char *group, *category, *longName, *sym, *name, *designation;
} namings[] = {
	{ "osc", "Noise", "Frequncy", "noise_freq", "Noise Freq", NULL },
	{ "osc", "Nois/Osc", "Mix", "noise_mix", "Noise Mix", NULL },
	{ "osc", NULL, "Waveform", "wave", "Waveform", NULL },
	{ "osc", NULL, "Coarse", "coarse", "Coarse", NULL },
	{ "osc", NULL, "Fine", "fine", "Fine", NULL },
	{ "osc", NULL, "PulsWdth", "pwm", "Pulse Width", NULL },

	{ "ampeg", NULL, "Attack", "attack", "Attack", "param:attack" },
	{ "ampeg", NULL, "Decay", "decay", "Decay", "param:decay" },
	{ "ampeg", NULL, "Dcy Clsd", "decay_closed", "Decay Closed", "param:decay" },
	{ "ampeg", NULL, "Dcy Open", "decay_open", "Decay Open", NULL },
	{ "ampeg", NULL, "Slope", "slope", "Slope", NULL },
	{ "ampeg", NULL, "RepeatCt", "repeat", "Repeat", NULL },

	{ "mod", "Mod EG", "Decay", "eg_decay", "Mod EG Decay", NULL },
	{ "mod", "Veloc EG", "Slope", "eg_slope", "Mod EG Slope", NULL },
	{ "mod", "PitchMod", "Amount", "pitch_amount", "Pitch Mod", NULL },
	{ "mod", "Velocity", "DstParam", "vel_dest", "Velo Dest", NULL },
	{ "mod", "Velocity", "Amount", "vel_amount", "Velo Amount", NULL },
	{ "mod", "Velocity", "Vol mod", "vel_vol", "Velo Vol Mod", NULL },

	{ "fm", NULL, "Amount", "amount", "FM Amount", NULL },
	{ "fm", NULL, "Frequncy", "freq", "FM Freq", NULL },
	{ "fm", NULL, "Waveform", "wave", "FM Wave", NULL },
	{ "fm", NULL, "Mode", "mode", "FM Mode", NULL },

	{ "modosc", NULL, "Freqcy 1", "freq1", "Freq 1", NULL },
	{ "modosc", NULL, "Freqcy 2", "freq2", "Freq 2", NULL },
	{ "modosc", NULL, "Gain 1", "gain1", "Gain 1", NULL },
	{ "modosc", NULL, "Gain 2", "gain2", "Gain 2", NULL },
	{ "modosc", NULL, "Waveform", "wave", "Wave", NULL },

	{ "trans", NULL, "Waveform", "wave", "Waveform", NULL },
	{ "trans", NULL, "Volume", "volume", "Volume", NULL },
	{ "trans", NULL, "Frequncy", "freq", "Frequency", NULL },

	{ "filter", NULL, "Frequncy", "cutoff", "Cutoff", "param:cutoffFrequency" },
	{ "filter", NULL, "Resnance", "resonance", "Resonance", "param:resonance" },
	{ "filter", NULL, "Type", "type", "Type", NULL },
	{ "filter", NULL, "Overdriv", "drive", "Drive", NULL },

	{ "lfo", NULL, "Frequncy", "freq", "Frequency", NULL },
	{ "lfo", NULL, "ClockSnc", "sync", "Clock Sync", NULL },
	{ "lfo", NULL, "Amount", "amount", "Amount", NULL },
	{ "lfo", NULL, "Waveform", "wave", "Waveform", NULL },
	{ "lfo", NULL, "Retriggr", "retrigger", "Retrigger", NULL },
	{ "lfo", NULL, "Offset", "offset", "Offset", NULL },
	{ "lfo", NULL, "DstVoice", "dest_voice", "Dest Voice", NULL },
	{ "lfo", NULL, "DstParam", "dest_param", "Dest Param", NULL },

	{ "mix", NULL, "Volume", "volume", "Volume", NULL },
	{ "mix", NULL, "Panning", "pan", "Pan", NULL },
	{ "mix", NULL, "SampleRt", "sample_rate", "Sample Rate", NULL },
	{ "mix", NULL, "Overdriv", "drive", "Drive", NULL },
	{ "mix", NULL, "AudioOut", "out", "Audio Out", NULL },
};

// the FX bus (its page depends on the FX type: all its parameters, the
// type specific ones after the common ones; P1-P4 mean something else per
// type) and the master parameters (globals, not in kits)
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
};

// ---- the ports ----

typedef struct
{
	char sym[64], name[64], label[64];	// label: in modulation target names
	const char *designation, *comment;
	uint16_t param;
	uint8_t kind, min, max, center, def;
	int group;
	int index;				// in the group
} Port;

typedef struct
{
	char sym[32], name[64], abbr[32];
	const char *cls;
} Group;

static Port ports[MAX_PORTS];
static int numPorts;
static Group groups[MAX_GROUPS];
static int numGroups;

static int addGroup (const char *sym, const char *name, const char *abbr, const char *cls)
{
	if (numGroups >= MAX_GROUPS)
	{
		fprintf (stderr, "gen_ttl: too many groups\n");
		exit (1);
	}
	Group *g = &groups[numGroups];
	snprintf (g->sym, sizeof g->sym, "%s", sym);
	snprintf (g->name, sizeof g->name, "%s", name);
	snprintf (g->abbr, sizeof g->abbr, "%s", abbr);
	g->cls = cls;
	return numGroups++;
}

static Port *addPort (int group, uint16_t param, const char *sym, const char *name, const char *designation)
{
	if (numPorts >= MAX_PORTS)
	{
		fprintf (stderr, "gen_ttl: too many ports\n");
		exit (1);
	}
	Port *p = &ports[numPorts++];
	memset (p, 0, sizeof *p);
	snprintf (p->sym, sizeof p->sym, "%s", sym);
	snprintf (p->name, sizeof p->name, "%s", name);
	p->designation = designation;
	p->param = param;
	p->group = group;
	p->kind = lxr_engine_param_kind (param);
	lxr_engine_param_range (param, &p->min, &p->max);
	p->center = p->kind == LXR_KIND_SIGNED ? lxr_engine_param_center (param) : 0;
	p->def = lxr_engine_get_parameter (param);
	return p;
}

static const struct naming *findNaming (const char *group, uint16_t param)
{
	const char *cat = lxr_engine_param_name (param, LXR_NAME_CATEGORY);
	const char *name = lxr_engine_param_name (param, LXR_NAME_LONG);
	for (unsigned i = 0; i < sizeof namings / sizeof namings[0]; i++)
	{
		const struct naming *n = &namings[i];
		if (!strcmp (n->group, group) && !strcmp (n->longName, name) && (!n->category || !strcmp (n->category, cat)))
		{
			return n;
		}
	}
	return NULL;
}

static int portOfParam (uint16_t param)
{
	for (int i = 0; i < numPorts; i++)
	{
		if (ports[i].param == param)
		{
			return i;
		}
	}
	return -1;
}

static void collect (void)
{
	for (uint8_t v = 0; v < LXR_ENGINE_SLOTS; v++)
	{
		const struct voice *voice = &voices[v];
		for (uint8_t s = 0; s < LXR_ENGINE_SUB_PAGES; s++)
		{
			const uint8_t sub = subOrder[s];
			uint16_t params[16];
			int n = 0;
			for (uint8_t k = 0; k < 8; k++)
			{
				const uint16_t par = lxr_engine_page_param (v, sub, k);
				if (par && *lxr_engine_param_name (par, LXR_NAME_LONG))
				{
					params[n++] = par;
				}
			}
			// the open hi-hat (page 6, the 2nd trigger of slot 6):
			// its own parameters (its decay) go into the hi-hat's groups
			if (v == LXR_ENGINE_SLOTS - 1)
			{
				for (uint8_t k = 0; k < 8; k++)
				{
					const uint16_t par = lxr_engine_page_param (LXR_ENGINE_PAGES - 1, sub, k);
					int known = 0;
					for (uint8_t q = 0; q < LXR_ENGINE_SUB_PAGES && !known; q++)
					{
						for (uint8_t j = 0; j < 8 && !known; j++)
						{
							known = lxr_engine_page_param (v, q, j) == par;
						}
					}
					if (par && !known && *lxr_engine_param_name (par, LXR_NAME_LONG))
					{
						params[n++] = par;
					}
				}
			}
			if (!n)
			{
				continue;
			}

			const char *gname, *gsym, *cls;
			subGroup (sub, params[0], &gname, &gsym, &cls);
			char sym[32], name[64];
			snprintf (sym, sizeof sym, "%s_%s", voice->sym, gsym);
			snprintf (name, sizeof name, "%s %s", voice->name, gname);
			const int g = addGroup (sym, name, voice->abbr, cls);

			const int first = numPorts;
			for (int i = 0; i < n; i++)
			{
				const struct naming *nm = findNaming (gsym, params[i]);
				if (!nm)
				{
					fprintf (stderr, "gen_ttl: no name for parameter %u (%s / %s) in group %s\n", params[i],
						 lxr_engine_param_name (params[i], LXR_NAME_CATEGORY),
						 lxr_engine_param_name (params[i], LXR_NAME_LONG), sym);
					exit (1);
				}
				Port *p = addPort (g, params[i], nm->sym, nm->name, nm->designation);
				p->index = i;
			}
			// number duplicates within the group
			for (int i = first; i < numPorts; i++)
			{
				int count = 0, nr = 0;
				for (int j = first; j < numPorts; j++)
				{
					if (!strcmp (ports[i].sym, ports[j].sym))
					{
						count++;
						nr += j <= i;
					}
				}
				if (count > 1)
				{
					// all of them, from the last one back (the
					// others still compare equal)
					char base[64], bname[64];
					snprintf (base, sizeof base, "%s", ports[i].sym);
					snprintf (bname, sizeof bname, "%s", ports[i].name);
					for (int j = first, k = 1; j < numPorts; j++)
					{
						if (!strcmp (ports[j].sym, base))
						{
							snprintf (ports[j].sym, sizeof ports[j].sym, "%s%d", base, k);
							snprintf (ports[j].name, sizeof ports[j].name, "%s %d", bname, k);
							k++;
						}
					}
					(void) nr;
				}
			}
		}
	}

	int g = addGroup ("fx", "FX Bus", "FX", NULL);
	for (unsigned i = 0; i < sizeof fxParams / sizeof fxParams[0]; i++)
	{
		Port *p = addPort (g, fxParams[i].param, fxParams[i].sym, fxParams[i].name, NULL);
		p->comment = fxParams[i].comment;
		p->index = (int) i;
	}
	g = addGroup ("master", "Master", "Master", NULL);
	for (unsigned i = 0; i < sizeof masterParams / sizeof masterParams[0]; i++)
	{
		Port *p = addPort (g, masterParams[i].param, masterParams[i].sym, masterParams[i].name, NULL);
		p->comment = masterParams[i].comment;
		p->index = (int) i;
	}

	// full symbols (group symbol prefix), labels, checks
	for (int i = 0; i < numPorts; i++)
	{
		Port *p = &ports[i];
		char sym[64];
		snprintf (sym, sizeof sym, "%s_%s", groups[p->group].sym, p->sym);
		snprintf (p->sym, sizeof p->sym, "%s", sym);
		snprintf (p->label, sizeof p->label, "%s %s", groups[p->group].name, p->name);
		if (p->max <= p->min)
		{
			fprintf (stderr, "gen_ttl: parameter %u (%s) has no range\n", p->param, p->sym);
			exit (1);
		}
		if (p->designation && (p->kind != LXR_KIND_NUMBER || p->min != 0))
		{
			fprintf (stderr, "gen_ttl: %s: a designation needs a plain number from 0\n", p->sym);
			exit (1);
		}
		for (const char *c = p->sym; *c; c++)
		{
			if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
			{
				fprintf (stderr, "gen_ttl: bad symbol %s\n", p->sym);
				exit (1);
			}
		}
		for (int j = 0; j < i; j++)
		{
			if (!strcmp (ports[j].sym, p->sym) || ports[j].param == p->param)
			{
				fprintf (stderr, "gen_ttl: %s: duplicate symbol or parameter (%s)\n", p->sym, ports[j].sym);
				exit (1);
			}
		}
	}
}

// ---- output ----

#define PORT_CONTROL0	3	// after the MIDI input and the two audio outputs

static void ttlString (FILE *f, const char *s)
{
	fputc ('"', f);
	for (; *s; s++)
	{
		if (*s == '"' || *s == '\\')
		{
			fputc ('\\', f);
		}
		fputc (*s, f);
	}
	fputc ('"', f);
}

static float portValue (const Port *p, int value)
{
	return (float) (value - p->center);
}

// the label of a modulation target (an index into modTargets): voice and
// the port's group and name; numbered when a label repeats
static void targetLabel (uint8_t target, char *text, unsigned size)
{
	if (target == 0)
	{
		snprintf (text, size, "Off");
		return;
	}
	const uint16_t param = modTargets[target].param;
	const uint8_t voice = voiceFromModTargValue (target);
	const int port = portOfParam (param);
	if (port >= 0)
	{
		// "BD Osc Coarse": voice, group (without the voice), name; the
		// group left out where the name says it ("FM Amount", "Pitch Mod")
		const Port *p = &ports[port];
		const Group *g = &groups[p->group];
		const char *vabbr = voice ? voices[voice - 1].abbr : "All";
		const char *gname = g->name;
		if (voice && !strncmp (gname, voices[voice - 1].name, strlen (voices[voice - 1].name)))
		{
			gname += strlen (voices[voice - 1].name) + 1;
		}
		if (!strcmp (gname, "Mod") || !strcmp (gname, "FX Bus") || !strncmp (p->name, gname, strlen (gname)))
		{
			snprintf (text, size, "%s %s", vabbr, p->name);
		}
		else
		{
			snprintf (text, size, "%s %s %s", vabbr, gname, p->name);
		}
		return;
	}
	if (target == 1)
	{
		snprintf (text, size, "All Sample Rate");	// PAR_VOICE_DECIMATION_ALL, no port
		return;
	}
	char t[32];
	lxr_engine_param_text (149, target, t, sizeof t);	// a velocity destination: the engine's text
	snprintf (text, size, "%s %s", voice ? voices[voice - 1].abbr : "All", t);
}

static void writeScalePoints (FILE *f, const Port *p)
{
	char labels[256][64];
	const int n = p->max - p->min + 1;
	const int isTarget = (p->param >= 149 && p->param <= 154) || (p->param >= 167 && p->param <= 172);
	for (int v = p->min; v <= p->max; v++)
	{
		char *t = labels[v - p->min];
		if (isTarget)
		{
			targetLabel ((uint8_t) v, t, 64);
		}
		else
		{
			lxr_engine_param_text (p->param, (uint8_t) v, t, 64);
		}
	}
	fprintf (f, " ;\n\t\tlv2:scalePoint ");
	for (int i = 0; i < n; i++)
	{
		int dup = 0;
		for (int j = 0; j < n; j++)
		{
			dup |= j != i && !strcmp (labels[i], labels[j]);
		}
		char label[80];
		if (dup)
		{
			snprintf (label, sizeof label, "%s (%d)", labels[i], p->min + i);
		}
		else
		{
			snprintf (label, sizeof label, "%s", labels[i]);
		}
		fprintf (f, "%s[ rdfs:label ", i ? " , " : "");
		ttlString (f, label);
		fprintf (f, " ; rdf:value %d ]", (int) portValue (p, p->min + i));
	}
}

static void writePlugin (FILE *f)
{
	fprintf (f,
		 "# generated by tools/gen_ttl.c from the drum engine's tables: do not edit\n"
		 "@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n"
		 "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
		 "@prefix foaf: <http://xmlns.com/foaf/0.1/> .\n"
		 "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
		 "@prefix midi: <http://lv2plug.in/ns/ext/midi#> .\n"
		 "@prefix param: <http://lv2plug.in/ns/ext/parameters#> .\n"
		 "@prefix pg: <http://lv2plug.in/ns/ext/port-groups#> .\n"
		 "@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
		 "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
		 "@prefix time: <http://lv2plug.in/ns/ext/time#> .\n"
		 "@prefix urid: <http://lv2plug.in/ns/ext/urid#> .\n"
		 "\n");

	// the groups, lv2:displayPriority as Zynthian reads it (lv2core
	// namespace): the first group highest
	fprintf (f,
		 "<" URI "#out>\n"
		 "\ta pg:StereoGroup , pg:OutputGroup ;\n"
		 "\tlv2:symbol \"out\" ;\n"
		 "\tlv2:name \"Output\" .\n\n");
	for (int g = 0; g < numGroups; g++)
	{
		fprintf (f, "<" URI "#%s>\n\ta pg:InputGroup%s%s ;\n\tlv2:symbol \"%s\" ;\n\tlv2:name ", groups[g].sym,
			 groups[g].cls ? " , " : "", groups[g].cls ? groups[g].cls : "", groups[g].sym);
		ttlString (f, groups[g].name);
		fprintf (f, " ;\n\tlv2:displayPriority %d .\n\n", numGroups - g);
	}

	fprintf (f,
		 "<" URI ">\n"
		 "\ta lv2:Plugin , lv2:InstrumentPlugin , doap:Project ;\n"
		 "\tdoap:name \"" NAME "\" ;\n"
		 "\tlv2:project <" URI "> ;\n"
		 "\tdoap:license <https://spdx.org/licenses/GPL-3.0-only> ;\n"
		 "\tdoap:maintainer [\n"
		 "\t\tfoaf:name \"Hannes Janetzek\" ;\n"
		 "\t\tfoaf:mbox <mailto:hannes.janetzek@gmail.com> ;\n"
		 "\t\tfoaf:homepage <https://github.com/hjanetzek/mdt-lv2>\n"
		 "\t] ;\n"
		 "\tdoap:homepage <https://github.com/hjanetzek/mdt-lv2> ;\n"
		 "\tdoap:shortdesc \"Synthesized drums\" ;\n"
		 "\tlv2:minorVersion %d ;\n"
		 "\tlv2:microVersion %d ;\n"
		 "\trdfs:comment \"Synthesized drums: six voices (bass drum, two toms, snare, cymbal, hi-hat closed and open) "
		 "from MicroDexed-touch's drum engine, the voices of the OpenElixir-02 firmware (based on the open source "
		 "LXR V2 firmware by Erica Synths). General MIDI drum notes, every voice parameter as a control.\" ;\n"
		 "\tlv2:requiredFeature urid:map ;\n"
		 "\tlv2:optionalFeature lv2:hardRTCapable , lv2:isLive ;\n"
		 "\tpg:mainOutput <" URI "#out> ;\n",
		 MINOR_VERSION, MICRO_VERSION);

	fprintf (f,
		 "\tlv2:port [\n"
		 "\t\ta lv2:InputPort , atom:AtomPort ;\n"
		 "\t\tatom:bufferType atom:Sequence ;\n"
		 "\t\tatom:supports midi:MidiEvent , time:Position ;\n"
		 "\t\tlv2:designation lv2:control ;\n"
		 "\t\tlv2:index 0 ;\n"
		 "\t\tlv2:symbol \"midi_in\" ;\n"
		 "\t\tlv2:name \"MIDI In\"\n"
		 "\t] , [\n"
		 "\t\ta lv2:OutputPort , lv2:AudioPort ;\n"
		 "\t\tlv2:index 1 ;\n"
		 "\t\tlv2:symbol \"out_l\" ;\n"
		 "\t\tlv2:name \"Left\" ;\n"
		 "\t\tpg:group <" URI "#out> ;\n"
		 "\t\tlv2:designation pg:left\n"
		 "\t] , [\n"
		 "\t\ta lv2:OutputPort , lv2:AudioPort ;\n"
		 "\t\tlv2:index 2 ;\n"
		 "\t\tlv2:symbol \"out_r\" ;\n"
		 "\t\tlv2:name \"Right\" ;\n"
		 "\t\tpg:group <" URI "#out> ;\n"
		 "\t\tlv2:designation pg:right\n"
		 "\t]");

	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		fprintf (f, " , [\n\t\ta lv2:InputPort , lv2:ControlPort ;\n\t\tlv2:index %d ;\n\t\tlv2:symbol \"%s\" ;\n\t\tlv2:name ",
			 PORT_CONTROL0 + i, p->sym);
		ttlString (f, p->name);
		if (p->comment)
		{
			fprintf (f, " ;\n\t\trdfs:comment ");
			ttlString (f, p->comment);
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
	fprintf (f, " .\n");
}

static void writeManifest (FILE *f)
{
	fprintf (f,
		 "# generated by tools/gen_ttl.c: do not edit\n"
		 "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
		 "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
		 "\n"
		 "<" URI ">\n"
		 "\ta lv2:Plugin , lv2:InstrumentPlugin ;\n"
		 "\tlv2:binary <" BINARY "> ;\n"
		 "\trdfs:seeAlso <" TTL "> .\n");
}

static int isModTargetParam (uint16_t param)
{
	return (param >= 149 && param <= 154) || (param >= 167 && param <= 172);
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
		 "#define EDS_NUM_PORTS\t\t(EDS_PORT_CONTROL0 + EDS_NUM_CONTROLS)\n"
		 "\n"
		 "/* control port i (index EDS_PORT_CONTROL0 + i): engine parameter param,\n"
		 "   value = round (port value) + offset, in min..max; target: a modulation\n"
		 "   target (set after the other parameters); def: the port's default */\n"
		 "typedef struct\n"
		 "{\n"
		 "\tuint16_t param;\n"
		 "\tuint8_t min, max;\n"
		 "\tint8_t offset;\n"
		 "\tuint8_t target;\n"
		 "\tfloat def;\n"
		 "} eds_control;\n"
		 "\n"
		 "static const eds_control eds_controls[EDS_NUM_CONTROLS] = {\n",
		 PORT_CONTROL0, numPorts);
	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		fprintf (f, "\t{ %3u, %u, %3u, %2d, %d, %6.1ff },\t/* %3d %s */\n", p->param, p->min, p->max, p->center,
			 isModTargetParam (p->param), portValue (p, p->def), PORT_CONTROL0 + i, p->sym);
	}
	fprintf (f, "};\n\n#endif\n");
}

static FILE *openOut (const char *dir, const char *name)
{
	char path[4096];
	if (dir)
	{
		snprintf (path, sizeof path, "%s/%s", dir, name);
	}
	else
	{
		snprintf (path, sizeof path, "%s", name);
	}
	FILE *f = fopen (path, "w");
	if (!f)
	{
		perror (path);
		exit (1);
	}
	return f;
}

int main (int argc, char **argv)
{
	lxr_engine_init ();
	collect ();

	if (argc == 2 && !strcmp (argv[1], "--symbols"))
	{
		for (int i = 0; i < numPorts; i++)
		{
			printf ("%s\n", ports[i].sym);
		}
		return 0;
	}
	if (argc == 2 && !strcmp (argv[1], "--pages"))
	{
		// the Zynthian pages: 4 controls per page per group
		for (int g = 0; g < numGroups; g++)
		{
			int n = 0;
			for (int i = 0; i < numPorts; i++)
			{
				n += ports[i].group == g;
			}
			printf ("%-20s %2d controls, %d page%s\n", groups[g].name, n, (n + 3) / 4, n > 4 ? "s" : "");
		}
		printf ("%d groups, %d controls\n", numGroups, numPorts);
		return 0;
	}
	if (argc != 3)
	{
		fprintf (stderr, "usage: gen_ttl BUNDLE_DIR PORTS_H | --symbols | --pages\n");
		return 2;
	}

	FILE *f = openOut (argv[1], "manifest.ttl");
	writeManifest (f);
	fclose (f);
	f = openOut (argv[1], TTL);
	writePlugin (f);
	fclose (f);
	f = openOut (NULL, argv[2]);
	writePortsH (f);
	fclose (f);
	return 0;
}
