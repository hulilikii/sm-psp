#!/bin/sh
# Host test for the door slide state machine and snapshot capture (psp_renderer.c). Run from the repository root.
set -e
R=src/psp/psp_renderer.c
a=$(grep -n '^/\* ---- Instant door transition (SM_PSP_DOOR_SNAP)' $R | cut -d: -f1)
b=$(grep -n '^/\* True if a VRAM page of \[word0' $R | cut -d: -f1)
sed -n "${a},$((b-1))p" $R > /tmp/door_snap_state.inc
c=$(grep -n 'The compose list is checked, not assumed' $R | cut -d: -f1)
d=$(grep -n 'TileVert \*base = sceGuGetMemory((int)((kComposeStrips' $R | cut -d: -f1)
sed -n "${c},$((d-3))p" $R > /tmp/door_snap_capture.inc
f=$(grep -n '^static bool DsListFits(void) {' $R | cut -d: -f1)
sed -n "${f},$((f+4))p" $R > /tmp/door_snap_fits.inc
${CC:-cc} -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -I. -o /tmp/test_door_snap_state tools/test_door_snap_state.c
/tmp/test_door_snap_state
