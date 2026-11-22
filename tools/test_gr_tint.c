/* Host test for Gate 5x: the planner cut out of psp_renderer.c by tools/test_gr_tint.sh. Checks that a planned ramp stays
 * within kGrTol8 of every row's flat colour (model of the emitted vertex colours), that flat segments are bit exact, that
 * segments tile the runs without gaps, and the cases that must NOT become ramps. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { kMaxLines = 256, kMaxTintRanges = 6 };
typedef struct TintRun { uint16_t r0, r1, fix, v0; uint8_t half, cwm, cwl, cwr, vn; } TintRun;
typedef struct TintRange { uint16_t r0, r1, first, n; uint8_t mask, neg; } TintRange;
static TintRun g_tint[kMaxLines];
static TintRange g_trange[kMaxTintRanges];
static int g_ntint, g_ntr;
static uint32_t g_frame;
static uint32_t BakeColor(uint16_t c, int brightness, bool transparent) {
  int r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
  uint32_t r = (uint32_t)(((r5 << 3) | (r5 >> 2)) * brightness / 15), g = (uint32_t)(((g5 << 3) | (g5 >> 2)) * brightness / 15),
           b = (uint32_t)(((b5 << 3) | (b5 >> 2)) * brightness / 15);
  return (transparent ? 0u : 0xff000000u) | b << 16 | g << 8 | r;
}
#define PspLog(...) ((void)0)
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "/tmp/gr_data.inc"
#include "/tmp/gr_plan.inc"

static int fails, allow_gap;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void Begin(void) { g_ntint = 0; g_ntr = 0; memset(g_tint, 0, sizeof g_tint); }
static void Run(int r0, int r1, int fix) {
  g_tint[g_ntint++] = (TintRun){(uint16_t)r0, (uint16_t)r1, (uint16_t)fix, 0, 0, 0, 0, 0, 0};
}
static void OneRange(void) { g_ntr = 1; g_trange[0] = (TintRange){g_tint[0].r0, g_tint[g_ntint - 1].r1, 0, (uint16_t)g_ntint, 1, 0}; }
static int Seg(int s, int c, int row, int ch) {  /* modelled 8-bit value of segment s, row, channel (0 r, 1 g, 2 b) */
  const GrSeg *g = &g_grseg[s];
  int a = (g->c0 >> (8 * ch)) & 255, b = (g->c1 >> (8 * ch)) & 255;
  (void)c;
  return (int)(a + (b - a) * ((float)(row - g->r0) + 0.5f) / (float)(g->r1 - g->r0) + 0.5f);
}
static int Flat(int fix, int ch) { int l = (fix >> (5 * ch)) & 31; return (l << 3) | (l >> 2); }
static int RowFix(int row) { for (int i = 0; i < g_ntint; i++) if (row >= g_tint[i].r0 && row < g_tint[i].r1) return g_tint[i].fix; return -1; }

static void CheckPlan(const char *name, bool want_ramp) {
  bool got = GrPlan();
  CHECK(got == want_ramp, "%s: GrPlan=%d want %d", name, got, want_ramp);
  int next = g_tint[0].r0, ramps = 0;
  for (int s = 0; s < g_ngr; s++) {
    const GrSeg *g = &g_grseg[s];
    if (!allow_gap || s == 0) CHECK(g->r0 == next, "%s: seg %d starts at %d, expected %d (gap/overlap)", name, s, g->r0, next);
    next = g->r1;
    if (!g->ramp) {
      CHECK(g->c0 == g->c1, "%s: flat seg %d has two colours", name, s);
      CHECK(g->r1 - g->r0 >= 1, "%s: empty seg", name);
      for (int ch = 0; ch < 3; ch++) CHECK((int)((g->c0 >> (8 * ch)) & 255) == Flat(RowFix(g->r0), ch), "%s: flat seg %d not exact", name, s);
    } else {
      ramps++;
      CHECK(g->r1 - g->r0 >= kGrMinRows && g->r1 - g->r0 <= kGrMaxRows, "%s: ramp rows %d", name, g->r1 - g->r0);
      for (int y = g->r0; y < g->r1; y++)
        for (int ch = 0; ch < 3; ch++) {
          int d = Seg(s, 0, y, ch) - Flat(RowFix(y), ch);
          if (d < 0) d = -d;
          CHECK(d <= kGrTol8 + 1, "%s: seg %d row %d ch %d err %d", name, s, y, ch, d);  /* +1: the test rounds the model */
        }
    }
  }
  if (!allow_gap) CHECK(next == g_tint[g_ntint - 1].r1, "%s: segments end at %d, runs at %d", name, next, g_tint[g_ntint - 1].r1);
  CHECK((ramps > 0) == want_ramp, "%s: ramps=%d", name, ramps);
}
static int Fix(int r, int g, int b) { return r | g << 5 | b << 10; }

int main(void) {
  /* 1. one level per row, all channels rising from 4: a single ramp */
  Begin(); for (int i = 0; i < 24; i++) Run(10 + i, 11 + i, Fix(4 + i / 2, 6 + i / 3, 8 + i)); OneRange(); CheckPlan("dense", true);
  for (int s = 0; s < g_ngr; s++) printf("dense seg %d: rows %d..%d ramp=%d c0=%06x c1=%06x\n", s, g_grseg[s].r0, g_grseg[s].r1, g_grseg[s].ramp, g_grseg[s].c0 & 0xffffff, g_grseg[s].c1 & 0xffffff);
  /* 2. falling ramp with 3-row steps */
  Begin(); for (int i = 0; i < 10; i++) Run(20 + 3 * i, 23 + 3 * i, Fix(28 - i, 20, 15 - i / 2)); OneRange(); CheckPlan("falling", true);
  /* 3. plateaus only: flat, no ramp (exact path untouched) */
  Begin(); Run(0, 40, Fix(5, 5, 5)); Run(40, 90, Fix(6, 6, 6)); OneRange(); CheckPlan("plateaus", false);
  /* 4. 2-level steps every 6 rows: error would exceed the bound -> must not become a ramp over the whole stretch */
  Begin(); for (int i = 0; i < 8; i++) Run(6 * i, 6 * i + 6, Fix(2 * i + 1, 8, 8)); OneRange(); CheckPlan("coarse", false);  /* 2-level steps every 6 rows exceed the bound: stays flat */
  /* 5. non-monotone: peak must not be bridged */
  Begin(); int v[] = {4, 5, 6, 7, 8, 7, 6, 5, 4}; for (int i = 0; i < 9; i++) Run(2 * i, 2 * i + 2, Fix(v[i], 3, 3)); OneRange(); CheckPlan("peak", true);
  for (int s = 0; s < g_ngr; s++) if (g_grseg[s].ramp) CHECK(!(g_grseg[s].r0 < 8 && g_grseg[s].r1 > 10), "peak bridged by seg %d", s);
  /* 6. colour window change splits the ramp; half flag change splits the group */
  Begin(); for (int i = 0; i < 12; i++) Run(i, i + 1, Fix(3 + i, 3, 3)); g_tint[6].cwm = 1; g_tint[6].cwl = 4; g_tint[6].cwr = 9; OneRange(); CheckPlan("cw", true);
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].ramp && g_grseg[s].r0 <= 6 && g_grseg[s].r1 > 6 && (g_grseg[s].r0 < 6 || g_grseg[s].r1 > 7)), "ramp crosses window change");
  Begin(); for (int i = 0; i < 12; i++) Run(i, i + 1, Fix(3 + i, 3, 3)); for (int i = 6; i < 12; i++) g_tint[i].half = 1; OneRange(); CheckPlan("half", true);
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].r0 < 6 && g_grseg[s].r1 > 6), "ramp crosses half change");
  /* 7. rows that do not touch (gap) never join */
  Begin(); for (int i = 0; i < 5; i++) Run(i, i + 1, Fix(3 + i, 3, 3)); for (int i = 0; i < 5; i++) Run(20 + i, 21 + i, Fix(8 + i, 3, 3)); OneRange(); g_trange[0].r1 = 25; allow_gap = 1; CheckPlan("gap", true); allow_gap = 0;
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].r0 < 5 && g_grseg[s].r1 > 5), "ramp crosses a row gap");
  /* 8. start at level 0: no clamping allowed, must stay exact */
  Begin(); for (int i = 0; i < 8; i++) Run(i, i + 1, Fix(i, 0, 0)); OneRange(); (void)GrPlan();
  for (int s = 0; s < g_ngr; s++) if (g_grseg[s].ramp) { for (int y = g_grseg[s].r0; y < g_grseg[s].r1; y++) { int d = Seg(s, 0, y, 0) - Flat(RowFix(y), 0); CHECK(d <= kGrTol8 + 1 && d >= -kGrTol8 - 1, "level-0 ramp err %d", d); } }
  /* 9. two ranges: segments never cross a range border */
  Begin(); for (int i = 0; i < 16; i++) Run(i, i + 1, Fix(2 + i, 3, 3)); g_ntr = 2; g_trange[0] = (TintRange){0, 8, 0, 8, 1, 0}; g_trange[1] = (TintRange){8, 16, 8, 8, 1, 1};
  CHECK(GrPlan(), "two ranges"); CHECK(g_grr0[0] == 0 && g_grn[0] + g_grn[1] == g_ngr && g_grr0[1] == g_grn[0], "range index");
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].r0 < 8 && g_grseg[s].r1 > 8), "segment crosses a range border");
  /* 10. random stress: every ramp stays inside the tolerance and the tiling is gapless */
  uint32_t x = 12345;
  for (int it = 0; it < 20000; it++) {
    Begin(); int row = 0, n = 1 + (int)(x % 40); int r = 10, g = 10, b = 10;
    for (int i = 0; i < n && row < 240; i++) {
      x = x * 1664525u + 1013904223u; int len = 1 + (int)((x >> 8) % 10), dr = (int)((x >> 12) % 3) - 1, dg = (int)((x >> 14) % 3) - 1, db = (int)((x >> 16) % 3) - 1;
      r += dr; g += dg; b += db; if (r < 0) r = 0; if (r > 31) r = 31; if (g < 0) g = 0; if (g > 31) g = 31; if (b < 0) b = 0; if (b > 31) b = 31;
      Run(row, row + len, Fix(r, g, b)); row += len;
    }
    OneRange(); (void)GrPlan();
    int nx = g_tint[0].r0;
    for (int s = 0; s < g_ngr; s++) {
      const GrSeg *gs = &g_grseg[s]; if (gs->r0 != nx) { CHECK(0, "stress tiling it=%d", it); break; } nx = gs->r1;
      if (gs->ramp) for (int y = gs->r0; y < gs->r1; y++) for (int ch = 0; ch < 3; ch++) { int d = Seg(s, 0, y, ch) - Flat(RowFix(y), ch); if (d < 0) d = -d; if (d > kGrTol8 + 1) { CHECK(0, "stress err %d it=%d", d, it); break; } }
      else if (gs->c0 != gs->c1) CHECK(0, "stress flat");
    }
    if (nx != g_tint[g_ntint - 1].r1) CHECK(0, "stress end it=%d", it);
    if (fails > 10) break;
  }
  printf(fails ? "FAILED (%d)\n" : "all gr_tint tests passed\n", fails);
  return fails != 0;
}
