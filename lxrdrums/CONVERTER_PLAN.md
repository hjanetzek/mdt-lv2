# LXR projects to Zynthian: kits as LV2 presets / ZS3s, patterns and songs as zynseq

Goal: a tool that turns LXR-02 / OpenElixir-02 project folders (kits,
patterns, songs, tempo) into a Zynthian snapshot with an eDrumSynth chain
and the patterns in Zynthian's sequencer (zynseq), so a user loads one
snapshot and plays the project: patterns from the launcher, songs as phrase
chains, kits switched by ZS3s (sub-snapshots: controller values, no plugin
reload). Plus the kits as an LV2 presets bundle for use without the
snapshot.

Status: implemented, desktop-tested (2026-11-26): eDrumSynth 0.2.0 (the
LXR CC switch, §4.3), `tools/lxrformat.py`, `tools/lxr2zynthian.py`,
`tests/converter_test.py`; the implementation notes, deviations and the
factory projects' counts: §8. The hybrid layout for Zynthian's pad view
(new default, desktop-tested 2026-11-26): §9 and its implementation notes
§9.1. Next: the phone (§5's last item, §9's checks, with the
device-testing session). Plan of 2026-11-25; depends on eDrumSynth v2
(`PLAN.md` §13).

## 1. Facts

**LXR project** (`port/LXR_PATTERN_CONVERTER.md` in the cynthian repo; its
parser `port/tools/lxr2mdt.py`): a folder with up to 64 each of
`NN-NAME.SND` (kit: 8-byte name, 247 parameters, OpenElixir-02's engine
block), `NN-NAME.PAT` (pattern: per track 64 steps of velocity / active,
probability 0-100, note (60 = tuned pitch), two parameter locks; V3 adds
flam and shift per step; main step bits 4 bars × 16; shuffle; 7 track
lengths 1-64; the kit number), `NN-NAME.SNG` (64 pattern numbers, 64 = end,
65 = loop), `GLO.CFG` (first byte the tempo), `PRJ.NFO` (name). A lock sets
a parameter (the LXR's MIDI numbering: up to 127 a CC, above an NRPN) for its
step; the parameter returns to the kit value at the track's next step with a
lock.

**Zynthian** (zynthian-ui fork; `zynthian_state_schema.py` documents every
key; save `zynthian_state_manager.py:1028 get_state()`, load `:1171`):
- Snapshot `.zss`: one JSON object, write `schema_version: 6` (5 triggers a
  legacy conversion). `chains` (chain 1: `slots
  [{"1": "JV/eDrumSynth"}, {"2": "MI"}]`, the mixer strip `MI` listed
  explicitly; chain 0 the main mixbus `[{"-1": "MR"}]`), `zs3` (`zs3-0` =
  the base state, required; restores controllers, routing, MIDI learn after
  the chains and zynseq), `zynseq` (JSON), no `midi_profile_state`.
  File name `snapshots/<bank>/NNN-Name.zss` (NNN = the program number).
- ZS3 entry: `title`, `active_chain` (both required), `chains` (per chain:
  `restore`, `midi_chan`, `midi_in`, `audio_out` (always, it is reset
  otherwise), ...), `processors` (per id: `preset_info` (a preset URI) or
  null, `controllers` {symbol: {"value": v}}; only applied for chains listed
  in `chains`; ids < 0 always: `-4` tempo `bpm` / `bpb`). Omit `global` and
  `midi_capture` in per-kit ZS3s. Restoring a ZS3 never adds / removes
  chains, never reloads plugins, never touches zynseq; controller changes
  go to jalv as `symbol=value` (only values that differ from the cache), so
  not sample accurate. With both a preset and controller values, controllers
  equal to stale cached values may not be resent: use `preset_info: null`
  plus all controllers.
- ZS3 triggers: program change (`"<chan>/<pgm>"`, fallback `"*/<pgm>"`),
  including **a program change event inside a zynseq pattern** (the
  sequencer's output reaches the UI; polled every 10 ms), CUIAs
  `zs3_next` / `zs3_prev` / `zs3_load`, MIDI-learned PC.
- zynseq JSON: `tempo`, `bpb`, `patns` {id: pattern}, `scenes[].phrases[]`
  (launcher rows): `name`, `repeat` (bars), `followAction` 1 relative,
  `followParam` (+1 next, -N back), `followRepeat`, `tempo`, and 32
  `sequences` (columns 0-15 = MIDI channels, all 32 must be present). A
  sequence: `repeat` (0 off, 255 loop), `group` (mutually exclusive),
  `tracks [{"chan", "patns": {start in 1920-per-beat clocks: pattern id}}]`.
  A pattern: `beats`, `steps` (steps per beat in 1,2,3,4,6,8,12,24),
  `swing` / `swingDiv`, `chance`, `events` (15 values: step, offset (steps),
  duration, cmd, v1start, v1end, v2start, v2end, stutter speed / velocity fx
  / ramp, chance %, play frequency (every n-th loop), stutter chance /
  frequency). Note: cmd 144, v1 = note, v2start = velocity, **v2end 0**. CC:
  cmd 176, v1 = CC, v2 = value (start = end). PC: cmd 192. Overlapping events
  with the same cmd + v1 replace each other. Nothing plays after a load.
- No timeline arranger: songs are phrases chained by follow actions.
  Launching is quantized to the bar; sequences in one group replace each
  other at the bar.
- zynseq row names: `zynthian-data/zynseq/keymaps.json` matches the
  processor's preset path to a `.midnam` (e.g. `GM-Drumkits`).
- Validation: zynthian-ui's `tests/native/zynseq/conftest.py` loads the real
  `libzynseq.so` (ctypes) with `setState` / `getState`;
  `tools/test_factory_snapshots.py` checks snapshots on a device. No
  snapshot generator exists yet.

## 2. Tool

`lxrdrums/tools/lxr2zynthian.py` (Python 3, no dependencies), with the LXR
parser as a module (`lxrformat.py`, adapted from cynthian's
`port/tools/lxr2mdt.py`, copied: mdt-lv2 is self-contained) and the port map
of eDrumSynth (`edrumsynth.lv2/ports.json` or a tools-side JSON written by
`gen_ttl`: symbol, parameter number, engine family, conversion, the engine
selectors; shared with v2's kit converter).

```
lxr2zynthian.py PROJDIR [PROJDIR...] [--patterns N...] [--songs N...]
                [--mode multi|kit] [--name NAME] [--prog NNN]
                [--out DIR] [--presets] [--install HOST]
```
Writes `NNN-Name.zss` per project (or one for all given), and with
`--presets` the project's kits as an LV2 presets bundle
(`eDrumSynth-<project>.presets.lv2`). `--install` copies both to a Zynthian
(`/zynthian/zynthian-my-data/snapshots/<bank>/`,
`/zynthian/zynthian-my-data/presets/lv2/`) over ssh.

## 3. The mapping

| LXR | Zynthian |
|---|---|
| project | one snapshot: an eDrumSynth chain (+ MI), zynseq state, ZS3s |
| kit (SND) | a ZS3 `"*/N"` ("Kit NN NAME"): all eDrumSynth controllers incl. the engine selectors, `preset_info: null`; also an LV2 preset (`--presets`) |
| pattern | a launcher phrase ("NN NAME"): its tracks' patterns in the columns (§3.1), `repeat` = its bars |
| pattern's kit | a program change event (cmd 192) switching its ZS3 (§3.2) |
| step active + velocity | note event, velocity |
| step note | Multi mode: the note on the track's channel (60 = tuned pitch); Kit mode: lost (the GM note) |
| step probability | event chance % |
| parameter locks | CC / NRPN events on the track (§4.3), plus restore events |
| flam (x2..x8) | stutter speed (retriggers in the step) |
| shift (-7..+7 eighths of a step) | event offset (negative: the previous step, offset 1 - x) |
| shuffle | the pattern's swing (mapped from the LXR's shuffle table) |
| track length | Multi mode: the track's pattern length where it is a multiple of 4 steps; otherwise unrolled to the pattern's bars (as lxr2mdt.py) |
| song | a chain of phrases (copies of the pattern phrases sharing the same zynseq patterns), `followAction` relative +1; loop flag: back to the first; end: stop |
| tempo | zynseq `tempo` and zs3-0's `-4` `bpm` |
| project name | snapshot name |

### 3.1 Two layouts

- **Multi (default)**: eDrumSynth's MIDI Mode = Multi, the chain on all MIDI
  channels (`midi_chan` 0xffff, `zmop_set_midi_chan_all`). LXR track n →
  launcher column n-1 (channel n-1), one zynseq pattern per track, notes =
  the step notes (pitch kept), per-track lengths possible. Columns of one
  phrase start together; a phrase swap replaces them at the bar.
- **Kit**: MIDI Mode = Kit, the chain on channel 10, one zynseq pattern per
  LXR pattern in column 9 with the GM notes (named rows with a keymap entry
  `"eDrumSynth": "GM-Drumkits"` in zynthian-data). Simpler to edit in
  Zynthian; loses step notes and per-track lengths.

### 3.2 Kits

- One ZS3 per kit the selected patterns use; zs3-0 has the first pattern's
  kit. A pattern whose kit differs from the previous one gets a program
  change event: for songs at the last step of the preceding phrase (the
  ZS3 is applied by the UI thread and jalv's stdin, so it lands tens of ms
  later: measure on the phone), for launcher use at the pattern's step 0 or
  left to the user (`zs3_next`, an external PC). Decide after measuring.
- Later option, if ZS3 switching is too slow or jittery: an internal kit
  bank in eDrumSynth (LV2 state + a kit-select port or PC, sample accurate),
  filled from the project.

## 4. What eDrumSynth needs

1. v2 (`PLAN.md` §13): engines per slot (kits with other engine layouts),
   Multi MIDI mode (step notes), the kit converter's mapping.
2. The machine-readable port map (§2) written by `gen_ttl`.
3. **v2.1: an "LXR CC" switch** (port, **default off**, decided 2026-11-25: on,
   the pedals Zynthian passes (CC 64/66/67/69), keyboard CCs (7, 10, 11)
   and panic messages (120-123) would change drum parameters, and the
   engine would drift from the ports unseen): CCs and NRPNs (99 /
   98 / 6) on any channel go to `lxr_engine_midi_cc()` (CC n = parameter
   n-1, NRPN n = 128 + n), as the LXR. The snapshot turns it on and puts the
   used CC numbers (and 6, 98, 99) into the chain's `cc_route` (synth chains
   drop other CCs). The converter inserts the restore events (the kit value
   at the track's next locked step, as the LXR's automation nodes) and a
   restore at the pattern's end. A lock changes the engine, not the port:
   ports keep their values, and the next ZS3 or port change wins.

## 5. Tests

- Parser: synthetic project folders written by the test (all pattern
  versions V1-V3, songs with end / loop, kits with and without the engine
  block); no third-party projects in git.
- Mapping: per feature (velocity, note, probability, flam, shift, shuffle,
  locks with restores, lengths, songs) against expected zynseq events.
- The zynseq JSON through the real `libzynseq.so` (`setState` then
  `getState`, compare), when a zynthian-ui checkout and the library are
  available (skipped otherwise; path from an environment variable).
- The snapshot's schema against `zynthian_state_schema.py`'s keys.
- Real projects (cynthian's `tmp/lxr`: factory PROJ01-05, HRTL, the DLC
  demo; read only, never committed): convert all, count what isn't mapped
  (`-v` as lxr2mdt.py).
- On the phone (with the device-testing session and the user): load a
  snapshot, play patterns from the launcher, a song, kit switching by ZS3
  (and its latency), locks audible.

## 6. Steps

1. Port map from `gen_ttl` (with v2).
2. `lxrformat.py` + tests (synthetic projects).
3. Kits: ZS3s and the presets bundle (reuses v2's kit mapping).
4. Patterns: Multi and Kit layouts, launcher phrases; tests incl. libzynseq.
5. Songs, tempo, swing, flam, shift, probability.
6. eDrumSynth v2.1 "LXR CC" + locks in the converter.
7. The real projects; then the phone.
8. zynthian-data: the `eDrumSynth` keymap entry (through the device-testing
   session / nura's zynthian-data package).

## 7. Decisions (2026-11-26)

- Default layout: **Multi** (Kit as an option).
- Parameter locks via eDrumSynth v2.1's "LXR CC" switch, **off by default**;
  the snapshots turn it on.
- Kit changes: **ZS3s, decided** (2026-11-26; no latency measurement, no
  internal kit bank): they are the only way to switch kits while browsing
  sequences without reloading the plugin. One ZS3 per pattern ("NN NAME":
  the pattern's kit controllers, `active_phrase` = its launcher row), in
  pattern order, so `zs3_next` / `zs3_prev` (or program changes) browse the
  patterns; program change events inside patterns stay optional.
- Test data: the projects in cynthian's `tmp/lxr` (never committed);
  **snapshots generated from them may be installed on the phone**
  (coordinated with the device-testing session).

## 8. Implementation notes (2026-11-26)

Where the implementation settles or differs from §1-§7:

- **eDrumSynth 0.2.0** (not 0.1.1: a new port is a minor version in LV2's
  rules): `master_lxr_cc`, a toggle in the Master group after MIDI Mode,
  index 308 (all others unchanged), frozen in `tests/symbols.txt`; the
  port map has `lxr_cc_port`. On: CCs and NRPNs on any channel go to
  `lxr_engine_midi_cc()`, except the **engine selection NRPN 98**
  (OpenElixir-02's value = slot * 4 + engine), which the plugin drops: the
  engine selector ports own the engines (a switch by NRPN would reset the
  slot behind the ports' backs and the next `run()` would switch it back).
  The plugin keeps its own copy of the NRPN selection to know it. A CC
  changes only the engine, the port cache stays: the next port change
  wins; a port reloaded with an unchanged value (a ZS3 with the same kit)
  doesn't undo a CC, which is why the converter writes the restores.
  render test: off changes nothing; on is bit exact against the engine
  driven by `lxr_engine_midi_cc()` for host blocks 1-4096; the engine
  NRPN changes nothing.
- **ZS3s: one per pattern** (decided 2026-11-26, replacing §3's "a ZS3 per
  kit"): ids `"*/N"` in pattern order (N = the pattern's index in the
  snapshot, so dict order, `zs3_next` / `zs3_prev` and program change N
  agree), title "NN NAME", the pattern's kit as every eDrumSynth control
  (`preset_info` null; the master controls: DJ filter / reso / limiter at
  their defaults, MIDI Mode, LXR CC on), `active_phrase` its launcher row,
  chain 1 with `midi_chan`, `midi_in`, `audio_in`, `audio_out`; no
  `global` / `midi_capture`. zs3-0 = the first pattern's plus the tempo
  (processor -4: `bpm`, `bpb`); `last_zs3_id` = the first pattern's, so
  `zs3_next` goes to the second. Patterns used only by songs (not in
  `--patterns`) get ZS3s after the others, selecting their first song row.
  More than 128 patterns (`--one`): the rest as `"zs3-N"` (stepping only).
  Chain 0 is in no ZS3: Zynthian's default for the main mixbus output
  stays (a generated snapshot shouldn't name the device's playback ports).
- **Program change events: optional** (`--pc-events`, default off; the
  latency test of §7's first version dropped). `songs`: a PC (cmd 192) at
  the last step of a song row whose next row (the first, when looping) has
  another kit; `all`: also at step 0 of each pattern's own row (reloads
  the ZS3 at each loop). They sit in a column of their own (16, MIDI
  channel 16: `"16/N"` isn't a ZS3, so Zynthian falls back to `"*/N"`)
  with a pattern per row, so the patterns shared with the launcher rows
  carry no PCs.
- **Rows and columns**: launcher rows' children loop (repeat 255), the
  row's `repeat` = the pattern's bars, no follow action (Zynthian's own
  rows likewise). Every track column (Multi 1-7, Kit 10) is present with a
  non-zero repeat in every row, also when the pattern doesn't use the
  track (the library's empty default pattern): starting a row doesn't stop
  the previous row's children, only sequences of the same group replace
  each other, so an unused column would keep playing the previous
  pattern's track. Song rows' children play `ceil(row / track length)`
  times (a native 12-step track plays 24 steps in a 16-step row and is
  replaced at the next row's start), the song's last row of a non-looping
  song then stops; a looping one-row song's children loop. Song rows
  reference the launcher rows' zynseq pattern ids (editing one edits
  both).
- **Locks, as the LXR's source** (`autoNode_setDestination()`: the restore
  happens at the track's next *played* step, whether it has a lock or not;
  §1 said "the next step with a lock"): lock -> CC n / NRPN n - 128 (99 =
  0, 98, 6) at its step, before the note; restore to the kit's value
  (the engine's: out of range kit values as the init kit's, as the kit
  presets) at the next played step unless that step locks the same
  parameter. A lock whose next played step is after the loop is restored
  at the pattern's last step (offset 7/8 step), so changing the pattern
  restores it too ("restore at the pattern end"); a lock on the last step
  itself can only be restored at the loop (counted). Locks on steps that
  don't play are ignored (the LXR parses locks when a step plays), counted.
  Probability skips only notes in zynseq: locks and restores of a step that
  doesn't play by chance still happen (the LXR skips them); not counted.
- **zynseq's one-event rule**: events with the same command and value 1 at
  one step replace each other, whatever their offset. So per step one
  value per CC and **one NRPN**: a lock beats a restore; of two NRPN locks
  at one step (two lock slots, or two tracks in Kit mode) the later stays;
  an NRPN restore that meets another NRPN is dropped; all counted. Events
  of a step are written NRPN (99, 98, 6), CCs, notes: zynseq plays a
  step's events in list order (the schedule is a multimap by time, equal
  times in insertion order), so the NRPN arrives complete and before the
  note. Overlapping notes of one note number (a shift onto a step with the
  same note) keep the first, counted.
- **Flams**: the LXR's x2..x8 = 2..8 evenly spaced hits in the step
  (`flamIdxToPattern()`); zynseq's stutter alternates note-off / note-on
  every 1/s step, so stutter speed s = 2 * hits (checked in the tests).
  **Shift**: offset +n/8 step; negative: the previous step (wrapping in the
  track) at 1 - n/8. No flams or shifts in the factory projects.
- **Shuffle -> swing**: the LXR (stock firmware's sync path, the one
  without sub steps) delays every second 16th by shuffle / 127 * 2/3 of a
  step (`seq_getDeltaTShuffleDelay()` in 0.25 ms ticks); zynseq's swing
  (swingDiv 1) delays every second step by swing / 2 steps, so swing =
  shuffle * 4 / 3 / 127, limited to 1.0 (shuffle above 95, counted). The
  table `seq_shuffleTable` of §3 belongs to the old 128 sub step
  sequencer and isn't used. Swing applies to notes only (locks of a swung
  step come slightly before its note: harmless). Not checked by ear.
- **Lengths**: the pattern's length = its longest track rounded up to whole
  bars (as lxr2mdt.py); track length 0 = 16. Multi: lengths in multiples
  of 4 steps are native (zynseq patterns are whole beats), others unrolled
  to the pattern's length (restarting with it; counted when the length
  doesn't divide it). Kit: every track tiled into the one pattern.
- **Step notes in Kit mode**: lost (counted per step). In Multi the note
  is the track channel's note (60 = the tuned pitch; the factory projects
  store 63 on most steps, played as on the LXR).
- **Locks on CC 0** (bank select, no parameter in the LXR or the engine):
  left out, counted.
- **Kits**: a pattern's missing kit file: the init kit (the port defaults;
  restores to the init kit's values), counted. Placeholder kits are kept
  when a pattern uses them (only `--presets` leaves them out, as
  snd2lv2.py).
- **Several projects** (`--one`): one snapshot, rows and ZS3s named with
  the project, each row with its project's tempo (`tempo` override) when
  the tempos differ; otherwise one snapshot per project, `--prog`
  counting up.
- **Not done**: `--install` runs ssh / scp as written but wasn't run
  against a device; the `eDrumSynth` keymap entry in zynthian-data (§6.8,
  only useful for Kit mode's row names); a kits-only project (no patterns)
  makes no snapshot (use snd2lv2.py).
- **Tests** (`ctest`: `converter`): synthetic projects only; the zynseq
  JSON round trip through the real `libzynseq.so` (built from zynthian-ui
  on the desktop: `cmake -S zynlibs/zynseq -B build && cmake --build
  build`, then `EDS_LIBZYNSEQ=build/libzynseq.so`; only `setState` /
  `getState`, without `init()`: no JACK client) is equal for every
  pattern, row and column, apart from: patterns without events are not
  kept by the library (empty columns get its default pattern), and the
  event chance comes back truncated (`int(float(c / 100) * 100)`: 33 can
  return as 32; the comparison allows 1). The real factory snapshots
  (below) went through it too (both modes, `--one`, `--pc-events all`).

### Real projects (cynthian's `tmp/lxr`, 2026-11-26; read only, output in scratch)

Factory PROJ01-05, HRTL UNTZKITS (PROJ00) and the DLC demo project, one
snapshot each, Multi (`--presets`):

| Project | Patterns | Kits (ZS3 kits) | zynseq patterns | Presets | BPM |
|---|---|---|---|---|---|
| PROJ01 Easy Yo | 8 | 7 | 55 | 8 | 92 |
| PROJ02 P0$T IDM | 9 | 4 | 47 | 5 | 202 |
| PROJ03 SpainDub | 4 | 2 | 27 | 3 | 50 |
| PROJ04 Hrtlevel | 16 | 13 | 110 | 14 | 148 |
| PROJ05 Beatsies | 9 | 9 | 61 | 9 | 120 |
| UNTZKITS | 16 | 16 | 112 | 16 | 132 |
| DLCprjct | 16 | 16 | 108 | 16 | 132 |
| all | 78 | 67 | 520 | 71 | |

Songs: none (every `.SNG` in these projects is empty, as lxr2mdt.py
found). The DLC kits-only project has no patterns. Snapshots 76-311 KB (compact JSON; most of it the ZS3s' 306 controllers each).

In the 78 patterns: 407 locks on played steps (23 of them NRPNs, never
two NRPNs at one step), 312 steps with a probability below 100 %, 3
patterns with shuffle (all within the swing range); no flams, no shifts;
every kit present. Not converted (Multi; `-v` for each):

| What | Count |
|---|---|
| track lengths that aren't multiples of 4 steps (unrolled, restarting with the pattern) | 55 |
| parameter locks on steps that don't play (ignored, as the LXR) | 44 |
| locks on the pattern's last step (restored at the loop only) | 8 |
| locks on CC 0 (no parameter) | 6 |

Kit mode instead: step notes 5276 steps (the factory projects' 63s),
tracks not dividing the pattern 66, the same locks. The `--one` snapshots
of all 78 patterns (1.3 MB Multi) round-trip through libzynseq in both
modes.


## 9. Pad view: the hybrid layout with track chains (planned 2026-11-26)

**Finding (phone test):** the snapshots load and the ZS3s play, but
Zynthian's pad view showed no patterns. It draws one column per chain and
reads that chain's sequence by the chain's MIDI channel
(`zyngui/zynthian_launcher_data.py` `pad_sequence_state()`); a chain on all
channels (`midi_chan` 0xffff, the Multi layout) has no column. The zynseq
data itself is right (launcher phrases with step-sequencer patterns).

**Facts**: a MIDI chain with `midi_thru` forwards to the **first processor
of each chain in its `midi_out`, directly to the plugin's port**, bypassing
the target chain's zmop channel filter (`zynautoconnect/zynthian_autoconnect.py`
~900-930). **An empty MIDI chain forwards its MIDI router output**
(`zyngine/zynthian_chain.py` `_rebuild_midi_routes()`: `elif
len(self.midi_slots) == 0 and self.midi_thru: sources = self.midi_in`;
corrected 2026-11-26, the user pointed it out). A chain's own zmop filters
(and doesn't translate) its channel.

**Layout "hybrid" (new default):**
- Chain 1: eDrumSynth + MI on **channel 10**, MIDI Mode Multi, LXR CC on;
  its column holds every track that plays at a constant pitch over the
  pattern's length (GM notes). zynseq's direct output on channels 1-7 is
  filtered by this chain's zmop, so nothing plays twice.
- A track constant note other than 60 (the factory projects' 63) is folded
  into the kit's tuning (the voice's oscillator coarse tune; verify by a
  render test that coarse is in semitones and both give the same pitch), so
  such tracks stay in the kit column.
- **Track chains, only where needed:** a MIDI chain "LXR Tn" on channel
  n-1 (n = 1..7) for a track whose step notes vary, or whose length doesn't
  match the pattern's (polyrhythm, played natively instead of unrolled):
  a pure MIDI chain (no processors), `midi_thru: true`, `midi_out: [1]`; its
  column holds that track's patterns as chromatic notes; eDrumSynth (Multi)
  plays channel n-1 as track n.
- `--track-chains needed|all|none` (default needed; `all` = seven track
  chains for every track, a config with all options; `none` = the Kit
  layout without step notes).
- ZS3s: zs3-0 lists every chain (track chains with `midi_out`); the
  per-pattern ZS3s list only chain 1 (kit controllers), so the track
  chains' routing isn't touched.

No pass-through plugin is needed (an earlier draft planned one, "mThru");
track chains cost no extra jalv process.

**To verify on the phone:** pads in every column, no doubled notes,
melodic tracks at pitch, polyrhythmic tracks drifting as on the LXR, ZS3
browsing still switching kits.

### 9.1 Implementation notes (2026-11-26)

Where the implementation settles or differs from §9:

- **Options**: `--mode hybrid` (default) | `multi-all` (§8's Multi layout,
  kept: it plays right, the pad view just has no column for it) | `kit`
  (= hybrid with `--track-chains none`: §8's Kit layout replaced; MIDI
  Mode Multi on channel 10 is the same kit, and the tuning folds keep the
  constant notes' pitch, which Kit mode lost). `--track-chains needed`
  (default) | `all` | `none` for hybrid.
- **Track chains are pure MIDI chains** (no processors; the mThru plugin of
  the first draft dropped): an empty chain with `midi_thru` forwards its
  zmop's output to the chains in its `midi_out` (`zynthian_chain.py`
  `_rebuild_midi_routes()`: `elif len(self.midi_slots) == 0 and
  self.midi_thru: sources = self.midi_in`, then `for output in
  self.midi_out: self.midi_routes[output] = sources`), and autoconnect
  maps a chain id destination to that chain's first synth processor's jack
  port (`zynthian_autoconnect.py` "Chain MIDI routing"), bypassing chain
  1's channel filter. Snapshot chain entry as `get_state()` writes a chain
  without processors: `{"title": "LXR Tn", "midi_chan": n-1, "midi_thru":
  true, "audio_thru": false, "zmop_index": n, "cc_route": [...], "slots":
  [], "zctrls": []}`, chain id n+1 (T1 = 2 .. T7 = 8), zmop n (chain 1
  keeps 0); listed after chain 1, before chain 0 (the pad view's order).
  `midi_out` isn't chain state in Zynthian, only ZS3 state, so zs3-0 has
  each track chain with `midi_chan`, `midi_in` (`ZynMidiRouter:ch<zmop>_out`),
  `midi_out: [1]`, `audio_in: []`, `audio_out: [0]` (what `save_zs3()`
  writes for a MIDI chain); the per-pattern ZS3s list chain 1 only.
  `cc_route`: the same list as chain 1's on every track chain (a track's
  locks play in its column, through its chain; a chain without a synth
  never calls `set_zmop_options()`, so its zmop's CC drop flag is whatever
  it was: the route makes the locked CCs pass either way).
- **Which tracks need a chain** (`needed`, over every converted pattern,
  the launcher rows' and the song-only ones): step notes that vary within
  the track's length; a length that doesn't divide the pattern's steps (a
  length that divides it plays the same tiled into the kit column, so it
  isn't a polyrhythm); track 7 when its constant note differs from track
  6's in a pattern (one slot, one tuning: track 6's note folds, track 7's
  chain plays its notes less that fold). Once chained, all of the track's
  patterns are in its column (chromatic notes, no fold of its own); the
  converter prints each chain's reasons and patterns.
- **Track chain patterns**: the track's own length repeated to whole beats
  (zynseq patterns are whole beats: lcm(length, 4) steps, 7 -> 28, 12 ->
  12), looping (`repeat` 255) against the row as the LXR's tracks run
  against the pattern; in songs `ceil(row / length)` plays as in §8.
- **Tuning folds** (verified: render test `tuning fold`, 140 cases, bit
  exact): the trigger's note moves not only the coarse tune but every
  oscillator `osc_setBaseNote()` gets: the drum engine's FM oscillator
  (FM Freq, role 5), the cymbal's and hi-hat's two mod oscillators (Mod
  Osc Freq 1 / 2, roles 5 and 2); the snare engine only its main
  oscillator (its noise freq stays). Coarse is in semitones (`note =
  coarse + baseNote - 60`, `MidiNoteFrequencies[]`). So note 60 + k equals
  those ports + k with note 60, for every engine on every track (open
  hi-hat included), k = +3, -5, +12. Values that would leave the range
  (factory kits have coarse 127 often) are limited to it: still exact,
  the oscillator limits the note to 0..127 alike, and an LFO / velocity on
  the tuning multiplies the frequency (`modNodeValue`) afterwards, so it
  doesn't interact (also tested at both ends). Per pattern: the fold is in
  that pattern's ZS3 (a kit shared by patterns with different constant
  notes gets each pattern's), the restores use the folded kit values, and
  a lock of a folded parameter (CC / NRPN of coarse or FM / mod freq) is
  folded too.
- **Columns**: the kit column (9, "Kit") and each track chain's column
  (n-1, "Tn Voice", e.g. "T2 Tom1": the pad shows 7 characters) are
  present in every row with a non-zero repeat (empty: the library's
  default pattern), as §8, so swapping rows replaces every column; with
  `all` the kit column is still there (empty).
- **Real projects** (cynthian's `tmp/lxr`, the 7 of §8, `needed`): track
  chains and why (a track can have both reasons):

  | Project | Track chains | Melodic (step notes vary) | Polyrhythm | Folded patterns |
  |---|---|---|---|---|
  | PROJ01 Easy Yo | 4: T3 T4 T5 T6 | T3 T4 T5 | T6 (12/16, 7/32) | 8 of 8 |
  | PROJ02 P0$T IDM | 5: T1 T2 T3 T5 T6 | T1 T2 T3 T5 | T3 (14/16), T5 (3/16), T6 (7/16) | 8 of 9 |
  | PROJ03 SpainDub | 2: T2 T4 | T2 T4 | - | 4 of 4 |
  | PROJ04 Hrtlevel | 5: T1 T2 T3 T5 T6 | T2 T3 | T1 T2 T3 T5 T6 | 16 of 16 |
  | PROJ05 Beatsies | 6: T1-T6 | T1 T2 T3 T4 T5 | T1 T2 T5 T6 | 9 of 9 |
  | UNTZKITS | 6: T2-T7 | T5 | T2-T7 (mostly pattern 15) | 16 of 16 |
  | DLCprjct | 7: all | T1-T6 | T1-T7 | 0 (every track chained) |

  No hi-hat slot conflicts (track 7 only chained for polyrhythms). 35
  track chains in 7 snapshots; the fold took the 63s of every kit-column
  track. Snapshots 72-300 KB; all through libzynseq and the schema check
  (also `--one` with `--pc-events all` in hybrid needed / all, kit,
  multi-all). `--mode kit` (no chains): 837 steps' notes lost (§8's Kit
  mode: 5276) and 66 polyrhythmic tracks restarting.
- **CPU**: a track chain is no process (no jalv); the zmop and a JACK
  connection per chain.
- **To check on the phone** (§9's list): the pad view's columns (kit and
  each track chain), nothing doubled (chain 1 on channel 10 only), the
  chained tracks at pitch and drifting, ZS3 browsing switching kits and
  folds while the track chains keep their routing; that a snapshot with
  pure MIDI chains loads (`slots: []`) and Zynthian's autoconnect really
  routes them after the zs3-0 restore (`rebuild_graph()` after setting
  `midi_out`).
