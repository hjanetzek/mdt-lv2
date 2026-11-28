# eDrumSynth: the LXR's own sequencer in the plugin

Goal: port OpenElixir-02's step sequencer (the LXR-02's: patterns, songs,
parameter locks, flams, shift, probabilities, track lengths, a kit per
pattern, shuffle, Euclid) into eDrumSynth, so the plugin plays LXR projects
natively and sample-accurately, following Zynthian's tempo and transport,
edited through a "panel" of control ports (a Zynthian widget, MIDI learn,
controllers). Version 0.3.0.

Status: plan (2026-11-28, decided with the user and the zynthian-ui
"plugin-widgets" session).

## 1. Decisions

1. **Port the original sequencer**, not a new one: OpenElixir-02
   (https://github.com/tunefish777/openelixir-02, commit `383b012`, the one
   the engine copy comes from) `Core/Src/Soundengine/Sequencer/`
   (`sequencer.c`, `clockSync.c`, `EuklidGenerator.c` and their headers),
   copied into `engine/Synth_LXR/lxr/` unchanged, changes marked `LV2:` and
   listed in `ORIGIN.md` (as the engine). The stock firmware's sequencer
   (`~/bastel/github/erica-synths-lxr-desktop`) is the reference for what
   OpenElixir-02 changed.
2. **Interfaces, in this order:**
   1. **Panel ports now** (§3): a window into the sequencer modelled on the
      LXR's front panel, as control ports. Works with today's Zynthian
      widgets (Tk and Qt, zynwidgets), MIDI learn, controllers.
   2. **LV2 State for persistence** (§4): all patterns, songs, kits and
      settings; `save preset` captures them. Saving the state with Zynthian
      snapshots / ZS3s is a zynthian-side extension (plugin-widgets offered
      it: e.g. a per-snapshot preset the snapshot references), a separate
      step.
   3. **Patch properties later** (§6): the full data model over
      `patch:Set` / `patch:Get`, when the panel hits its limits; needs the
      jalv fork's line protocol (plugin-widgets: `patch set|get <URI>
      <type> <value>`, notification lines). The plugin's internal model is
      designed so they can be added without changing the panel ports.
3. **Who owns what:** the plugin's sequencer owns the LXR-native part
   (patterns with locks, flams, probabilities, lengths, kit per pattern,
   songs); zynseq starts / stops it through Zynthian's transport and stays
   the tool for arranging with other instruments. The zynseq converter
   (`CONVERTER_PLAN.md`) stays as an option for editing in zynseq.
4. **The widget** is built by the plugin-widgets session in zynwidgets (a
   `Steps` grid component and an eDrumSynth sequencer page, Tk and Qt)
   against the panel ports; we send them the port list.

## 2. Facts

- **Zynthian widgets** get only float monitors (all output control ports as
  one text line per jalv `monitors` poll, every 30-100 ms) and write input
  control ports (`zctrl.set_value`); no atom / patch / string data. Keep the
  panel's outputs at about 50-100 floats, animated values at ≤ 20-30 Hz,
  ≤ 24 bits packed per float (a 16-step mask fits); prefer separate floats
  for velocities etc. Actions as `lv2:toggled` / `pprops:trigger` ports,
  selectors as enumeration / integer ports.
- **`pprops:notOnGUI`**: check in the jalv fork (`~/bastel/github/jalv`,
  `controls` and `symbol=value` on such a port) whether zynthian can still
  list and set it; if not, the panel ports get their own group (Zynthian
  shows it as its own pages) instead of being hidden.
- **Snapshots** store controller values and a preset reference, not LV2
  state; `save preset` stores the state.
- **Transport:** zynseq is the JACK timebase master; the jalv fork forwards
  JACK transport as `time:Position` (frame, speed, bar, barBeat,
  beatsPerMinute) to atom inputs declaring `atom:supports time:Position`
  (eDrumSynth's does). JACK transport rolls when zynseq runs; test start,
  stop, locate and tempo changes.
- **The LXR's data**: per pattern 7 tracks × 64 steps (velocity / active,
  probability, note, two locks; flam, shift), main step bits, track
  lengths, shuffle, kit number; 64 patterns (~3.6 KB each), 64 kits (255
  bytes), songs (64 entries), globals (`port/LXR_PATTERN_CONVERTER.md` in
  cynthian; the parser `tools/lxrformat.py`).

## 3. Panel ports (draft; the implementation finalises the list)

A Sequencer group (or a few: Transport, Pattern, Step), appended after
eDrumSynth's existing ports (indices and symbols of 0.2.0 unchanged).

Inputs:
- Transport: run mode (enumeration: Off / Host transport / Internal), play
  (toggle, for Internal), internal tempo, swing / shuffle (the pattern's).
- Pattern: playing pattern (0-63, switches at the pattern end as the LXR),
  edit pattern (0-63, or "follow playing"), song mode (toggle), song (0-63),
  the edit pattern's kit (0-63 or "none").
- Track / page / step selection: track (0-6), page (0-3: steps 1-16,
  17-32, ...), selected step (0-15 on the page).
- The 16 step buttons of the page (triggers: toggle the step on the
  selected track, as the LXR's step buttons).
- The selected step's fields: velocity, note, probability, lock 1
  parameter / value, lock 2 parameter / value, flam, shift.
- Track: length (1-64), mute (7 toggles, or a mute for the selected track),
  Euclid (steps, fill, rotation) if cheap.
- Actions (triggers): clear track / pattern, copy / paste pattern, undo if
  the sequencer has it.

Outputs (monitors):
- The page's active-step mask of the selected track (16 bits), the 16
  velocities of the page, optionally the 7 tracks' masks for the page (7
  floats: a grid view of all tracks).
- Playhead step per track or the pattern's (0-63), the playing and the next
  pattern, song position, trigger flashes (7 bits, held until the next
  poll), the selected step's fields as read back (so a controller's change
  shows).

## 4. State and files

- LV2 State (`state:interface`): one property holding the project as a
  binary chunk (the LXR's formats: patterns, kits, songs, globals; a header
  with a version), saved and restored outside `run()`; restore swaps a
  prepared copy in at a block boundary (no allocation, no locks in `run()`;
  the worker extension if the swap needs it).
- Loading LXR projects: a tool writes an LV2 preset bundle whose preset
  carries the project as state (`lxr2zynthian.py --internal` or
  `lxr2preset.py`), and optionally a snapshot referencing it (`preset_info`
  in zs3-0). Possibly later an `atom:Path` property for loading a project
  folder from the device.
- Kits: the per-pattern kit loads into the engine at the pattern start (247
  parameters through the parameter setter, well below a millisecond, as on
  the LXR). Port values vs. sequencer-loaded kits: define and document which
  wins (e.g. a kit change from the sequencer updates the engine; port
  changes still apply on change), and keep v2's engine selectors working.

## 5. Real time and timing

- The sequencer is ticked from `run()`: its clock derived from the host's
  `time:Position` (BPM, bar / beat, rolling) or the internal tempo, at the
  LXR's resolution, sample-accurate within the block (events at their
  frame, engine blocks of 64 frames at 44.1 kHz behind the resampler: the
  trigger timing as v1's notes).
- Triggers, locks, flams, shift and kit changes act on the engine directly
  (as on the LXR); MIDI input keeps working alongside (Kit / Multi modes).
- No allocation, locks, file I/O or per-sample libm in `run()`.

## 6. Later: patch properties

`patch:writable` properties for the whole model (`#pattern` per index,
`#song`, `#kit`, `#project`), `patch:Set` / `patch:Get` on the atom input,
notifications on an atom output, with the jalv fork's line protocol and
widget support (plugin-widgets). Not in 0.3.0.

## 7. Tests

- The ported sequencer against expected triggers: a synthetic pattern
  (steps, velocities, probability with a fixed seed, flams, shift, track
  lengths / polymeter, shuffle) gives triggers at the expected frames;
  locks set and restore parameters as the LXR (`autoNode_setDestination()`);
  kit per pattern; songs (end, loop); pattern switching at the pattern end.
- Host transport: start / stop / locate / tempo change via `time:Position`
  in the test host (extend `common/testhost` with time events).
- Panel ports: each input does what the LXR's panel does; outputs reflect
  the state; the 16 step triggers toggle steps.
- State: save → restore round trip (bit exact project), restore while
  running, a converted LXR project as a preset renders like the engine
  driven by the reference sequencer.
- All existing tests keep passing; lint; frozen symbols (new symbols
  added); CPU (the sequencer's cost on the desktop, then the phone).
- Phone / Duet (with device-testing and the user): Zynthian's transport
  starting it, the plugin-widgets widget.

## 8. Steps

1. Fetch OpenElixir-02 at `383b012`, copy the sequencer sources into the
   engine copy; build them with shims (UI, LEDs, SD, MIDI clock, the
   front panel) as the engine's.
2. Sequencer driven from `run()` with the host transport / internal clock;
   triggers into the engine; tests.
3. LV2 State (project chunk), restore at a block boundary; tests.
4. Panel ports (final list; the notOnGUI check), generated into the TTL /
   port map; tests. Send the port list to plugin-widgets.
5. The project-to-preset tool (and the snapshot referencing it); tests with
   synthetic projects, a read-only run on `tmp/lxr`.
6. Docs, version 0.3.0, draft APKBUILD; then the armv7 build, phone checks
   and CI through device-testing.
