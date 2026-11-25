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
enum { kGameWidth = 256 };
static bool g_img_ok = true, g_imgh_ok = true, g_img_skip_ok = true, g_ge_active = true;
static uint8_t g_img[2][8];
static int g_dirty_all, g_copies, g_copy_dst[8], g_cap_bytes;
static uint16_t g_tex_mem[4];
static uint16_t *g_texture = g_tex_mem;
static void ImgMarkAllDirty(void) { g_dirty_all++; }
static void PspLog(const char *f, ...) { (void)f; }
static void sceGuCopyImage(int psm, int sx, int sy, int w, int h, int sw, void *src, int dx, int dy, int dw, void *dst) {
  (void)psm, (void)sx, (void)sy, (void)w, (void)h, (void)sw, (void)src, (void)dx, (void)dy, (void)dw;
  g_copy_dst[g_copies++] = (int)((uintptr_t)dst & 0x1fffff);
  g_cap_bytes += 32;  /* 8 commands (pspsdk sceGuCopyImage) */
}
static void sceGuTexSync(void) { g_cap_bytes += 4; }
static bool g_ds_ok_init = true;

/* the compose-list guard: DsListFits is the renderer's own; the bound and the list position are stand-ins */
enum { kListBytes = 512 * 1024, kComposeTail = 1024 };
static uint32_t g_gu_list[4], g_mock_used, g_mock_bound;
static uint32_t CurListUsed(const uint32_t *list) { (void)list; return g_mock_used; }
static uint32_t ComposeBytesBound(uint32_t *mem, uint32_t *calls) { *mem = *calls = 0; return g_mock_bound; }
#include "/tmp/door_snap_fits.inc"

#include "/tmp/door_snap_state.inc"

static void compose(const ComposeSrc *s, bool hud) {
#include "/tmp/door_snap_capture.inc"
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static void restored(const char *what) {
  CHECK(g_ds_state == kDsIdle && g_img_ok && g_imgh_ok && g_img_skip_ok, "%s: idle + layer images restored", what);
}

int main(void) {
  g_ds_ok = true;
  const ComposeSrc good = {g_tex_mem, GU_PSM_5650, 256, 256, 256}, sw = {&g_dirty_all, GU_PSM_5650, 256, 256, 256};
  for (unsigned dir = 0; dir < 4; dir++) {
    /* normal door: arm, snapshot A, ready, begin, snapshot B, slide, release */
    g_copies = 0;
    g_cap_bytes = 0;
    PspRenderer_DoorSnapArm();
    CHECK(g_ds_state == kDsArmed && !g_img_ok && !g_imgh_ok && !g_img_skip_ok, "armed turns the layer images off");
    CHECK(!PspRenderer_DoorSnapReady(), "not ready before the old frame is captured");
    compose(&good, true);
    CHECK(PspRenderer_DoorSnapReady() && g_copies == 1 && g_copy_dst[0] == kVramImg0, "A captured into slot 0");
    CHECK(g_cap_bytes == 36, "capture emits %d B (CopyImage 32 + TexSync 4)", g_cap_bytes);
    CHECK(!PspRenderer_DoorSnapHold() && g_ds_state == kDsHaveA, "no hold before the commit");
    PspRenderer_DoorSnapBegin(dir, -37);
    CHECK(g_ds_state == kDsWaitB, "begun");
    compose(&good, false);  /* HUD layout unresolved: refused, retried */
    CHECK(g_ds_state == kDsWaitB && g_copies == 1, "B refused without a resolved HUD");
    compose(&sw, true);     /* not the GE composition source */
    CHECK(g_ds_state == kDsWaitB && g_copies == 1, "B refused from another source");
    compose(&good, true);
    CHECK(g_ds_state == kDsPlay && g_copies == 2 && g_copy_dst[1] == kVramImg0 + kImgBytes && g_ds_step == 0, "B captured into slot 1");
    const int n = DsSteps(dir);
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
  /* A refused: the game falls back */
  PspRenderer_DoorSnapArm();
  compose(&good, false);
  CHECK(!PspRenderer_DoorSnapReady(), "A refused: not ready");
  PspRenderer_DoorSnapAbort();
  restored("abort after refused A");
  /* the game never commits */
  PspRenderer_DoorSnapArm();
  compose(&good, true);
  int frames = 0;
  while (g_ds_state != kDsIdle && frames < 50) { PspRenderer_DoorSnapHold(); frames++; }
  CHECK(frames <= kDsMaxHaveA + 1, "uncommitted snapshot expires (%d)", frames);
  restored("expired");
  /* B never valid */
  PspRenderer_DoorSnapArm();
  compose(&good, true);
  PspRenderer_DoorSnapBegin(0, 0);
  frames = 0;
  while (PspRenderer_DoorSnapHold() && frames < 50) frames++;
  CHECK(frames == kDsMaxWaitB, "waiting for B gives up after %d frames (%d)", kDsMaxWaitB, frames);
  restored("B timeout");
  /* GE inactive / resources missing / already running */
  g_ge_active = false;
  PspRenderer_DoorSnapArm();
  CHECK(g_ds_state == kDsIdle && g_img_ok, "no arm without the GE path");
  g_ge_active = true;
  g_ds_ok = false;
  PspRenderer_DoorSnapArm();
  CHECK(g_ds_state == kDsIdle && g_img_ok, "no arm without resources");
  g_ds_ok = true;
  PspRenderer_DoorSnapArm();
  PspRenderer_DoorSnapArm();  /* a second arm must not save the cleared flags */
  PspRenderer_DoorSnapAbort();
  restored("double arm");
  /* the compose-list guard: DsListFits is exact at the limit, and a transition that does not fit is dropped before anything
   * is emitted (no copy, state idle, layer images restored) */
  g_mock_used = 40; g_mock_bound = (uint32_t)kListBytes - kComposeTail - 40;
  CHECK(DsListFits(), "fits exactly at the limit");
  g_mock_bound++;
  CHECK(!DsListFits(), "one byte over does not fit");
  g_mock_used = 0; g_mock_bound = 4432 + 20000;  /* a realistic bound (snapshots 4432 B + the rest of the composition) */
  CHECK(DsListFits(), "realistic bound fits");
  g_mock_used = (uint32_t)kListBytes; g_mock_bound = 0;
  PspRenderer_DoorSnapArm();
  g_copies = 0;
  compose(&good, true);
  CHECK(g_ds_state == kDsIdle && g_copies == 0 && g_img_ok, "armed transition dropped before it emits a command");
  g_mock_used = 0; g_mock_bound = 1000;
  PspRenderer_DoorSnapArm();
  compose(&good, true);
  PspRenderer_DoorSnapBegin(2, 0);
  g_mock_used = (uint32_t)kListBytes; g_copies = 0;  /* the list fills up while waiting for B */
  compose(&good, true);
  CHECK(g_ds_state == kDsIdle && g_copies == 0 && g_img_ok && !PspRenderer_DoorSnapHold(), "WaitB dropped when the list is full");
  g_mock_used = 0;
  /* software frame / resume release */
  PspRenderer_DoorSnapArm();
  compose(&good, true);
  DoorSnapRelease();
  restored("release");
  /* Begin without a snapshot does nothing */
  PspRenderer_DoorSnapBegin(1, 5);
  CHECK(g_ds_state == kDsIdle, "begin without snapshot");
  printf("%s\n", fails ? "FAILED" : "OK");
  return fails != 0;
}
