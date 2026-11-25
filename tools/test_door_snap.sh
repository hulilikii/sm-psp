#!/bin/sh
# Host test for the door slide geometry (src/psp/psp_door_snap.h) against the game's own door scroll. Run from the repository root.
set -e
# one copy of the easing math: the game must use src/door_ease.h, not a private copy
if grep -nE "^static (inline )?int (DoorEaseCum|DoorEaseRemainW|DoorPerpRemaining)\(" src/sm_80.c; then
  echo "FAILED: sm_80.c defines its own easing functions"; exit 1
fi
grep -q '#include "door_ease.h"' src/sm_80.c || { echo "FAILED: sm_80.c does not include door_ease.h"; exit 1; }
${CC:-cc} -O2 -Wall -Wextra -I. -o /tmp/test_door_snap tools/test_door_snap.c
/tmp/test_door_snap
