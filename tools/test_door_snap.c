/* Host test for src/psp/psp_door_snap.h (the geometry of the dual-snapshot door slide) against the game's own door scroll.
 *
 * REF_* below is the original code, copied verbatim from src/sm_80.c at the baseline (b833b759): DoorEaseRemainW,
 * DoorPerpRemaining, DoorEaseCum, DoorAlignDistance, and the per-step bookkeeping of DoorPerpStepY / DoorPerpStepX and
 * DoorTransition_Right / _Left / _Down / _Up (step index, direction, int16 camera arithmetic). The test
 *   1. checks src/door_ease.h (now the single copy used by the game and the slide) against REF_* exhaustively,
 *   2. replays the game's scroll for every door direction, alignment and destination, step by step, and requires the
 *      snapshot offsets (DsOffsets) to equal destination - camera and -(camera - start camera) at every step, the camera to
 *      land exactly on the destination, and no drift, sign flip or overshoot on the way. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "src/psp/psp_door_snap.h"

/* ---- REF: verbatim from the baseline sm_80.c ---- */
static int REF_DoorEaseRemainW(int k, int n) {
  const int nn = n * n;
  if (2 * k <= n)
    return nn - 2 * k * k;
  const int m = n - k;
  return 2 * m * m;
}
static int REF_DoorPerpRemaining(int rem, int i, int n) {
  const int wi = REF_DoorEaseRemainW(i, n), wj = REF_DoorEaseRemainW(i + 1, n);
  const int num = rem * wj;
  return (num + (num >= 0 ? wi / 2 : -(wi / 2))) / wi;
}
static int REF_DoorEaseCum(int k, int n, int total) {
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
static int REF_DoorStepPx(int i, int n, int total) {  /* SM_DOOR_EASE branch */
  return REF_DoorEaseCum(i + 1, n, total) - REF_DoorEaseCum(i, n, total);
}
static int REF_DoorAlignDistance(uint16_t pos) {
  const int low = pos & 0xFF;
  return (low & 0x80) ? 256 - low : -low;
}
/* DoorPerpStepY / DoorPerpStepX on one coordinate */
static void REF_PerpStep(uint16_t *layer1, uint16_t dest, int i, int n) {
  const int rem = (int16_t)(uint16_t)(*layer1 - dest);
  *layer1 = (uint16_t)(*layer1 + REF_DoorPerpRemaining(rem, i, n) - rem);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void check_shared_code(void) {
  for (int n = 1; n <= 80; n++) {
    for (int k = -2; k <= n + 2; k++) {
      if (k >= 0)
        CHECK(DoorEaseRemainW(k, n) == REF_DoorEaseRemainW(k, n), "RemainW k %d n %d", k, n);
      for (int total = 0; total <= 300; total += 4)
        CHECK(DoorEaseCum(k, n, total) == REF_DoorEaseCum(k, n, total), "Cum k %d n %d total %d", k, n, total);
    }
    for (int i = 0; i < n; i++)
      for (int rem = -512; rem <= 512; rem++)
        CHECK(DoorPerpRemaining(rem, i, n) == REF_DoorPerpRemaining(rem, i, n), "Perp rem %d i %d n %d", rem, i, n);
  }
}

/* One door: the game's scroll, replayed, against the snapshot offsets. */
static void check_door(unsigned dir, int align, uint16_t dest_main, uint16_t dest_perp) {
  const int n = DsSteps(dir), total = (dir & 2) ? kDsTotalV : kDsTotalH;
  const int sign = (dir & 1) ? -1 : 1;  /* camera moves +x / +y for right / down doors, -x / -y for left / up */
  /* DoorTransitionScrollingSetup: new camera on the destination, the axis across the door offset by -align; the main axis
   * starts `total` before the end of its travel */
  uint16_t perp = (uint16_t)(dest_perp - align), main = (uint16_t)(dest_main - sign * total);
  const uint16_t perp0 = perp, main0 = main;
  int prev_abs = abs((int16_t)(uint16_t)(perp - dest_perp));
  const int rem0_sign = (int16_t)(uint16_t)(perp - dest_perp) > 0 ? 1 : (int16_t)(uint16_t)(perp - dest_perp) < 0 ? -1 : 0;
  for (int k = 0; k <= n; k++) {
    if (k > 0) {  /* step k-1 of the game: DoorTransition_* with step index i = k-1 */
      const int i = k - 1, d = REF_DoorStepPx(i, n, total);
      main = (uint16_t)(main + sign * d);
      REF_PerpStep(&perp, dest_perp, i, n);
    }
    int o[4];
    DsOffsets(dir, k, align, o);
    const int new_main = (int16_t)(uint16_t)(dest_main - main), new_perp = (int16_t)(uint16_t)(dest_perp - perp);
    const int old_main = -(int16_t)(uint16_t)(main - main0), old_perp = -(int16_t)(uint16_t)(perp - perp0);
    const int onm = (dir & 2) ? o[3] : o[2], onp = (dir & 2) ? o[2] : o[3];
    const int oom = (dir & 2) ? o[1] : o[0], oop = (dir & 2) ? o[0] : o[1];
    CHECK(onm == new_main && onp == new_perp && oom == old_main && oop == old_perp,
          "dir %u align %d dest %04x/%04x k %d: got new (%d,%d) old (%d,%d), game new (%d,%d) old (%d,%d)", dir, align, dest_main,
          dest_perp, k, onm, onp, oom, oop, new_main, new_perp, old_main, old_perp);
    const int rem = (int16_t)(uint16_t)(perp - dest_perp);
    CHECK(abs(rem) <= prev_abs, "perpendicular remainder grows: dir %u align %d k %d", dir, align, k);
    CHECK(rem == 0 || (rem > 0 ? 1 : -1) == rem0_sign, "perpendicular remainder changes sign: dir %u align %d k %d", dir, align, k);
    prev_abs = abs(rem);
    /* the two images stay `total` apart along the door and `align` across it, whatever the rounding */
    CHECK(abs(onm - oom) == total, "main spacing dir %u k %d", dir, k);
    CHECK(onp - oop == align, "cross spacing dir %u align %d k %d: %d", dir, align, k, onp - oop);
    if (k == 0)
      CHECK(oom == 0 && oop == 0 && onp == align, "start dir %u align %d", dir, align);
    if (k == n)  /* the exact destination: no drift, nothing left over */
      CHECK(main == dest_main && perp == dest_perp && onm == 0 && onp == 0 && oop == -align,
            "landing dir %u align %d dest %04x/%04x: main %04x perp %04x", dir, align, dest_main, dest_perp, main, perp);
  }
}

int main(void) {
  check_shared_code();
  static const uint16_t dests[] = {0x0000, 0x0001, 0x00C0, 0x0100, 0x1234, 0x7FF0, 0x8000, 0xFFC0, 0xFFFF};
  long doors = 0;
  for (unsigned dir = 0; dir < 4; dir++)
    for (int align = -256; align <= 256; align++)
      for (unsigned a = 0; a < sizeof dests / sizeof dests[0]; a++) {
        check_door(dir, align, dests[a], dests[(a * 5 + 3) % (sizeof dests / sizeof dests[0])]);
        doors++;
      }
  /* the alignments the game can actually produce (DoorAlignDistance of any camera position) */
  for (unsigned dir = 0; dir < 4; dir++)
    for (int pos = 0; pos < 0x10000; pos += 7) {
      check_door(dir, REF_DoorAlignDistance((uint16_t)pos), (uint16_t)(pos * 3), (uint16_t)(pos ^ 0x5a5a));
      doors++;
    }
  /* DsOffsets clamps its step */
  int a[4], b[4];
  DsOffsets(0, -5, 100, a); DsOffsets(0, 0, 100, b);
  CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3], "negative step clamps to 0");
  DsOffsets(3, 999, -77, a); DsOffsets(3, kDsStepsV, -77, b);
  CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3], "step past the end clamps to N");
  /* rounding of the viewport mapping is symmetric and exact on a whole slide */
  CHECK(DsDispPx(256) == 360 && DsDispPx(-256) == -360 && DsDispPx(0) == 0 && DsDispPx(192) == 270, "dispPx");
  for (int v = -300; v <= 300; v++)
    CHECK(DsDispPx(v) == -DsDispPx(-v), "dispPx symmetry %d", v);
  printf("%s (%ld door replays)\n", fails ? "FAILED" : "OK", doors);
  return fails != 0;
}
