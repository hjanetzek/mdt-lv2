#!/usr/bin/env python3
"""LXR-02 / OpenElixir-02 projects as Zynthian snapshots with eDrumSynth.

  lxr2zynthian.py PROJDIR [PROJDIR...] [--patterns N...] [--songs N...]
                  [--mode multi|kit] [--name NAME] [--prog NNN] [--one]
                  [--out DIR] [--presets] [--pc-events songs|all]
                  [--install HOST [--bank BANK]] [--ports PORTS_JSON] [-v]

Each project (an LXR project folder: NN-*.PAT, NN-*.SND, NN-*.SNG,
GLO.CFG, PRJ.NFO) becomes a snapshot NNN-Name.zss (schema 6) in --out
(default: here); --one puts all projects given into one snapshot. Load it
on Zynthian and play the patterns from the launcher:

- one eDrumSynth chain (chain 1, with its mixer strip) and the main mixbus;
- zynseq: one launcher row (phrase) per pattern ("NN NAME"), then each song
  as a chain of rows (follow actions: the next row, at the end back to the
  song's first row if it loops, else stop), sharing the patterns' zynseq
  patterns; tempo from GLO.CFG;
- one ZS3 per pattern, in pattern order ("NN NAME", program change N =
  the pattern's index): the pattern's kit as every eDrumSynth control (no
  preset, no plugin reload) and its launcher row selected, so zs3_next /
  zs3_prev or program changes browse the patterns with their kits; zs3-0
  is the first pattern's. Patterns only in songs follow (their first song
  row selected).

Layouts (--mode): multi (default): eDrumSynth's MIDI Mode Multi, the chain
on all MIDI channels, LXR track n in launcher column n-1 (MIDI channel n),
one zynseq pattern per track with the step notes and the track's own length
(multiples of 4 steps; others unrolled to the pattern's bars); kit: MIDI
Mode Kit, the chain on channel 10, one zynseq pattern per LXR pattern in
column 10 with General MIDI drum notes (step notes and track lengths lost).

Steps: velocity, probability (event chance), flams (stutter), shift
(event offset), shuffle (the pattern's swing), parameter locks as CC / NRPN
events with the restores to the kit values (the LXR's semantics: when the
track's next step plays; a lock that would be restored at the loop is
restored at the pattern's last step instead, so leaving the pattern
restores it too). The snapshot turns on eDrumSynth's "LXR CC" switch and
routes the CCs used (and 6, 98, 99) to the chain.

--pc-events songs: a program change event (MIDI channel 16, column 16)
at the last step of a song row whose next row has another kit, switching
that pattern's ZS3; all: also at step 0 of each pattern's launcher row (it
reloads the ZS3 at every loop). Default: none (ZS3s by hand, zs3_next or
external program changes).
--presets: also the project's kits as an LV2 presets bundle
(eDrumSynth-<project>.presets.lv2, as tools/snd2lv2.py).
--install HOST: copies the snapshots to root@HOST:/zynthian/zynthian-my-
data/snapshots/BANK/ (default bank 000) and the presets bundles to
.../presets/lv2/ with ssh / scp.
-v: every step feature that isn't converted (always: the counts).

Python 3, no dependencies; the LXR format: lxrformat.py, the port map:
edrumsynth_ports.json, the kit mapping: snd2lv2.py (next to this script).
CONVERTER_PLAN.md (lxrdrums/) describes the mapping and its limits.
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lxrformat as lxr  # noqa: E402
import snd2lv2  # noqa: E402

ENGINE = "JV/eDrumSynth"
CHAIN, PROC, MIXER_PROC = 1, 1, 2
STEPS_PER_BEAT = 4          # LXR steps are 16ths
BPB = 4
PPQN = 1920                 # zynseq clocks per beat
COLUMNS = 32                # sequences per phrase (0-15: MIDI channels)
KIT_CHANNEL = 9             # MIDI channel 10
PC_CHANNEL = 15             # MIDI channel 16: the program change column
ALL_CHANNELS = 0xFFFF       # a chain on every MIDI channel
GM_NOTES = (36, 45, 41, 38, 49, 42, 46)     # track 1-7 in Kit mode (src/notemap.h)
TRACK_NAMES = ("BD", "T1", "T2", "SD", "CY", "HC", "HO")
CC_NOTE, CC_CONTROL, CC_PROGRAM = 144, 176, 192
CC_DATA, CC_NRPN_FINE, CC_NRPN_COARSE = 6, 98, 99
END_RESTORE_OFFSET = 0.875  # the restore at the pattern's last step: late in it
LOCK, RESTORE = 2, 1        # priorities at one step
MAIN_OUT = 0                # chain audio_out: the main mixbus
FOLLOW_NONE, FOLLOW_RELATIVE = 0, 1
LOOP = 255                  # sequence repeat: forever
SWING_PER_SHUFFLE = 4.0 / 3.0 / 127.0
MAX_PROGRAM = 127


class Report:
    """what isn't converted (counts; -v: each one)"""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self.counts = {}

    def note(self, what, detail=None):
        self.counts[what] = self.counts.get(what, 0) + 1
        if self.verbose and detail:
            print("  %s: %s" % (what, detail))

    def merge(self, other):
        for k, v in other.counts.items():
            self.counts[k] = self.counts.get(k, 0) + v


def event(step, cmd, v1, v2, offset=0.0, duration=1.0, v2end=None, stutter=0, chance=100):
    """a zynseq event (15 values: zynseq.cpp setState())"""
    return [step, round(offset, 4), duration, cmd, v1, v1, v2, v2 if v2end is None else v2end,
            stutter, 0, 0, chance, 1, 100, 1]


class Lane:
    """the events of one zynseq pattern, with zynseq's rule that events
    with the same command and value 1 at one step replace each other: per
    step one value per CC and one NRPN (CC 99 / 98 / 6), locks before
    restores, notes once per note number"""

    def __init__(self, steps, report, where):
        self.steps = steps
        self.report = report
        self.where = where
        self.ccs = {}       # step -> {cc: (number, value, priority, offset)}
        self.nrpns = {}     # step -> (number, value, priority, offset)
        self.notes = {}     # step -> {note: event}
        self.used_ccs = set()

    def control(self, step, number, value, priority, offset=0.0):
        """a parameter (LXR MIDI numbering: up to 127 CC n, above NRPN
        n - 128) to value at step: a lock replaces a restore (of the same
        CC: expected; of another NRPN: reported), of two locks the later
        one stays (reported)"""
        if number <= 127:
            slot, key, what = self.ccs.setdefault(step, {}), number, "CC %d" % number
        else:
            slot, key, what = self.nrpns, step, "NRPN %d" % (number - 128)
        have = slot.get(key)
        new = (number, value, priority, offset)
        if have is not None:
            if have[:2] == new[:2]:
                if priority > have[2]:
                    slot[key] = new
                return
            # the one that goes: the restore, else the earlier lock
            lost = new if priority < have[2] else have
            if lost[2] == RESTORE:
                if lost[0] != (have if lost is new else new)[0]:
                    self.report.note("NRPN restores dropped (another NRPN at the step: zynseq keeps one)",
                                     "%s step %d: NRPN %d" % (self.where, step + 1, lost[0] - 128))
            elif number <= 127:
                self.report.note("locks of one CC at one step (Kit mode: tracks; zynseq keeps one): the later kept",
                                 "%s step %d: %s" % (self.where, step + 1, what))
            else:
                self.report.note("NRPN locks at one step (zynseq keeps one): the later kept",
                                 "%s step %d: NRPN %d" % (self.where, step + 1, lost[0] - 128))
            if lost is new:
                return
        slot[key] = new
        if number <= 127:
            self.used_ccs.add(number)
        else:
            self.used_ccs.update((CC_NRPN_COARSE, CC_NRPN_FINE, CC_DATA))

    def note(self, step, offset, note, velocity, chance, hits):
        notes = self.notes.setdefault(step, {})
        if note in notes:
            self.report.note("notes at one step with the same note number (shift): dropped",
                             "%s step %d: note %d" % (self.where, step + 1, note))
            return
        notes[note] = event(step, CC_NOTE, note, velocity, offset, 1.0, 0, 2 * hits if hits > 1 else 0, chance)

    def events(self):
        """in step order; at a step the NRPN (99, 98, 6), the CCs, then the
        notes (zynseq plays events of a step in this order)"""
        out = []
        for step in range(self.steps):
            if step in self.nrpns:
                number, value, _, offset = self.nrpns[step]
                nrpn = number - 128
                out += [event(step, CC_CONTROL, CC_NRPN_COARSE, nrpn >> 7, offset),
                        event(step, CC_CONTROL, CC_NRPN_FINE, nrpn & 0x7F, offset),
                        event(step, CC_CONTROL, CC_DATA, value, offset)]
            for cc, (_, value, _, offset) in sorted(self.ccs.get(step, {}).items()):
                out.append(event(step, CC_CONTROL, cc, value, offset))
            for note in sorted(self.notes.get(step, {})):
                out.append(self.notes[step][note])
        return out


def lock_param(number):
    """the engine parameter of a lock number (CC n: n - 1, NRPN: n)"""
    return number - 1 if number <= 127 else number


def add_track(lane, pattern, track, steps, kit_params, mode, report, where):
    """track's steps of pattern into lane (steps long: the track looped or
    unrolled); kit_params: the kit's parameter values (restores)"""
    length = pattern.lengths[track]
    src = [pattern.tracks[track][i % length] for i in range(steps)]
    triggers = [i for i in range(steps) if src[i].active]
    for i in triggers:
        s = src[i]
        # the note: Multi plays the step note on the track's channel, Kit the GM note
        if mode == "multi":
            note = s.note
        else:
            note = GM_NOTES[track]
            if s.note != lxr.DEFAULT_NOTE:
                report.note("step notes (Kit mode: the voices' tuned pitch)", "%s step %d: note %d" % (where, i + 1, s.note))
        step, offset = i, 0.0
        if s.shift > 0:
            offset = s.shift / 8.0
        elif s.shift < 0:
            step, offset = (i - 1) % steps, 1.0 + s.shift / 8.0
        lane.note(step, offset, note, s.velocity, s.probability, s.flam)

    # parameter locks: set when the step plays, restored to the kit value
    # when the track's next step plays (unless that step locks it again)
    def locked_at(i):
        return {lock[0] for lock in src[i].locks if lock and lock[0] != 0}

    for q, i in enumerate(triggers):
        for lock in src[i].locks:
            if not lock:
                continue
            number, value = lock
            if number == 0:
                if i < length:
                    report.note("locks on CC 0 (bank select: no parameter)", "%s step %d" % (where, i + 1))
                continue
            lane.control(i, number, value, LOCK)
            param = lock_param(number)
            if param >= len(kit_params):
                report.note("locks of parameters outside kits (not restored)", "%s step %d: %d" % (where, i + 1, number))
                continue
            restore = kit_params[param]
            if q + 1 < len(triggers):
                nxt = triggers[q + 1]
                if number not in locked_at(nxt):
                    lane.control(nxt, number, restore, RESTORE)
            elif i < steps - 1:
                # the next step that plays is after the loop: restored at the
                # last step (also when the pattern changes)
                lane.control(steps - 1, number, restore, RESTORE, END_RESTORE_OFFSET)
            else:
                report.note("locks on the pattern's last step (restored at the loop, not when the pattern changes)",
                            "%s step %d" % (where, i + 1))
                if triggers[0] != i and number not in locked_at(triggers[0]):
                    lane.control(triggers[0], number, restore, RESTORE)


def zynseq_pattern(steps, events, swing=0.0):
    return {"beats": steps // STEPS_PER_BEAT, "steps": steps, "scale": 0, "tonic": 0, "refNote": 60, "zoom": 0,
            "quantize": 0, "swingDiv": 1, "swing": swing, "humanTime": 0.0, "humanVel": 0.0, "chance": 100,
            "events": events}


def kit_controls(m, kit, mode):
    """every eDrumSynth control for a kit (None: the init kit): port symbol -> value"""
    values = {}
    if kit is not None:
        values = snd2lv2.kit_values(m, kit.params, kit.engines, lambda text: None)
    controls = {}
    for p in m["ports"]:
        s = p["symbol"]
        if s in values:
            v = values[s]
        elif p["type"] == "midi_mode":
            v = m["midi_mode_port"]["values"][mode]
        elif p["type"] == "lxr_cc":
            v = m["lxr_cc_port"]["values"]["on"]
        else:
            v = p["default"]
        controls[s] = float(v) if p["kind"] == "float" else int(v)
    return controls


def kit_param_values(m, kit):
    """the engine's value of every kit parameter with the kit loaded (the
    restore values): the ports' values where a port has the parameter (out
    of range values: the init kit's, as the engine), else the kit's"""
    if kit is None:
        params = [0] * m["kit_size"]
        for p in m["ports"]:
            if p["type"] == "param" and p["kit"] and p["param"] < m["kit_size"]:
                params[p["param"]] = p["kit_init"]
        return params
    params = list(kit.params)
    values = snd2lv2.kit_values(m, kit.params, kit.engines, lambda text: None)
    names = m["engines"]
    for p in m["ports"]:
        if p["type"] != "param" or not p["kit"] or p["param"] >= m["kit_size"]:
            continue
        if p["slot"] is not None and names[kit.engines[p["slot"]]] not in p["engines"]:
            continue
        params[p["param"]] = int(values[p["symbol"]] + p["offset"])
    return params


class Converted:
    """one LXR pattern as zynseq patterns: per column (pattern id, length
    in steps)"""

    def __init__(self, project, nr, pattern):
        self.project = project
        self.nr = nr
        self.pattern = pattern
        self.columns = {}       # column -> (pattern id, steps)
        self.steps = pattern.bars() * lxr.BAR
        self.title = "%02d %s" % (nr, pattern.name) if pattern.name else "%02d" % nr
        self.kit = None
        self.zs3 = None         # its ZS3 id
        self.program = None     # its program change (None: none)
        self.phrase = None      # its launcher row


class Builder:
    def __init__(self, m, mode, report):
        self.m = m
        self.mode = mode
        self.report = report
        self.patns = {}
        self.next_id = 1
        self.used_ccs = set()
        self.patterns = []      # Converted, in order
        self.zs3_order = []     # Converted with a ZS3, in order
        self.phrases = []

    def add_patn(self, steps, events, swing=0.0):
        pid = self.next_id
        self.next_id += 1
        self.patns[str(pid)] = zynseq_pattern(steps, events, swing)
        return pid

    def swing(self, pattern, where):
        if not pattern.shuffle:
            return 0.0
        swing = pattern.shuffle * SWING_PER_SHUFFLE
        if swing > 1.0:
            self.report.note("shuffle above zynseq's swing range (limited to 1.0)", "%s: %d" % (where, pattern.shuffle))
            swing = 1.0
        return round(swing, 4)

    def convert_pattern(self, project, nr):
        pattern = project.pattern(nr)
        c = Converted(project, nr, pattern)
        c.kit = project.kit(pattern.kit)
        where = "%s pattern %02d" % (project.name, nr)
        if c.kit is None:
            self.report.note("patterns whose kit is missing (the init kit used)", "%s: kit %d" % (where, pattern.kit))
        kit_params = kit_param_values(self.m, c.kit)
        swing = self.swing(pattern, where)
        for t in range(lxr.TRACKS):
            for i, s in enumerate(pattern.tracks[t][:pattern.lengths[t]]):
                if not s.active and any(s.locks):
                    self.report.note("parameter locks on steps that don't play (the LXR ignores them too)",
                                     "%s track %d step %d" % (where, t + 1, i + 1))
        if self.mode == "multi":
            for t in range(lxr.TRACKS):
                length = pattern.lengths[t]
                if not any(s.active for s in pattern.tracks[t][:length]):
                    continue
                if length % STEPS_PER_BEAT == 0:
                    steps = length
                else:
                    steps = c.steps
                    if steps % length:
                        self.report.note("track lengths that aren't multiples of 4 steps (unrolled to the pattern's bars: "
                                         "the track restarts with the pattern)", "%s track %d: %d" % (where, t + 1, length))
                lane = Lane(steps, self.report, "%s track %d" % (where, t + 1))
                add_track(lane, pattern, t, steps, kit_params, self.mode, self.report, lane.where)
                self.used_ccs |= lane.used_ccs
                c.columns[t] = (self.add_patn(steps, lane.events(), swing), steps)
        else:
            lane = Lane(c.steps, self.report, where)
            for t in range(lxr.TRACKS):
                length = pattern.lengths[t]
                if not any(s.active for s in pattern.tracks[t][:length]):
                    continue
                if c.steps % length:
                    self.report.note("track lengths that don't divide the pattern (Kit mode: the track restarts with the "
                                     "pattern)", "%s track %d: %d" % (where, t + 1, length))
                add_track(lane, pattern, t, c.steps, kit_params, self.mode, self.report, "%s track %d" % (where, t + 1))
            self.used_ccs |= lane.used_ccs
            c.columns[KIT_CHANNEL] = (self.add_patn(c.steps, lane.events(), swing), c.steps)
        self.patterns.append(c)
        return c

    def phrase(self, name, c, song_steps=None, follow=(FOLLOW_NONE, 0), pc=None, tempo=0.0):
        """a launcher row playing c's zynseq patterns; song_steps: in a song
        (the children play once through the row), else they loop; pc:
        (step, program) a program change event in column 16"""
        sequences = []
        used = range(lxr.TRACKS) if self.mode == "multi" else (KIT_CHANNEL,)
        for col in range(COLUMNS):
            seq = {"mode": 1, "group": col, "name": "", "repeat": 0, "followAction": FOLLOW_NONE, "followParam": 0,
                   "tracks": [{"chan": col if col < 16 else 0, "output": 0, "map": 0, "patns": {}}]}
            if col in c.columns:
                pid, steps = c.columns[col]
                seq["tracks"][0]["patns"] = {"0": pid}
                seq["repeat"] = LOOP if song_steps is None else max(1, math.ceil(song_steps / steps))
            elif col in used:
                # silent (the library's empty pattern): replaces what the
                # column played in the row before
                seq["repeat"] = LOOP if song_steps is None else 1
            if col in used:
                seq["name"] = TRACK_NAMES[col] if self.mode == "multi" else "Kit"
            if col == PC_CHANNEL and pc is not None:
                step, program = pc
                pid = self.add_patn(c.steps, [event(step, CC_PROGRAM, program, 0)])
                seq["tracks"][0]["patns"] = {"0": pid}
                seq["repeat"] = LOOP if song_steps is None else 1
                seq["name"] = "Kit PC"
            sequences.append(seq)
        if c.phrase is None:
            c.phrase = len(self.phrases)    # a pattern only in songs: its first row
        self.phrases.append({"name": name[:32], "mode": 1, "bpb": BPB, "tempo": tempo, "repeat": c.steps // lxr.BAR,
                             "followAction": follow[0], "followParam": follow[1], "playFlags": 0, "followRepeat": 0,
                             "sequences": sequences})
        return len(self.phrases) - 1


def safe_name(name):
    name = re.sub(r"[/\\:\0]+", "_", name).strip()
    return name or "LXR"


def convert(projects, m, mode="multi", patterns=None, songs=None, name=None, pc_events=None, report=None):
    """the snapshot (a dict) of projects (lxrformat.Project); patterns /
    songs: the numbers to convert (None: all non-empty ones; with several
    projects, of each); returns (snapshot, summary)"""
    report = report or Report()
    b = Builder(m, mode, report)
    several = len(projects) > 1
    bpms = {p.bpm for p in projects}
    tempo = projects[0].bpm

    def phrase_tempo(project):
        return float(project.bpm) if len(bpms) > 1 else 0.0

    def prefix(project):
        return "%s " % project.name if several else ""

    # the patterns' launcher rows, in project and pattern order
    converted = {}
    for p in projects:
        nrs = p.pattern_numbers() if patterns is None else [n for n in patterns if p.files["PAT"].get(n)]
        for n in nrs:
            if p.pattern(n).is_empty():
                report.note("empty patterns asked for (left out)", "%s pattern %02d" % (p.name, n))
                continue
            converted[(id(p), n)] = b.convert_pattern(p, n)
        if patterns is not None:
            for n in patterns:
                if n not in p.files["PAT"]:
                    report.note("patterns asked for that the project hasn't", "%s pattern %02d" % (p.name, n))
    launch = list(b.patterns)
    if not launch:
        raise lxr.FormatError("no patterns to convert")
    for c in launch:
        assign_zs3(b, c)
    for c in launch:
        pc = (0, c.program) if pc_events == "all" and c.program is not None else None
        b.phrase(prefix(c.project) + c.title, c, pc=pc, tempo=phrase_tempo(c.project))

    # songs: chains of rows sharing the patterns' zynseq patterns
    nsongs = 0
    for p in projects:
        nrs = p.song_numbers() if songs is None else songs
        for sn in nrs:
            song = p.song(sn)
            if song is None:
                report.note("songs asked for that the project hasn't", "%s song %02d" % (p.name, sn))
                continue
            order = []
            for n in song.patterns:
                key = (id(p), n)
                if key not in converted:
                    if p.pattern(n) is None or p.pattern(n).is_empty():
                        report.note("song positions with an empty or missing pattern (left out)",
                                    "%s song %02d: pattern %d" % (p.name, sn, n))
                        continue
                    converted[key] = assign_zs3(b, b.convert_pattern(p, n))     # a pattern not asked for
                order.append(converted[key])
            if not order:
                report.note("empty songs (left out)", "%s song %02d" % (p.name, sn))
                continue
            nsongs += 1
            sname = song.name or "Song %02d" % sn
            for k, c in enumerate(order):
                last = k == len(order) - 1
                if not last:
                    follow = (FOLLOW_RELATIVE, 1)
                elif song.loop:
                    follow = (FOLLOW_RELATIVE, -(len(order) - 1))
                else:
                    follow = (FOLLOW_NONE, 0)
                nxt = order[k + 1] if not last else (order[0] if song.loop else None)
                pc = None
                if pc_events and nxt is not None and nxt.pattern.kit != c.pattern.kit and nxt.program is not None:
                    pc = (c.steps - 1, nxt.program)
                # a looping song of one row: its children loop
                steps = None if (song.loop and len(order) == 1) else c.steps
                b.phrase("%s%s %d: %s" % (prefix(p), sname, k + 1, c.title), c, steps, follow, pc,
                         phrase_tempo(p))

    snapshot = build_snapshot(b, b.zs3_order, mode, tempo, name or (projects[0].name if not several else "LXR"))
    summary = ("%d pattern%s, %d song%s (%d launcher rows), %d kits, %d zynseq patterns, %d BPM, %s mode"
               % (len(launch), "" if len(launch) == 1 else "s", nsongs, "" if nsongs == 1 else "s", len(b.phrases),
                  len({id(c.kit) for c in launch}), len(b.patns), tempo, mode))
    return snapshot, summary


def assign_zs3(b, c):
    """c's ZS3 id and program change: the next in order ("*/N"; above
    program 127 a plain "zs3-N" without one)"""
    n = len(b.zs3_order)
    if n <= MAX_PROGRAM:
        c.program, c.zs3 = n, "*/%d" % n
    else:
        c.zs3 = "zs3-%d" % (n + 1)
    b.zs3_order.append(c)
    return c


def zs3_entry(m, c, mode, title, extra=None):
    midi_chan = ALL_CHANNELS if mode == "multi" else KIT_CHANNEL
    entry = {
        "title": title,
        "active_chain": CHAIN,
        "active_phrase": c.phrase,
        "chains": {str(CHAIN): {"midi_chan": midi_chan, "midi_in": ["ZynMidiRouter:ch0_out"], "audio_in": [],
                                "audio_out": [MAIN_OUT]}},
        "processors": {str(PROC): {"bank_info": None, "bank_subdir_info": None, "preset_info": None,
                                   "preset_subdir_info": None, "controllers": {
                                       s: {"value": v} for s, v in kit_controls(m, c.kit, mode).items()}}},
    }
    if extra:
        entry["processors"].update(extra)
    return entry


def build_snapshot(b, launch, mode, tempo, name):
    m = b.m
    midi_chan = ALL_CHANNELS if mode == "multi" else KIT_CHANNEL
    cc_route = sorted(b.used_ccs | {CC_DATA, CC_NRPN_FINE, CC_NRPN_COARSE})
    chains = {
        str(CHAIN): {"title": "LXR %s" % name, "midi_chan": midi_chan, "midi_thru": False, "audio_thru": False,
                     "zmop_index": 0, "cc_route": cc_route,
                     "slots": [{str(PROC): ENGINE}, {str(MIXER_PROC): "MI"}], "zctrls": []},
        "0": {"title": "Main", "midi_chan": None, "midi_thru": False, "audio_thru": True, "zmop_index": None,
              "cc_route": [], "slots": [{"-1": "MR"}], "zctrls": []},
    }
    first = launch[0]
    tempo_proc = {"-4": {"bank_info": None, "bank_subdir_info": None, "preset_info": None, "preset_subdir_info": None,
                         "controllers": {"bpm": {"value": float(tempo)}, "bpb": {"value": BPB}}}}
    zs3 = {"zs3-0": zs3_entry(m, first, mode, first.title, tempo_proc)}
    for c in launch:
        title = ("%s %s" % (c.project.name, c.title)) if len({x.project for x in launch}) > 1 else c.title
        zs3[c.zs3] = zs3_entry(m, c, mode, title)
    zynseq = {"tempo": float(tempo), "bpb": BPB, "scene": 0, "patns": b.patns,
              "scenes": [{"name": "Scene 1", "phrases": b.phrases}]}
    return {"schema_version": 6, "chains": chains, "zs3": zs3, "last_zs3_id": first.zs3,
            "zynseq": zynseq, "gui": {"pinned_chains": 1}}


def write_presets(m, project, out):
    banks = []
    for bank, files in snd2lv2.collect([project.folder]):
        presets = [k for k in (snd2lv2.read_kit(m, f, False, False) for f in files) if k]
        if presets:
            banks.append((project.name, presets))
    if not banks:
        return None, 0
    path = os.path.join(out, "eDrumSynth-%s.presets.lv2" % re.sub(r"[^A-Za-z0-9_-]+", "-", project.name).strip("-"))
    return path, snd2lv2.write_bundle(m, banks, path)


def install(host, bank, snapshots, bundles):
    base = "/zynthian/zynthian-my-data"
    cmds = [["ssh", "root@%s" % host, "mkdir -p '%s/snapshots/%s' '%s/presets/lv2'" % (base, bank, base)]]
    if snapshots:
        cmds.append(["scp"] + snapshots + ["root@%s:%s/snapshots/%s/" % (host, base, bank)])
    if bundles:
        cmds.append(["scp", "-r"] + bundles + ["root@%s:%s/presets/lv2/" % (host, base)])
    for cmd in cmds:
        print("+ " + " ".join(cmd))
        subprocess.run(cmd, check=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("projects", nargs="+", metavar="PROJDIR")
    ap.add_argument("--patterns", type=int, nargs="+", metavar="N", help="these patterns (default: all non-empty)")
    ap.add_argument("--songs", type=int, nargs="*", metavar="N", help="these songs (default: all non-empty; none: --songs)")
    ap.add_argument("--mode", choices=("multi", "kit"), default="multi")
    ap.add_argument("--name", help="the snapshot's name (default: the project's)")
    ap.add_argument("--prog", type=int, default=0, help="the program number NNN of the (first) snapshot")
    ap.add_argument("--one", action="store_true", help="all projects in one snapshot")
    ap.add_argument("--out", default=".", help="the output folder")
    ap.add_argument("--presets", action="store_true", help="also the project's kits as an LV2 presets bundle")
    ap.add_argument("--pc-events", choices=("songs", "all"), help="program change events switching the kits' ZS3s")
    ap.add_argument("--install", metavar="HOST", help="copy the files to a Zynthian (ssh / scp as root)")
    ap.add_argument("--bank", default="000", help="the snapshot bank folder on the Zynthian (default 000)")
    ap.add_argument("--ports", default=os.path.join(HERE, "edrumsynth_ports.json"))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    m = snd2lv2.load_ports(args.ports)
    if "lxr_cc_port" not in m:
        sys.exit("lxr2zynthian: %s is older than eDrumSynth 0.2.0 (no LXR CC port)" % args.ports)
    try:
        projects = [lxr.Project(p) for p in args.projects]
    except (lxr.FormatError, OSError) as e:
        sys.exit("lxr2zynthian: %s" % e)
    groups = [projects] if args.one else [[p] for p in projects]
    os.makedirs(args.out, exist_ok=True)
    total = Report(args.verbose)
    written, bundles = [], []
    for i, group in enumerate(groups):
        report = Report(args.verbose)
        name = args.name if args.name and (len(groups) == 1) else None
        try:
            snapshot, summary = convert(group, m, args.mode, args.patterns, args.songs, name, args.pc_events, report)
        except (lxr.FormatError, OSError) as e:
            print("%s: %s" % (", ".join(p.folder for p in group), e), file=sys.stderr)
            continue
        title = name or (group[0].name if len(group) == 1 else "LXR")
        path = os.path.join(args.out, "%03d-%s.zss" % (args.prog + i, safe_name(title)))
        with open(path, "w", encoding="utf-8") as f:
            json.dump(snapshot, f, separators=(",", ":"))
            f.write("\n")
        written.append(path)
        print("%s: %s" % (path, summary))
        for what, count in sorted(report.counts.items()):
            print("  not converted: %s (%d)" % (what, count))
        total.merge(report)
        if args.presets:
            for p in group:
                bundle, count = write_presets(m, p, args.out)
                if bundle:
                    bundles.append(bundle)
                    print("%s: %d presets" % (bundle, count))
    if len(groups) > 1 and total.counts:
        print("all: not converted:")
        for what, count in sorted(total.counts.items()):
            print("  %s (%d)" % (what, count))
    if args.install and written:
        install(args.install, args.bank, written, bundles)
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
