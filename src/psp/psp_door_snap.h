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
#include <stdint.h>
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
static inline void DsOffsetsN(unsigned dir, int n, int step, int align, int out[4]) {
  const int total = (dir & 2) ? kDsTotalV : kDsTotalH;
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

static inline void DsOffsets(unsigned dir, int step, int align, int out[4]) {
  DsOffsetsN(dir, DsSteps(dir), step, align, out);
}

/* The slide's length follows Samus's momentum as she enters the door, so the room moves like she does rather than like an inserted
 * animation: the quadratic ease (DoorEaseCum) accelerates and brakes harder over fewer steps when she is fast, softer over more
 * when she is slow. speed_q8 = her speed along the door's axis in 1/256 px per frame (|x base + run speed| for a horizontal door,
 * |y speed| for a vertical one). Linear between kDsSpeedLo (slowest, kDsStepsXMax steps) and kDsSpeedHi (fastest, kDsStepsXMin);
 * the original length (64 / 56) falls in the middle for an ordinary run (H) / jump (V). The slide always spans the same 256 / 224 px. */
enum { kDsSpeedLo = 256, kDsSpeedHiH = 1024, kDsSpeedHiV = 1536,
       kDsStepsHMin = 48, kDsStepsHMax = 80, kDsStepsVMin = 42, kDsStepsVMax = 70 };

static inline int DsStepsForSpeed(unsigned dir, int speed_q8) {
  const int hi = (dir & 2) ? kDsSpeedHiV : kDsSpeedHiH;
  const int nmin = (dir & 2) ? kDsStepsVMin : kDsStepsHMin, nmax = (dir & 2) ? kDsStepsVMax : kDsStepsHMax;
  const int span = hi - kDsSpeedLo;
  int v = speed_q8 < kDsSpeedLo ? kDsSpeedLo : speed_q8 > hi ? hi : speed_q8;
  return nmax - ((nmax - nmin) * (v - kDsSpeedLo) + span / 2) / span;
}

/* Source pixels -> viewport pixels (360x270 viewport from 256x192 source rows = 45/32), rounded away from zero. */
static inline int DsDispPx(int src_px) {
  return (src_px * 45 + (src_px >= 0 ? 16 : -16)) / 32;
}

/* Samus's walk-in. The instant load leaves her where the live scroll's last step leaves her (end); the slide shows the new
 * room without her at the position the scroll started her from (start: one screen beyond the door, hidden), and she walks
 * to end over the live frames that follow, animated (Samus_Animate each frame) at a steady kDsWalkSpeed px per frame, so
 * the walk takes distance / speed frames (at least kDsWalkMin, at most the scroll's own length). Positions are the game's
 * 16-bit world coordinates; the difference is taken signed so a wrap of the coordinate cannot send her the long way round.
 * Step 0 is `start`, step n is exactly `end`; every step moves toward `end`, never past it. */
enum { kDsWalkSpeed = 2, kDsWalkMin = 8 };

static inline int DsWalkSteps(unsigned dir, int distance) {
  int n = (distance + kDsWalkSpeed - 1) / kDsWalkSpeed;
  if (distance < 0)
    n = 0;
  if (n < kDsWalkMin)
    n = kDsWalkMin;
  return n > DsSteps(dir) ? DsSteps(dir) : n;
}

/* The walk continues at her entry speed (clamped to a sensible walking range) instead of a fixed pace, so she carries on through
 * the door as she came in. speed_q8 as above. Same limits as DsWalkSteps. */
enum { kDsWalkSpeedLo = 256, kDsWalkSpeedHi = 768 };

static inline int DsWalkStepsSpeed(unsigned dir, int distance, int speed_q8) {
  const int v = speed_q8 < kDsWalkSpeedLo ? kDsWalkSpeedLo : speed_q8 > kDsWalkSpeedHi ? kDsWalkSpeedHi : speed_q8;
  int n = distance > 0 ? (distance * 256 + v - 1) / v : 0;
  if (n < kDsWalkMin)
    n = kDsWalkMin;
  return n > DsSteps(dir) ? DsSteps(dir) : n;
}

static inline uint16_t DsWalkPos(uint16_t start, uint16_t end, int step, int n) {
  const int d = (int16_t)(uint16_t)(end - start);
  if (step <= 0)
    return start;
  if (step >= n)
    return end;
  const int num = d * step;
  return (uint16_t)(start + (num + (num >= 0 ? n / 2 : -(n / 2))) / n);
}

/* Where the walk starts on the door's axis. The scroll starts Samus a screen beyond the door (fallback), so most of her walk would
 * be spent hidden off-screen (or under the HUD). The door cap is the doorway itself: the hack Nodever2_Door_Transitions places her
 * there too (its algorithms 2 and 5: the destination door's cap block, +0x10 below / -0x38 above the cap for vertical doors).
 * Use the cap when it lies ahead of the fallback and no further along than the end (so she still walks in the door's direction and
 * never farther than before); otherwise keep the fallback. dir: 0 right, 1 left, 2 down, 3 up. */
static inline uint16_t DsWalkStartAxis(unsigned dir, uint16_t cap, uint16_t fallback, uint16_t end) {
  const int sgn = (dir & 1) ? -1 : 1;
  const int dcap = sgn * (int)(int16_t)(uint16_t)(end - cap);
  const int dfb = sgn * (int)(int16_t)(uint16_t)(end - fallback);
  return (dcap >= 0 && dcap <= dfb) ? cap : fallback;
}
