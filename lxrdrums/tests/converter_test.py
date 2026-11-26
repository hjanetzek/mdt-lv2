#!/usr/bin/env python3
"""Tests of tools/lxrformat.py and tools/lxr2zynthian.py (run by ctest):

  converter_test.py [-v]

- the parser on synthetic project folders written here (patterns V1-V3,
  songs with end / loop, kits with and without the engine block, a short
  kit, GLO.CFG, PRJ.NFO); no third party projects;
- the mapping per feature against the expected zynseq events (velocity,
  step notes, probability, flams, shift, shuffle, parameter locks with
  their restores, CC / NRPN, track lengths, Kit mode, songs, ZS3s, program
  change events);
- the snapshot's keys against zynthian-ui's zynthian_state_schema.py
  (ZYNTHIAN_UI, default ~/bastel/github/zynthian-ui; skipped without it);
- the zynseq JSON through the real libzynseq.so: setState, getState,
  compared (EDS_LIBZYNSEQ = the library, else zynthian-ui's build folders;
  skipped without it). The library is only loaded (in a child process): no
  init(), no JACK client.
"""
import ast
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True     # no __pycache__ in the source tree
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)
import lxrformat as lxr  # noqa: E402
import lxr2zynthian as conv  # noqa: E402
import snd2lv2  # noqa: E402

PORTS = snd2lv2.load_ports(os.path.join(TOOLS, "edrumsynth_ports.json"))
ZYNTHIAN_UI = os.environ.get("ZYNTHIAN_UI", os.path.expanduser("~/bastel/github/zynthian-ui"))


# ---- synthetic projects ----

def blank_track(timing=0x70):
    return [[0, 100, 60, 255, 0, 255, 0, timing] for _ in range(lxr.STEPS)]


class Pat:
    """a pattern being written: steps[t][i] = [vol, prob, note, p1n, p1v, p2n, p2v, timing]"""

    def __init__(self, name="PAT", version=3, kit=0):
        self.name, self.version, self.kit = name, version, kit
        self.steps = [blank_track() for _ in range(lxr.TRACKS)]
        self.main = [[0] * 4 for _ in range(lxr.TRACKS)]
        self.shuffle = 0
        self.lengths = [16] * lxr.TRACKS

    def hit(self, t, i, vel=100, prob=100, note=60, locks=(), flam=0, shift=0):
        s = self.steps[t][i]
        s[0], s[1], s[2] = 0x80 | vel, prob, note
        for k, (n, v) in enumerate(locks):
            s[3 + 2 * k], s[4 + 2 * k] = n, v
        s[7] = (shift + 7) << 4 | flam
        self.main[t][i // 16] |= 1 << (i % 16)
        return self

    def data(self):
        return lxr.pack_pattern(self.name, self.version, self.steps, self.main, self.shuffle, self.lengths, self.kit)


def kit_params(seed=0):
    """a kit: the init kit's values, a few changed"""
    params = [0] * lxr.KIT_SIZE
    for p in PORTS["ports"]:
        if p["type"] == "param" and p["kit"] and p["param"] < lxr.KIT_SIZE:
            params[p["param"]] = p["kit_init"]
    params[50] = (21 + seed * 7) % 128    # bd amp decay
    params[37] = 127 - seed               # bd cutoff
    return params


class Project:
    def __init__(self, root, name="PROJ00"):
        self.dir = os.path.join(root, name)
        os.makedirs(self.dir)

    def write(self, file_name, data):
        with open(os.path.join(self.dir, file_name), "wb") as f:
            f.write(data)
        return self

    def pattern(self, nr, pat):
        return self.write("%02d-%s.PAT" % (nr, pat.name[:5]), pat.data())

    def kit(self, nr, name, params, engines=None, count=lxr.KIT_SIZE):
        return self.write("%02d-%s.SND" % (nr, name[:5]), lxr.pack_kit(name, params, engines, count))

    def song(self, nr, name, patterns, end=lxr.SONG_END):
        return self.write("%02d-%s.SNG" % (nr, name[:5]), lxr.pack_song(name, patterns, end))

    def globals(self, bpm, name="TESTPROJ"):
        self.write("GLO.CFG", bytes([bpm]) + bytes(40))
        return self.write("PRJ.NFO", name.encode().ljust(8, b"\0") + bytes(8))


def events_of(snapshot, phrase, column):
    z = snapshot["zynseq"]
    seq = z["scenes"][0]["phrases"][phrase]["sequences"][column]
    patns = seq["tracks"][0]["patns"]
    if not patns:
        return None, seq
    return z["patns"][str(patns["0"])], seq


def notes(patn):
    return [e for e in patn["events"] if e[3] == 144]


def controls(patn):
    return [e for e in patn["events"] if e[3] == 176]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="lxr2zynthian-test-")
        self.proj = Project(self.tmp)
        self.proj.globals(133)
        self.proj.kit(0, "KITZERO", kit_params(0))
        self.proj.kit(1, "KITONE", kit_params(1), engines=[0, 0, 1, 1, 2, 3])

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def convert(self, mode="multi-all", **kw):
        report = conv.Report()
        snap, summary = conv.convert([lxr.Project(self.proj.dir)], PORTS, mode=mode, report=report, **kw)
        return snap, report


# ---- the parser ----

class ParserTest(Base):
    def test_versions(self):
        for version in (1, 2, 3):
            p = Pat("V%d" % version, version, kit=1 if version > 1 else 0)
            p.hit(0, 0, vel=90, prob=50, note=63, locks=[(51, 10), (200, 20)], flam=2 if version > 2 else 0,
                  shift=-3 if version > 2 else 0)
            p.hit(6, 63)
            p.lengths[2] = 7
            p.lengths[3] = 0        # 0: 16
            p.shuffle = 40
            self.proj.pattern(version, p)
            data = p.data()
            self.assertEqual(len(data), lxr.PATTERN_SIZES[version])
            q = lxr.parse_pattern(data)
            self.assertEqual((q.name, q.version, q.shuffle), ("V%d" % version, version, 40))
            self.assertEqual(q.kit, 1 if version > 1 else 0)
            self.assertEqual(q.lengths, [16, 16, 7, 16, 16, 16, 16])
            s = q.tracks[0][0]
            self.assertEqual((s.active, s.velocity, s.probability, s.note), (True, 90, 50, 63))
            self.assertEqual(s.locks, ((51, 10), (200, 20)))
            self.assertEqual((s.flam, s.shift), (3, -3) if version > 2 else (1, 0))
            self.assertTrue(q.tracks[6][63].active)
            self.assertFalse(q.tracks[6][62].active)
            self.assertEqual(q.bars(), 1)
        proj = lxr.Project(self.proj.dir)
        self.assertEqual(proj.pattern_numbers(), [1, 2, 3])
        self.assertEqual((proj.name, proj.bpm), ("TESTPROJ", 133))

    def test_bad_pattern(self):
        with self.assertRaises(lxr.FormatError):
            lxr.parse_pattern(b"SHORT\0\0\0PatV03" + bytes(100))

    def test_songs(self):
        self.proj.song(0, "ENDS", [3, 1, 2])
        self.proj.song(1, "LOOPS", [0, 0, 1], end=lxr.SONG_LOOP)
        self.proj.song(2, "EMPTY", [])
        proj = lxr.Project(self.proj.dir)
        s = proj.song(0)
        self.assertEqual((s.name, s.patterns, s.loop), ("ENDS", [3, 1, 2], False))
        s = proj.song(1)
        self.assertEqual((s.name, s.patterns, s.loop), ("LOOPS", [0, 0, 1], True))
        self.assertEqual(proj.song_numbers(), [0, 1])

    def test_kits(self):
        self.proj.kit(2, "OLDKIT", kit_params(2), count=200)
        proj = lxr.Project(self.proj.dir)
        k = proj.kit(0)
        self.assertEqual((k.name, k.engines, k.has_engine_block), ("KITZERO", [0, 0, 0, 1, 2, 3], False))
        self.assertEqual(k.params, kit_params(0))
        k = proj.kit(1)
        self.assertEqual((k.engines, k.has_engine_block), ([0, 0, 1, 1, 2, 3], True))
        k = proj.kit(2)
        self.assertEqual(k.count, 200)
        self.assertEqual(k.params[:200], kit_params(2)[:200])
        self.assertEqual(k.params[200:], [0] * 47)

    def test_defaults(self):
        os.remove(os.path.join(self.proj.dir, "GLO.CFG"))
        os.remove(os.path.join(self.proj.dir, "PRJ.NFO"))
        proj = lxr.Project(self.proj.dir)
        self.assertEqual((proj.name, proj.bpm), ("PROJ00", 120))


# ---- the mapping ----

class MappingTest(Base):
    def test_steps(self):
        p = Pat("STEPS", kit=0)
        p.hit(0, 0, vel=127).hit(0, 4, vel=1, prob=30).hit(1, 2, note=72).hit(2, 5, flam=1).hit(2, 9, flam=5)
        p.hit(3, 6, shift=3).hit(3, 12, shift=-2).hit(3, 0, shift=-7)
        self.proj.pattern(0, p)
        snap, report = self.convert()
        bd, seq = events_of(snap, 0, 0)
        self.assertEqual(seq["repeat"], 255)
        self.assertEqual((bd["beats"], bd["steps"]), (4, 16))
        self.assertEqual(notes(bd), [[0, 0.0, 1.0, 144, 60, 60, 127, 0, 0, 0, 0, 100, 1, 100, 1],
                                     [4, 0.0, 1.0, 144, 60, 60, 1, 0, 0, 0, 0, 30, 1, 100, 1]])
        t1, seq = events_of(snap, 0, 1)
        self.assertEqual([(e[0], e[4], e[5]) for e in notes(t1)], [(2, 72, 72)])
        self.assertEqual(seq["tracks"][0]["chan"], 1)
        t2, _ = events_of(snap, 0, 2)
        self.assertEqual([(e[0], e[8]) for e in notes(t2)], [(5, 4), (9, 16)])     # x2: 2 hits, x8: 8 hits
        sd, _ = events_of(snap, 0, 3)
        self.assertEqual([(e[0], e[1]) for e in notes(sd)], [(6, 0.375), (11, 0.75), (15, 0.125)])
        # unused columns: silent (the library's empty pattern) for 0-6, off above
        self.assertEqual(events_of(snap, 0, 5)[0], None)
        self.assertEqual(events_of(snap, 0, 5)[1]["repeat"], 255)
        self.assertEqual(events_of(snap, 0, 9)[1]["repeat"], 0)
        self.assertEqual(len(snap["zynseq"]["scenes"][0]["phrases"][0]["sequences"]), 32)
        self.assertEqual(report.counts, {})

    def test_stutter_hits(self):
        # zynseq's stutter: speed s = alternating note off / on every
        # 1/s step, so 2h gives h evenly spaced hits (as the LXR's flam)
        clocks = conv.PPQN // conv.STEPS_PER_BEAT
        for hits in lxr.FLAM_HITS[1:]:
            speed, ons, t = 2 * hits, 1, 0
            n = clocks // speed
            count = 0
            while t + n < clocks - 1:
                t += n
                ons += count % 2
                count += 1
            self.assertEqual(ons, hits)

    def test_shuffle(self):
        for shuffle, swing in ((0, 0.0), (48, round(48 * 4 / 3 / 127, 4)), (127, 1.0)):
            p = Pat("SH")
            p.shuffle = shuffle
            p.hit(0, 0)
            self.proj.pattern(0, p)
            snap, report = self.convert()
            self.assertEqual((events_of(snap, 0, 0)[0]["swing"], events_of(snap, 0, 0)[0]["swingDiv"]), (swing, 1))
            self.assertEqual(bool(report.counts), shuffle == 127)

    def test_locks(self):
        kit = kit_params(0)
        p = Pat("LOCKS")
        # track 1: CC 51 (parameter 50) at step 2, restored at the next
        # step that plays (6); step 6 locks NRPN 69 (parameter 197) and CC 38,
        # restored at the pattern's last step (no step after it plays)
        p.hit(0, 2, locks=[(51, 5)]).hit(0, 6, locks=[(197, 100), (38, 20)])
        # track 2: a lock on the last step: restored at the loop (step 0);
        # a lock re-locked at the next step is not restored there
        p.hit(1, 0).hit(1, 4, locks=[(51, 9)]).hit(1, 8, locks=[(51, 11)]).hit(1, 15, locks=[(38, 3)])
        p.steps[2][3][3:5] = [51, 1]       # a lock on a step that doesn't play
        self.proj.pattern(0, p)
        snap, report = self.convert()
        bd, _ = events_of(snap, 0, 0)
        cc = [(e[0], e[1], e[4], e[6]) for e in controls(bd)]
        self.assertEqual(cc, [(2, 0.0, 51, 5), (6, 0.0, 99, 0), (6, 0.0, 98, 69), (6, 0.0, 6, 100), (6, 0.0, 38, 20),
                              (6, 0.0, 51, kit[50]), (15, 0.875, 99, 0), (15, 0.875, 98, 69),
                              (15, 0.875, 6, kit[197]), (15, 0.875, 38, kit[37])])
        for e in controls(bd):
            self.assertEqual((e[5], e[7]), (e[4], e[6]))   # start = end: no interpolation
        # events of a step: the NRPN, the CCs, then the note
        self.assertEqual([e[3] for e in bd["events"] if e[0] == 6], [176] * 5 + [144])
        t1, _ = events_of(snap, 0, 1)
        cc = [(e[0], e[4], e[6]) for e in controls(t1)]
        self.assertEqual(cc, [(0, 38, kit[37]), (4, 51, 9), (8, 51, 11), (15, 38, 3), (15, 51, kit[50])])
        self.assertEqual(report.counts, {
            "locks on the pattern's last step (restored at the loop, not when the pattern changes)": 1,
            "parameter locks on steps that don't play (the LXR ignores them too)": 1})
        self.assertEqual(snap["chains"]["1"]["cc_route"], [6, 38, 51, 98, 99])
        self.assertEqual(snap["zs3"]["*/0"]["processors"]["1"]["controllers"]["master_lxr_cc"], {"value": 1})

    def test_lock_conflicts(self):
        p = Pat("CONFL", version=2)
        # two NRPN locks at one step: zynseq keeps one
        p.hit(0, 0, locks=[(197, 1), (200, 2)]).hit(0, 4)
        p.hit(0, 1, locks=[(0, 5)])        # CC 0: no parameter
        self.proj.pattern(0, p)
        snap, report = self.convert()
        self.assertEqual(report.counts["locks on CC 0 (bank select: no parameter)"], 1)
        self.assertIn("NRPN restores dropped (another NRPN at the step: zynseq keeps one)", report.counts)

    def test_lengths(self):
        p = Pat("LEN")
        p.lengths = [16, 12, 7, 32, 16, 16, 16]
        p.hit(0, 0).hit(1, 0).hit(1, 11).hit(2, 0).hit(2, 6).hit(3, 31)
        self.proj.pattern(0, p)
        snap, report = self.convert()
        phrase = snap["zynseq"]["scenes"][0]["phrases"][0]
        self.assertEqual(phrase["repeat"], 2)       # bars
        t1, _ = events_of(snap, 0, 1)
        self.assertEqual((t1["steps"], [e[0] for e in notes(t1)]), (12, [0, 11]))      # native
        t2, _ = events_of(snap, 0, 2)
        self.assertEqual((t2["steps"], [e[0] for e in notes(t2)]), (32, [0, 6, 7, 13, 14, 20, 21, 27, 28]))  # unrolled
        self.assertEqual(sum(report.counts.values()), 1)
        # Kit mode: one pattern, GM notes, tracks tiled to the pattern
        snap, report = self.convert(mode="kit")
        kit, seq = events_of(snap, 0, 9)
        self.assertEqual((kit["steps"], seq["tracks"][0]["chan"]), (32, 9))
        got = sorted((e[0], e[4]) for e in notes(kit))
        expect = sorted([(0, 36), (16, 36)] + [(s, 45) for s in (0, 11, 12, 23, 24)] +
                        [(s, 41) for s in (0, 6, 7, 13, 14, 20, 21, 27, 28)] + [(31, 38)])
        self.assertEqual(got, expect)
        self.assertEqual(snap["chains"]["1"]["midi_chan"], 9)
        self.assertEqual(snap["zs3"]["*/0"]["processors"]["1"]["controllers"]["master_midi_mode"], {"value": 1})
        self.assertEqual(sum(report.counts.values()), 2)   # 12 and 7 don't divide 32

    def test_kit_mode_step_notes(self):
        # a constant note: folded into the tuning; varying notes: lost
        p = Pat("NOTES")
        p.hit(4, 0, note=63).hit(1, 0, note=60).hit(1, 4, note=67)
        self.proj.pattern(0, p)
        snap, report = self.convert(mode="kit")
        self.assertEqual(sorted(e[4] for e in notes(events_of(snap, 0, 9)[0])), [45, 45, 49])
        self.assertEqual(report.counts, {"step notes (no track chain: the kit's pitch)": 1})
        self.assertEqual(list(snap["chains"]), ["1", "0"])

    def test_zs3s(self):
        for nr, kit in ((0, 1), (3, 0), (5, 1)):
            self.proj.pattern(nr, Pat("P%d" % nr, kit=kit).hit(0, 0))
        self.proj.pattern(7, Pat("EMPTY"))
        snap, report = self.convert()
        zs3 = snap["zs3"]
        self.assertEqual(list(zs3), ["zs3-0", "*/0", "*/1", "*/2"])
        self.assertEqual([zs3[k]["title"] for k in zs3], ["00 P0", "00 P0", "03 P3", "05 P5"])
        self.assertEqual([zs3[k]["active_phrase"] for k in zs3], [0, 0, 1, 2])
        self.assertEqual(snap["last_zs3_id"], "*/0")
        k1 = lxr.Project(self.proj.dir).kit(1)
        want = conv.kit_controls(PORTS, k1, "multi")
        for k in ("*/0", "*/2"):
            ctl = {s: v["value"] for s, v in zs3[k]["processors"]["1"]["controllers"].items()}
            self.assertEqual(ctl, want)
            self.assertEqual(ctl["t2_mix_engine"], 1)
            self.assertEqual(ctl["master_midi_mode"], 1)
            self.assertIsNone(zs3[k]["processors"]["1"]["preset_info"])
        self.assertEqual(len(zs3["*/1"]["processors"]["1"]["controllers"]), len(PORTS["ports"]))
        self.assertEqual(zs3["*/1"]["processors"]["1"]["controllers"]["bd_ampeg_decay"]["value"], 21)
        for k in zs3:
            self.assertEqual(zs3[k]["chains"]["1"]["audio_out"], [0])
            self.assertEqual(zs3[k]["chains"]["1"]["midi_chan"], 0xFFFF)
            self.assertNotIn("global", zs3[k])
            self.assertNotIn("midi_capture", zs3[k])
        self.assertEqual(zs3["zs3-0"]["processors"]["-4"]["controllers"]["bpm"], {"value": 133.0})
        self.assertNotIn("-4", zs3["*/1"]["processors"])
        self.assertEqual(snap["zynseq"]["tempo"], 133.0)
        self.assertEqual(len(snap["zynseq"]["scenes"][0]["phrases"]), 3)

    def test_songs(self):
        self.proj.pattern(0, Pat("A", kit=0).hit(0, 0))
        p = Pat("B", kit=1).hit(0, 0)
        p.lengths[0] = 32
        self.proj.pattern(1, p)
        self.proj.pattern(2, Pat("C", kit=0).hit(1, 0))
        self.proj.song(0, "ENDS", [0, 1, 9])            # 9: missing
        self.proj.song(1, "LOOPS", [1, 0, 0], end=lxr.SONG_LOOP)
        self.proj.song(2, "ONE", [2], end=lxr.SONG_LOOP)
        snap, report = self.convert(patterns=[0, 1])
        phrases = snap["zynseq"]["scenes"][0]["phrases"]
        follow = [(ph["name"], ph["repeat"], ph["followAction"], ph["followParam"]) for ph in phrases]
        self.assertEqual(follow, [("00 A", 1, 0, 0), ("01 B", 2, 0, 0),
                                  ("ENDS 1: 00 A", 1, 1, 1), ("ENDS 2: 01 B", 2, 0, 0),
                                  ("LOOPS 1: 01 B", 2, 1, 1), ("LOOPS 2: 00 A", 1, 1, 1), ("LOOPS 3: 00 A", 1, 1, -2),
                                  ("ONE 1: 02 C", 1, 1, 0)])
        # songs share the launcher rows' zynseq patterns; their children
        # play once through the row (a looping one-row song: loops)
        self.assertEqual(events_of(snap, 2, 0)[1]["tracks"], events_of(snap, 0, 0)[1]["tracks"])
        self.assertEqual(events_of(snap, 2, 0)[1]["repeat"], 1)
        self.assertEqual(events_of(snap, 3, 0)[1]["repeat"], 1)
        self.assertEqual(events_of(snap, 3, 1)[1]["repeat"], 1)        # silent column
        self.assertEqual(events_of(snap, 7, 1)[1]["repeat"], 255)
        self.assertEqual(report.counts["song positions with an empty or missing pattern (left out)"], 1)
        # pattern 2 is only in a song: no launcher row; its ZS3 after the
        # others, selecting its song row
        self.assertEqual(list(snap["zs3"]), ["zs3-0", "*/0", "*/1", "*/2"])
        self.assertEqual(snap["zs3"]["*/2"]["active_phrase"], 7)

    def test_pc_events(self):
        self.proj.pattern(0, Pat("A", kit=0).hit(0, 0))
        self.proj.pattern(1, Pat("B", kit=1).hit(0, 0))
        self.proj.song(0, "S", [0, 0, 1], end=lxr.SONG_LOOP)
        snap, _ = self.convert()
        for ph in range(5):
            self.assertIsNone(events_of(snap, ph, 15)[0])
        snap, _ = self.convert(pc_events="songs")
        self.assertIsNone(events_of(snap, 0, 15)[0])
        self.assertIsNone(events_of(snap, 2, 15)[0])               # A -> A: no change
        pc, seq = events_of(snap, 3, 15)                             # A -> B
        self.assertEqual(pc["events"], [[15, 0.0, 1.0, 192, 1, 1, 0, 0, 0, 0, 0, 100, 1, 100, 1]])
        self.assertEqual((seq["tracks"][0]["chan"], seq["repeat"]), (15, 1))
        pc, _ = events_of(snap, 4, 15)                               # B -> A (the loop)
        self.assertEqual([e[4] for e in pc["events"]], [0])
        snap, _ = self.convert(pc_events="all")
        pc, seq = events_of(snap, 1, 15)
        self.assertEqual(([e[:5] for e in pc["events"]], seq["repeat"]), ([[0, 0.0, 1.0, 192, 1]], 255))

    def test_missing_kit(self):
        self.proj.pattern(0, Pat("NOKIT", kit=9).hit(0, 0, locks=[(51, 3)]).hit(0, 8))
        snap, report = self.convert()
        self.assertEqual(report.counts["patterns whose kit is missing (the init kit used)"], 1)
        ctl = snap["zs3"]["*/0"]["processors"]["1"]["controllers"]
        for p in PORTS["ports"]:
            if p["kit"]:
                self.assertEqual(ctl[p["symbol"]]["value"], p["default"])
        bd, _ = events_of(snap, 0, 0)
        self.assertEqual([(e[0], e[4], e[6]) for e in controls(bd)], [(0, 51, 3), (8, 51, 21)])

    def test_several_projects(self):
        self.proj.pattern(0, Pat("A", kit=0).hit(0, 0))
        other = Project(self.tmp, "PROJ01")
        other.globals(90, "OTHER")
        other.kit(0, "K", kit_params(3))
        other.pattern(4, Pat("B", kit=0).hit(0, 0))
        report = conv.Report()
        snap, _ = conv.convert([lxr.Project(self.proj.dir), lxr.Project(other.dir)], PORTS, report=report)
        phrases = snap["zynseq"]["scenes"][0]["phrases"]
        self.assertEqual([(ph["name"], ph["tempo"]) for ph in phrases], [("TESTPROJ 00 A", 133.0), ("OTHER 04 B", 90.0)])
        self.assertEqual([z["title"] for z in snap["zs3"].values()], ["00 A", "TESTPROJ 00 A", "OTHER 04 B"])
        ctl = snap["zs3"]["*/1"]["processors"]["1"]["controllers"]
        self.assertEqual(ctl["bd_ampeg_decay"]["value"], kit_params(3)[50])

    def test_cli(self):
        self.proj.pattern(0, Pat("A", kit=1).hit(0, 0))
        out = os.path.join(self.tmp, "out")
        with open(os.devnull, "w") as null:
            old, sys.stdout = sys.stdout, null
            try:
                rc = conv.main([self.proj.dir, "--out", out, "--prog", "7", "--presets", "--mode", "kit"])
            finally:
                sys.stdout = old
        self.assertEqual(rc, 0)
        files = sorted(os.listdir(out))
        self.assertEqual(files, ["007-TESTPROJ.zss", "eDrumSynth-TESTPROJ.presets.lv2"])
        with open(os.path.join(out, files[0])) as f:
            self.assertEqual(json.load(f)["chains"]["1"]["midi_chan"], 9)
        self.assertTrue(os.path.isfile(os.path.join(out, files[1], "manifest.ttl")))


# ---- the hybrid layout ----

def port(symbol):
    return conv.prepare_ports(PORTS)["by_symbol"][symbol]


def limited(symbol, value):
    p = port(symbol)
    return min(p["max"], max(p["min"], value))


class HybridTest(Base):
    def hybrid(self, **kw):
        return self.convert(mode="hybrid", **kw)

    def ctl(self, snap, zs3, symbol):
        return snap["zs3"][zs3]["processors"]["1"]["controllers"][symbol]["value"]

    def test_fold(self):
        # constant notes 63 (BD), 60 (SD), 58 (CY): all in the kit column,
        # GM notes, the tunings folded in the pattern's ZS3; a lock of the
        # bass drum's coarse tune (CC 9 = parameter 8) folded too, its
        # restore the folded kit value
        p = Pat("FOLD", kit=1)      # kit 1: slot 3 (T2) runs the snare engine
        p.hit(0, 0, note=63, locks=[(9, 40)]).hit(0, 8, note=63).hit(3, 4).hit(4, 2, note=58).hit(2, 6, note=61)
        self.proj.pattern(0, p)
        snap, report = self.hybrid()
        self.assertEqual(list(snap["chains"]), ["1", "0"])
        self.assertEqual(snap["chains"]["1"]["midi_chan"], 9)
        kit, seq = events_of(snap, 0, 9)
        self.assertEqual((seq["tracks"][0]["chan"], seq["name"], seq["repeat"]), (9, "Kit", 255))
        self.assertEqual(sorted((e[0], e[4]) for e in notes(kit)), [(0, 36), (2, 49), (4, 38), (6, 41), (8, 36)])
        for col in range(9):
            self.assertEqual(events_of(snap, 0, col)[1]["repeat"], 0)
        base = conv.kit_controls(PORTS, lxr.Project(self.proj.dir).kit(1))
        z = "*/0"
        self.assertEqual(self.ctl(snap, z, "master_midi_mode"), 1)
        for sym, k in (("bd_osc_coarse", 3), ("bd_fm_freq", 3), ("cy_osc_coarse", -2), ("cy_modosc_freq1", -2),
                       ("cy_modosc_freq2", -2), ("t2_osc_coarse", 1), ("sd_osc_coarse", 0), ("t1_osc_coarse", 0)):
            self.assertEqual(self.ctl(snap, z, sym), limited(sym, base[sym] + k), sym)
        # the snare engine (T2 in kit 1): only the coarse tune moves
        self.assertEqual(self.ctl(snap, z, "t2_noise_freq"), base["t2_noise_freq"])
        self.assertEqual(self.ctl(snap, "zs3-0", "bd_osc_coarse"), base["bd_osc_coarse"] + 3)
        cc = [(e[0], e[4], e[6]) for e in controls(kit)]
        coarse = base["bd_osc_coarse"] + port("bd_osc_coarse")["offset"]     # the engine value
        self.assertEqual(cc, [(0, 9, 43), (8, 9, coarse + 3)])
        self.assertEqual(report.counts, {})

    def test_fold_per_pattern(self):
        # one kit, two patterns with other constant notes: each ZS3 its fold
        self.proj.pattern(0, Pat("A", kit=0).hit(0, 0, note=63))
        self.proj.pattern(1, Pat("B", kit=0).hit(0, 0, note=55).hit(0, 4, note=55))
        self.proj.pattern(2, Pat("C", kit=0).hit(0, 0))
        snap, _ = self.hybrid()
        base = conv.kit_controls(PORTS, lxr.Project(self.proj.dir).kit(0))["bd_osc_coarse"]
        self.assertEqual([self.ctl(snap, z, "bd_osc_coarse") for z in ("*/0", "*/1", "*/2")],
                         [base + 3, base - 5, base])
        self.assertEqual(list(snap["chains"]), ["1", "0"])

    def test_fold_limited(self):
        # a coarse tune at the top of the range: the fold is limited (the
        # oscillator limits the note alike: exact), not counted
        params = kit_params(0)
        params[port("bd_osc_coarse")["param"]] = 127
        self.proj.kit(2, "HIGH", params)
        self.proj.pattern(0, Pat("HI", kit=2).hit(0, 0, note=63))
        snap, report = self.hybrid()
        self.assertEqual(self.ctl(snap, "*/0", "bd_osc_coarse"), port("bd_osc_coarse")["max"])
        self.assertEqual(report.counts, {})
        self.assertEqual(list(snap["chains"]), ["1", "0"])

    def test_melodic_track(self):
        # track 2's notes vary: its own chain (MIDI, no processors, thru to
        # chain 1) and column; the kit column leaves it out
        a = Pat("MEL", kit=0).hit(0, 0).hit(1, 0, note=60).hit(1, 4, note=67, locks=[(51, 5)]).hit(1, 8, note=55)
        self.proj.pattern(0, a)
        self.proj.pattern(1, Pat("NOT2", kit=1).hit(0, 0, note=62))     # track 2 silent here
        snap, report = self.hybrid()
        self.assertEqual(list(snap["chains"]), ["1", "3", "0"])
        chain = snap["chains"]["3"]
        self.assertEqual(chain, {"title": "LXR T2", "midi_chan": 1, "midi_thru": True, "audio_thru": False,
                                 "zmop_index": 2, "cc_route": snap["chains"]["1"]["cc_route"], "slots": [],
                                 "zctrls": []})
        self.assertIn(51, chain["cc_route"])
        kit, _ = events_of(snap, 0, 9)
        self.assertEqual([e[4] for e in notes(kit)], [36])
        t2, seq = events_of(snap, 0, 1)
        self.assertEqual((seq["tracks"][0]["chan"], seq["repeat"], seq["name"]), (1, 255, "T2 Tom1"))
        self.assertEqual([(e[0], e[4]) for e in notes(t2)], [(0, 60), (4, 67), (8, 55)])
        self.assertEqual([(e[0], e[4]) for e in controls(t2)], [(4, 51), (8, 51)])
        # the other row: column 2 silent (replaces the previous row's), kit
        # column folded
        self.assertEqual(events_of(snap, 1, 1)[0], None)
        self.assertEqual(events_of(snap, 1, 1)[1]["repeat"], 255)
        for ph in snap["zynseq"]["scenes"][0]["phrases"]:
            self.assertEqual(len(ph["sequences"]), 32)
            self.assertEqual([i for i, q in enumerate(ph["sequences"]) if q["repeat"]], [1, 9])
        # ZS3s: zs3-0 with the track chain's routing, the others chain 1 only
        z0 = snap["zs3"]["zs3-0"]["chains"]
        self.assertEqual(z0["3"], {"midi_chan": 1, "midi_in": ["ZynMidiRouter:ch2_out"], "midi_out": [1],
                                   "audio_in": [], "audio_out": [0]})
        self.assertEqual(z0["1"]["midi_chan"], 9)
        for z in ("*/0", "*/1"):
            self.assertEqual(list(snap["zs3"][z]["chains"]), ["1"])
        self.assertEqual(report.counts, {})

    def test_polyrhythm(self):
        # lengths that don't divide the pattern: own chains, native lengths
        # (repeated to whole beats); a length that divides it: the kit column
        p = Pat("POLY", kit=0)
        p.lengths = [16, 12, 7, 8, 16, 16, 16]
        p.hit(0, 0).hit(1, 0).hit(1, 11).hit(2, 0).hit(2, 3).hit(3, 2)
        self.proj.pattern(0, p)
        snap, report = self.hybrid()
        self.assertEqual(list(snap["chains"]), ["1", "3", "4", "0"])
        t2, _ = events_of(snap, 0, 1)
        self.assertEqual((t2["beats"], t2["steps"], [e[0] for e in notes(t2)]), (3, 12, [0, 11]))
        t3, _ = events_of(snap, 0, 2)
        self.assertEqual((t3["beats"], t3["steps"]), (7, 28))
        self.assertEqual([e[0] for e in notes(t3)], [0, 3, 7, 10, 14, 17, 21, 24])
        kit, _ = events_of(snap, 0, 9)
        self.assertEqual(sorted((e[0], e[4]) for e in notes(kit)), [(0, 36), (2, 38), (10, 38)])
        self.assertEqual(report.counts, {})

    def test_hihat_slot(self):
        # tracks 6 and 7 share slot 6: track 6's note folds, track 7 with
        # another constant note gets a chain, its notes less the fold
        self.proj.pattern(0, Pat("HH", kit=0).hit(5, 0, note=63).hit(6, 4, note=65).hit(6, 8, note=65))
        snap, report = self.hybrid()
        self.assertEqual(list(snap["chains"]), ["1", "8", "0"])
        self.assertEqual(snap["chains"]["8"]["midi_chan"], 6)
        ho, _ = events_of(snap, 0, 6)
        self.assertEqual([e[4] for e in notes(ho)], [62, 62])
        self.assertEqual([e[4] for e in notes(events_of(snap, 0, 9)[0])], [42])
        base = conv.kit_controls(PORTS, lxr.Project(self.proj.dir).kit(0))["hh_osc_coarse"]
        self.assertEqual(self.ctl(snap, "*/0", "hh_osc_coarse"), limited("hh_osc_coarse", base + 3))
        # the same notes: both in the kit column
        self.proj.pattern(0, Pat("HH", kit=0).hit(5, 0, note=63).hit(6, 4, note=63))
        snap, report = self.hybrid()
        self.assertEqual(list(snap["chains"]), ["1", "0"])

    def test_track_chains_all_none(self):
        p = Pat("X", kit=0)
        p.lengths[2] = 7
        p.hit(0, 0, note=63).hit(1, 0, note=60).hit(1, 4, note=62).hit(2, 0)
        self.proj.pattern(0, p)
        snap, report = self.hybrid(track_chains="all")
        self.assertEqual(list(snap["chains"]), ["1"] + [str(t + 2) for t in range(7)] + ["0"])
        self.assertEqual([snap["chains"][str(t + 2)]["midi_chan"] for t in range(7)], list(range(7)))
        self.assertEqual(len(snap["zs3"]["zs3-0"]["chains"]), 8)
        self.assertEqual([e[4] for e in notes(events_of(snap, 0, 0)[0])], [63])     # no fold: the note
        self.assertEqual(notes(events_of(snap, 0, 9)[0]), [])
        self.assertEqual(self.ctl(snap, "*/0", "bd_osc_coarse"),
                         conv.kit_controls(PORTS, lxr.Project(self.proj.dir).kit(0))["bd_osc_coarse"])
        self.assertEqual([i for i, q in enumerate(snap["zynseq"]["scenes"][0]["phrases"][0]["sequences"])
                          if q["repeat"]], list(range(7)) + [9])
        snap, report = self.hybrid(track_chains="none")
        self.assertEqual(list(snap["chains"]), ["1", "0"])
        self.assertEqual(report.counts, {"step notes (no track chain: the kit's pitch)": 1,
                                         "track lengths that don't divide the pattern (no track chain: the track "
                                         "restarts with the pattern)": 1})
        snap2, _ = self.convert(mode="kit")
        self.assertEqual(snap2, snap)

    def test_songs(self):
        # a song-only pattern counts for the track chains too
        self.proj.pattern(0, Pat("A", kit=0).hit(0, 0))
        self.proj.pattern(1, Pat("B", kit=0).hit(4, 0, note=50).hit(4, 2, note=52))
        self.proj.song(0, "S", [0, 1], end=lxr.SONG_LOOP)
        snap, _ = self.hybrid(patterns=[0])
        self.assertEqual(list(snap["chains"]), ["1", "6", "0"])
        phrases = snap["zynseq"]["scenes"][0]["phrases"]
        self.assertEqual([ph["name"] for ph in phrases], ["00 A", "S 1: 00 A", "S 2: 01 B"])
        self.assertEqual(events_of(snap, 1, 4)[1]["repeat"], 1)      # silent in the song row
        cy, seq = events_of(snap, 2, 4)
        self.assertEqual(([e[4] for e in notes(cy)], seq["repeat"]), ([50, 52], 1))

    def test_cli_default(self):
        self.proj.pattern(0, Pat("A", kit=1).hit(1, 0, note=61).hit(1, 4, note=62))
        out = os.path.join(self.tmp, "out")
        with open(os.devnull, "w") as null:
            old, sys.stdout = sys.stdout, null
            try:
                rc = conv.main([self.proj.dir, "--out", out])
            finally:
                sys.stdout = old
        self.assertEqual(rc, 0)
        with open(os.path.join(out, "000-TESTPROJ.zss")) as f:
            snap = json.load(f)
        self.assertEqual((snap["chains"]["1"]["midi_chan"], list(snap["chains"])), (9, ["1", "3", "0"]))


# ---- zynthian_state_schema.py ----

def load_schema():
    path = os.path.join(ZYNTHIAN_UI, "zynthian_state_schema.py")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and node.targets[0].id == "ZynthianState":
            return ast.literal_eval(node.value)
    return None


class SchemaTest(Base):
    def test_keys(self):
        schema = load_schema()
        if schema is None:
            self.skipTest("no zynthian-ui checkout (ZYNTHIAN_UI=%s)" % ZYNTHIAN_UI)
        self.proj.pattern(0, Pat("A", kit=0).hit(0, 0, locks=[(200, 3)]).hit(0, 4).hit(2, 0, note=62).hit(2, 4, note=65))
        self.proj.pattern(1, Pat("B", kit=1).hit(1, 0, note=63))
        self.proj.song(0, "S", [0, 1], end=lxr.SONG_LOOP)
        for mode, tc in (("multi-all", "needed"), ("kit", "needed"), ("hybrid", "needed"), ("hybrid", "all")):
            snap, _ = self.convert(mode=mode, pc_events="songs", track_chains=tc)
            snap = json.loads(json.dumps(snap))         # as written

            def sub(keys, ref, what):
                extra = set(keys) - set(ref)
                self.assertFalse(extra, "%s: keys not in the schema: %s" % (what, sorted(extra)))

            sub(snap, schema, "snapshot")
            self.assertEqual(snap["schema_version"], schema["schema_version"])
            self.assertNotIn("midi_profile_state", snap)
            for cid, chain in snap["chains"].items():
                sub(chain, schema["chains"]["1"], "chain %s" % cid)
            self.assertEqual(snap["chains"]["0"]["slots"], [{"-1": "MR"}])
            zref = schema["zs3"]["zs3-0"]
            self.assertIn("zs3-0", snap["zs3"])
            for zid, z in snap["zs3"].items():
                sub(z, zref, "zs3 %s" % zid)
                self.assertIn("title", z)
                self.assertIn("active_chain", z)
                for cid, c in z["chains"].items():
                    sub(c, zref["chains"]["1"], "zs3 %s chain %s" % (zid, cid))
                    self.assertIn("audio_out", c)
                for pid, p in z["processors"].items():
                    sub(p, zref["processors"]["1"], "zs3 %s processor %s" % (zid, pid))
                    for s, c in p["controllers"].items():
                        sub(c, zref["processors"]["1"]["controllers"]["volume"], "controller %s" % s)
            zs, zsref = snap["zynseq"], schema["zynseq"]
            sub(zs, zsref, "zynseq")
            pref = zsref["patns"]["1"]
            for pid, p in zs["patns"].items():
                sub(p, pref, "pattern %s" % pid)
                for e in p["events"]:
                    self.assertEqual(len(e), len(pref["events"][0]))
            for scene in zs["scenes"]:
                sub(scene, zsref["scenes"][0], "scene")
                for ph in scene["phrases"]:
                    sub(ph, zsref["scenes"][0]["phrases"][0], "phrase")
                    self.assertEqual(len(ph["sequences"]), 32)
                    for seq in ph["sequences"]:
                        sref = zsref["scenes"][0]["phrases"][0]["sequences"][0]
                        sub(seq, sref, "sequence")
                        for t in seq["tracks"]:
                            sub(t, sref["tracks"][0], "track")


# ---- libzynseq ----

def find_libzynseq():
    path = os.environ.get("EDS_LIBZYNSEQ")
    if path:
        return path if os.path.isfile(path) else None
    for p in ("tests/native/build/libzynseq.so", "zynlibs/zynseq/build/libzynseq.so"):
        p = os.path.join(ZYNTHIAN_UI, p)
        if os.path.isfile(p):
            return p
    return None


# setState() then getState() in a child process (the library's global
# state, its stderr noise); no init(): no JACK client
ROUNDTRIP = r"""
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
lib.setState.restype = ctypes.c_bool
lib.setState.argtypes = [ctypes.c_char_p]
lib.getState.restype = ctypes.c_char_p
if not lib.setState(sys.stdin.buffer.read()):
    sys.exit(3)
sys.stdout.buffer.write(lib.getState())
"""


class LibZynseqTest(Base):
    def roundtrip(self, lib, state):
        r = subprocess.run([sys.executable, "-I", "-c", ROUNDTRIP, lib], input=json.dumps(state).encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace")[-2000:])
        return json.loads(r.stdout)

    def compare(self, ours, got):
        self.assertEqual(got["tempo"], ours["tempo"])
        self.assertEqual(got["bpb"], ours["bpb"])
        # patterns: the library keeps those with events, under their ids
        for pid, p in ours["patns"].items():
            if not p["events"]:
                continue
            g = got["patns"].get(pid)
            self.assertIsNotNone(g, "pattern %s lost" % pid)
            for k in ("beats", "steps", "swingDiv", "chance", "refNote"):
                self.assertEqual(g[k], p[k], "pattern %s %s" % (pid, k))
            self.assertAlmostEqual(g["swing"], p["swing"], places=5)
            self.assertEqual(len(g["events"]), len(p["events"]), "pattern %s events" % pid)
            for a, b in zip(p["events"], g["events"]):
                # the event chance: float / 100, back as int (x * 100):
                # truncation may lose 1
                self.assertEqual(b[:11] + b[12:], a[:11] + a[12:], "pattern %s event %s / %s" % (pid, a, b))
                self.assertIn(a[11] - b[11], (0, 1), "pattern %s event chance %s / %s" % (pid, a, b))
                self.assertAlmostEqual(b[1], a[1], places=5)
        ph_ours, ph_got = ours["scenes"][0]["phrases"], got["scenes"][0]["phrases"]
        self.assertEqual(len(ph_got), len(ph_ours))
        for a, b in zip(ph_ours, ph_got):
            for k in ("name", "bpb", "tempo", "repeat", "followAction", "followParam", "followRepeat"):
                self.assertEqual(b[k], a[k], "phrase %s %s" % (a["name"], k))
            self.assertEqual(len(b["sequences"]), 32)
            for col, (sa, sb) in enumerate(zip(a["sequences"], b["sequences"])):
                for k in ("group", "repeat", "name"):
                    self.assertEqual(sb[k], sa[k], "phrase %s column %d %s" % (a["name"], col, k))
                ta, tb = sa["tracks"][0], sb["tracks"][0]
                self.assertEqual(tb["chan"], ta["chan"])
                if ta["patns"]:
                    self.assertEqual(tb["patns"], {k: v for k, v in ta["patns"].items()})
                else:
                    # the library's empty default pattern (or none)
                    for pid in tb.get("patns", {}).values():
                        self.assertNotIn(str(pid), got["patns"])

    def test_roundtrip(self):
        lib = find_libzynseq()
        if not lib:
            self.skipTest("no libzynseq.so (EDS_LIBZYNSEQ, or a zynthian-ui build in %s)" % ZYNTHIAN_UI)
        p = Pat("ALL", kit=0)
        p.lengths = [16, 12, 7, 32, 16, 16, 16]
        p.shuffle = 30
        p.hit(0, 0, vel=120, prob=33, locks=[(51, 5), (200, 7)]).hit(0, 8, flam=3).hit(1, 3, note=70, shift=2)
        p.hit(2, 2, shift=-4).hit(3, 31, locks=[(38, 1)])
        self.proj.pattern(0, p)
        self.proj.pattern(1, Pat("B", kit=1).hit(4, 0).hit(5, 2).hit(6, 4))
        self.proj.song(0, "S", [0, 1, 1], end=lxr.SONG_LOOP)
        self.proj.song(1, "E", [1, 0])
        for mode, tc in (("multi-all", "needed"), ("kit", "needed"), ("hybrid", "needed"), ("hybrid", "all")):
            for pc in (None, "songs"):
                snap, _ = self.convert(mode=mode, pc_events=pc, track_chains=tc)
                ours = json.loads(json.dumps(snap["zynseq"]))
                got = self.roundtrip(lib, ours)
                self.compare(ours, got)


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
