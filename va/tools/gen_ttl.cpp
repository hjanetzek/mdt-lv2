/*
 * gen_ttl.cpp - writes the mTrio and mAnalog LV2 bundles' TTL (manifest.ttl,
 * <name>.ttl, presets.ttl) and the plugins' port tables (src/ports_<name>.h)
 * from MicroDexed-touch's parameter tables (tools/mdt_tables.h: the editors,
 * the boot defaults, the preset loaders) and the engines' own tables (wave
 * names, presets: engine/).
 *
 *   gen_ttl trio|analog BUNDLE_DIR PORTS_H   write the files
 *   gen_ttl trio|analog --symbols            list the control port symbols
 *   gen_ttl trio|analog --pages              the Zynthian pages
 *
 * The outputs are committed (cross builds don't run this program); a test
 * regenerates them and compares. Port symbols are API (Zynthian presets,
 * snapshots and MIDI learn store them): they come from the layout tables
 * below, not from MDT's editor labels, and must not change once released
 * (tests/symbols_<name>.txt).
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "analog_engine.h"
#include "mdt_tables.h"
#include "params.h"
#include "trio_engine.h"
#include "ttl.h"	// common/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINOR_VERSION	0
#define MICRO_VERSION	1
#define PORT_CONTROL0	3	// after the MIDI input and the two audio outputs

// ---- the plugins ----

// a control port: an MDT editor (label) or one of the plugin's own
// (label NULL: param, range and default here); the symbol is
// <group>_<sym>
struct Layout
{
	const char *group;	// group symbol
	const char *label;	// MDT editor label, NULL: the plugin's own
	const char *sym, *name, *designation, *unit, *comment;
	int param, min, max, def;	// for the plugin's own ports
};

struct GroupDef
{
	const char *sym, *name, *cls;
};

static const GroupDef groupDefs[] = {
	{ "osc", "Osc", "param:OscillatorControls" },
	{ "filter", "Filter", "param:FilterControls" },
	{ "ampeg", "Amp EG", "param:EnvelopeControls" },
	{ "mod", "Mod", NULL },
	{ "master", "Master", NULL },
};

#define VELOCITY_PORT                                                                                     \
	{ "mod", NULL, "velocity", "Velocity", NULL, "units:pc",                                          \
	  "velocity to level and cutoff; 0: as MicroDexed-touch (velocity ignored)", VA_VELOCITY, 0, 100, 0 }
#define BEND_PORT                                                                                         \
	{ "master", NULL, "bend_range", "Bend Range", NULL, "units:semitone12TET", "pitch bend range",    \
	  VA_BEND_RANGE, 0, 24, 2 }
#define AMPEG_PORTS                                                                                       \
	{ "ampeg", "ATK", "attack", "Attack", "param:attack", NULL, NULL, 0, 0, 0, 0 },                   \
	{ "ampeg", "DEC", "decay", "Decay", "param:decay", NULL, NULL, 0, 0, 0, 0 },                      \
	{ "ampeg", "SUS", "sustain", "Sustain", "param:sustain", NULL,                                    \
	  "as MicroDexed-touch: the full level from 50 up", 0, 0, 0, 0 },                                 \
	{ "ampeg", "REL", "release", "Release", "param:release", NULL, NULL, 0, 0, 0, 0 }
#define MASTER_PORTS                                                                                      \
	{ "master", "LEVEL", "level", "Level", NULL, "units:pc", NULL, 0, 0, 0, 0 },                      \
	{ "master", "PAN", "pan", "Pan", NULL, NULL, "negative: left, positive: right", 0, 0, 0, 0 },     \
	{ "master", "TRANSP", "transpose", "Transpose", NULL, "units:semitone12TET", NULL, 0, 0, 0, 0 }, \
	BEND_PORT

static const Layout trioLayout[] = {
	{ "osc", "WAVE", "wave", "Wave", NULL, NULL, "three oscillators: the wave set", 0, 0, 0, 0 },
	{ "osc", "INT2", "interval2", "Interval 2", NULL, "units:semitone12TET", "oscillator 2", 0, 0, 0, 0 },
	{ "osc", "INT3", "interval3", "Interval 3", NULL, "units:semitone12TET", "oscillator 3", 0, 0, 0, 0 },
	{ "osc", "DTN", "detune", "Detune", NULL, "units:cent", "oscillator 2 up, 3 down", 0, 0, 0, 0 },
	{ "filter", "CUT", "cutoff", "Cutoff", "param:cutoffFrequency", NULL, NULL, 0, 0, 0, 0 },
	{ "filter", "RES", "resonance", "Resonance", "param:resonance", NULL, NULL, 0, 0, 0, 0 },
	{ "filter", "MODE", "mode", "Mode", NULL, NULL, NULL, 0, 0, 0, 0 },
	AMPEG_PORTS,
	{ "mod", "PW", "pw", "Pulse Width", NULL, NULL, NULL, 0, 0, 0, 0 },
	{ "mod", "ENV>PW", "env_pw", "Env to PW", NULL, NULL, "amp envelope to pulse width", 0, 0, 0, 0 },
	{ "mod", "ENV>CUT", "env_cutoff", "Env to Cutoff", NULL, NULL, "amp envelope to cutoff", 0, 0, 0, 0 },
	VELOCITY_PORT,
	MASTER_PORTS,
};

static const Layout analogLayout[] = {
	{ "osc", "WAVE", "wave", "Wave", NULL, NULL, "both oscillators", 0, 0, 0, 0 },
	{ "osc", "DTN", "detune", "Detune", NULL, "units:cent", "oscillator 2", 0, 0, 0, 0 },
	{ "osc", "MIX", "mix", "Osc Mix", NULL, NULL, "0: oscillator 1, 127: oscillator 2", 0, 0, 0, 0 },
	{ "osc", "NOIS", "noise", "Noise", NULL, NULL, NULL, 0, 0, 0, 0 },
	{ "filter", "CUT", "cutoff", "Cutoff", "param:cutoffFrequency", NULL, NULL, 0, 0, 0, 0 },
	{ "filter", "RES", "resonance", "Resonance", "param:resonance", NULL, NULL, 0, 0, 0, 0 },
	{ "filter", "DRV", "drive", "Drive", NULL, NULL, NULL, 0, 0, 0, 0 },
	{ "filter", "KTR", "key_track", "Key Track", NULL, NULL,
	  "as MicroDexed-touch: lowers the cutoff by a fixed amount (it does not follow the key)", 0, 0, 0, 0 },
	AMPEG_PORTS,
	{ "mod", "ENV>CUT", "env_cutoff", "Env to Cutoff", NULL, NULL, "amp envelope to cutoff", 0, 0, 0, 0 },
	{ "mod", "ENV>PW", "env_pw", "Env to PW", NULL, NULL, "amp envelope to pulse width (wave PWM)", 0, 0, 0, 0 },
	VELOCITY_PORT,
	MASTER_PORTS,
};

// display names of the engines' enumerations (MDT's short names in the
// engine tables -> labels); unknown names stop the generator
static const struct
{
	const char *mdt, *label;
} enumNames[] = {
	// N_TRIO_WAVE (the wave sets: TRIO_SETS in trio_engine.cpp)
	{ "SAW3", "Saw x3" }, { "PLS3", "Pulse x3" }, { "PPT", "Pulse Pulse Tri" }, { "SST", "Saw Saw Tri" },
	{ "TRI3", "Tri x3" }, { "S&T", "Saw&Tri" }, { "P&S", "Pulsed Saw" }, { "P+N", "Pulse + Noise" },
	{ "NOIS", "Noise" }, { "SYNC", "Sync Saw" }, { "SYNCP", "Sync Pulse" }, { "SYNC3", "Sync Saw + Tri" },
	{ "RING", "Ring" }, { "RING3", "Ring + Tri" }, { "R+S", "Ring + Sync Saw" }, { "R+SP", "Ring + Sync Pulse" },
	// N_TRIO_MODE
	{ "LP", "Low Pass" }, { "BP", "Band Pass" }, { "HP", "High Pass" }, { "NOT", "Notch" },
	// N_ANALOG_WAVE
	{ "SAW", "Saw" }, { "SQR", "Square" }, { "TRI", "Triangle" }, { "SIN", "Sine" }, { "PWM", "PWM" },
};

struct Synth
{
	const char *key;		// command line
	int analog;
	const char *name, *sym;		// doap:name, bundle / binary / symbol prefix
	const char *shortdesc, *comment;
	const Layout *layout;
	int numLayout;
	const mdt_editor *editors;
	int numEditors;
	const preset_t *presets;
	int numPresets;
};

static const Synth synths[] = {
	{ "trio", 0, "mTrio", "mtrio", "Three oscillator synth",
	  "MicroDexed-touch's Trio: three oscillators per voice in 16 wave sets (saws, pulses, triangles, "
	  "noise, hard sync, ring modulation), a state variable filter (low pass, band pass, high pass, notch), "
	  "an ADSR envelope; eight voices.",
	  trioLayout, (int) NELEM (trioLayout), MDT_TRIO_EDITORS, (int) NELEM (MDT_TRIO_EDITORS), TRIO_PRESETS,
	  TRIO_NPRESETS },
	{ "analog", 1, "mAnalog", "manalog", "Two oscillator analog style synth",
	  "MicroDexed-touch's Analog: two band limited oscillators (saw, square, triangle, sine, PWM) with "
	  "detune and noise, drive, a low pass state variable filter, an ADSR envelope; eight voices.",
	  analogLayout, (int) NELEM (analogLayout), MDT_ANALOG_EDITORS, (int) NELEM (MDT_ANALOG_EDITORS),
	  ANALOG_PRESETS, ANALOG_NPRESETS },
};

// ---- the ports ----

struct Port
{
	char sym[64];
	const Layout *l;
	int group, index;	// group, index in the group
	int param;
	int min, max;		// the engine's (MDT's) range
	int offset;		// port value = engine value - offset
	int def;		// engine value
	const char *const *names;	// enumeration labels (MDT's), or NULL
};

static const Synth *synth;
static char uri[128];
static Port ports[64];
static int numPorts;
static int groupOf[NELEM (groupDefs)];	// groupDefs index -> used group number, -1
static int numGroups;
static int defaults[VA_NUM_PARAMS];

static const mdt_editor *findEditor (const char *label)
{
	for (int i = 0; i < synth->numEditors; i++)
	{
		if (!strcmp (synth->editors[i].label, label))
		{
			return &synth->editors[i];
		}
	}
	fprintf (stderr, "gen_ttl: no MDT editor %s\n", label);
	exit (1);
}

static int groupIndex (const char *sym)
{
	for (unsigned g = 0; g < NELEM (groupDefs); g++)
	{
		if (!strcmp (groupDefs[g].sym, sym))
		{
			return (int) g;
		}
	}
	fprintf (stderr, "gen_ttl: no group %s\n", sym);
	exit (1);
}

static const char *enumLabel (const char *mdt)
{
	for (unsigned i = 0; i < NELEM (enumNames); i++)
	{
		if (!strcmp (enumNames[i].mdt, mdt))
		{
			return enumNames[i].label;
		}
	}
	fprintf (stderr, "gen_ttl: no label for %s\n", mdt);
	exit (1);
}

static void collect (void)
{
	snprintf (uri, sizeof uri, TTL_HOMEPAGE "/%s", synth->sym);
	mdt_boot_defaults (synth->analog, &synth->presets[0], defaults);
	for (unsigned g = 0; g < NELEM (groupDefs); g++)
	{
		groupOf[g] = -1;
	}

	int count[NELEM (groupDefs)] = { 0 };
	for (int i = 0; i < synth->numLayout; i++)
	{
		const Layout *l = &synth->layout[i];
		Port *p = &ports[numPorts++];
		memset (p, 0, sizeof *p);
		p->l = l;
		const int g = groupIndex (l->group);
		if (groupOf[g] < 0)
		{
			groupOf[g] = numGroups++;
		}
		p->group = g;
		p->index = count[g]++;
		snprintf (p->sym, sizeof p->sym, "%s_%s", l->group, l->sym);
		if (l->label)
		{
			const mdt_editor *e = findEditor (l->label);
			if (e->param == MDT_NONE)
			{
				fprintf (stderr, "gen_ttl: editor %s is not exposed\n", l->label);
				exit (1);
			}
			p->param = e->param;
			p->min = e->min;
			p->max = e->max;
		}
		else
		{
			p->param = l->param;
			p->min = l->min;
			p->max = l->max;
		}
		p->def = defaults[p->param];
		p->offset = p->param == VA_PAN ? 20 : 0;	// MDT's pan 0..40: -20..20
		if (p->param == VA_P_E0)
		{
			p->names = synth->analog ? N_ANALOG_WAVE : N_TRIO_WAVE;
		}
		else if (!synth->analog && p->param == VA_P_E4)
		{
			p->names = N_TRIO_MODE;
		}

		// checks
		if (p->max <= p->min || p->def < p->min || p->def > p->max)
		{
			fprintf (stderr, "gen_ttl: %s: bad range or default\n", p->sym);
			exit (1);
		}
		if (l->designation && (p->names || p->min != 0 || p->offset))
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
		for (int j = 0; j < numPorts - 1; j++)
		{
			if (!strcmp (ports[j].sym, p->sym) || ports[j].param == p->param)
			{
				fprintf (stderr, "gen_ttl: %s: duplicate symbol or parameter\n", p->sym);
				exit (1);
			}
		}
	}
	// every exposed editor has a port
	for (int i = 0; i < synth->numEditors; i++)
	{
		int found = synth->editors[i].param == MDT_NONE;
		for (int j = 0; j < numPorts && !found; j++)
		{
			found = ports[j].param == synth->editors[i].param;
		}
		if (!found)
		{
			fprintf (stderr, "gen_ttl: MDT editor %s has no port\n", synth->editors[i].label);
			exit (1);
		}
	}
}

static int portValue (const Port *p, int value)
{
	return value - p->offset;
}

// preset labels where title case of MDT's name doesn't read well
static const struct
{
	const char *mdt, *label;
} presetLabels[] = {
	{ "PWM STR", "PWM Strings" },
	{ "SQR BASS", "Square Bass" },
};

// preset URI part and label from MDT's name: "FAT BASS" -> fat_bass, "Fat Bass"
static void presetNames (const char *mdt, char *sym, size_t symSize, char *label, size_t labelSize)
{
	for (unsigned i = 0; i < NELEM (presetLabels); i++)
	{
		if (!strcmp (presetLabels[i].mdt, mdt))
		{
			char tmp[64];
			snprintf (tmp, sizeof tmp, "%s", mdt);
			for (char *c = tmp; *c; c++)
			{
				*c = *c == ' ' ? '_' : (*c >= 'A' && *c <= 'Z') ? (char) (*c - 'A' + 'a') : *c;
			}
			snprintf (sym, symSize, "%s", tmp);
			snprintf (label, labelSize, "%s", presetLabels[i].label);
			return;
		}
	}
	size_t s = 0, l = 0;
	int word = 1;
	for (const char *c = mdt; *c && s + 1 < symSize && l + 1 < labelSize; c++)
	{
		const char ch = *c;
		if (ch == ' ')
		{
			sym[s++] = '_';
			label[l++] = ' ';
			word = 1;
			continue;
		}
		sym[s++] = ch >= 'A' && ch <= 'Z' ? (char) (ch - 'A' + 'a') : ch;
		label[l++] = word || !(ch >= 'A' && ch <= 'Z') ? ch : (char) (ch - 'A' + 'a');
		word = 0;
	}
	sym[s] = 0;
	label[l] = 0;
}

// ---- output ----

static void writePlugin (FILE *f)
{
	fprintf (f, "# generated by tools/gen_ttl.cpp from MicroDexed-touch's tables: do not edit\n");
	ttl_prefixes (f, "atom doap foaf lv2 midi param pg rdf rdfs units urid");

	ttl_output_group (f, uri);
	for (unsigned g = 0; g < NELEM (groupDefs); g++)
	{
		if (groupOf[g] >= 0)
		{
			ttl_group (f, uri, groupDefs[g].sym, groupDefs[g].name, groupDefs[g].cls, numGroups - groupOf[g]);
		}
	}

	fprintf (f, "<%s>\n\ta lv2:Plugin , lv2:InstrumentPlugin , doap:Project ;\n\tdoap:name \"%s\" ;\n"
		    "\tlv2:project <%s> ;\n",
		 uri, synth->name, uri);
	ttl_project (f);
	fprintf (f, "\tdoap:shortdesc ");
	ttl_string (f, synth->shortdesc);
	fprintf (f, " ;\n\tlv2:minorVersion %d ;\n\tlv2:microVersion %d ;\n\trdfs:comment ", MINOR_VERSION,
		 MICRO_VERSION);
	ttl_string (f, synth->comment);
	fprintf (f, " ;\n"
		    "\tlv2:requiredFeature urid:map ;\n"
		    "\tlv2:optionalFeature lv2:hardRTCapable , lv2:isLive ;\n"
		    "\tpg:mainOutput <%s#out> ;\n",
		 uri);
	ttl_io_ports (f, uri, "midi:MidiEvent");

	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		const Layout *l = p->l;
		fprintf (f, " , [\n\t\ta lv2:InputPort , lv2:ControlPort ;\n\t\tlv2:index %d ;\n\t\tlv2:symbol \"%s\" ;\n\t\tlv2:name ",
			 PORT_CONTROL0 + i, p->sym);
		ttl_string (f, l->name);
		if (l->comment)
		{
			fprintf (f, " ;\n\t\trdfs:comment ");
			ttl_string (f, l->comment);
		}
		// integer ports with integer literals, the widget ports' (plain floats) with decimals
		fprintf (f, l->designation ? " ;\n\t\tlv2:default %d.0 ;\n\t\tlv2:minimum %d.0 ;\n\t\tlv2:maximum %d.0"
					   : " ;\n\t\tlv2:default %d ;\n\t\tlv2:minimum %d ;\n\t\tlv2:maximum %d",
			 portValue (p, p->def), portValue (p, p->min), portValue (p, p->max));
		if (p->names)
		{
			fprintf (f, " ;\n\t\tlv2:portProperty lv2:integer , lv2:enumeration ;\n\t\tlv2:scalePoint ");
			for (int v = p->min; v <= p->max; v++)
			{
				fprintf (f, "%s[ rdfs:label ", v > p->min ? " , " : "");
				ttl_string (f, enumLabel (p->names[v]));
				fprintf (f, " ; rdf:value %d ]", portValue (p, v));
			}
		}
		else if (!l->designation)
		{
			// widget ports stay plain floats (Zynthian's envelope and
			// filter widgets), the others step in integers
			fprintf (f, " ;\n\t\tlv2:portProperty lv2:integer");
		}
		if (l->unit)
		{
			fprintf (f, " ;\n\t\tunits:unit %s", l->unit);
		}
		if (l->designation)
		{
			fprintf (f, " ;\n\t\tlv2:designation %s", l->designation);
		}
		fprintf (f, " ;\n\t\tpg:group <%s#%s> ;\n\t\tlv2:displayPriority %d\n\t]", uri, l->group, 100 - p->index);
	}
	fprintf (f, " .\n");
}

static void writeManifest (FILE *f)
{
	char binary[64], ttl[64];
	snprintf (binary, sizeof binary, "%s.so", synth->sym);
	snprintf (ttl, sizeof ttl, "%s.ttl", synth->sym);
	fprintf (f, "# generated by tools/gen_ttl.cpp: do not edit\n");
	ttl_prefixes (f, "lv2 pset rdfs");
	ttl_manifest_plugin (f, uri, binary, ttl);

	// the presets (and their bank) are found through the manifest
	// (lv2:appliesTo the plugin), their port values in presets.ttl
	fprintf (f, "\n<%s#bank_factory>\n\ta pset:Bank ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label \"Factory\" ;\n"
		    "\trdfs:seeAlso <presets.ttl> .\n",
		 uri, uri);
	for (int i = 0; i < synth->numPresets; i++)
	{
		char sym[64], label[64];
		presetNames (synth->presets[i].name, sym, sizeof sym, label, sizeof label);
		fprintf (f, "\n<%s#preset_%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label ", uri, sym, uri);
		ttl_string (f, label);
		fprintf (f, " ;\n\trdfs:seeAlso <presets.ttl> .\n");
	}
}

static void writePresets (FILE *f)
{
	fprintf (f, "# generated by tools/gen_ttl.cpp from MicroDexed-touch's presets: do not edit\n");
	ttl_prefixes (f, "lv2 pset rdfs");
	fprintf (f, "<%s#bank_factory>\n\ta pset:Bank ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label \"Factory\" .\n", uri, uri);
	for (int i = 0; i < synth->numPresets; i++)
	{
		const preset_t *pr = &synth->presets[i];
		char sym[64], label[64];
		presetNames (pr->name, sym, sizeof sym, label, sizeof label);
		int values[VA_NUM_PARAMS], set[VA_NUM_PARAMS];
		mdt_apply_preset (synth->analog, pr, values, set);
		fprintf (f, "\n<%s#preset_%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label ", uri, sym, uri);
		ttl_string (f, label);
		fprintf (f, " ;\n\tpset:bank <%s#bank_factory> ;\n\tlv2:port ", uri);
		// the parameters MDT's preset loader sets, in port order (level,
		// pan, transpose and the plugin's own stay as they are)
		int first = 1;
		for (int k = 0; k < numPorts; k++)
		{
			const Port *p = &ports[k];
			if (!set[p->param])
			{
				continue;
			}
			fprintf (f, "%s[\n\t\tlv2:symbol \"%s\" ;\n\t\tpset:value %d.0\n\t]", first ? "" : " , ", p->sym,
				 portValue (p, values[p->param]));
			first = 0;
		}
		fprintf (f, " .\n");
	}
}

static void writePortsH (FILE *f)
{
	fprintf (f,
		 "/* generated by tools/gen_ttl.cpp from MicroDexed-touch's tables: do not edit */\n"
		 "#ifndef VA_PORTS_H_\n"
		 "#define VA_PORTS_H_\n"
		 "\n"
		 "#include \"params.h\"\n"
		 "\n"
		 "#include <stdint.h>\n"
		 "\n"
		 "#define VA_URI\t\t\t\"%s\"\n"
		 "#define VA_NAME\t\t\t\"%s\"\n"
		 "#define VA_PORT_MIDI_IN\t\t0\n"
		 "#define VA_PORT_OUT_L\t\t1\n"
		 "#define VA_PORT_OUT_R\t\t2\n"
		 "#define VA_PORT_CONTROL0\t%d\n"
		 "#define VA_NUM_CONTROLS\t\t%d\n"
		 "#define VA_NUM_PORTS\t\t(VA_PORT_CONTROL0 + VA_NUM_CONTROLS)\n"
		 "\n"
		 "/* control port i (index VA_PORT_CONTROL0 + i): voice layer parameter\n"
		 "   param (params.h), value = round (port value) + offset, in min..max;\n"
		 "   def: the port's default */\n"
		 "typedef struct\n"
		 "{\n"
		 "\tuint8_t param;\n"
		 "\tint8_t offset;\n"
		 "\tint16_t min, max;\n"
		 "\tfloat def;\n"
		 "} va_control;\n"
		 "\n"
		 "static const va_control va_controls[VA_NUM_CONTROLS] = {\n",
		 uri, synth->name, PORT_CONTROL0, numPorts);
	static const char *const paramNames[VA_NUM_PARAMS] = {
		"VA_P_E0", "VA_P_E1", "VA_P_E2", "VA_P_E3", "VA_P_E4", "VA_P_E5", "VA_P_E6", "VA_P_E7",
		"VA_ENV_TO_CUT", "VA_ENV_TO_PW", "VA_ATTACK", "VA_DECAY", "VA_SUSTAIN", "VA_RELEASE",
		"VA_LEVEL", "VA_PAN", "VA_TRANSPOSE", "VA_BEND_RANGE", "VA_VELOCITY",
	};
	for (int i = 0; i < numPorts; i++)
	{
		const Port *p = &ports[i];
		char param[32];
		snprintf (param, sizeof param, "%s,", paramNames[p->param]);
		fprintf (f, "\t{ %-15s %2d, %3d, %3d, %5d.0f },\t/* %2d %s */\n", param, p->offset, p->min, p->max,
			 portValue (p, p->def), PORT_CONTROL0 + i, p->sym);
	}
	fprintf (f, "};\n\n#endif\n");
}

int main (int argc, char **argv)
{
	for (unsigned i = 0; argc >= 2 && i < NELEM (synths); i++)
	{
		if (!strcmp (argv[1], synths[i].key))
		{
			synth = &synths[i];
		}
	}
	if (!synth)
	{
		fprintf (stderr, "usage: gen_ttl trio|analog BUNDLE_DIR PORTS_H | --symbols | --pages\n");
		return 2;
	}
	collect ();

	if (argc == 3 && !strcmp (argv[2], "--symbols"))
	{
		for (int i = 0; i < numPorts; i++)
		{
			printf ("%s\n", ports[i].sym);
		}
		return 0;
	}
	if (argc == 3 && !strcmp (argv[2], "--pages"))
	{
		// the Zynthian pages: 4 controls per page per group
		for (unsigned g = 0; g < NELEM (groupDefs); g++)
		{
			if (groupOf[g] < 0)
			{
				continue;
			}
			int n = 0;
			printf ("%-8s", groupDefs[g].name);
			for (int i = 0; i < numPorts; i++)
			{
				if (ports[i].group == (int) g)
				{
					printf ("%s%s", n ? ", " : " ", ports[i].l->name);
					n++;
				}
			}
			printf ("  (%d page%s)\n", (n + 3) / 4, n > 4 ? "s" : "");
		}
		printf ("%d groups, %d controls\n", numGroups, numPorts);
		return 0;
	}
	if (argc != 4)
	{
		fprintf (stderr, "usage: gen_ttl trio|analog BUNDLE_DIR PORTS_H | --symbols | --pages\n");
		return 2;
	}

	char ttl[64];
	snprintf (ttl, sizeof ttl, "%s.ttl", synth->sym);
	FILE *f = ttl_open (argv[2], "manifest.ttl");
	writeManifest (f);
	fclose (f);
	f = ttl_open (argv[2], ttl);
	writePlugin (f);
	fclose (f);
	f = ttl_open (argv[2], "presets.ttl");
	writePresets (f);
	fclose (f);
	f = ttl_open (NULL, argv[3]);
	writePortsH (f);
	fclose (f);
	return 0;
}
