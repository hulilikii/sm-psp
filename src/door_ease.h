#pragma once
/* The eased door scroll's integer math, shared by sm_80.c (the game's door scroll) and src/psp/psp_door_snap.h (the PSP
 * snapshot slide), so that both follow the very same rounding. Moved verbatim from sm_80.c; plain int arithmetic only. */

// Door/elevator-room scrolling: quadratic ease in/out (SM_DOOR_EASE in config.h; 0 = original constant 4 px per step).
// The original moves 4 px per step for N steps (64 horizontal, 56 vertical), N*4 px in total. The eased path moves the
// same total in the same N steps: cumulative(k) = total * (2t^2 for t < 1/2, 1 - 2(1-t)^2 otherwise), t = k/N, rounded.
// Samus's step is her original per-step speed scaled by step/4, so her total distance is unchanged and exact.
static inline int DoorEaseCum(int k, int n, int total) {
  if (k <= 0)
    return 0;
  if (k >= n)
    return total;
  const int nn = n * n;
  if (2 * k <= n)
    return (2 * total * k * k + nn / 2) / nn;
  const int m = n - k;
  return total - (2 * total * m * m + nn / 2) / nn;
}

// SM_DOOR_DIAG: camera alignment on the axis across the door. weight(k) = n^2 * (1 - cum(k)/total) is 0 at k = n, so the
// last step lands exactly on the destination.
static inline int DoorEaseRemainW(int k, int n) {
  const int nn = n * n;
  if (2 * k <= n)
    return nn - 2 * k * k;
  const int m = n - k;
  return 2 * m * m;
}

// Remaining distance to the destination after step i (0-based, i < n), given the remaining distance before it.
static inline int DoorPerpRemaining(int rem, int i, int n) {
  const int wi = DoorEaseRemainW(i, n), wj = DoorEaseRemainW(i + 1, n);
  const int num = rem * wj;
  return (num + (num >= 0 ? wi / 2 : -(wi / 2))) / wi;
}
