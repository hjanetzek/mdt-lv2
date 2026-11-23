/*
 * ttl.h - TTL writing helpers for the plugins' generators (tools/gen_ttl.*):
 * strings, prefixes, the project block, port groups (with Zynthian's
 * lv2:displayPriority), the MIDI input and the stereo output group.
 * Header only, C and C++.
 *
 * Copyright (C) 2026 Hannes Janetzek
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef MDT_LV2_TTL_H_
#define MDT_LV2_TTL_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TTL_HOMEPAGE	"https://github.com/hjanetzek/mdt-lv2"

// a quoted string literal
static inline void ttl_string (FILE *f, const char *s)
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

// @prefix lines for the space separated prefix names, in that order
static inline void ttl_prefixes (FILE *f, const char *names)
{
	static const struct
	{
		const char *name, *uri;
	} known[] = {
		{ "atom", "http://lv2plug.in/ns/ext/atom#" },
		{ "doap", "http://usefulinc.com/ns/doap#" },
		{ "foaf", "http://xmlns.com/foaf/0.1/" },
		{ "lv2", "http://lv2plug.in/ns/lv2core#" },
		{ "midi", "http://lv2plug.in/ns/ext/midi#" },
		{ "param", "http://lv2plug.in/ns/ext/parameters#" },
		{ "pg", "http://lv2plug.in/ns/ext/port-groups#" },
		{ "pset", "http://lv2plug.in/ns/ext/presets#" },
		{ "rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#" },
		{ "rdfs", "http://www.w3.org/2000/01/rdf-schema#" },
		{ "time", "http://lv2plug.in/ns/ext/time#" },
		{ "units", "http://lv2plug.in/ns/extensions/units#" },
		{ "urid", "http://lv2plug.in/ns/ext/urid#" },
	};
	const char *p = names;
	while (*p)
	{
		while (*p == ' ')
			p++;
		const char *e = p;
		while (*e && *e != ' ')
			e++;
		if (e == p)
			break;
		const size_t n = (size_t) (e - p);
		for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
		{
			if (strlen (known[i].name) == n && !strncmp (known[i].name, p, n))
			{
				fprintf (f, "@prefix %s: <%s> .\n", known[i].name, known[i].uri);
			}
		}
		p = e;
	}
	fprintf (f, "\n");
}

// the stereo output group <uri#out> (its ports: ttl_io_ports)
static inline void ttl_output_group (FILE *f, const char *uri)
{
	fprintf (f,
		 "<%s#out>\n"
		 "\ta pg:StereoGroup , pg:OutputGroup ;\n"
		 "\tlv2:symbol \"out\" ;\n"
		 "\tlv2:name \"Output\" .\n\n",
		 uri);
}

// a control port group: Zynthian shows a page (of 4 controls) per group,
// in lv2:displayPriority order (the lv2core namespace, as Zynthian reads
// it); cls: an extra class (param:EnvelopeControls, ...) or NULL
static inline void ttl_group (FILE *f, const char *uri, const char *sym, const char *name, const char *cls,
			      int priority)
{
	fprintf (f, "<%s#%s>\n\ta pg:InputGroup%s%s ;\n\tlv2:symbol \"%s\" ;\n\tlv2:name ", uri, sym, cls ? " , " : "",
		 cls ? cls : "", sym);
	ttl_string (f, name);
	fprintf (f, " ;\n\tlv2:displayPriority %d .\n\n", priority);
}

// the project lines of a plugin (licence, maintainer, homepage)
static inline void ttl_project (FILE *f)
{
	fprintf (f,
		 "\tdoap:license <https://spdx.org/licenses/GPL-3.0-only> ;\n"
		 "\tdoap:maintainer [\n"
		 "\t\tfoaf:name \"Hannes Janetzek\" ;\n"
		 "\t\tfoaf:mbox <mailto:hannes.janetzek@gmail.com> ;\n"
		 "\t\tfoaf:homepage <" TTL_HOMEPAGE ">\n"
		 "\t] ;\n"
		 "\tdoap:homepage <" TTL_HOMEPAGE "> ;\n");
}

// "lv2:port" with ports 0..2: the atom MIDI input (supports: e.g.
// "midi:MidiEvent , time:Position"), the left and right outputs of
// <uri#out>; the caller continues with " , [ ... ]" or " .\n"
static inline void ttl_io_ports (FILE *f, const char *uri, const char *supports)
{
	fprintf (f,
		 "\tlv2:port [\n"
		 "\t\ta lv2:InputPort , atom:AtomPort ;\n"
		 "\t\tatom:bufferType atom:Sequence ;\n"
		 "\t\tatom:supports %s ;\n"
		 "\t\tlv2:designation lv2:control ;\n"
		 "\t\tlv2:index 0 ;\n"
		 "\t\tlv2:symbol \"midi_in\" ;\n"
		 "\t\tlv2:name \"MIDI In\"\n"
		 "\t] , [\n"
		 "\t\ta lv2:OutputPort , lv2:AudioPort ;\n"
		 "\t\tlv2:index 1 ;\n"
		 "\t\tlv2:symbol \"out_l\" ;\n"
		 "\t\tlv2:name \"Left\" ;\n"
		 "\t\tpg:group <%s#out> ;\n"
		 "\t\tlv2:designation pg:left\n"
		 "\t] , [\n"
		 "\t\ta lv2:OutputPort , lv2:AudioPort ;\n"
		 "\t\tlv2:index 2 ;\n"
		 "\t\tlv2:symbol \"out_r\" ;\n"
		 "\t\tlv2:name \"Right\" ;\n"
		 "\t\tpg:group <%s#out> ;\n"
		 "\t\tlv2:designation pg:right\n"
		 "\t]",
		 supports, uri, uri);
}

// the manifest's plugin entry
static inline void ttl_manifest_plugin (FILE *f, const char *uri, const char *binary, const char *ttl)
{
	fprintf (f,
		 "<%s>\n"
		 "\ta lv2:Plugin , lv2:InstrumentPlugin ;\n"
		 "\tlv2:binary <%s> ;\n"
		 "\trdfs:seeAlso <%s> .\n",
		 uri, binary, ttl);
}

// a file for writing (dir/name, or name when dir is NULL); exits on errors
static inline FILE *ttl_open (const char *dir, const char *name)
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

#endif
