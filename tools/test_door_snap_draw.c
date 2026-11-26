/* Host test for the GE quads of the door slide: DrawDoorSnap / DsImage are cut out of psp_renderer.c by
 * tools/test_door_snap_draw.sh and run against a recording stand-in for libgu. For every door direction, step and cross-door
 * alignment: the quads tile the 360x270 viewport exactly once, every texture coordinate stays inside the 256x192 image rows
 * (so the unwritten slot rows are never sampled), the old image alone is drawn before the new snapshot exists, and step 0 maps
 * the old image 1:1 like the normal composition.
 * Command list: the stand-in charges each libgu call what the PSPSDK sources (sdk/gu) emit: 4 B per GE command, i.e.
 * sceGuTexMode 12, sceGuTexImage 16, sceGuTexFlush 4, sceGuDrawArray 16, sceGuGetMemory(n) = n rounded up to 4 + 8. The
 * worst case over all frames must stay within kDsExactBytes / kDsMaxQuads / kDsMaxCalls (the constants psp_renderer.c bounds
 * the compose list with), only texture mode / image / vertex memory / sprite draws may be issued, and the texture state
 * left behind must be the composition source's (no state leaks into the HUD pass). */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "src/psp/psp_door_snap.h"

enum { kGameWidth = 256, kCompX = 60, kCompY = 1, kCompW = 360, kCompH = 270 };
enum { kDsIdle, kDsArmed, kDsHaveA, kDsWaitB, kDsPlay };
enum { GU_PSM_5650 = 0, GU_PSM_8888 = 3, GU_FALSE = 0 };
typedef struct TileVert { float u, v; int16_t x, y, z; } TileVert;
typedef struct ComposeSrc { const void *ptr; int psm, w, h, stride; } ComposeSrc;
static float g_uvh[2] = {0.5f, 0.5f};
static uint8_t g_ds_state, g_ds_dir, g_ds_n;
static int g_test_n;  /* 0: the default slide length of the direction */
enum { kDsTopRows = 32, kDsTopRow0 = 192 };
static bool g_ds_top[2];
static uint16_t g_ds_step;
static int16_t g_ds_align;
static uint8_t g_tex_mem[2][16];
static const uint8_t *g_img_tex[2] = {g_tex_mem[0], g_tex_mem[1]};

typedef struct Rect { int tex, x0, y0, x1, y1; float u0, v0, u1, v1; } Rect;
static Rect g_rects[64];
static int g_nrect, g_bound = -1;
static int g_bytes, g_calls;
static int g_last_psm = -1, g_last_w, g_last_h, g_last_stride;
static const void *g_last_tex;
static TileVert g_mem[128];
static int g_mem_n;
static void *sceGuGetMemory(int n) {
  void *p = &g_mem[g_mem_n]; g_mem_n += n / (int)sizeof(TileVert);
  g_bytes += ((n + 3) / 4) * 4 + 8; g_calls++;
  return p;
}
static void sceGuTexMode(int a, int b, int c, int d) { (void)b, (void)c, (void)d; g_last_psm = a; g_bytes += 12; g_calls++; }
static void sceGuTexImage(int l, int w, int h, int s, const void *p) {
  (void)l;
  g_bound = p == g_img_tex[0] ? 0 : p == g_img_tex[1] ? 1 : 2;
  g_last_w = w; g_last_h = h; g_last_stride = s; g_last_tex = p;
  g_bytes += 16; g_calls++;
}
static void sceGuTexFlush(void) { g_bytes += 4; g_calls++; }
static void GeDraw(int type, int n, const void *v) {
  (void)type, (void)n;
  g_bytes += 16; g_calls++;
  const TileVert *t = v;
  g_rects[g_nrect++] = (Rect){g_bound, t[0].x, t[0].y, t[1].x, t[1].y, t[0].u, t[0].v, t[1].u, t[1].v};
}

#define kVtTile 1
#include "/tmp/door_snap_compose_quad.inc"
#include "/tmp/door_snap_draw.inc"

static int fails, g_max_quads, g_max_bytes, g_max_calls, g_with_top, g_band_quads;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void run(unsigned state, unsigned dir, int step, int align, int *tex_seen) {
  static uint8_t cover[272][480];
  memset(cover, 0, sizeof cover);
  g_nrect = 0;
  g_mem_n = 0;
  g_ds_state = (uint8_t)state;
  g_ds_dir = (uint8_t)dir;
  g_ds_n = (uint8_t)(g_test_n ? g_test_n : DsSteps(dir));
  g_ds_step = (uint16_t)step;
  g_ds_align = (int16_t)align;
  g_ds_top[0] = false;  /* the old room never has them */
  g_ds_top[1] = g_with_top && dir == 2;
  static const int src_tag;
  const ComposeSrc s = {&src_tag, GU_PSM_8888, 512, 256, 512};  /* the composition source, as the HUD pass will need it again */
  g_bytes = g_calls = 0;
  DrawDoorSnap(&s);
  CHECK(g_nrect > 0 && g_nrect <= kDsMaxQuads, "rect count %d", g_nrect);
  CHECK(g_last_psm == s.psm && g_last_tex == s.ptr && g_last_w == s.w && g_last_h == s.h && g_last_stride == s.stride,
        "texture state not restored (psm %d w %d h %d stride %d)", g_last_psm, g_last_w, g_last_h, g_last_stride);
  if (g_nrect > g_max_quads) g_max_quads = g_nrect;
  if (g_bytes > g_max_bytes) g_max_bytes = g_bytes;
  if (g_calls > g_max_calls) g_max_calls = g_calls;
  tex_seen[0] = tex_seen[1] = 0;
  for (int i = 0; i < g_nrect; i++) {
    const Rect *r = &g_rects[i];
    if (r->v0 >= (float)kDsTopRow0) g_band_quads++;
    CHECK(r->tex == 0 || r->tex == 1, "texture %d dir %u step %d", r->tex, dir, step);
    if (r->tex == 0 || r->tex == 1) tex_seen[r->tex] = 1;
    CHECK(r->x0 >= kCompX && r->x1 <= kCompX + kCompW && r->y0 >= kCompY && r->y1 <= kCompY + kCompH && r->x1 > r->x0 && r->y1 > r->y0,
          "rect outside viewport (%d,%d)-(%d,%d)", r->x0, r->y0, r->x1, r->y1);
    for (int y = r->y0; y < r->y1; y++)
      for (int x = r->x0; x < r->x1; x++)
        if (y >= 0 && y < 272 && x >= 0 && x < 480) cover[y][x]++;
    /* the vertex coordinates sit on pixel corners; the GE samples at pixel centres (ComposeQuad's g_uvh offset) */
    const float eps = 0.01f, dw = (float)(r->x1 - r->x0), dh = (float)(r->y1 - r->y0);
    const float us[2] = {r->u0 + 0.5f * (r->u1 - r->u0) / dw, r->u1 - 0.5f * (r->u1 - r->u0) / dw};
    const float vs[2] = {r->v0 + 0.5f * (r->v1 - r->v0) / dh, r->v1 - 0.5f * (r->v1 - r->v0) / dh};
    for (int k = 0; k < 2; k++) {
      CHECK(us[k] >= 0.5f - eps && us[k] <= 255.5f + eps, "u %f out of the image", us[k]);
      const bool in_img = vs[k] >= 0.5f - eps && vs[k] <= 191.5f + eps;
      const bool in_band = g_ds_top[r->tex] && vs[k] >= (float)kDsTopRow0 + 0.5f - eps && vs[k] <= (float)(kDsTopRow0 + kDsTopRows) - 0.5f + eps;
      CHECK(in_img || in_band, "v %f outside the 192 rows (and the band %d)", vs[k], (int)g_ds_top[r->tex]);
    }
  }
  for (int y = kCompY; y < kCompY + kCompH; y++)
    for (int x = kCompX; x < kCompX + kCompW; x++)
      if (cover[y][x] != 1) { CHECK(0, "pixel (%d,%d) covered %d times, dir %u step %d align %d", x, y, cover[y][x], dir, step, align); return; }
}

int main(void) {
  long runs = 0;
  int seen[2];
  static const int aligns[] = {-256, -200, -127, -37, -1, 0, 1, 37, 128, 200, 255, 256};
  for (g_with_top = 0; g_with_top < 2; g_with_top++)
  for (unsigned dir = 0; dir < 4; dir++) {
    const int n = DsSteps(dir);
    for (unsigned a = 0; a < sizeof aligns / sizeof aligns[0]; a++) {
      run(kDsWaitB, dir, 0, aligns[a], seen);
      CHECK(seen[0] && !seen[1], "waiting for the new snapshot must draw the old one only (dir %u)", dir);
      for (int step = 0; step <= n; step++) {  /* step n is the destination: it is drawn too */
        run(kDsPlay, dir, step, aligns[a], seen);
        runs++;
        if (step == 0) {
          CHECK(seen[0] && !seen[1], "step 0 shows the old image only (dir %u)", dir);
          /* identity mapping: the first quad's corner is texel (0.5, 0.5) at the viewport's top-left */
          const Rect *r = &g_rects[0];
          const float uc = r->u0 + 0.5f * (r->u1 - r->u0) / (float)(r->x1 - r->x0), vc = r->v0 + 0.5f * (r->v1 - r->v0) / (float)(r->y1 - r->y0);
          CHECK(g_nrect == 1 && r->x0 == kCompX && r->y0 == kCompY && r->x1 == kCompX + kCompW && r->y1 == kCompY + kCompH &&
                fabsf(uc - 0.5f) < 0.01f && fabsf(vc - 0.5f) < 0.01f, "step 0 mapping dir %u (%d quads)", dir, g_nrect);
        }
        if (step == n)
          CHECK(seen[1], "the last step shows the new image (dir %u)", dir);
      }
    }
  }
  /* momentum-scaled slide lengths: the quads still tile the viewport exactly once at every step of every length */
  {
    static const int lens[] = {42, 48, 56, 64, 70, 80};
    for (g_with_top = 0; g_with_top < 2; g_with_top++)
      for (unsigned dir = 0; dir < 4; dir++)
        for (unsigned li = 0; li < sizeof lens / sizeof lens[0]; li++) {
          g_test_n = lens[li];
          for (int step = 0; step <= g_test_n; step++)
            for (int align = -256; align <= 256; align += 83) {
              run(kDsPlay, dir, step, align, seen);
              runs++;
            }
        }
    g_test_n = 0;
  }
  /* the gap between the leading image and the lower one is the lower room's rows under the HUD: 45 viewport rows of the band,
   * continuous with the image (the band's last row meets row 0), nothing replicated in between (at most the rounding row) */
  g_with_top = 1;
  /* up door: no band; the gap between the new image (above) and the old one is the new image's bottom row (v 191.5) repeated */
  for (int step = 1; step < DsSteps(3); step++) {
    run(kDsPlay, 3, step, 0, seen);
    int o[4];
    DsOffsets(3, step, 0, o);
    const int oldtop = kCompY + DsDispPx(o[1]), newbot = kCompY + DsDispPx(o[3]) + kCompH;
    int rep = 0;
    for (int i = 0; i < g_nrect; i++) {
      const Rect *r = &g_rects[i];
      CHECK(r->v1 < (float)kDsTopRow0 && r->v0 < (float)kDsTopRow0, "dir 3 step %d: a quad samples the band rows (v %f)", step, r->v0);
      if (r->tex == 1 && r->v0 > 191.0f && r->v1 > 191.0f && r->v0 == r->v1 && r->x0 == kCompX) rep += r->y1 - r->y0;
    }
    int want = oldtop - (newbot < kCompY ? kCompY : newbot);
    if (oldtop > kCompY + kCompH) want = kCompY + kCompH - (newbot < kCompY ? kCompY : newbot);
    if (want < 0) want = 0;
    CHECK(rep >= want - 2 && rep <= want + 2 + 0, "dir 3 step %d: %d repeated bottom-row rows in the gap (want %d)", step, rep, want);
  }
  for (unsigned dir = 2; dir < 3; dir++)
    for (int step = 1; step < DsSteps(dir); step++) {
      run(kDsPlay, dir, step, 0, seen);
      int band_rows = 0, rep_rows = 0, last_band_v = -1;
      for (int i = 0; i < g_nrect; i++) {
        const Rect *r = &g_rects[i];
        if (r->x0 != kCompX) continue;  /* one column of quads: align 0 has no x edges */
        if (r->v0 >= (float)kDsTopRow0 && r->v1 > r->v0) { band_rows += r->y1 - r->y0; last_band_v = (int)r->v1; }
        else if (r->v0 >= (float)kDsTopRow0) rep_rows += r->y1 - r->y0;
      }
      int o[4];
      DsOffsets(dir, step, 0, o);
      const int dy = kCompY + DsDispPx(dir == 2 ? o[3] : o[1]);  /* top of the lower image */
      int lo = dy - DsDispPx(kDsTopRows), hi = dy;
      if (lo < kCompY) lo = kCompY;
      if (hi > kCompY + kCompH) hi = kCompY + kCompH;
      const int want = hi > lo ? hi - lo : 0;
      CHECK(band_rows >= want - 2 && band_rows <= want + 2, "dir %u step %d: %d band rows in the gap (want %d)", dir, step, band_rows, want);
      if (hi == dy && want >= 4)  /* the band's last row is visible: it must be the slot's row 223 */
        CHECK(last_band_v >= 222 && last_band_v <= 223, "dir %u step %d: the band ends at v %d (want 223.5)", dir, step, last_band_v);
      CHECK(rep_rows <= 2, "dir %u step %d: %d replicated rows", dir, step, rep_rows);
    }
  g_with_top = 0;
  /* every alignment the offsets can take, every step, both states: command-list use only (the coverage above is the slow part) */
  for (g_with_top = 0; g_with_top < 2; g_with_top++)
  for (unsigned dir = 0; dir < 4; dir++)
    for (int align = -256; align <= 256; align++)
      for (int step = 0; step <= DsSteps(dir); step++)
        for (int st = kDsWaitB; st <= kDsPlay; st++) {
          g_ds_top[0] = false; g_ds_top[1] = g_with_top && dir == 2;
          g_ds_state = (uint8_t)st; g_ds_dir = (uint8_t)dir; g_ds_n = (uint8_t)DsSteps(dir); g_ds_step = (uint16_t)step; g_ds_align = (int16_t)align;
          g_nrect = 0; g_mem_n = 0; g_bytes = g_calls = 0;
          static const int tag;
          const ComposeSrc s = {&tag, GU_PSM_8888, 512, 256, 512};
          DrawDoorSnap(&s);
          CHECK(g_last_tex == s.ptr, "state not restored");
          if (g_nrect > g_max_quads) g_max_quads = g_nrect;
          if (g_bytes > g_max_bytes) g_max_bytes = g_bytes;
          if (g_calls > g_max_calls) g_max_calls = g_calls;
        }
  /* draws + texture calls only; the memory calls are the quads' own (counted in the bytes); the capture's 36 B / 2 calls are
   * not part of DrawDoorSnap (tools/test_door_snap_state.sh) */
  CHECK(g_max_bytes + 68 <= kDsExactBytes, "worst case %d B + capture exceeds kDsExactBytes %d", g_max_bytes, (int)kDsExactBytes);
  CHECK(g_max_quads <= kDsMaxQuads, "quads %d", g_max_quads);
  const int libgu_calls_no_mem = g_max_calls - g_max_quads;  /* every quad has one GetMemory call: not a command-emitting call */
  CHECK(libgu_calls_no_mem + 3 <= kDsMaxCalls, "calls %d + capture 3 exceed kDsMaxCalls %d", libgu_calls_no_mem, (int)kDsMaxCalls);
  CHECK(g_band_quads > 0, "the rows under the HUD are never drawn");
  printf("%s (%ld slide frames; worst case %d quads, %d B, %d calls + capture 68 B / 3 calls; %d band quads; constants: %d B, %d quads, %d calls)\n",
         fails ? "FAILED" : "OK", runs, g_max_quads, g_max_bytes, g_max_calls, g_band_quads, (int)kDsExactBytes, (int)kDsMaxQuads, (int)kDsMaxCalls);
  return fails != 0;
}
