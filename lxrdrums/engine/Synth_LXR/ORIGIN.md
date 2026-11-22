# Origin of this copy

`engine/Synth_LXR/` is a copy of MicroDexed-touch's `third-party/Synth_LXR`
(src/, lxr/, LICENSE, NOTICE.md, README.md, library.json), taken unchanged
from:

- repository: MicroDexed-touch (https://codeberg.org/hjanetzek/MicroDexed-touch),
  branch `circle-port` of the cynthian port
- commit: `ca6518a82dec521f5f7798def5258d8ac9a18ee4` (2026-11-09)
- last commit touching `third-party/Synth_LXR`: `198eaa955da1837a8f2e949726efb137943db98d`
  ("Synth_LXR: two mixes (LO, HI) with a voice split")

The plugin builds only `src/lxr_engine.c` (with `-std=gnu17 -I lxr -I lxr/shim`);
`src/synth_lxr.cpp/.h` (the Teensy Audio object) and `library.json` are kept
for comparison with MDT and are not built.

## Changes in this copy

Changes for the LV2 plugin are marked `LV2:` in the code and listed here, so
they can be carried back to MicroDexed-touch by hand.

- none yet
