#!/usr/bin/env python3
"""An LXR-02 / OpenElixir-02 project as an eDrumSynth preset that plays it
with the plugin's own sequencer (eDrumSynth 0.3.0).

  lxr2preset.py PROJDIR [-o BUNDLE] [--name NAME] [--pattern N | --song N]
                [--snapshot DIR [--prog NNN] [--presets-dir PATH]]
                [--ports PORTS_JSON] [-v]

The project folder (NN-*.PAT, NN-*.SND, NN-*.SNG, GLO.CFG, PRJ.NFO)
becomes an LV2 presets bundle (default: eDrumSynth-<project>.project.lv2
here) with one preset: its state is the whole project (every pattern, kit
and song; the plugin's project chunk, tools/edrumsynth_ports.json
"project"), its port values the start kit (as tools/snd2lv2.py) and the
sequencer's panel: Host mode (Zynthian's transport starts and stops it),
the project's tempo (GLO.CFG) for the Internal mode, the start pattern or
song, kit autoload as the project's pattern change mode.

--pattern N: start with pattern N (default: the first pattern with steps);
--song N: song mode with song N (its first pattern starts).
The start kit (the engine's sound when the preset loads, also in the
state): the start pattern's kit, else kit 0, else the LXR's init kit.

--snapshot DIR: also a Zynthian snapshot NNN-Name.zss (--prog, default
000) with one eDrumSynth chain (MIDI channel 10) whose preset is this one
(preset_info: the bundle installed in --presets-dir, default Zynthian's
/zynthian/zynthian-my-data/presets/lv2) and the project's tempo; no
controller values (the preset sets the sound).

Install the bundle like snd2lv2.py's (~/.lv2/, or on Zynthian
/zynthian/zynthian-my-data/presets/lv2/, then refresh the presets).

Python 3, no dependencies; the LXR format: lxrformat.py, the port map and
the chunk's layout: edrumsynth_ports.json, the kit mapping: snd2lv2.py.
"""
import argparse
import base64
import json
import os
import re
import sys

sys.dont_write_bytecode = True     # no __pycache__ next to the tools
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lxrformat as lxr  # noqa: E402
import snd2lv2  # noqa: E402

NAME_SIZE = 8
NO_AUTOMATION = 255
ENGINE_BLOCK = b"OE02"
ZYNTHIAN_PRESETS = "/zynthian/zynthian-my-data/presets/lv2"
# GLO.CFG: the LXR's globals from PAR_BPM; PAR_PATTERN_MODE's place
# (Parameters.h: PAR_BPM .. PAR_MIDI_NOTE7, PAR_PATTERN_MODE, PAR_MIXER_LIMITER)
GLOBAL_BPM, GLOBAL_PATTERN_MODE = 0, 29


def name_bytes(data):
    """the 8 name bytes of a file, as the plugin keeps them (raw)"""
    return bytes(data[:NAME_SIZE]).ljust(NAME_SIZE, b"\0")


def clear_pattern():
    """a cleared pattern as a V3 pattern file (seq_clearPattern() of a
    pattern set that was zero: shuffle 0, kit 0)"""
    out = bytearray(NAME_SIZE) + b"PatV03"
    for t in range(lxr.TRACKS):
        for i in range(lxr.STEPS):
            out += bytes([127 | (0x80 if i % 8 == 0 else 0), 100, lxr.DEFAULT_NOTE, NO_AUTOMATION, 0, NO_AUTOMATION, 0])
        out += bytes([lxr.SHIFT_NONE << 4] * lxr.STEPS)
    out += bytes(lxr.TRACKS * 4 * 2)
    out += bytes([0]) + bytes([lxr.BAR] * lxr.TRACKS) + bytes([0])
    return bytes(out)


def pattern_v3(data):
    """an LXR pattern file (V1, V2, V3) as the plugin stores it (a V3
    pattern file, the bytes as they are: lxr_sequencer.c readPattern()),
    or None if it's not one"""
    version, o = 1, 8
    if len(data) >= 14 and data[8:12] == b"PatV" and data[12:13] == b"0" and data[13:14] in (b"2", b"3"):
        version, o = data[13] - ord("0"), 14
    need = o + lxr.TRACKS * lxr.STEPS * (8 if version > 2 else 7) + lxr.TRACKS * 4 * 2 + 1 + lxr.TRACKS + (version > 1)
    if len(data) < need:
        return None
    out = bytearray(name_bytes(data)) + b"PatV03"
    for t in range(lxr.TRACKS):
        out += data[o:o + lxr.STEPS * 7]
        o += lxr.STEPS * 7
        if version > 2:
            out += data[o:o + lxr.STEPS]
            o += lxr.STEPS
        else:
            out += bytes([lxr.SHIFT_NONE << 4] * lxr.STEPS)
    out += data[o:o + lxr.TRACKS * 8]
    o += lxr.TRACKS * 8
    out += data[o:o + 1 + lxr.TRACKS]
    o += 1 + lxr.TRACKS
    out.append(data[o] if version > 1 else 0)
    return bytes(out)


def kit_parts(m, data):
    """(name bytes, the 247 parameters, the engines) of an LXR kit file as
    the plugin reads it (lxr_sequencer.c readKit()), or None"""
    if len(data) < NAME_SIZE + 128:
        return None
    size = m["kit_size"]
    n = min(len(data) - NAME_SIZE, size)
    params = bytes(data[NAME_SIZE:NAME_SIZE + n]) + bytes(size - n)
    engines = snd2lv2.kit_engines(m, data[NAME_SIZE + n:])
    return name_bytes(data), params, engines


def kit_file(name, params, engines):
    return name + params + ENGINE_BLOCK + bytes([len(engines)]) + bytes(engines)


def init_kit(m):
    """the LXR's init kit's parameters, from the port map"""
    params = bytearray(m["kit_size"])
    for p in m["ports"]:
        if p.get("kit_init") is not None and p["param"] is not None:
            params[p["param"]] = p["kit_init"]
    for par, v in m["unmapped_kit_params"].items():
        params[int(par)] = v
    stock = [m["engines"].index(s["stock_engine"]) for s in m["slots"]]
    return bytes(params), stock


def default_song(nr):
    return bytes([0] + [lxr.SONG_END] * (lxr.SONG_LENGTH - 1)) if nr == 0 else bytes([lxr.SONG_END] * lxr.SONG_LENGTH)


def read(path):
    with open(path, "rb") as f:
        return f.read()


class ProjectChunk:
    """an LXR project folder as the plugin's project chunk"""

    def __init__(self, m, folder, warn):
        self.m = m
        pm = m["project"]
        if pm["version"] != 1 or pm["header"] != 48:
            raise lxr.FormatError("the port map's project chunk is version %d (this tool writes 1)" % pm["version"])
        self.project = lxr.Project(folder)
        self.clear = clear_pattern()
        assert len(self.clear) == pm["pattern_file"]
        self.patterns = {}
        for nr, path in sorted(self.project.files["PAT"].items()):
            v3 = pattern_v3(read(path))
            if v3 is None:
                warn("%s: not a pattern, left out" % path)
                continue
            self.patterns[nr] = v3
        self.kits = {}
        for nr, path in sorted(self.project.files["SND"].items()):
            parts = kit_parts(m, read(path))
            if parts is None:
                warn("%s: not a kit, left out" % path)
                continue
            self.kits[nr] = parts
        self.songs = {}
        for nr, path in sorted(self.project.files["SNG"].items()):
            data = read(path)
            if len(data) < NAME_SIZE:
                warn("%s: not a song, left out" % path)
                continue
            entries = bytes(data[NAME_SIZE:NAME_SIZE + lxr.SONG_LENGTH])
            self.songs[nr] = (name_bytes(data), entries + bytes([lxr.SONG_END] * (lxr.SONG_LENGTH - len(entries))))
        self.globals = bytearray(pm["globals"])
        glo = lxr.find_file(folder, "GLO.CFG")
        if glo:
            data = read(glo)
            self.globals[:min(len(data), len(self.globals))] = data[:len(self.globals)]
        nfo = lxr.find_file(folder, "PRJ.NFO")
        self.name = name_bytes(read(nfo)) if nfo else bytes(NAME_SIZE)

    def bpm(self):
        return self.project.bpm

    def kit_autoload(self):
        return self.globals[GLOBAL_PATTERN_MODE] >= 1

    def pattern_kit(self, nr):
        v3 = self.patterns.get(nr, self.clear)
        return v3[-1]

    def has_steps(self, nr):
        v3 = self.patterns.get(nr)
        if not v3:
            return False
        o = 14 + lxr.TRACKS * lxr.STEPS * 8
        return any(v3[o:o + lxr.TRACKS * 8])

    def start_kit(self, pattern):
        """(name, params, engines) of the kit in the engine at the start"""
        for nr in ((self.pattern_kit(pattern), 0) if self.kit_autoload() else (0,)):
            if nr in self.kits:
                return self.kits[nr]
        params, engines = init_kit(self.m)
        return bytes(NAME_SIZE), params, engines

    def chunk(self, pattern, song, song_mode):
        """the chunk (lxr_sequencer.c: lxr_seq_chunk_write()'s layout)"""
        pm = self.m["project"]
        pmask = sum(1 << nr for nr, v3 in self.patterns.items() if v3 != self.clear)
        kmask = sum(1 << nr for nr in self.kits)
        smask = sum(1 << nr for nr, (name, entries) in self.songs.items()
                    if entries != default_song(nr) or name != bytes(NAME_SIZE))
        header = bytearray(pm["header"])
        header[0:4] = pm["magic"].encode()
        header[4:6] = pm["version"].to_bytes(2, "little")
        header[6:8] = pm["header"].to_bytes(2, "little")
        header[8:16] = pmask.to_bytes(8, "little")
        header[16:24] = kmask.to_bytes(8, "little")
        header[24:32] = smask.to_bytes(8, "little")
        header[32] = pattern
        header[33] = song
        header[34] = (1 if song_mode else 0) | 2
        header[35] = len(self.globals)
        header[36:44] = self.name
        out = bytearray(header)
        for nr in range(lxr.STEPS):
            if pmask >> nr & 1:
                out += self.patterns[nr]
        for nr in range(lxr.STEPS):
            if kmask >> nr & 1:
                out += kit_file(*self.kits[nr])
        for nr in range(lxr.STEPS):
            if smask >> nr & 1:
                out += self.songs[nr][0] + self.songs[nr][1]
        out += self.globals
        name, params, engines = self.start_kit(pattern)
        out += kit_file(name, params, engines)
        return bytes(out)


def safe_name(name):
    return re.sub(r"[^A-Za-z0-9_-]+", "-", name).strip("-") or "project"


PREFIXES = ("@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
            "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
            "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
            "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
            "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n\n")


def port_values(m, pc, pattern, song, song_mode, warn):
    """symbol -> value: the start kit's ports, the panel's inputs"""
    _, params, engines = pc.start_kit(pattern)
    values = snd2lv2.kit_values(m, params, engines, warn)
    panel = {p["symbol"]: p for p in m["panel_ports"] if p["direction"] == "input"}
    for sym, p in panel.items():
        values[sym] = p["default"]
    values.update({
        "seq_mode": 1,
        "seq_tempo": max(panel["seq_tempo"]["min"], min(panel["seq_tempo"]["max"], pc.bpm())),
        "seq_pattern": pattern,
        "seq_song_mode": 1 if song_mode else 0,
        "seq_song": song,
        "seq_kit_autoload": 1 if pc.kit_autoload() else 0,
        "seq_kit": pc.pattern_kit(pattern) if pc.pattern_kit(pattern) < 64 else 64,
    })
    return values


def write_bundle(m, pc, out, label, pattern, song, song_mode, warn):
    """the bundle: manifest.ttl, the preset; returns the preset's file name"""
    uri = m["uri"]
    os.makedirs(out, exist_ok=True)
    name = snd2lv2.file_name(label, set())
    bank = "manifest.ttl#bank1"
    chunk = pc.chunk(pattern, song, song_mode)
    values = port_values(m, pc, pattern, song, song_mode, warn)
    # the panel's settings only: its edit ports (selection, step fields,
    # actions; "saved": false, pprops:notAutomatic) aren't stored, the
    # project is the state
    order = [p["symbol"] for p in m["ports"] if p["kit"]] + [p["symbol"] for p in m["panel_ports"]
                                                              if p["direction"] == "input" and p.get("saved", True)]
    with open(os.path.join(out, "manifest.ttl"), "w", encoding="utf-8") as f:
        f.write("# generated by lxr2preset.py (eDrumSynth %s)\n" % m["version"] + PREFIXES)
        f.write("<%s>\n\ta pset:Bank ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label %s .\n\n"
                % (bank, uri, snd2lv2.ttl_string("LXR %s" % label)))
        f.write("<%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label %s ;\n\trdfs:seeAlso <%s> .\n"
                % (name, uri, snd2lv2.ttl_string(label), name))
    ports = " , ".join("[\n\t\tlv2:symbol \"%s\" ;\n\t\tpset:value %.1f\n\t]" % (s, values[s]) for s in order)
    with open(os.path.join(out, name), "w", encoding="utf-8") as f:
        f.write("# generated by lxr2preset.py: an LXR project (its patterns, kits, songs) as eDrumSynth's state\n"
                + PREFIXES)
        f.write("<%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label %s ;\n\tpset:bank <%s> ;\n"
                "\tlv2:port %s ;\n\tstate:state [\n\t\t<%s> \"%s\"^^xsd:base64Binary\n\t] .\n"
                % (name, uri, snd2lv2.ttl_string(label), bank, ports, m["project"]["property"],
                   base64.b64encode(chunk).decode()))
    return name, chunk


def snapshot_state(m, preset_text, preset_name):
    """the preset as zynthian's embedded plugin state (zynthian-ui
    docs/snapshot-plugin-state.md, branch plugin-state): "lv2_state" of the
    processor in zs3-0: {"plugin": URI, "files": {"state.ttl": text,
    "manifest.ttl": text}}; state.ttl is the preset with itself as subject
    (relative URIs only) and without the bank"""
    uri = m["uri"]
    head = "<%s>\n\ta pset:Preset ;" % preset_name
    if head not in preset_text:
        raise ValueError("preset %s: unexpected layout" % preset_name)
    state = preset_text.replace(head, "<state.ttl>\n\ta pset:Preset ;", 1)
    state = re.sub(r"\tpset:bank <[^>]*> ;\n", "", state, count=1)
    manifest = ("# generated by lxr2preset.py: zynthian's embedded plugin state\n" + PREFIXES
                + "<state.ttl>\n\ta pset:Preset ;\n\tlv2:appliesTo <%s> ;\n\trdfs:seeAlso <state.ttl> .\n" % uri)
    return {"plugin": uri, "files": {"manifest.ttl": manifest, "state.ttl": state}}


def write_snapshot(path, title, preset_uri, bank_uri, bpm, lv2_state=None):
    """a Zynthian snapshot (schema 6): eDrumSynth on MIDI channel 10 with
    the preset, the tempo; lv2_state: the project embedded (zynthian loads
    it after the preset, so the snapshot works without the bundle too)"""
    engine, chain, proc, mixer = "JV/eDrumSynth", 1, 1, 2
    snap = {
        "schema_version": 6,
        "chains": {
            str(chain): {"title": title, "midi_chan": 9, "midi_thru": False, "audio_thru": False, "zmop_index": 0,
                         "cc_route": [], "slots": [{str(proc): engine}, {str(mixer): "MI"}], "zctrls": []},
            "0": {"title": "Main", "midi_chan": None, "midi_thru": False, "audio_thru": True, "zmop_index": None,
                  "cc_route": [], "slots": [{"-1": "MR"}], "zctrls": []},
        },
        "zs3": {"zs3-0": {
            "title": title,
            "active_chain": chain,
            "chains": {str(chain): {"midi_chan": 9, "midi_in": ["ZynMidiRouter:ch0_out"], "audio_in": [],
                                    "audio_out": [0]}},
            "processors": {
                str(proc): {"bank_info": [bank_uri, None, "LXR %s" % title, None], "bank_subdir_info": None,
                            "preset_info": [preset_uri, None, title, bank_uri], "preset_subdir_info": None,
                            "controllers": {}},
                "-4": {"bank_info": None, "bank_subdir_info": None, "preset_info": None, "preset_subdir_info": None,
                       "controllers": {"bpm": {"value": float(bpm)}, "bpb": {"value": 4}}},
            },
        }},
        "last_zs3_id": "zs3-0",
        "zynseq": {"tempo": float(bpm), "bpb": 4, "scene": 0, "patns": {}, "scenes": [{"name": "Scene 1", "phrases": []}]},
        "gui": {"pinned_chains": 1},
    }
    if lv2_state:
        snap["zs3"]["zs3-0"]["processors"][str(proc)]["lv2_state"] = lv2_state
    with open(path, "w", encoding="utf-8") as f:
        json.dump(snap, f, indent=1)
    return snap


def convert(m, folder, out=None, name=None, pattern=None, song=None, snapshot=None, prog=0,
            presets_dir=ZYNTHIAN_PRESETS, verbose=False):
    """converts; returns a dict (bundle, preset file, chunk, snapshot path)"""
    def warn(text):
        if verbose:
            print("%s: %s" % (folder, text), file=sys.stderr)

    pc = ProjectChunk(m, folder, warn)
    label = name or pc.project.name
    song_mode = song is not None
    if song_mode:
        if not 0 <= song < lxr.SONG_LENGTH:
            raise lxr.FormatError("song %d: 0-63" % song)
        entries = pc.songs.get(song, (None, default_song(song)))[1]
        first = entries[0] if entries[0] < 64 else 0
        start, song_nr = first, song
    else:
        if pattern is None:
            pattern = next((nr for nr in sorted(pc.patterns) if pc.has_steps(nr)), 0)
        if not 0 <= pattern < 64:
            raise lxr.FormatError("pattern %d: 0-63" % pattern)
        start, song_nr = pattern, 0
    bundle = out or "eDrumSynth-%s.project.lv2" % safe_name(label)
    preset, chunk = write_bundle(m, pc, bundle, label, start, song_nr, song_mode, warn)
    result = {"bundle": bundle, "preset": preset, "chunk": chunk, "label": label, "pattern": start,
              "patterns": len(pc.patterns), "kits": len(pc.kits), "songs": len(pc.songs), "bpm": pc.bpm(),
              "snapshot": None}
    if snapshot:
        os.makedirs(snapshot, exist_ok=True)
        base = "file://%s/%s" % (presets_dir.rstrip("/"), os.path.basename(os.path.normpath(bundle)))
        path = os.path.join(snapshot, "%03d-%s.zss" % (prog, label))
        with open(os.path.join(bundle, preset), encoding="utf-8") as f:
            state = snapshot_state(m, f.read(), preset)
        write_snapshot(path, label, "%s/%s" % (base, preset), "%s/manifest.ttl#bank1" % base, pc.bpm(), state)
        result["snapshot"] = path
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description="an LXR project as an eDrumSynth preset (its sequencer plays it)")
    ap.add_argument("project", help="the LXR project folder")
    ap.add_argument("-o", "--out", help="the bundle (default eDrumSynth-<project>.project.lv2)")
    ap.add_argument("--name", help="the preset's name (default: the project's)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--pattern", type=int, help="the start pattern (default: the first with steps)")
    g.add_argument("--song", type=int, help="song mode with this song")
    ap.add_argument("--snapshot", metavar="DIR", help="also a Zynthian snapshot using the preset")
    ap.add_argument("--prog", type=int, default=0, help="the snapshot's program number (NNN-Name.zss)")
    ap.add_argument("--presets-dir", default=ZYNTHIAN_PRESETS,
                    help="where the bundle is installed (the snapshot's preset URI)")
    ap.add_argument("--ports", default=os.path.join(HERE, "edrumsynth_ports.json"), help="the port map")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    m = snd2lv2.load_ports(args.ports)
    if "project" not in m or "panel_ports" not in m:
        sys.exit("lxr2preset: %s is older than eDrumSynth 0.3.0 (no sequencer)" % args.ports)
    try:
        r = convert(m, args.project, args.out, args.name, args.pattern, args.song, args.snapshot, args.prog,
                    args.presets_dir, args.verbose)
    except (lxr.FormatError, OSError) as e:
        sys.exit("lxr2preset: %s" % e)
    print("%s: preset %s (%d patterns, %d kits, %d songs, %d bpm, start pattern %d, %d bytes of state)"
          % (r["bundle"], r["label"], r["patterns"], r["kits"], r["songs"], r["bpm"], r["pattern"], len(r["chunk"])))
    if r["snapshot"]:
        print(r["snapshot"])


if __name__ == "__main__":
    main()
