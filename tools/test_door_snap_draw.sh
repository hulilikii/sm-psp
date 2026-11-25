#!/bin/sh
# Host test for the door slide quads (DrawDoorSnap in psp_renderer.c). Run from the repository root.
set -e
R=src/psp/psp_renderer.c
a=$(grep -n '^/\* The two snapshots (slot 0 old room' $R | cut -d: -f1)
b=$(grep -n '^/\* ---- Native message overlay' $R | cut -d: -f1)
sed -n "${a},$((b-3))p" $R > /tmp/door_snap_draw.inc
q=$(grep -n '^static void ComposeQuad(' $R | cut -d: -f1)
sed -n "${q},$((q+6))p" $R > /tmp/door_snap_compose_quad.inc
${CC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I. -o /tmp/test_door_snap_draw tools/test_door_snap_draw.c -lm
/tmp/test_door_snap_draw
