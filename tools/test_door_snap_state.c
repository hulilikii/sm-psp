/* Host test for the door slide state machine in psp_renderer.c (Arm / Ready / Begin / Hold, the composition's snapshot
 * capture, the releases). The code is cut out of the renderer by tools/test_door_snap_state.sh and run against stand-ins
 * for libgu and the layer image state. Checks that every path ends idle with the layer images restored, that the slide lasts
 * exactly DsSteps frames, and that nothing can leave the game held. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "src/psp/psp_door_snap.h"

#define CASSERT(c) _Static_assert(c, #c)
#define SM_PSP_DOOR_SNAP 1
#define SM_PSP_DOOR_GE 0
enum { kImgBytes = 131072, kVramImg0 = 1720320, GU_PSM_5650 = 0 };
typedef struct ComposeSrc { const void *ptr; int psm, w, h, stride; } ComposeSrc;
enum { kGameWidth = 256, kHudRowsMax = 32 };
typedef struct Ppu Ppu;
static int g_scratch_calls, g_scratch_fail_at;
static bool ppu_drawLineScratch(Ppu *p, int line, uint8_t *dst, int bg3_ref_line) {  /* stand-in: a pattern that identifies the line */
  (void)p;
  if (bg3_ref_line != kHudRowsMax + 1) return false;  /* the renderer must pin BG3 to the first gameplay line */
  g_scratch_calls++;
  if (g_scratch_fail_at && line == g_scratch_fail_at)
    return false;
  for (int x = 0; x < 256; x++) { dst[x * 4] = (uint8_t)(line * 8); dst[x * 4 + 1] = (uint8_t)x; dst[x * 4 + 2] = 0xf8; dst[x * 4 + 3] = 0xff; }
  return true;
}
static inline uint16_t Rgb565(const uint8_t *p) { return (uint16_t)((p[0] >> 3) | (p[1] >> 2) << 5 | (p[2] >> 3) << 11); }
static uint32_t sceKernelGetSystemTimeLow(void) { return 0; }
static int g_wb;
static void sceKernelDcacheWritebackRange(const void *p, unsigned n) { (void)p, (void)n; g_wb++; }
static bool g_img_ok = true, g_imgh_ok = true, g_img_skip_ok = true, g_ge_active = true;
static uint8_t g_img[2][8];
static int g_dirty_all, g_copies, g_copy_dst[8], g_copy_dy[8], g_copy_h[8], g_cap_bytes;
static uint16_t g_tex_mem[4];
static uint16_t *g_texture = g_tex_mem;
static void ImgMarkAllDirty(void) { g_dirty_all++; }
static void PspLog(const char *f, ...) { (void)f; }
static void sceGuCopyImage(int psm, int sx, int sy, int w, int h, int sw, void *src, int dx, int dy, int dw, void *dst) {
  (void)psm, (void)sx, (void)sy, (void)w, (void)sw, (void)src, (void)dx, (void)dw;
  if (g_copies < 8) {  /* the log holds 8 copies; the count itself keeps going */
    g_copy_dy[g_copies] = dy;
    g_copy_h[g_copies] = h;
    g_copy_dst[g_copies] = (int)((uintptr_t)dst & 0x1fffff);
  }
  g_copies++;
  g_cap_bytes += 32;  /* 8 commands (pspsdk sceGuCopyImage) */
}
static void sceGuTexSync(void) { g_cap_bytes += 4; }
static bool g_ds_ok_init = true;

/* the compose-list guard: DsListFits is the renderer's own; the bound and the list position are stand-ins */
enum { kListBytes = 512 * 1024, kComposeListBytes = 256 * 1024, kComposeTail = 1024 };
static uint32_t g_gu_list[4], g_mock_used, g_mock_bound;
static uint32_t g_gl_cap = kListBytes;  /* the open list's capacity (psp_gl_guard.h): g_gu_list, or the 256 KB g_gu_list2 of PSP_ASYNC=1 */
static uint32_t CurListUsed(void) { return g_mock_used; }
static uint32_t ComposeBytesBound(uint32_t *mem, uint32_t *calls) { *mem = *calls = 0; return g_mock_bound; }
#include "/tmp/door_snap_fits.inc"

#include "/tmp/door_snap_state.inc"

static void compose(const ComposeSrc *s, bool hud) {
#include "/tmp/door_snap_capture.inc"
}

static int fails;
static unsigned dir_for_arm;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static void restored(const char *what) {
  CHECK(g_ds_state == kDsIdle && g_img_ok && g_imgh_ok && g_img_skip_ok, "%s: idle + layer images restored", what);
}

int main(void) {
  g_ds_ok = true;
  const ComposeSrc good = {g_tex_mem, GU_PSM_5650, 256, 256, 256}, sw = {&g_dirty_all, GU_PSM_5650, 256, 256, 256};
  for (unsigned dir = 0; dir < 4; dir++) {
    /* normal door: arm, snapshot A, ready, begin, snapshot B, slide, release. The slide length is whatever Begin was given
     * (the momentum-scaled DsStepsForSpeed): a different one per direction, the extremes included. */
    const int slide_n = dir == 0 ? kDsStepsHMin : dir == 1 ? kDsStepsHMax : dir == 2 ? kDsStepsVMin : kDsStepsVMax;
    CHECK(PspRenderer_DoorSnapCanArm(), "idle: the game may keep Samus out of the snapshot");
    g_copies = 0;
    g_cap_bytes = 0;
    dir_for_arm = dir;
    PspRenderer_DoorSnapArm(dir_for_arm);
    CHECK(!PspRenderer_DoorSnapCanArm(), "armed: no second arm (and Samus is no longer hidden)");
    CHECK(g_ds_state == kDsArmed && !g_img_ok && !g_imgh_ok && !g_img_skip_ok, "armed turns the layer images off");
    CHECK(!PspRenderer_DoorSnapReady(), "not ready before the old frame is captured");
    compose(&good, true);
    CHECK(PspRenderer_DoorSnapReady() && g_copies == 1 && g_copy_dst[0] == kVramImg0, "A captured into slot 0");
    CHECK(g_cap_bytes == 36, "capture emits %d B (CopyImage 32 + TexSync 4)", g_cap_bytes);
    CHECK(!PspRenderer_DoorSnapHold() && g_ds_state == kDsHaveA, "no hold before the commit");
    PspRenderer_DoorSnapBegin(dir, -37, slide_n);
    CHECK(g_ds_state == kDsWaitB, "begun");
    compose(&good, false);  /* HUD layout unresolved: refused, retried */
    CHECK(g_ds_state == kDsWaitB && g_copies == 1, "B refused without a resolved HUD");
    compose(&sw, true);     /* not the GE composition source */
    CHECK(g_ds_state == kDsWaitB && g_copies == 1, "B refused from another source");
    compose(&good, true);
    CHECK(g_ds_state == kDsPlay && g_copies == 2 && g_copy_dst[1] == kVramImg0 + kImgBytes && g_ds_step == 0, "B captured into slot 1");
    const int n = slide_n;
    int held = 0, last_drawn = (int)g_ds_step;  /* the capture frame draws step 0 */
    while (PspRenderer_DoorSnapHold()) {
      held++;
      /* the composition of a held frame draws g_ds_step (DrawDoorSnap): consecutive, and the last one is step n */
      CHECK((int)g_ds_step == last_drawn + 1 && g_ds_state == kDsPlay, "dir %u: step %d follows %d", dir, (int)g_ds_step, last_drawn);
      last_drawn = (int)g_ds_step;
      CHECK(held < 1000, "hold never ends");
      if (held >= 1000) break;
    }
    CHECK(last_drawn == n, "dir %u: the last step drawn is %d, want the destination step %d", dir, last_drawn, n);
    CHECK(held == n, "dir %u: held %d frames, want %d (steps 1..%d; the game runs on frame N+%d)", dir, held, n, n, n + 1);
    restored("slide end");
    CHECK(!PspRenderer_DoorSnapHold(), "released");
  }
  /* the rows under the HUD: only the new room of a DOWN door (slot 1, frame N; the up door's gap is the new room's edge row, the
   * old room never has them). The old comment follows, superseded: Up door: the old room (slot 0, the armed frame); down door:
   * the new room (slot 1, frame N); horizontal doors: none. The hook runs for lines 1..33 of the frame being captured. */
  for (unsigned dir = 0; dir < 4; dir++) {
    dir_for_arm = dir;
    PspRenderer_DoorSnapArm(dir);
    g_copies = 0; g_cap_bytes = 0; g_scratch_calls = 0; g_wb = 0;
    for (int l = 1; l <= 33; l++) DsCaptureTop(NULL, l);
    compose(&good, true);
    const bool want_a = false;  /* the old room never gets rows under the HUD */
    CHECK(g_ds_top[0] == want_a && !g_ds_top[1], "dir %u: A rows under the HUD %d (want %d)", dir, (int)g_ds_top[0], (int)want_a);
    CHECK(g_scratch_calls == (want_a ? 32 : 0), "dir %u: %d scratch lines for A", dir, g_scratch_calls);
    CHECK(g_copies == (want_a ? 2 : 1), "dir %u: A copies %d", dir, g_copies);
    if (want_a) {
      CHECK(g_copy_dst[1] == kVramImg0 && g_copy_dy[1] == kDsTopRow0 && g_copy_h[1] == kDsTopRows, "top rows go to slot 0 rows 192..223");
      CHECK(g_cap_bytes == 68 && g_wb == 1, "A capture with top rows: %d B, %d writebacks", g_cap_bytes, g_wb);
      for (int l = 1; l <= 32; l++)  /* the staged rows are the drawn lines, converted */
        for (int x = 0; x < 256; x += 51) {
          const uint8_t px[4] = {(uint8_t)(l * 8), (uint8_t)x, 0xf8, 0xff};
          CHECK(g_ds_top_stage[(l - 1) * 256 + x] == Rgb565(px), "stage row %d x %d", l, x);
        }
    }
    PspRenderer_DoorSnapBegin(dir, 0, DsSteps(dir));
    g_copies = 0; g_scratch_calls = 0;
    for (int l = 1; l <= 33; l++) DsCaptureTop(NULL, l);
    compose(&good, true);
    const bool want_b = dir == 2;
    CHECK(g_ds_top[1] == want_b && (g_ds_top[0] == want_a || g_ds_state == kDsIdle), "dir %u: B rows under the HUD %d (want %d)", dir, (int)g_ds_top[1], (int)want_b);
    CHECK(g_scratch_calls == (want_b ? 32 : 0), "dir %u: %d scratch lines for B", dir, g_scratch_calls);
    CHECK(g_ds_state == kDsPlay, "dir %u: playing", dir);
    DoorSnapRelease();
    CHECK(!g_ds_top[0] && !g_ds_top[1] && !g_ds_top_ready, "release clears the top-row flags");
    restored("top rows");
  }
  /* the armed frame (the old room) stages nothing; the new room of a down door is staged on frame N and may fail or go stale:
   * no top rows then, the edge-row replication stays */
  dir_for_arm = 3;
  PspRenderer_DoorSnapArm(3);
  g_scratch_calls = 0;
  for (int l = 1; l <= 33; l++) DsCaptureTop(NULL, l);
  CHECK(!g_ds_top_ready && g_scratch_calls == 0, "the old room's frame is never staged");
  compose(&good, true);
  PspRenderer_DoorSnapBegin(2, 0, 0);
  for (int l = 1; l <= 33; l++) DsCaptureTop(NULL, l);
  CHECK(g_ds_top_ready, "stage ready before the capture of B");
  DsCaptureTop(NULL, 1);
  CHECK(!g_ds_top_ready, "the next frame's first line drops a stale stage");
  g_scratch_fail_at = 10;
  for (int l = 1; l <= 33; l++) DsCaptureTop(NULL, l);
  CHECK(!g_ds_top_ready, "a failed scratch draw gives no stage");
  g_scratch_fail_at = 0;
  compose(&good, true);
  CHECK(g_ds_state == kDsPlay && !g_ds_top[1], "B captured without top rows when the scratch draw failed");
  DoorSnapRelease();
  g_ge_active = false;
  PspRenderer_DoorSnapArm(3);
  g_scratch_calls = 0;
  for (int l = 1; l <= 33; l++) DsCaptureTop(NULL, l);
  CHECK(g_scratch_calls == 0, "no scratch draw when idle / GE off");
  g_ge_active = true;
  /* A refused: the game falls back */
  PspRenderer_DoorSnapArm(dir_for_arm);
  compose(&good, false);
  CHECK(!PspRenderer_DoorSnapReady(), "A refused: not ready");
  PspRenderer_DoorSnapAbort();
  restored("abort after refused A");
  /* the game never commits */
  PspRenderer_DoorSnapArm(dir_for_arm);
  compose(&good, true);
  int frames = 0;
  while (g_ds_state != kDsIdle && frames < 50) { PspRenderer_DoorSnapHold(); frames++; }
  CHECK(frames <= kDsMaxHaveA + 1, "uncommitted snapshot expires (%d)", frames);
  restored("expired");
  /* B never valid */
  PspRenderer_DoorSnapArm(dir_for_arm);
  compose(&good, true);
  PspRenderer_DoorSnapBegin(0, 0, 0);
  frames = 0;
  while (PspRenderer_DoorSnapHold() && frames < 50) frames++;
  CHECK(frames == kDsMaxWaitB, "waiting for B gives up after %d frames (%d)", kDsMaxWaitB, frames);
  restored("B timeout");
  /* Begin without a length (0) slides the original length */
  for (unsigned dir = 0; dir < 4; dir++) {
    PspRenderer_DoorSnapArm(dir);
    compose(&good, true);
    PspRenderer_DoorSnapBegin(dir, 0, 0);
    compose(&good, true);
    int held = 0;
    while (PspRenderer_DoorSnapHold() && held < 1000) held++;
    CHECK(held == DsSteps(dir), "dir %u: default slide %d frames, want %d", dir, held, DsSteps(dir));
    restored("default length");
  }
  /* GE inactive / resources missing / already running: nothing arms, so the game must not hide Samus either */
  g_ge_active = false;
  CHECK(!PspRenderer_DoorSnapCanArm(), "no GE: Samus stays");
  g_ge_active = true;
  g_ds_ok = false;
  CHECK(!PspRenderer_DoorSnapCanArm(), "no resources: Samus stays");
  g_ds_ok = true;
  g_ge_active = false;
  PspRenderer_DoorSnapArm(dir_for_arm);
  CHECK(g_ds_state == kDsIdle && g_img_ok, "no arm without the GE path");
  g_ge_active = true;
  g_ds_ok = false;
  PspRenderer_DoorSnapArm(dir_for_arm);
  CHECK(g_ds_state == kDsIdle && g_img_ok, "no arm without resources");
  g_ds_ok = true;
  PspRenderer_DoorSnapArm(dir_for_arm);
  PspRenderer_DoorSnapArm(dir_for_arm);  /* a second arm must not save the cleared flags */
  PspRenderer_DoorSnapAbort();
  restored("double arm");
  /* the compose-list guard: DsListFits is exact at the limit, and a transition that does not fit is dropped before anything
   * is emitted (no copy, state idle, layer images restored) */
  g_mock_used = 40; g_mock_bound = (uint32_t)kListBytes - kComposeTail - 40;
  CHECK(DsListFits(), "fits exactly at the limit");
  g_mock_bound++;
  CHECK(!DsListFits(), "one byte over does not fit");
  g_mock_used = 0; g_mock_bound = 5064 + 20000;  /* a realistic bound (snapshots 5064 B + the rest of the composition) */
  CHECK(DsListFits(), "realistic bound fits");
  /* PSP_ASYNC=1 composes into the 256 KB g_gu_list2: the check is against that list's capacity, not the 512 KB frame list's */
  g_gl_cap = kComposeListBytes;
  g_mock_used = 40; g_mock_bound = (uint32_t)kComposeListBytes - kComposeTail - 40;
  CHECK(DsListFits(), "256 KB list: fits exactly at the limit");
  g_mock_bound++;
  CHECK(!DsListFits(), "256 KB list: one byte over does not fit");
  g_mock_used = 200 * 1024; g_mock_bound = 60 * 1024;  /* fits the 512 KB list, not the 256 KB one */
  CHECK(!DsListFits(), "256 KB list: 200 KB used + 60 KB bound is refused");
  g_gl_cap = kListBytes;
  CHECK(DsListFits(), "the same position fits the 512 KB list (PSP_ASYNC=0 / 2)");
  g_mock_used = 0;
  g_mock_used = (uint32_t)kListBytes; g_mock_bound = 0;
  PspRenderer_DoorSnapArm(dir_for_arm);
  g_copies = 0;
  compose(&good, true);
  CHECK(g_ds_state == kDsIdle && g_copies == 0 && g_img_ok, "armed transition dropped before it emits a command");
  g_mock_used = 0; g_mock_bound = 1000;
  PspRenderer_DoorSnapArm(dir_for_arm);
  compose(&good, true);
  PspRenderer_DoorSnapBegin(2, 0, 0);
  g_mock_used = (uint32_t)kListBytes; g_copies = 0;  /* the list fills up while waiting for B */
  compose(&good, true);
  CHECK(g_ds_state == kDsIdle && g_copies == 0 && g_img_ok && !PspRenderer_DoorSnapHold(), "WaitB dropped when the list is full");
  g_mock_used = 0;
  /* software frame / resume release */
  PspRenderer_DoorSnapArm(dir_for_arm);
  compose(&good, true);
  DoorSnapRelease();
  restored("release");
  /* Begin without a snapshot does nothing */
  PspRenderer_DoorSnapBegin(1, 5, 0);
  CHECK(g_ds_state == kDsIdle, "begin without snapshot");
  printf("%s\n", fails ? "FAILED" : "OK");
  return fails != 0;
}
