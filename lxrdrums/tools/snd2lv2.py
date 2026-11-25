#!/usr/bin/env python3
"""LXR / MicroDexed-touch synth kits (.snd) as an eDrumSynth LV2 presets bundle.

  snd2lv2.py [-o BUNDLE] [--all] [--ports PORTS_JSON] [-v] PATH...

PATH: a .snd file, a folder of .snd files, or a tree such as MDT's
/SYNTHKITS (its own kits and one bank per folder below it). Every folder
with kits becomes a preset bank (pset:Bank, named after the folder), every
kit a preset ("NN NAME": the slot number of the file name NN-NAME.snd and
the kit's name). A kit file is the 8-byte name, the 247 sound parameters
and, from OpenElixir-02, its engine block ("OE02", the engine of each
slot); kits without one have the stock engines.

The bundle (default: edrumsynth-<name of the first PATH>.presets.lv2 here)
is installed like any LV2 bundle: ~/.lv2/, or on Zynthian
/zynthian/zynthian-my-data/presets/lv2/ (then refresh the presets, e.g.
in the webconf, or restart the UI).

A preset sets every kit port: the engine selectors, the ports of the
engines the kit's slots run (the kit's values), the other engines' ports
(their defaults) and the FX bus; not the master controls that aren't in
kits (DJ filter, limiter, MIDI mode). Placeholder kits (Initkit, EMPTY) are
left out unless --all.

The port map is tools/edrumsynth_ports.json (written by tools/gen_ttl.c,
next to this script): for each port its kit parameter, slot, engines and
the value conversion (port value = kit value - offset).
"""
import argparse
import json
import os
import re
import sys

ENGINE_BLOCK = b"OE02"


def load_ports(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def kit_engines(m, block):
    """the engine of each slot (indices into m["engines"]) from the engine
    block after the parameters (as lxr_engine_decode_engines())"""
    names = m["engines"]
    engines = [names.index(s["stock_engine"]) for s in m["slots"]]
    if len(block) >= 5 + len(engines) and block[:4] == ENGINE_BLOCK and block[4] >= len(engines):
        for slot in range(len(engines)):
            if block[5 + slot] < len(names):
                engines[slot] = block[5 + slot]
    return engines


def kit_values(m, params, engines, warn):
    """port symbol -> value of a kit (params: its sound parameters)"""
    names = m["engines"]
    values = {}
    for p in m["ports"]:
        if not p["kit"]:
            continue
        if p["type"] == "engine":
            values[p["symbol"]] = engines[p["slot"]]
            continue
        if p["slot"] is not None and names[engines[p["slot"]]] not in p["engines"]:
            values[p["symbol"]] = p["default"]  # an engine the slot doesn't run
            continue
        x = params[p["param"]]
        if not p["min"] + p["offset"] <= x <= p["max"] + p["offset"]:
            # the engine ignores it: the value stays the init kit's
            warn("parameter %d (%s) = %d is out of range, %d used" % (p["param"], p["symbol"], x, p["kit_init"]))
            x = p["kit_init"]
        values[p["symbol"]] = x - p["offset"]
    for par, init in m["unmapped_kit_params"].items():
        if params[int(par)] != init:
            warn("parameter %s (not a port) = %d is not converted" % (par, params[int(par)]))
    return values


def is_placeholder(file_name, name):
    stem = re.sub(r"^\d\d-", "", os.path.splitext(file_name)[0])
    return bool(re.match(r"(?i)(INITK|IN__K|EMPTY)", stem) or re.match(r"(?i)in.{0,3}kit$", name))


def read_kit(m, path, keep_all, verbose):
    """(label, values) of a kit file, or None"""
    size = m["kit_name_size"] + m["kit_size"]
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < size:
        print("%s: not a kit (%d bytes)" % (path, len(data)), file=sys.stderr)
        return None
    name = data[:m["kit_name_size"]].split(b"\0")[0].decode("latin1").strip()
    file_name = os.path.basename(path)
    if not keep_all and is_placeholder(file_name, name):
        if verbose:
            print("%s: placeholder kit, left out" % path, file=sys.stderr)
        return None
    params = data[m["kit_name_size"]:size]
    engines = kit_engines(m, data[size:])

    def warn(text):
        if verbose:
            print("%s: %s" % (path, text), file=sys.stderr)

    values = kit_values(m, params, engines, warn)
    slot = re.match(r"(\d\d)-", file_name)
    if not name:
        name = re.sub(r"^\d\d-", "", os.path.splitext(file_name)[0])
    label = "%s %s" % (slot.group(1), name) if slot else name
    return label, values


def is_kit_file(name):
    return name.lower().endswith(".snd")


def collect(paths):
    """[(bank label, [kit file, ...])], banks in the order found"""
    banks = []
    for path in paths:
        path = os.path.normpath(path)
        if os.path.isfile(path):
            banks.append((os.path.basename(os.path.dirname(os.path.abspath(path))), [path]))
            continue
        if not os.path.isdir(path):
            sys.exit("snd2lv2: %s: no such file or folder" % path)
        for folder, dirs, files in os.walk(path):
            dirs.sort()
            kits = sorted(os.path.join(folder, f) for f in files if is_kit_file(f))
            if kits:
                banks.append((os.path.basename(folder), kits))
    # bank names that repeat: with the folder above
    labels = [b[0] for b in banks]
    for i, (label, kits) in enumerate(banks):
        if labels.count(label) > 1:
            above = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(kits[0]))))
            banks[i] = ("%s/%s" % (above, label), kits)
    return banks


def ttl_string(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def file_name(label, used):
    base = re.sub(r"[^a-z0-9]+", "_", label.lower()).strip("_") or "kit"
    name, n = base, 2
    while name in used:
        name, n = "%s_%d" % (base, n), n + 1
    used.add(name)
    return name + ".ttl"


PREFIXES = ("@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
            "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
            "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n\n")


def write_bundle(m, banks, out):
    """writes the bundle; returns the number of presets"""
    uri = m["uri"]
    os.makedirs(out, exist_ok=True)
    manifest = ["# generated by snd2lv2.py (eDrumSynth %s)\n" % m["version"], PREFIXES]
    order = [p["symbol"] for p in m["ports"] if p["kit"]]
    used, count = set(), 0
    for b, (bank, presets) in enumerate(banks, 1):
        bank_uri = "manifest.ttl#bank%d" % b
        manifest.append("<%s>\n\ta pset:Bank ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label %s .\n\n"
                        % (bank_uri, uri, ttl_string(bank)))
        for label, values in presets:
            name = file_name("%02d %s" % (b, label), used)
            manifest.append("<%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label %s ;\n"
                            "\trdfs:seeAlso <%s> .\n\n" % (name, uri, ttl_string(label), name))
            ports = " , ".join("[\n\t\tlv2:symbol \"%s\" ;\n\t\tpset:value %.1f\n\t]" % (s, values[s]) for s in order)
            with open(os.path.join(out, name), "w", encoding="utf-8") as f:
                f.write("# generated by snd2lv2.py\n" + PREFIXES)
                f.write("<%s>\n\ta pset:Preset ;\n\tlv2:appliesTo <%s> ;\n\trdfs:label %s ;\n\tpset:bank <%s> ;\n"
                        "\tlv2:port %s .\n" % (name, uri, ttl_string(label), bank_uri, ports))
            count += 1
    with open(os.path.join(out, "manifest.ttl"), "w", encoding="utf-8") as f:
        f.write("".join(manifest).rstrip("\n") + "\n")
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", metavar="PATH")
    ap.add_argument("-o", "--out", help="the bundle folder (a name ending in .lv2)")
    ap.add_argument("--all", action="store_true", help="keep placeholder kits")
    ap.add_argument("--ports", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "edrumsynth_ports.json"),
                    help="the port map (default: edrumsynth_ports.json next to this script)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    m = load_ports(args.ports)
    banks = []
    for bank, files in collect(args.paths):
        presets = [k for k in (read_kit(m, f, args.all, args.verbose) for f in files) if k]
        if presets:
            banks.append((bank, presets))
    if not banks:
        sys.exit("snd2lv2: no kits found")
    out = args.out
    if not out:
        first = os.path.basename(os.path.normpath(args.paths[0]))
        first = re.sub(r"[^a-z0-9]+", "-", os.path.splitext(first)[0].lower()).strip("-") or "kits"
        out = "edrumsynth-%s.presets.lv2" % first
    count = write_bundle(m, banks, out)
    print("%s: %d presets in %d banks" % (out, count, len(banks)))


if __name__ == "__main__":
    main()
