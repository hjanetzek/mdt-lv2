"""LXR-02 / OpenElixir-02 project files: patterns, songs, kits, tempo, name.

A project folder (PROJxx) holds up to 64 of each, named NN-NAME.EXT (NN
00-63):

- NN-NAME.SND, a kit: 8 byte name, the 247 sound parameters (PAR_*), from
  OpenElixir-02 its engine block ("OE02", size, the engine of each of the 6
  slots). Older LXR kits have fewer parameters (the others load as 0).
- NN-NAME.PAT, a pattern: 8 byte name, the version tag "PatV02" / "PatV03"
  (V1 has none); per track (7): 64 steps of 7 bytes (volume: bit 7 active,
  bits 0-6 velocity; probability 0-100; note, 60 = the voice's own pitch;
  two parameter locks: number, value, 255 = none), V3 then 64 timing bytes
  (low nibble flam: off, x2, x3, x4, x6, x8; high nibble shift 0-14, 7 =
  none, in eighths of a step); then the main steps (per track 4 bars x 16
  bits, a set bit plays the step), the shuffle (0-127), the 7 track lengths
  (1-64; 0 = 16) and, from V2, the kit number. Sizes: V1 3208 bytes, V2
  3215, V3 3663.
- NN-NAME.SNG, a song: 8 byte name, 64 pattern numbers (64 = end, 65 =
  loop back to the start).
- GLO.CFG: the globals from PAR_BEGINNING_OF_GLOBALS on, the first byte the
  tempo (PAR_BPM). PRJ.NFO: the project's name.

A parameter lock's number is the LXR's MIDI numbering (as
midiParser_ccHandler() takes it): up to 127 the CC n (parameter n - 1),
above that the NRPN n - 128 (parameter n). The LXR sets it when the step
plays and returns the parameter to its kit value when the track's next step
plays (autoNode_setDestination()).

Adapted from cynthian's port/tools/lxr2mdt.py (copied: mdt-lv2 is
self-contained). Python 3, no dependencies.
"""
import os
import re

TRACKS = 7              # LXR tracks (track 7: the open hi-hat, slot 6's second trigger)
STEPS = 64              # steps per track (4 bars of 16)
BAR = 16                # steps per bar (16ths)
DEFAULT_NOTE = 60       # SEQ_DEFAULT_NOTE: the voice at its tuned pitch
NO_LOCK = 255
SONG_LENGTH = 64
SONG_END, SONG_LOOP = 64, 65
KIT_SIZE = 247          # END_OF_SOUND_PARAMETERS
NAME_SIZE = 8
ENGINE_BLOCK = b"OE02"
STOCK_ENGINES = (0, 0, 0, 1, 2, 3)      # drum, drum, drum, snare, cymbal, hi-hat
FLAM_HITS = (1, 2, 3, 4, 6, 8)          # the flam index: hits in the step (flamIdxToPattern())
SHIFT_NONE = 7
PATTERN_SIZES = {1: 3208, 2: 3215, 3: 3663}


class FormatError(Exception):
    pass


def name8(data):
    return data[:NAME_SIZE].split(b"\0")[0].decode("latin1").strip()


class Step:
    """one step of a track"""
    __slots__ = ("active", "velocity", "probability", "note", "locks", "flam", "shift")

    def __init__(self, raw, timing, active):
        vol, prob, note, p1n, p1v, p2n, p2v = raw
        self.active = active                    # the main step bit
        self.velocity = max(1, vol & 0x7F)
        self.probability = min(prob, 100)
        self.note = note
        # (number, value) per lock slot, None: no lock
        self.locks = tuple(None if n == NO_LOCK else (n, v & 0x7F) for n, v in ((p1n, p1v), (p2n, p2v)))
        flam = timing & 0x0F
        self.flam = FLAM_HITS[flam] if flam < len(FLAM_HITS) else 1     # hits in the step
        shift = timing >> 4
        self.shift = shift - SHIFT_NONE if shift <= 14 else 0           # eighths of a step, -7..+7


class Pattern:
    def __init__(self, name, version, tracks, shuffle, lengths, kit):
        self.name = name
        self.version = version
        self.tracks = tracks        # [track][step] Step
        self.shuffle = shuffle      # 0-127
        self.lengths = lengths      # per track, 1-64
        self.kit = kit              # kit number (V1: 0)

    def is_empty(self):
        return not any(s.active for t in self.tracks for s in t)

    def bars(self):
        """the pattern's bars: its longest track rounded up to whole bars"""
        return (max(self.lengths) + BAR - 1) // BAR


def parse_pattern(data):
    """an LXR pattern file's bytes (OpenElixir-02 preset_readPatternData())"""
    if data[8:12] == b"PatV":
        try:
            version = int(data[12:14])
        except ValueError:
            raise FormatError("unknown pattern version tag %r" % data[8:14])
        o = 14
    else:
        version, o = 1, 8
    if version not in PATTERN_SIZES:
        raise FormatError("unknown pattern version %d" % version)
    if len(data) < PATTERN_SIZES[version] - (1 if version > 1 else 0):
        raise FormatError("%d bytes, a V%d pattern has %d" % (len(data), version, PATTERN_SIZES[version]))
    raw, timing = [], []
    for t in range(TRACKS):
        raw.append([data[o + i * 7:o + i * 7 + 7] for i in range(STEPS)])
        o += STEPS * 7
        if version > 2:
            timing.append(data[o:o + STEPS])
            o += STEPS
        else:
            timing.append(bytes([SHIFT_NONE << 4] * STEPS))
    main = [[int.from_bytes(data[o + (t * 4 + b) * 2:o + (t * 4 + b) * 2 + 2], "little") for b in range(4)]
            for t in range(TRACKS)]
    o += TRACKS * 4 * 2
    shuffle = data[o] & 0x7F
    lengths = [x if 1 <= x <= STEPS else BAR for x in data[o + 1:o + 1 + TRACKS]]
    kit = data[o + 1 + TRACKS] if version > 1 and len(data) > o + 1 + TRACKS else 0
    tracks = [[Step(raw[t][i], timing[t][i], bool(main[t][i // BAR] >> (i % BAR) & 1)) for i in range(STEPS)]
              for t in range(TRACKS)]
    return Pattern(name8(data), version, tracks, shuffle, lengths, kit)


def read_pattern(path):
    with open(path, "rb") as f:
        return parse_pattern(f.read())


class Song:
    def __init__(self, name, patterns, loop):
        self.name = name
        self.patterns = patterns    # pattern numbers in order
        self.loop = loop            # True: back to the first at the end; False: stop


def parse_song(data):
    patterns, loop = [], False
    for x in data[NAME_SIZE:NAME_SIZE + SONG_LENGTH]:
        if x == SONG_END:
            break
        if x == SONG_LOOP:
            loop = True
            break
        patterns.append(x)
    return Song(name8(data), patterns, loop)


def read_song(path):
    with open(path, "rb") as f:
        return parse_song(f.read())


class Kit:
    def __init__(self, name, params, engines, count):
        self.name = name
        self.params = params        # KIT_SIZE values (missing ones 0)
        self.engines = engines      # the engine of each slot (0 drum .. 3 hi-hat)
        self.has_engine_block = None
        self.count = count          # parameters in the file


def parse_kit(data):
    """name, the 247 sound parameters, the engines (as lxr_engine_decode_engines())"""
    if len(data) < NAME_SIZE + 128:
        raise FormatError("%d bytes: not a kit" % len(data))
    body = data[NAME_SIZE:NAME_SIZE + KIT_SIZE]
    params = list(body) + [0] * (KIT_SIZE - len(body))
    engines = list(STOCK_ENGINES)
    block = data[NAME_SIZE + KIT_SIZE:]
    has_block = len(block) >= 5 + len(engines) and block[:4] == ENGINE_BLOCK and block[4] >= len(engines)
    if has_block:
        for slot in range(len(engines)):
            if block[5 + slot] < 4:
                engines[slot] = block[5 + slot]
    kit = Kit(name8(data), params, engines, len(body))
    kit.has_engine_block = has_block
    return kit


def read_kit(path):
    with open(path, "rb") as f:
        return parse_kit(f.read())


def find_file(folder, name):
    """a file of the folder by name, any case"""
    for f in os.listdir(folder):
        if f.upper() == name.upper():
            return os.path.join(folder, f)
    return None


def read_bpm(folder, default=120):
    """GLO.CFG's tempo (its first byte, PAR_BPM)"""
    path = find_file(folder, "GLO.CFG")
    if path:
        with open(path, "rb") as f:
            d = f.read(1)
        if d and 20 <= d[0] <= 250:
            return d[0]
    return default


def read_project_name(folder):
    """PRJ.NFO's name, else the folder's"""
    path = find_file(folder, "PRJ.NFO")
    if path:
        with open(path, "rb") as f:
            name = name8(f.read())
        if name:
            return name
    return os.path.basename(os.path.normpath(folder))


class Project:
    """an LXR project folder: its files by number (parsed on demand)"""

    def __init__(self, folder):
        if not os.path.isdir(folder):
            raise FormatError("%s: not a folder" % folder)
        self.folder = folder
        self.name = read_project_name(folder)
        self.bpm = read_bpm(folder)
        self.files = {"PAT": {}, "SND": {}, "SNG": {}}
        for f in sorted(os.listdir(folder)):
            m = re.match(r"(\d\d)-.*\.(PAT|SND|SNG)$", f, re.I)
            if m and int(m.group(1)) < 64:
                self.files[m.group(2).upper()].setdefault(int(m.group(1)), os.path.join(folder, f))
        self._cache = {}

    def _get(self, ext, nr, reader):
        key = (ext, nr)
        if key not in self._cache:
            path = self.files[ext].get(nr)
            self._cache[key] = reader(path) if path else None
        return self._cache[key]

    def pattern(self, nr):
        return self._get("PAT", nr, read_pattern)

    def kit(self, nr):
        return self._get("SND", nr, read_kit)

    def song(self, nr):
        return self._get("SNG", nr, read_song)

    def pattern_numbers(self, nonempty=True):
        nrs = sorted(self.files["PAT"])
        return [n for n in nrs if not nonempty or not self.pattern(n).is_empty()]

    def song_numbers(self, nonempty=True):
        nrs = sorted(self.files["SNG"])
        return [n for n in nrs if not nonempty or self.song(n).patterns]


# ---- writers (the tests' synthetic projects) ----

def pack_pattern(name, version, tracks, main, shuffle, lengths, kit=0):
    """the file bytes of a pattern: tracks[t][i] = (vol, prob, note, p1n,
    p1v, p2n, p2v, timing), main[t] = 4 x 16 bit words, lengths: 7 bytes"""
    out = bytearray(name.encode("latin1")[:NAME_SIZE].ljust(NAME_SIZE, b"\0"))
    if version > 1:
        out += b"PatV%02d" % version
    for t in range(TRACKS):
        for i in range(STEPS):
            out += bytes(tracks[t][i][:7])
        if version > 2:
            out += bytes(tracks[t][i][7] for i in range(STEPS))
    for t in range(TRACKS):
        for b in range(4):
            out += main[t][b].to_bytes(2, "little")
    out.append(shuffle)
    out += bytes(lengths)
    if version > 1:
        out.append(kit)
    assert len(out) == PATTERN_SIZES[version], (version, len(out))
    return bytes(out)


def pack_song(name, patterns, end=SONG_END):
    entries = list(patterns) + [end]
    entries += [SONG_END] * (SONG_LENGTH - len(entries))
    return name.encode("latin1")[:NAME_SIZE].ljust(NAME_SIZE, b"\0") + bytes(entries[:SONG_LENGTH])


def pack_kit(name, params, engines=None, count=KIT_SIZE):
    out = name.encode("latin1")[:NAME_SIZE].ljust(NAME_SIZE, b"\0") + bytes(params[:count])
    if engines is not None:
        out += ENGINE_BLOCK + bytes([len(engines)]) + bytes(engines)
    return out
