# LXR projects to Zynthian: kits as LV2 presets / ZS3s, patterns and songs as zynseq

Goal: a tool that turns LXR-02 / OpenElixir-02 project folders (kits,
patterns, songs, tempo) into a Zynthian snapshot with an eDrumSynth chain
and the patterns in Zynthian's sequencer (zynseq), so a user loads one
snapshot and plays the project: patterns from the launcher, songs as phrase
chains, kits switched by ZS3s (sub-snapshots: controller values, no plugin
reload). Plus the kits as an LV2 presets bundle for use without the
snapshot.

Status: plan (2026-11-25). Depends on eDrumSynth v2 (`PLAN.md` §13: engines
per slot, Multi MIDI mode, kit converter) and a small v2.1 addition (§4.3).

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
- Kit changes: **measure the ZS3 latency first** (a test snapshot on the
  phone), then decide between ZS3 by program change events and an internal
  kit bank.
- Test data: the projects in cynthian's `tmp/lxr` (never committed);
  **snapshots generated from them may be installed on the phone**
  (coordinated with the device-testing session).
