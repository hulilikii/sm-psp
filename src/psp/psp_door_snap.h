#pragma once
/* Geometry of the dual-snapshot door slide (SM_PSP_DOOR_SNAP). Pure integer math, no PSP or game state, so that
 * tools/test_door_snap.sh can check it on the host.
 *
 * The default (live) door scroll moves the camera N steps (64 horizontal, 56 vertical) by a quadratic ease in/out over
 * `total` px (256 / 224): DoorEaseCum. With SM_DOOR_DIAG the new room's camera also starts `align` px off the destination
 * across the door (the old camera's distance to the nearest 256 boundary, DoorAlignDistance) and closes that offset step by
 * step with DoorPerpRemaining, a recursion with its own signed rounding (sm_80.c DoorPerpStepX / DoorPerpStepY). Both come
 * from src/door_ease.h, the same code the game runs, and the slide replays the recursion step by step (never a closed form),
 * so the snapshots sit exactly where the live scroll's camera would be, with no drift and an exact landing on step N.
 * All displacements are in SNES source pixels; +x right, +y down. */
#include "src/door_ease.h"

enum { kDsStepsH = 64, kDsStepsV = 56, kDsTotalH = 256, kDsTotalV = 224 };

static inline int DsSteps(unsigned dir) {
  return (dir & 2) ? kDsStepsV : kDsStepsH;
}

/* Displacement of the outgoing (old) and incoming (new) image after `step` of DsSteps(dir) steps.
 * dir = door_direction & 3 (0 right, 1 left, 2 down, 3 up); align = signed px the old camera would have to move across
 * the door to reach the nearest 256 boundary (0 when aligned), as DoorTransitionScrollingSetup subtracts it from the new
 * camera. out = {old x, old y, new x, new y}.
 *
 * The game keeps rem = camera - destination across the door: -align before the first step, then
 * rem = DoorPerpRemaining(rem, i, n) in step i. The new image is displaced by destination - camera = -rem; the old image
 * follows the camera from where it started: -(camera - start) = -(rem - (-align)) = -align - rem. */
static inline void DsOffsets(unsigned dir, int step, int align, int out[4]) {
  const int n = DsSteps(dir), total = (dir & 2) ? kDsTotalV : kDsTotalH;
  if (step < 0)
    step = 0;
  if (step > n)
    step = n;
  const int cum = DoorEaseCum(step, n, total);
  int rem = -align;
  for (int i = 0; i < step; i++)
    rem = DoorPerpRemaining(rem, i, n);
  const int np = -rem, op = -align - rem;                          /* across the door */
  const int fwd = (dir & 1) ? 1 : -1;                              /* old image: left/up for right/down doors */
  const int om = fwd * cum, nm = -fwd * (total - cum);             /* main axis */
  if (dir & 2) {
    out[0] = op; out[1] = om; out[2] = np; out[3] = nm;
  } else {
    out[0] = om; out[1] = op; out[2] = nm; out[3] = np;
  }
}

/* Source pixels -> viewport pixels (360x270 viewport from 256x192 source rows = 45/32), rounded away from zero. */
static inline int DsDispPx(int src_px) {
  return (src_px * 45 + (src_px >= 0 ? 16 : -16)) / 32;
}
