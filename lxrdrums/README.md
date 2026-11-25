# eDrumSynth

Version 0.2.0 (v2.1: the "LXR CC" switch; v2: engines per voice slot, MIDI
modes, kit presets and the kit converter; changes: [Changes in
0.2.0](#changes-in-020), [Changes in 0.1.0](#changes-in-010)). The LXR
project converter (patterns, songs and kits as a Zynthian snapshot):
[LXR projects as Zynthian snapshots](#lxr-projects-as-zynthian-snapshots).

Synthesized drums as an LV2 instrument: MicroDexed-touch's drum engine (the
voices of the OpenElixir-02 firmware for the LXR-02 drum machine, based on
the open source LXR V2 firmware by Erica Synths), made for Zynthian on small
ARM devices (target: postmarketOS / nura armv7, Cortex-A7). Plain LV2 in C,
no GUI: Zynthian shows the controls in pages, with its envelope and filter
widgets on the amp EG and filter pages; desktop hosts show their generic UIs.

- URI `https://github.com/hjanetzek/mdt-lv2/edrumsynth`, bundle
  `edrumsynth.lv2`, binary `edrumsynth.so`, `doap:name` "eDrumSynth" (the
  Zynthian engine key `JV/eDrumSynth`). GPL-3.0-only (the engine's licence
  and NOTICE: `engine/Synth_LXR/LICENSE`, `NOTICE.md`). "LXR" is an Erica
  Synths trademark; this is not affiliated with Erica Synths.
- 6 voices on 7 tracks: bass drum, tom 1, tom 2, snare, cymbal, hi-hat closed
  and open; each voice slot runs any of the four engines (drum, snare,
  cymbal, hi-hat; default: the stock ones). The LXR's init kit, the FX bus
  (distortion, ring modulator, compressor, delay), the DJ filter and the
  limiter. Factory presets, and a converter for LXR / MDT `.snd` kits.
- One MIDI input (atom), one stereo output. One instance per process (the
  engine is global state; jalv / Zynthian run one plugin per process): a
  second instance in the same process is refused.
- The plan and its decisions: [PLAN.md](PLAN.md).

## MIDI: the General MIDI drum map, Kit and Multi mode

The Master group's **MIDI Mode** (`master_midi_mode`):

- **Kit** (default): note-ons on any channel trigger the tracks at their
  tuned pitch with the note's velocity (the table). Right for a Zynthian
  chain on one MIDI channel.
- **Multi**: channel 10 plays the drum map; channels 1-7 play tracks 1-7
  chromatically (note 60 = the tuned pitch, 72 an octave up; channel 7 is the
  open hi-hat), with the note's velocity; other channels are ignored. For a
  chain set to all MIDI channels ("ALL MIDI CHANNELS" in the chain options).

Note-offs and other notes are ignored; CCs too unless the LXR CC switch is
on (below; Zynthian MIDI-learns the controls instead); `src/notemap.h`.

| Notes | Track |
|---|---|
| 35, 36 | bass drum |
| 45, 47, 48, 50 | tom 1 |
| 41, 43 | tom 2 |
| 37, 38, 39, 40 | snare |
| 49, 51, 52, 55, 57, 59 | cymbal |
| 42, 44 | hi-hat closed |
| 46 | hi-hat open (closed and open choke each other) |

Events take effect at the next 64-frame engine block (1.45 ms at 44.1 kHz,
as the LXR); the timing doesn't depend on the host's block size. A
`time:Position` with `time:beatsPerMinute` sets the tempo the LFOs' clock
sync follows (else 120).

## LXR CC: the LXR's CC / NRPN map

The Master group's **LXR CC** (`master_lxr_cc`, a toggle, **off** by
default). On, CCs and NRPNs on any channel set the engine's parameters as
the LXR's MIDI implementation (`lxr_engine_midi_cc()`): CC n sets parameter
n-1, NRPN n (CC 99 coarse, CC 98 fine, then data entry CC 6) parameter
128 + n; CC 0 (bank select) and CC 1 (mod wheel) set nothing; the engine
selection NRPN 98 (OpenElixir-02: value = slot * 4 + engine) is dropped,
the engine selector ports own the engines. Off, CCs are ignored as before.

A CC changes the engine, not the control: the ports keep their values, and
the next change of a control (or of its voice's engine) wins. This is what
the LXR's parameter locks need: the LXR project converter plays them as
CC / NRPN events from zynseq, with the restores to the kit values as events
too, and its snapshots turn the switch on.

Why off by default: a Zynthian synth chain passes the sustain / sostenuto /
soft / hold pedals (CC 64, 66, 67, 69) and, depending on its `cc_route`,
keyboard CCs such as volume (7), pan (10), expression (11) and the panic
messages (120-123, sent when chains are removed or stopped). With the
switch on, each of them would set a drum parameter (CC 64 = parameter 63,
CC 121 = 120, ...), changing the sound without any control showing it.
Turn it on only where the CCs reaching the chain are meant for the engine
(the converter's snapshots: the chain's `cc_route` lists exactly the
locked CCs and 6, 98, 99).

## Ports and Zynthian pages

Port 0 is the MIDI input, 1 and 2 the outputs (`pg:mainOutput`), 3..308 the
306 controls: an engine selector per voice, every voice parameter of the
engine's menu pages for every engine, the FX bus and the master
parameters. One `pg:group` per voice sub page (and per engine family);
Zynthian orders groups and controls by `lv2:displayPriority` and shows 4
controls per page ("Bass Drum LFO 1", "Bass Drum LFO 2"):

| Group (per voice) | Controls | Pages |
|---|---|---|
| *Voice* Mix: **engine**, volume, pan, sample rate, drive, audio out | 6 | 2 |
| *Voice* Osc: coarse, fine, waveform, pulse width | 4 | 1 |
| *Voice* Amp EG: attack, decay, slope (hi-hat: decay closed, decay open) | 3 (4) | 1 |
| *Voice* Filter: cutoff, resonance, type, drive | 4 | 1 |
| *Voice* Transient: waveform, volume, frequency | 3 | 1 |
| *Voice* Mod EG (Drum/Snare): mod EG decay, slope, pitch mod | 3 | 1 |
| *Voice* FM (Drum): amount, freq, wave, mode | 4 | 1 |
| *Voice* Noise (Snare): noise freq, noise mix, repeat | 3 | 1 |
| *Voice* Mod Osc (Cym/HH): freq 1/2, gain 1/2, wave 1/2, repeat (cymbal only) | 7 | 2 |
| *Voice* Velocity: destination, amount, volume mod | 3 | 1 |
| *Voice* LFO: frequency, clock sync, amount, waveform, retrigger, offset, destination voice and parameter | 8 | 2 |
| FX Bus: type, dry/wet, audio out, distortion type, FX param 1-4, compressor ratio, ring mod waveform, delay mode | 11 | 3 |
| Master: DJ filter, DJ resonance, limiter, sample rate (all voices), MIDI mode, LXR CC | 6 | 2 |

The voices in track order (bass drum, tom 1, tom 2, snare, cymbal, hi-hat),
each 14 pages, then FX Bus and Master: 68 groups, 89 pages. The list:
`build/gen_ttl --pages`.

### Engines per voice

Each voice's **Engine** (`<voice>_mix_engine`: Drum, Snare, Cymbal,
Hi-Hat) selects the slot's voice engine, as OpenElixir-02's engine menu.
The parameters every engine has (oscillator, amp EG, filter, transient,
velocity, LFO, mix) are one set of controls per voice. The engine specific
ones (the LXR's 7 "roles" per slot: one parameter number each, whose meaning
depends on the engine) are in the engine groups, each control marked with
the engines it applies to (its `rdfs:comment`): Mod EG for the drum and
snare engines, FM for the drum, Noise for the snare, Mod Osc for the cymbal
and hi-hat engines. The plugin sends a slot only the controls of the
engine it runs; the other engine groups' controls keep their values and
have no effect until the engine is switched. Switching an engine resets
the slot in the engine (its parameters to the engine's defaults, the
modulation targets on its engine specific parameters off), then the
plugin sends the voice's controls again where they differ: the controls
win. Presets bring sensible values with them; the engine groups' defaults
are the engine's own (the values a switch to that engine gives).

The engine specific parameters per slot (`build/gen_ttl --engines`; slot 1:
82 FM amount / noise mix / mod osc gain 1, 83 FM freq / noise freq / mod
osc freq 1, 74 pitch mod / mod osc freq 2, 78 mod EG slope / mod osc gain
2, 70 mod EG decay / mod osc wave 2, 20 FM wave / mod osc wave 1, 134 FM
mode / repeat (snare, cymbal); the other slots have their own numbers,
the same way).

The open hi-hat (track 7, note 46, Multi channel 7) is the second trigger
of the hi-hat slot (6) whatever its engine: with another engine it
retriggers the slot's voice with its own amp decay (`hh_ampeg_decay_open`),
so the two tracks still choke each other.

### Symbols, types, widgets

- Symbols are `<voice>_<group>_<name>` (`bd_osc_coarse`,
  `hh_ampeg_decay_open`, `bd_fm_amount`, `sd_noise_freq`, `fx_p1`,
  `master_limiter`; the velocity controls kept 0.0.1's `<voice>_mod_vel_*`).
  **They are API** (Zynthian presets, snapshots, MIDI learn):
  `tests/symbols.txt` freezes them (0.1.0, plus 0.2.0's `master_lxr_cc`);
  never rename or remove one.
- Types: numbers are integer ports in the engine's range; signed values
  (tuning, pan, DJ filter) centred on 0 (coarse -60..67, pan -63..64);
  on/off as `lv2:toggled`; menus (engines, waveforms, filter types, ...)
  and the modulation destinations (229 targets: "BD Osc Coarse", ...) as
  enumerations. A destination on an engine specific parameter is labelled
  with what it is in each engine group: "BD FM Amount / Noise Mix / Mod
  Gain 1".
- Widgets: `param:attack` / `param:decay` on each amp EG's attack and decay
  (hi-hat: decay closed), `param:cutoffFrequency` / `param:resonance` on each
  filter: plain float ports 0..127 (Zynthian's widgets need that). The filter
  widget always draws a low pass, also for the other filter types.
- FX param 1-4 mean something else per FX type (distortion: drive, color,
  tone, volume; ring mod: frequency; compressor: attack, decay, threshold,
  gain; delay: time, range, tone, feedback): their `rdfs:comment` says it.
- `lv2:displayPriority` is in the lv2core namespace because Zynthian reads it
  there (the spec's is `port-props:displayPriority`);
  `../common/zynthian-schema.ttl` declares it for the validators.
- Not exposed: kit morph, the LO / HI outputs, parameters without a name in
  the menus (unused).
- `tools/edrumsynth_ports.json` (generated with the TTL) is the port map for
  tools: per control its symbol, index, engine parameter, slot, engines,
  range and value conversion (port value = engine value - offset), the
  engine selectors, the MIDI mode port and the LXR CC port.

## Presets and kits

Factory presets (bank "Factory", `edrumsynth.lv2/presets.ttl`, made here):

- **Init**: the init kit, the stock engines.
- **Two Snares**: tom 2 runs the snare engine as a clap (mostly noise
  through a band pass, EG repeats).
- **All Drums**: every slot runs the drum engine: the snare slot a high tom
  with a rim click, the cymbal an FM bell, the hi-hat short and long FM
  noise clicks.

A kit preset sets every control that is in a kit: the engine selectors,
all voice controls (the inactive engine groups' at their defaults), the FX
bus and the overall sample rate; not the DJ filter, the limiter or the MIDI
mode.

### Converting LXR / MDT kits

`tools/snd2lv2.py` (Python 3, no dependencies) turns `.snd` kits into an
LV2 presets bundle: a kit file, a folder of kits, or a tree such as MDT's
`/SYNTHKITS` (its own kits and one bank per folder below it); LXR project
folders work the same way. Each folder with kits becomes a bank, each kit a
preset "NN NAME". OpenElixir-02's engine block sets the engines (kits
without one: the stock engines); older LXR kits with fewer parameters load
with the missing ones 0, as in the engine. Placeholder kits ("Initkit",
EMPTY) are left out (`--all` keeps them).

```
tools/snd2lv2.py -o edrumsynth-kits.presets.lv2 /path/to/SYNTHKITS
cp -r edrumsynth-kits.presets.lv2 ~/.lv2/
# Zynthian: /zynthian/zynthian-my-data/presets/lv2/ (then refresh the presets)
```

No kits are shipped (the LXR's and MDT's kit collections have their own
licences). Kit parameters out of an engine's range (e.g. transient samples
the LXR had loaded) take the init kit's value, as the engine does.

## Audio

The engine renders 64-frame blocks of 16 bit stereo at a fixed 44.1 kHz
when the output needs them; at other host rates (48 kHz on the device) a
polyphase FIR resampler (`src/resampler.c`: Kaiser windowed sinc, 48 taps
per phase, exact phases for 44.1 -> 48 kHz, interpolated phases for odd
rates) converts them; at 44.1 kHz the output is the engine's, bit exact.
At 48 kHz / 960 frames per period that is 13 or 14 engine blocks per period.
`run()` does no allocations, locks, file I/O or libm calls of its own and
sets an engine parameter only when its port's rounded value changed.

## Build, test, install

Needs a C compiler, CMake (3.16 or newer), Ninja or make and the LV2
headers (`lv2-dev`). Everything else is in this directory (the engine is a
copy: `engine/Synth_LXR/ORIGIN.md`) and in `../common` (the generator's TTL
helpers `ttl.h` and the render test host `testhost/`, shared with the other
plugins of this repository).

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
cmake --install build --prefix /usr      # /usr/lib/lv2/edrumsynth.lv2
```

`build/edrumsynth.lv2/` is a complete bundle (`LV2_PATH=$PWD/build`). No
architecture flags in CMake: CFLAGS from the build decide.

Tests (`ctest`; the validators are skipped when not installed):

- `ttl_regenerate`: `edrumsynth.lv2/*.ttl`, `src/ports.h` and
  `tools/edrumsynth_ports.json` as committed equal the generator's output. They are generated by `tools/gen_ttl.c`
  (linked with the engine) and committed, because cross builds can't run
  it; after a change to the generator or the engine's tables run
  `cmake --build build --target regenerate`.
- `symbols_frozen`: every symbol in `tests/symbols.txt` still exists.
- `sord_validate`, `lv2_validate`: the bundle against the LV2 specs (with
  `../common/zynthian-schema.ttl`).
- `lv2lint`: warnings are errors (notes remain: no units, no port comments,
  version 0.x).
- `render`: a minimal host (`tests/render_test.c` on
  `../common/testhost`: dlopen + `lv2_descriptor`, urid:map, no lilv). At
  44.1 kHz the plugin equals the engine driven directly (bit exact) for host
  blocks of 1 to 4096 frames; every engine on every slot (with values on
  that engine's controls) equals the engine switched with
  `lxr_engine_set_engine()` and set to the same values, also for switches
  while playing; the inactive engine groups' controls change nothing; Kit
  mode ignores the channel, Multi mode plays channel 10 as the kit and
  channels 1-7 at the note's pitch (an octave up = twice the frequency),
  as the engine triggered directly; at 48 kHz blocks of 1, 64, 333, 480,
  960, 1001 and 4096 frames give the same signal, no NaN or clipping; the
  bass drum's pitch and level at 22.05 to 192 kHz match 44.1 kHz (1 %, 0.5
  dB); a second instance is refused; LXR CC: off, CCs and NRPNs change
  nothing; on, they render bit exact as the engine driven by
  `lxr_engine_midi_cc()` (several host blocks, several channels), the
  engine selection NRPN changes nothing. It prints the CPU time per sample.
- `presets` (with Python 3): every factory preset sets exactly the kit
  controls, in range, and renders as the engine set up with its values
  ("Init" as the defaults); synthetic kits (a `/SYNTHKITS` tree with a bank
  folder, engine blocks, an LXR kit without one, an older short kit, a
  placeholder and a non-kit file) converted by `tools/snd2lv2.py`: each
  preset renders bit exact as the engine with the kit loaded directly
  (`lxr_engine_load_kit_engines()`). `sord_validate_converted` validates
  the converted bundle.
- `resampler`: sines through the resampler (44.1 -> 48, 47.999, 96, 32
  kHz): gain, phase, and the rest (aliasing, images, noise) below -80 dB.

The tests don't run jalv or anything that creates a JACK client.

## Packaging

`tools/mktarball.sh [--worktree] [OUTDIR]` writes
`edrumsynth-VERSION.tar.gz` (HEAD, or the working tree; `lxrdrums/` and
`common/`) and its sha512sum line; `packaging/APKBUILD` is the draft recipe for nura-builds
(`aports/lv2/edrumsynth-lv2/`, `arch="all"`, CMake, ctest in `check()`).

## Changes in 0.2.0

- The LXR CC switch (`master_lxr_cc`, Master group, off by default): CCs
  and NRPNs to the engine as the LXR's MIDI implementation. A new port
  (index 308) after the others: the existing indices and symbols are
  unchanged.
- The port map has `lxr_cc_port`.

## Changes in 0.1.0

- Engines per voice slot (`<voice>_mix_engine`), the engine groups Mod EG,
  FM, Noise and Mod Osc per voice, Velocity its own group.
- MIDI Mode Kit / Multi (`master_midi_mode`), the overall sample rate
  (`master_sample_rate`, parameter 114, in kits).
- Factory presets, `tools/snd2lv2.py`, `tools/edrumsynth_ports.json`.
- Symbols renamed (the engine specific parameters, now in the engine
  groups): `<bd,t1,t2,sd>_mod_eg_decay` -> `_modeg_decay`, `_mod_eg_slope`
  -> `_modeg_slope`, `_mod_pitch_amount` -> `_modeg_pitch`;
  `sd_osc_noise_freq` -> `sd_noise_freq`, `sd_osc_noise_mix` ->
  `sd_noise_mix`, `sd_ampeg_repeat` -> `sd_noise_repeat`, `cy_ampeg_repeat`
  -> `cy_modosc_repeat`. All other 0.0.1 symbols are kept (with the same
  meaning for the stock engines); 84 are new. Port indices changed.

## Open decisions

- Kit morph (a second kit as LV2 state), the LO / HI outputs, a note map
  control.
- FX param 1-4 names (one meaning per FX type, static ports).
- Zynthian enabling: an `engine_config.json` default (`"JV/eDrumSynth"`,
  `ENABLED`, `CAT: "Percussion"`).
