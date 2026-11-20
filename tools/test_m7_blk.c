/* Host test for Gate 5w. Cuts M7PlaneCell, M7PlaneUpdate, the M7Res functions and M7PrepareBlk (between the M7BLK markers) and M7RenderRow
 * out of psp_renderer.c (tools/test_m7_blk.sh) and checks
 *  1. the swizzled plane against the pixels M7RenderRow reads, after random VRAM page writes (and that a write that is not
 *     marked dirty is detected: negative control),
 *  2. the min/max residue tables against scanning the segment,
 *  3. the windows and texture coordinates: an emulated GE (nearest sampling at the pixel centre of the interpolated u, v, with
 *     a random error of up to 1/1024 texel added to every sample) reads the plane through the window of each block and must
 *     give exactly the row M7RenderRow renders. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { kGameWidth = 256, kMaxLines = 240, kM7Pages = 64, kVramPageWords = 256, kVramPages = 128 };
enum { kM7PlaneW = 1024, kM7BlkW = 64, kM7BlkH = 16, kM7WinMax = 128, kM7MaxBlk = 64, kM7PlFull = 32 };
typedef struct LineSnap { uint8_t mossize, mosstart, m7on, m7fl; int16_t m7m[8]; } LineSnap;
typedef struct M7Cfg { int16_t m[8]; uint8_t fl; } M7Cfg;
typedef struct M7BSeg { float u0, v0, u1, v1; int16_t x0, x1, y, pad; } M7BSeg;
typedef struct M7BBlk { int16_t wx, wy; uint16_t first, n; } M7BBlk;
static uint16_t g_vram[0x8000];
static LineSnap g_lines[kMaxLines];
static int g_line_count, g_m7_rows, g_m7_bnseg, g_m7_bnblk, g_m7_nrng;
static bool g_m7_same, g_m7_blk, g_m7_img, g_m7b_ok = true, g_m7pl_valid;
static M7Cfg g_m7c;
static float g_uvh[2] = {0.5f, 0.5f};
static uint8_t g_m7pl[kM7PlaneW * kM7PlaneW] __attribute__((aligned(64)));
static uint8_t g_m7pl_dirty[64];
static M7BSeg g_m7bseg[kMaxLines * 4];
static M7BBlk g_m7bblk[kM7MaxBlk];
#define BLK_WHY(r) ((void)0)
enum { kBlkOk, kBlkOff, kBlkSame, kBlkFlags, kBlkWindow, kBlkPlane, kBlkStraddle };
static void sceKernelDcacheWritebackRange(const void *p, unsigned n) { (void)p; (void)n; }
#define SM_PSP_M7_ROW4 1
#include "/tmp/m7row_extract.inc"
#include "/tmp/m7blk_extract.inc"

static uint32_t rng = 0x2545F491u;
static uint32_t R(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static inline int SwzAddr(int X, int Y) { return (((Y >> 3) * 64 + (X >> 4)) << 7) + (Y & 7) * 16 + (X & 15); }
static int RefPixel(int X, int Y) {
  unsigned tile = g_vram[(Y >> 3) * 128 + (X >> 3)] & 0xff;
  return g_vram[tile * 64 + (Y & 7) * 8 + (X & 7)] >> 8;
}
static long ComparePlane(void) {
  long bad = 0;
  for (int Y = 0; Y < 1024; Y++)
    for (int X = 0; X < 1024; X++)
      if (g_m7pl[SwzAddr(X, Y)] != RefPixel(X, Y)) bad++;
  return bad;
}

static int TestPlane(bool negative) {
  for (int i = 0; i < 0x8000; i++) g_vram[i] = (uint16_t)R();
  g_m7pl_valid = false;
  memset(g_m7pl_dirty, 0, sizeof g_m7pl_dirty);
  M7PlaneUpdate();
  long bad = ComparePlane();
  int rounds = 60;
  for (int k = 0; k < rounds; k++) {
    int kind = (int)(R() % 4);
    int nw = kind == 0 ? 3 : kind == 1 ? 400 : kind == 2 ? 6000 : 30;  /* few words, one page worth, many pages, a few */
    for (int i = 0; i < nw; i++) {
      int w = (int)(R() % 0x8000);
      if (kind == 3) w = (int)(R() % 0x4000);
      uint16_t v = (uint16_t)R();
      if (R() % 3 == 0) v = (uint16_t)((g_vram[w] & 0xff00) | (v & 0xff));  /* low byte only: a map word */
      else if (R() % 2 == 0) v = (uint16_t)((v & 0xff00) | (g_vram[w] & 0xff)); /* high byte only: a tile pixel */
      g_vram[w] = v;
      if (w / kVramPageWords < kM7Pages && !(negative && i == 0)) g_m7pl_dirty[w / kVramPageWords] = 1;
    }
    M7PlaneUpdate();
    bad += ComparePlane();
    if (negative && bad) break;
  }
  return bad != 0;
}

static int TestRes(void) {
  long bad = 0;
  for (int it = 0; it < 4000; it++) {
    int step = (int)(R() % 256);
    M7Res t;
    M7ResBuild(&t, step, kM7BlkW);
    for (int r0 = 0; r0 < 256; r0++) {
      int lo = 255, hi = 0, mlo, mhi;
      for (int k = 0; k < kM7BlkW; k++) { int r = (r0 + step * k) & 255; if (r < lo) lo = r; if (r > hi) hi = r; }
      M7ResGet(&t, r0, &mlo, &mhi);
      if (lo != mlo || hi != mhi) bad++;
    }
  }
  printf("  residue tables: %ld mismatches\n", bad);
  return bad != 0;
}

static int TestFrames(double eps, long *accepted, long *rejected, long *pixels) {
  long bad = 0;
  *accepted = *rejected = *pixels = 0;
  for (int i = 0; i < 0x8000; i++) g_vram[i] = (uint16_t)R();
  g_m7pl_valid = false;
  memset(g_m7pl_dirty, 1, sizeof g_m7pl_dirty);
  for (int f = 0; f < 3000; f++) {
    int16_t m[8];
    int kind = (int)(R() % 5);
    int ang = (int)(R() % 360);
    double sc = 0.5 + (R() % 200) / 100.0;  /* 0.5 .. 2.5 as zoom */
    if (kind == 0) { m[0] = m[3] = 256; m[1] = m[2] = 0; }
    else if (kind <= 3) { m[0] = (int16_t)lrint(256 * cos(ang * M_PI / 180) * sc); m[1] = (int16_t)lrint(256 * sin(ang * M_PI / 180) * sc);
                          m[2] = (int16_t)-m[1]; m[3] = m[0]; }
    else { for (int j = 0; j < 4; j++) m[j] = (int16_t)((int)(R() % 1201) - 600); }
    int xc = (int)(R() % 1024), yc = (int)(R() % 1024);
    m[4] = (int16_t)xc; m[5] = (int16_t)yc;
    int sx = (R() % 3 == 0) ? (int)(R() % 8192) : xc, sy = (R() % 3 == 0) ? (int)(R() % 8192) : yc;  /* often scroll = centre */
    m[6] = (int16_t)sx; m[7] = (int16_t)sy;
    unsigned fl = (R() % 2) ? 1u : 0u;
    if (R() % 3 == 0) fl |= 8;
    if (R() % 40 == 0) fl |= 2;  /* unsupported: must be rejected */
    g_line_count = (R() % 4 == 0) ? (int)(R() % 224 + 1) : 224;
    g_m7_rows = 0;
    for (int r = 0; r < kMaxLines; r++) {
      memset(&g_lines[r], 0, sizeof g_lines[r]);
      if (r < g_line_count && R() % 25 != 0) { g_lines[r].m7on = 1; g_lines[r].m7fl = (uint8_t)fl; memcpy(g_lines[r].m7m, m, sizeof m); g_m7_rows++; }
    }
    memcpy(g_m7c.m, m, sizeof m); g_m7c.fl = (uint8_t)fl;
    g_m7_same = true;
    if (R() % 30 == 0) g_m7_same = false;
    if (R() % 6 == 0) { int w = (int)(R() % 0x4000); g_vram[w] = (uint16_t)R(); g_m7pl_dirty[w / kVramPageWords] = 1; }
    bool ok = M7PrepareBlk();
    if (!ok) { (*rejected)++; if (g_m7_blk) bad++; continue; }
    if (fl & ~9u || !g_m7_same) { printf("  accepted an unsupported frame\n"); bad++; continue; }
    (*accepted)++;
    for (int b = 0; b < g_m7_bnblk; b++) {
      const M7BBlk *bk = &g_m7bblk[b];
      for (int i = bk->first; i < bk->first + bk->n; i++) {
        const M7BSeg *sg = &g_m7bseg[i];
        uint8_t ref[256];
        M7RenderRow(&g_lines[sg->y], sg->y + 1, ref);
        int n = sg->x1 - sg->x0;
        for (int x = sg->x0; x < sg->x1; x++) {
          double t = (x - sg->x0 + 0.5) / n;  /* GE: linear interpolation to the pixel centre */
          double u = sg->u0 + (sg->u1 - sg->u0) * t, v = sg->v0 + (sg->v1 - sg->v0) * t;
          u += eps * ((double)(int)(R() % 2001) - 1000) / 1000.0;  /* error of the GE, per coordinate */
          v += eps * ((double)(int)(R() % 2001) - 1000) / 1000.0;
          int tx = (int)floor(u), ty = (int)floor(v);
          if (tx < 0 || ty < 0 || tx >= kM7WinMax || ty >= kM7WinMax) { bad++; continue; }  /* outside the bound window */
          int X = bk->wx + tx, Y = bk->wy + ty;
          if (X >= 1024 || Y >= 1024) { bad++; continue; }
          (*pixels)++;
          if (g_m7pl[SwzAddr(X, Y)] != ref[x]) bad++;
        }
      }
    }
  }
  return bad != 0 ? (int)(bad > 1000000 ? 1000000 : bad) : 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--negative")) {
    int bad = TestPlane(true);
    printf(bad ? "negative control: unmarked VRAM write detected (expected)\n" : "negative control: NOT detected -> test is blind\n");
    return bad ? 0 : 1;
  }
  int bad = 0;
  bad |= TestPlane(false);
  printf("  plane maintenance (full, partial, many pages): %s\n", bad ? "FAIL" : "ok");
  bad |= TestRes();
  const double eps_list[] = {0.0, 1.0 / 2048, 1.0 / 1024};
  for (int i = 0; i < 3; i++) {
    long acc, rej, px;
    int b = TestFrames(eps_list[i], &acc, &rej, &px);
    printf("  emulated GE, error +-%.5f texel: %ld frames drawn from the plane (%ld pixels), %ld sent to the CPU image, %s\n",
           eps_list[i], acc, px, rej, b ? "MISMATCH" : "exact");
    bad |= b;
  }
  printf(bad ? "FAIL\n" : "ok\n");
  return bad != 0;
}
