# eDrumSynth: the LXR's own sequencer in the plugin

Goal: port OpenElixir-02's step sequencer (the LXR-02's: patterns, songs,
parameter locks, flams, shift, probabilities, track lengths, a kit per
pattern, shuffle, Euclid) into eDrumSynth, so the plugin plays LXR projects
natively and sample-accurately, following Zynthian's tempo and transport,
edited through a "panel" of control ports (a Zynthian widget, MIDI learn,
controllers). Version 0.3.0.

Status: steps 1-5 implemented and desktop-tested (2026-11-28; the final
panel ports: §9, implementation notes: §10); step 6 (armv7, phone, CI) open.
Plan decided with the user and the zynthian-ui "plugin-widgets" session.

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

## 3. Panel ports (draft; the final list: §9)

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

Done (2026-11-28): 1-5 and the docs, version and draft APKBUILD of 6
(commits in mdt-lv2: the copy, the build with shims, the test host's
transport, the plugin (clock, state, panel), the tool, the docs). Open:
the armv7 package, the phone, the widget, Zynthian's snapshots (§10.9).

## 9. The panel ports (0.3.0, final)

109 control ports after 0.2.0's (indices 309-417; 0.2.0's indices and
symbols unchanged): 54 inputs in 9 groups (Zynthian: 15 pages), 55
outputs in 4 output groups (`pg:OutputGroup`; jalv's `monitors`). Symbols
are frozen (`tests/symbols.txt`). The same list with its groups and ranges
is in `tools/edrumsynth_ports.json` (`panel_ports`) and the TTL.

How the inputs act:
- **Settings** (mode, play, tempo, kit autoload, mutes, the selection):
  applied from the first `run()` on and whenever they change.
- **Project values** (`seq_pattern`, `seq_song_mode`, `seq_song`) and
  **edits** (the step's fields, locks, track length, shuffle, kit, the
  Euclid values): applied when their value changes, not at the first
  `run()`, and not in the `run()` that installs a restored state (a
  preset's port values come with its state: the state wins); an edit
  acts on the selection (edited pattern, track, selected step).
- **Triggers** (`pprops:trigger`, also `lv2:toggled`): act when the value
  rises above 0.5; a host that doesn't reset them must send 0 before the
  next 1.
- The selected step is `seq_page` × 16 + `seq_step` when one of them
  changes, or the step whose button was pressed last (`seq_out_selected`
  shows it).
- Masks: bit i (value 2^i) is step i + 1 of the page (16 bits); flashes
  and mutes: bit t (1 bass drum .. 64 open hi-hat). Every output is at
  most 16 bits.

| Index | Symbol | Dir | Type, range (default) | Meaning |
|---|---|---|---|---|
| | **Seq Transport** (`seq_transport`) | | | |
| 309 | `seq_mode` | in | enum 0 Off, 1 Host, 2 Internal (1) | Off: the sequencer does not play (MIDI only, as 0.2.0); Host: it follows the host's transport (time:Position: rolling, bar / beat, tempo); Internal: its own tempo, started by Play |
| 310 | `seq_play` | in | toggle (0) | Internal mode: on starts the sequencer at the start, off stops it (as the LXR's start / stop button) |
| 311 | `seq_tempo` | in | integer 20..300 (120) | Internal mode: the tempo in BPM |
| 312 | `seq_shuffle` | in | integer 0..127 (0) | the edited pattern's shuffle (odd steps late by up to 2/3 of a step); the playing pattern's at once |
| | **Seq Pattern** (`seq_pattern`) | | | |
| 313 | `seq_pattern` | in | integer 0..63 (0) | the pattern to play: it starts when the playing one ends (stopped: at once), as the LXR's pattern buttons |
| 314 | `seq_song_mode` | in | toggle (0) | on: the song plays its patterns in turn (from its start at the next start) |
| 315 | `seq_song` | in | integer 0..63 (0) | the song song mode plays |
| 316 | `seq_kit_autoload` | in | toggle (1) | on: a pattern loads its kit when it starts (the LXR's pattern change mode 'auto'); patterns without a kit in the project keep the sound |
| | **Seq Select** (`seq_select`) | | | |
| 317 | `seq_edit_pattern` | in | integer 0..64 (64) | the pattern the panel edits: 0-63, 64 the playing one |
| 318 | `seq_track` | in | enum 0 Bass Drum, 1 Tom 1, 2 Tom 2, 3 Snare, 4 Cymbal, 5 Hi-Hat Closed, 6 Hi-Hat Open (0) | the track the panel edits |
| 319 | `seq_page` | in | enum 0 1-16, 1 17-32, 2 33-48, 3 49-64 (0) | the 16 steps the step buttons and the page outputs show |
| 320 | `seq_step` | in | integer 0..15 (0) | the step of the page the step fields edit (a step button selects it too) |
| | **Seq Step** (`seq_step`) | | | |
| 321 | `seq_step_velocity` | in | integer 0..127 (127) | the selected step's velocity |
| 322 | `seq_step_note` | in | integer 0..127 (60) | the selected step's note: 60 plays the voice as tuned, others shift its oscillators in semitones |
| 323 | `seq_step_prob` | in | integer 0..100 (100) | the selected step's probability (%) |
| 324 | `seq_step_flam` | in | enum 0 Off, 1 x2, 2 x3, 3 x4, 4 x6, 5 x8 (0) | the selected step's hits |
| 325 | `seq_step_shift` | in | integer -7..7 (0) | the selected step's time shift in eighths of a step (negative: earlier) |
| | **Seq Step Locks** (`seq_locks`) | | | |
| 326 | `seq_lock1_target` | in | enum 0..228 (the modulation targets, labelled as the velocity / LFO destination ports) (0) | the parameter the selected step sets (a parameter lock: back to the kit's value at the track's next step) |
| 327 | `seq_lock1_value` | in | integer 0..127 (0) | the value lock 1 sets |
| 328 | `seq_lock2_target` | in | enum 0..228 (the modulation targets, labelled as the velocity / LFO destination ports) (0) | the second parameter lock |
| 329 | `seq_lock2_value` | in | integer 0..127 (0) | the value lock 2 sets |
| | **Seq Track** (`seq_track`) | | | |
| 330 | `seq_track_length` | in | integer 1..64 (16) | the edited track's length in steps (tracks of different lengths run against each other) |
| 331 | `seq_euclid_fill` | in | integer 1..64 (4) | the Euclid generator: steps spread evenly over the track's length |
| 332 | `seq_euclid_rotation` | in | integer 0..63 (0) | the Euclid generator's rotation |
| 333 | `seq_euclid` | in | trigger (0) | writes the Euclid pattern into the edited track (its steps replaced) |
| | **Seq Mutes** (`seq_mutes`) | | | |
| 334-340 | `seq_mute_bd` .. `seq_mute_hho` | in | toggle (0) | mutes the track (bd, t1, t2, sd, cy, hhc, hho: the sequencer and MIDI) |
| | **Seq Step Buttons** (`seq_buttons`) | | | |
| 341-356 | `seq_button1` .. `seq_button16` | in | trigger (0) | toggles the step of the page on the edited track, and selects it (the LXR's step buttons) |
| | **Seq Actions** (`seq_actions`) | | | |
| 357 | `seq_kit` | in | integer 0..64 (0) | the edited pattern's kit (0-63, 64 none) |
| 358 | `seq_store_kit` | in | trigger (0) | stores the sound (the kit as it is now) as the edited pattern's kit (none: into a free kit slot) |
| 359 | `seq_clear_track` | in | trigger (0) | clears the edited track |
| 360 | `seq_clear_pattern` | in | trigger (0) | clears the edited pattern |
| 361 | `seq_copy_pattern` | in | trigger (0) | remembers the edited pattern |
| 362 | `seq_paste_pattern` | in | trigger (0) | copies the remembered pattern into the edited one |
| | **Seq Status (outputs)** (`seq_status`) | | | |
| 363 | `seq_out_running` | out | toggle | the sequencer plays |
| 364 | `seq_out_playing` | out | integer 0..63 | the pattern playing |
| 365 | `seq_out_next` | out | integer 0..63 | the pattern after it |
| 366 | `seq_out_song_pos` | out | integer 0..63 | song mode: the song's entry playing |
| 367 | `seq_out_edit_pattern` | out | integer 0..63 | the pattern the panel edits |
| 368 | `seq_out_bpm` | out | float 0..1000 | the sequencer's tempo |
| 369 | `seq_out_flash` | out | integer 0..127 | a bit per track (1 bass drum .. 64 open hi-hat): triggered in the last 80 ms (sequencer and MIDI) |
| 370 | `seq_out_mutes` | out | integer 0..127 | a bit per track: muted |
| 371 | `seq_out_pattern_length` | out | integer 1..64 | the edited pattern's length (its longest track) |
| 372 | `seq_out_track_length` | out | integer 1..64 | the edited track's length |
| 373 | `seq_out_shuffle` | out | integer 0..127 | the edited pattern's shuffle |
| 374 | `seq_out_kit` | out | integer 0..64 | the edited pattern's kit (64 none) |
| 375 | `seq_out_kit_present` | out | toggle | the project holds the edited pattern's kit |
| 376 | `seq_out_selected` | out | integer 0..63 | the step the step fields edit (Edit Page and Edit Step, or the step button pressed last) |
| | **Seq Positions (outputs)** (`seq_positions`) | | | |
| 377-383 | `seq_out_pos_bd` .. `seq_out_pos_hho` | out | integer -1..63 | the track's step playing (-1: stopped) |
| | **Seq Page (outputs)** (`seq_page`) | | | |
| 384 | `seq_out_mask` | out | integer 0..65535 | the edited track's steps on the page: bit i (value 2^i) = step i + 1 of the page on |
| 385-391 | `seq_out_grid_bd` .. `seq_out_grid_hho` | out | integer 0..65535 | the track's steps on the page, as Page Steps |
| 392-407 | `seq_out_vel1` .. `seq_out_vel16` | out | integer 0..127 | the velocity of the edited track's step i of the page |
| | **Seq Step Read (outputs)** (`seq_read`) | | | |
| 408 | `seq_out_step_active` | out | toggle | the selected step plays |
| 409 | `seq_out_step_velocity` | out | integer 0..127 | the selected step's fields as they are |
| 410 | `seq_out_step_note` | out | integer 0..127 | the selected step's field (as the input of the same name) |
| 411 | `seq_out_step_prob` | out | integer 0..100 | the selected step's field (as the input of the same name) |
| 412 | `seq_out_step_flam` | out | enum 0 Off, 1 x2, 2 x3, 3 x4, 4 x6, 5 x8 | the selected step's field (as the input of the same name) |
| 413 | `seq_out_step_shift` | out | integer -7..7 | the selected step's field (as the input of the same name) |
| 414 | `seq_out_lock1_target` | out | enum 0..228 (the modulation targets, labelled as the velocity / LFO destination ports) | the selected step's field (as the input of the same name) |
| 415 | `seq_out_lock1_value` | out | integer 0..127 | the selected step's field (as the input of the same name) |
| 416 | `seq_out_lock2_target` | out | enum 0..228 (the modulation targets, labelled as the velocity / LFO destination ports) | the selected step's field (as the input of the same name) |
| 417 | `seq_out_lock2_value` | out | integer 0..127 | the selected step's field (as the input of the same name) |


## 10. Implementation notes (2026-11-28)

### 10.1 What is ported

The OpenElixir-02 `383b012` sources, unchanged but for the `LV2:` marks
(`engine/Synth_LXR/ORIGIN.md`): `sequencer.c` (patterns, steps with
velocity / note / probability, the two automation tracks (parameter locks)
with the LXR's restore, flams, shift with its prev / current / next step
logic and the chokes between tracks, track lengths and the master track,
shuffle, mutes, pattern switching at the master track's end, songs with
end and loop, the kit autoload per pattern, clear / copy, rolls, recording),
`EuklidGenerator.c` (the Euclid generator, `seq_euclid`), `clockSync.c`
(compiled, not used), `automationNode.c` (the locks), `SeqStep.h`. Built as
one unit with the glue (`src/lxr_sequencer.c`); the engine unit
(`lxr_engine.c`) uses the sequencer's tempo (LFO sync) and mutes.

Used by the plugin: everything a pattern plays (steps, locks, flams,
shift, probability, lengths, shuffle, mutes, kit per pattern, songs,
pattern switch), the front panel's edits as the LXR's `CortexParser.c`
does them (step fields, locks stored as the front panel sends them, track
length, shuffle, kit, Euclid, clear track / pattern, copy pattern, store
kit as `preset_saveDrumsetRam()`).

Not used (no panel ports or no meaning here): rolls (`seq_setRoll()`: a
performance key), recording (`seq_addNote()`, `seq_recordAutomation()`:
no record mode; the panel writes steps), the MIDI clock in / out
(`clockSync.c`: the host's transport instead), MIDI note out and program
changes, the trigger I/O expansion, the morph kit, track copy (no port;
cheap to add), `seq_tick()` itself (the clock below replaces its time
base).

### 10.2 The shims

`lxr/shim/lxr_glue.h` (declarations), `ledHandler.h`, `buttonHandler.h`
(inline no-ops) and `src/lxr_sequencer.c` (definitions):
- front panel and menu state the sequencer reads and writes
  (`frontParser_shownPattern / activeTrack / activeStep`,
  `menu_activeBar`, `menu_playedPattern`, `menu_setShownPattern()`, ...):
  plain variables; the panel API sets the selection there before an edit;
- LEDs, the display, the button modes: no-ops (the panel's outputs show
  the state instead);
- MIDI out (`uart_sendMidi()`, `usb_sendMidi()`, the filters), the front
  panel's UART bytes, MTC, the trigger outputs: no-ops;
- `MidiVoiceControl.c`: not taken; its note on is `lxr_engine_trigger()`
  (the voice, the engine's mutes and trigger counts), its note off was MIDI
  out only;
- `presetManager.c`: not taken; `preset_loadDrumsetRam()` loads a kit of
  the project (`lxr_engine_load_kit_engines()`: engines first, the
  targets last), a slot without a kit loads nothing; no SD card, no
  autosave timers;
- `systick_ticks`: defined, not used (the clock counts engine blocks);
- the probabilities' random numbers: `GetRngValue` defined as the glue's
  seeded xorshift32 around the `#include` of `sequencer.c` (the noise
  oscillators keep the engine's generator); seed 2463534242 at
  `lxr_engine_init()`, so a render repeats.

### 10.3 The clock and its accuracy

- The LXR calls `seq_tick()` once per engine block (64 frames); it plays
  one tick of its 96 PPQ clock (24 per 16th step) when the 4 kHz system
  tick says it is due: `seq_nextStep()` on a step's first tick,
  `seq_processFlam()` on each. `lxr_seq_block()` does the same with the
  time counted in ticks per block (position = base + blocks × ticks per
  block, no accumulation): **a tick plays in the first engine block that
  starts at or after its time**, one tick per block. An odd step's first
  tick is late by the shuffle (16 ticks × shuffle / 127, as
  `seq_calcDeltaT()`); ticks overdue after it follow one per block, as on
  the LXR (a flam on a shuffled step is compressed). Above about 430 bpm
  (a tick shorter than a block) the clock locates instead of falling more
  than a step behind.
- Accuracy: a step plays up to one engine block late (0 to 1.45 ms, 0.7
  ms on average at 44.1 kHz), the same for every host block size; the
  engine at 44.1 kHz is bit exact against the engine's sequencer driven
  directly (tests). The position is that of the moment the block is
  **heard**: in Host mode the plugin computes the host's position at the
  output frame where the next engine block starts (the output already
  rendered, at other rates the resampler's buffered input and its delay:
  `eds_rs_pending()`), so the resampler's delay is compensated (48 kHz
  test: onsets 0.0-0.9 ms after the beat, 0.56 ms on average).
- Host mode: `time:Position` (as jalv sends JACK's: on start, stop,
  locate, tempo change; frame, speed, bar, barBeat, beatUnit, beatsPerBar,
  beatsPerMinute). Rolling (speed > 0) starts the sequencer at the host's
  position; a position more than half a tick from the sequencer's is a
  locate, a smaller difference a correction; speed 0 stops it (as the
  LXR's stop: the position back to the start). Between events the clock
  runs on at the tempo (no drift: the engine's sample count is the
  host's, exactly at 48 kHz). Quarter notes = (bar × beatsPerBar +
  barBeat) × 4 / beatUnit; without bar / beat: the frame at the tempo.
  The tempo (LFO sync) follows the host in Off and Host modes, the
  internal tempo in Internal mode.
- Start and locate: the sequencer is put where it would be after the
  steps before (pattern, song position with runs of the same pattern, each
  track's step for polymeter, the shuffle parity), not chased; the next
  step plays at its time. A step due within the last two engine blocks
  plays at once (a start where the host's beat 0 lies in the output already
  rendered is up to about 2 ms late: 1.6 ms in the 48 kHz test). A locate
  past a song's end stops. A song that loops is reduced to one pass (a
  track running over the loop seam with the same pattern on both sides is
  restarted there: the only difference from playing it through).
- Internal mode: `seq_play` starts at the start / stops, `seq_tempo`;
  the host's transport is ignored. Off: the sequencer doesn't play (MIDI
  only, 0.2.0's behaviour).
- MIDI notes, CCs (LXR CC) and the sequencer's triggers all act at the
  next engine block, in the order they come.
- Real time: `lxr_seq_block()` costs a few comparisons per block and the
  LXR's step / flam processing at ticks; a pattern switch with its kit
  247 parameters (as the LXR); no allocation, locks, I/O; libm only in a
  locate (`ceil`) and on events (`fabs`).

### 10.4 The state

- One property `<https://github.com/hjanetzek/mdt-lv2/edrumsynth#project>`,
  `atom:Chunk`, flags POD and portable; declared in the TTL. Layout
  (version 1, little endian; `lxr_sequencer.c`): `"EDSQ"`, u16 version,
  u16 header size (48), u64 masks of the patterns, kits and songs that
  follow, the pattern playing, the song, flags (song mode, a current kit
  follows), the globals' count, the project's name (8 bytes); then the
  patterns as LXR V3 pattern files (3663 bytes), the kits as LXR kit
  files with OpenElixir-02's engine block (266), the songs as LXR song
  files (72), the globals (GLO.CFG's layout, 31 bytes), the current kit
  (the engine's at saving). Patterns equal to a cleared one, empty kit
  slots and default songs are left out: an empty project is 345 bytes, the
  real LXR projects about 236-256 KB (lilv writes it as base64). Readers
  skip a larger header of a later version.
- `save()` (may run alongside `run()`) writes the current project; edits
  made while it writes may be half in it (bytes only: memory safe).
  `restore()` parses into a new project (allocated there; about 0.2 ms
  for 256 KB on the desktop) and hands it over through an atomic pointer;
  `run()` installs it at its start (a block boundary: no allocation, no
  lock; the pattern set and songs are swapped as pointers, the playing
  pattern copied into the backup buffer, the current kit loaded: 0.05 ms
  on the desktop) and keeps the position when running (located in the new
  project, host mode corrected after the host's position). The replaced
  project is freed by the next `restore()` or `cleanup()`. jalv pauses
  `run()` around `restore()` (the plugin doesn't ask for
  `state:threadSafeRestore`); the swap works either way.
- A state without the property (0.2.0's presets) keeps the project.
- Round trip: save → restore → save is bit exact; a kit's values the
  engine refuses (out of range: a factory kit has a hi-hat transient wave
  of 24) are not in the engine's kit, so the current kit saved after
  restoring such a kit has the previous value there (the project's 64
  kits keep their bytes).

### 10.5 Kits and the ports

- A kit is loaded by the kit ports (each when its value changes), by the
  sequencer (a pattern's kit when the pattern starts, kit autoload on; the
  project's kit slots: from a converted project or `seq_store_kit`), or
  by a restored state (its current kit). **The last one wins**: after a
  kit the sequencer or a restore loaded, the plugin's port cache follows
  the engine, so the ports as they are don't change the sound; a port
  changed afterwards sets its parameter (the others stay the kit's).
- The engine selectors as in 0.2.0: a selector changed switches the slot
  and sends the slot's ports again where they differ (the ports win). A
  kit with other engines switches the slots too (the selectors' values
  stay as they are until changed).
- In the `run()` installing a restored state all kit ports count as seen
  (the state's kit wins over the preset's port values, which come in the
  same cycle with jalv).
- A port changed after a kit load wins only for that parameter; the kit
  of the next pattern overrides it again (as on the LXR, where a pattern
  change discards unsaved kit edits unless stored: `seq_store_kit`).

### 10.6 The project-to-preset tool

`tools/lxr2preset.py PROJDIR [-o BUNDLE] [--name] [--pattern N | --song
N] [--snapshot DIR [--prog NNN] [--presets-dir]]`: one preset per project
(its state the whole project, its port values the start kit and the
panel: Host mode, the project's tempo, the start pattern or song, kit
autoload from GLO.CFG's pattern change mode); `--snapshot` writes a
Zynthian snapshot whose eDrumSynth chain (MIDI channel 10) loads the
preset (`preset_info` with the bundle in Zynthian's presets folder), with
the project's tempo and no controller values. Written as a separate tool
(not `lxr2zynthian.py --internal`): it shares `lxrformat.py`, `snd2lv2.py`
and the port map; the chunk's layout is in the port map (`project`,
written by `gen_ttl` from the engine).

Real projects (`tmp/lxr`, read only, output in a scratch folder; not
committed): the DLC demo project, the DLC and UNTZ kit projects and the
factory PROJ00-05 converted (64 patterns each, 3-64 kits, 236-256 KB of
state; two factory kits with an out of range value: reported with -v).
Each chunk read and written again by the engine is identical; 8 s from
pattern 0 at the project's tempo: the kit-only projects silent (no steps),
the others play all tracks (no NaN; peaks at full scale with the LXR's
limiter, as their kits are); lilv restores the presets (as jalv); the
bundles pass sord_validate.

### 10.7 The notOnGUI finding

The jalv fork (`~/bastel/github/jalv`, `src/jalv.c` `create_port()`) does
not create a control for a port with `pprops:notOnGUI` unless
`show_hidden` (only the GTK UI's `--show-hidden`; not the console's
options): such a port is missing from `controls`, `monitors` and
`symbol=value` (`jalv_get_control_from_symbol()` searches the controls).
So the panel ports are visible ports in their own groups (Zynthian shows
the input groups as 15 pages after the voices', the outputs as monitors).

### 10.8 Tests

`tests/seq_test.c` (ctest `sequencer`, `project_preset`), `lilv_preset`
(with lilv), `converter_test.py` (`PresetTest`), the test host's
`th_seq_position()`:
- the ported sequencer on the engine: steps, velocities, notes (two
  tempos), flams x3 / x8, shift +2 / -3, x2 with +4 (into the next step),
  probability 0 / 100 / 50 (repeatable; another seed differs), polymeter
  3 / 5 / 16, shuffle 127 (with a flam's overdue tick) and 64, mutes, the
  pattern switch at the end, songs (end, loop, started inside, past the
  end), a start inside a pattern with a 3-step track: the trigger blocks
  as computed from the LXR's timing rules; locks render bit exact as the
  engine with the parameters set and set back at those blocks; kits per
  pattern loaded at the patterns' first steps (engines included);
- the plugin bit exact against the engine's sequencer driven the same way:
  host transport (start, a MIDI note while playing, tempo 120 -> 140,
  locate to bar 2, stop, a MIDI note while stopped, a start at bar 1 beat
  2), Internal mode, Off; restore while running (swapped at the block, the
  position kept); 48 kHz onsets; the panel (every input kind, the
  outputs, the saved project has the edits); state round trip, a state
  without a project, garbage refused; the kit ports' rules (§10.5);
- converted projects: synthetic V1-V3 patterns, kits with / without the
  engine block and a short one, a song with a loop, GLO.CFG: the chunk is
  the engine's, the preset plays bit exact as the engine with the project
  built from the files by the engine's own parser; sord_validate; lilv.
- CPU (desktop, x86_64, 48 kHz / 960): the sequencer playing the test
  project costs +0.1 to +0.5 % over the same density of MIDI hits; the
  render test's kit scenario (sequencer idle) is 0.2.0's within the noise
  (44.1 kHz: 219.1 -> 218.9 ns per sample, 48 kHz: 228.4 -> 229.8, five
  runs each).

### 10.9 Deviations from the plan, open items

- Steps 2-4 are one commit (the plugin's `run()`, ports and state are one
  file; their tests one program).
- `lxr2preset.py` instead of `lxr2zynthian.py --internal` (simpler, no
  zynseq); its snapshot is minimal (one chain, the preset, the tempo).
- The edit pattern's kit is 0-63 or 64 (none) as an integer port; the
  edit pattern 64 means "the playing one" (no enumeration of 65 values).
- Probability: the LXR compares a step with the random value drawn at the
  track's active step before (its prev / current / next pipeline for
  shifted steps), starting at 0: the first active step of each track after
  `lxr_engine_init()` always plays. Kept.
- A pattern switch keeps the edits (no backup revert; `LV2:` change).
- Undo, track copy, rolls and recording have no ports.
- Open: the armv7 package and the phone (CPU, Zynthian's transport
  starting the sequencer, the pages), CI through device-testing; the
  widget (plugin-widgets, from §9); Zynthian's snapshots: a snapshot stores
  controller values and a preset reference, not the state, so a project
  edited on the panel survives only through `save preset` (and a
  snapshot referencing that preset); restoring a snapshot sets the edit
  ports' stored values after the preset, which writes them into the
  selected step when they differ (the snapshot should not store the
  `seq_*` edit ports, or the widget should handle them: for
  plugin-widgets); patch properties (§6).
