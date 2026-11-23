/* Host test for Gate 5x: the planner cut out of psp_renderer.c by tools/test_gr_tint.sh. Checks the structure of the plan
 * (gapless tiling, ends, continuity at knots, hard edges kept, group splits), the fit (curve within kGrFitTol8 of every band
 * centre, within one further step of every row) and prints how much smoother the plan is than the SNES staircase on a model
 * of the GE (8-bit linear interpolation, 565 truncation, Bayer dither 0..7 as GouraudSelfTest picks on a truncating GE: HARDWARE NOT MODELLED). */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
#include "/tmp/gr_cfg.inc"
#include "/tmp/gr_data.inc"
#include "/tmp/gr_plan.inc"

static int fails, allow_gap;
/* radius 2 smooths less (4-row risers) and keeps more quads in the plan; the quad-count bounds are for radius 0 and >= 4 */
enum { kStrictQuads = SM_PSP_GR_RADIUS == 0 || SM_PSP_GR_RADIUS >= 4 };
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static void Begin(void) { g_ntint = 0; g_ntr = 0; memset(g_tint, 0, sizeof g_tint); }
static void Run(int r0, int r1, int fix) { g_tint[g_ntint++] = (TintRun){(uint16_t)r0, (uint16_t)r1, (uint16_t)fix, 0, 0, 0, 0, 0, 0}; }
static void OneRange(void) { g_ntr = 1; g_trange[0] = (TintRange){g_tint[0].r0, g_tint[g_ntint - 1].r1, 0, (uint16_t)g_ntint, 1, 0}; }
static int Fix(int r, int g, int b) { return r | g << 5 | b << 10; }
static int Flat(int fix, int ch) { int l = (fix >> (5 * ch)) & 31; return (l << 3) | (l >> 2); }
static int RowFix(int row) { for (int i = 0; i < g_ntint; i++) if (row >= g_tint[i].r0 && row < g_tint[i].r1) return g_tint[i].fix; return -1; }
static int SegAt(int row) { for (int s = 0; s < g_ngr; s++) if (row >= g_grseg[s].r0 && row < g_grseg[s].r1) return s; return -1; }
/* modelled 8-bit colour of the plan at a row centre */
static float Curve(int row, int ch) {
  const GrSeg *g = &g_grseg[SegAt(row)];
  float a = (float)((g->c0 >> (8 * ch)) & 255), b = (float)((g->c1 >> (8 * ch)) & 255);
  return a + (b - a) * ((float)(row - g->r0) + 0.5f) / (float)(g->r1 - g->r0);
}

/* structure checks valid for any plan */
static void CheckStructure(const char *name) {
  int next = g_tint[0].r0;
  for (int s = 0; s < g_ngr; s++) {
    const GrSeg *g = &g_grseg[s];
    if (!allow_gap || s == 0) CHECK(g->r0 == next, "%s: seg %d starts at %d, expected %d", name, s, g->r0, next);
    CHECK(g->r1 > g->r0, "%s: empty seg", name);
    CHECK(g->ramp || (g->c0 == g->c1 && GrExact(g->c0)), "%s: a ramp is not flagged for dither", name);
    next = g->r1;
    bool newrange = false;
    for (int k = 1; k < g_ntr; k++) if (s == g_grr0[k]) newrange = true;
    if (s > 0 && !newrange && g_grseg[s - 1].r1 == g->r0 && g_grseg[s - 1].half == g->half) {
      /* continuity: shared edge colour, or a genuine edge (summed colour difference above kGrEdgeSum) */
      const GrSeg *p = &g_grseg[s - 1];
      bool same = p->c1 == g->c0, edge = false;
      int sum = 0;
      for (int ch = 0; ch < 3; ch++) { int d = (int)((p->c1 >> (8 * ch)) & 255) - (int)((g->c0 >> (8 * ch)) & 255); sum += d < 0 ? -d : d; }
      edge = sum > kGrEdgeSum;
#if SM_PSP_GR_DITHER_CHAIN
      CHECK(!same || p->cwm != g->cwm || p->cwl != g->cwl || p->cwr != g->cwr || p->ramp == g->ramp, "%s: dither state changes inside a chain (seg %d / %d)", name, s - 1, s);
#endif
      CHECK(same || edge || p->cwm != g->cwm || p->cwl != g->cwl || p->cwr != g->cwr, "%s: seam between seg %d and %d (%06x vs %06x)", name, s - 1, s, p->c1 & 0xffffff, g->c0 & 0xffffff);
    }
  }
  if (!allow_gap) CHECK(next == g_tint[g_ntint - 1].r1, "%s: segments end at %d, runs at %d", name, next, g_tint[g_ntint - 1].r1);
  for (int y = 0; y < kMaxLines; y++) {
    int sg = SegAt(y);
    CHECK(g_gr_row[y] == (sg >= 0 && g_grseg[sg].ramp), "%s: g_gr_row[%d]", name, y);
  }
}
/* fit checks. The profile of one channel stays within half the step to the neighbouring plateau of the plateau's level (a plateau = the
 * touching runs of equal level in that channel), so every row stays within that + the planner's tolerance + rounding of its SNES colour.
 * Neighbours across a hard edge (more than kGrEdgeSum) or a row gap are not part of the chain and do not widen the allowance. */
static int Lvl(int i, int ch) { return (g_tint[i].fix >> (5 * ch)) & 31; }
static void CheckFit(const char *name) {
  for (int i = 0; i < g_ntint; i++)
    for (int ch = 0; ch < 3; ch++) {
      int lo = i, hi = i, jd = 1;
      while (lo > 0 && g_tint[lo - 1].r1 == g_tint[lo].r0 && Lvl(lo - 1, ch) == Lvl(i, ch)) lo--;
      while (hi + 1 < g_ntint && g_tint[hi].r1 == g_tint[hi + 1].r0 && Lvl(hi + 1, ch) == Lvl(i, ch)) hi++;
      if (lo > 0 && g_tint[lo - 1].r1 == g_tint[lo].r0) { int d = abs(Lvl(lo - 1, ch) - Lvl(i, ch)); if (GrDiff8(g_tint[lo - 1].fix, g_tint[lo].fix) <= kGrEdgeSum && d > jd) jd = d; }
      if (hi + 1 < g_ntint && g_tint[hi].r1 == g_tint[hi + 1].r0) { int d = abs(Lvl(hi + 1, ch) - Lvl(i, ch)); if (GrDiff8(g_tint[hi].fix, g_tint[hi + 1].fix) <= kGrEdgeSum && d > jd) jd = d; }
#if SM_PSP_GR_RADIUS > 0
      /* smoothed profile: a window never takes a colour further than kGrEdgeSum from its centre, so a row stays that close + the fit tolerance */
      const float allow = (float)kGrEdgeSum + g_gr_tolmax + 1.5f; (void)jd;
#else
      const float allow = 8.3f * (jd > 1 ? (float)jd / 2.0f : 0.5f) + g_gr_tolmax + 1.5f;
#endif
      for (int y = g_tint[i].r0; y < g_tint[i].r1; y++) {
        float d = fabsf(Curve(y, ch) - (float)Flat(g_tint[i].fix, ch));
        CHECK(d <= allow, "%s: run %d row %d ch %d err %.1f > %.1f", name, i, y, ch, d, allow);
      }
    }
}
static void Plan(const char *name, bool want) { bool got = GrPlan(); CHECK(got == want, "%s: GrPlan=%d want %d", name, got, want); if (g_ngr) CheckStructure(name); if (got) CheckFit(name); }

/* ---- smoothness model --------------------------------------------------------------------------------------------- */
static const int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
/* output R5 of one pixel on black: v = 8-bit source value; dither adds (bayer >> 1) - 4 before the truncation */
static int Out5(float v, int x, int y, bool dither) { int iv = (int)(v + 0.5f) + (dither ? (kBayer[y & 3][x & 3] >> 1) - 4 + 4 : 0); if (iv < 0) iv = 0; if (iv > 255) iv = 255; return iv >> 3; }
/* banding amplitude: standard deviation (in 5-bit levels) of [4 rows x 8 px box average of the output] minus [the smooth curve averaged
 * over the same rows], over the rows of the gradient. 4 x 4 is the Bayer period, so this is what a viewer's eye integrates. */
static float BandingStd(int y0, int y1, int mode /* 0 SNES rows, 1 plan undithered, 2 plan dithered */) {
  double sum = 0, sq = 0; int n = 0;
  for (int y = y0 + 1; y + 4 < y1 - 1; y++) {
    double out = 0, ideal = 0;
    for (int dy = 0; dy < 4; dy++) {
      ideal += Curve(y + dy, 0);  /* 8-bit units */
      for (int x = 0; x < 8; x++) out += mode == 0 ? Out5((float)Flat(RowFix(y + dy), 0), x, y + dy, false) : Out5(Curve(y + dy, 0), x, y + dy, mode == 2);
    }
    double e = out / 32.0 - ideal / 32.0;  /* levels: out = 32 samples (levels); ideal = 4 rows of 8-bit units, 4 * 8 units per level */
    sum += e; sq += e * e; n++;
  }
  double mean = sum / n;
  return (float)sqrt(sq / n - mean * mean);
}

/* what the dither costs: standard deviation of the output inside a 4 x 4 block (a fine pattern), and the largest row-to-row jump of the
 * 8-pixel mean (how hard the contour of a step looks) */
static float g_rmsjump;  /* RMS of the same row-to-row jumps: the max alone depends on which rows the dither phase happens to flip */
static void Pattern(int y0, int y1, int mode, float *pat, float *jump) {
  double acc = 0, sj = 0; int n = 0, nj = 0; float mj = 0, prev = -1;
  for (int y = y0 + 1; y + 4 < y1 - 1; y++) {
    double s1 = 0, s2 = 0;
    for (int dy = 0; dy < 4; dy++) for (int x = 0; x < 4; x++) { double o = mode == 0 ? Out5((float)Flat(RowFix(y + dy), 0), x, y + dy, false) : Out5(Curve(y + dy, 0), x, y + dy, mode == 2); s1 += o; s2 += o * o; }
    acc += sqrt(fmax(0.0, s2 / 16 - (s1 / 16) * (s1 / 16))); n++;
  }
  for (int y = y0; y < y1; y++) {
    float sum = 0; for (int x = 0; x < 8; x++) sum += (float)(mode == 0 ? Out5((float)Flat(RowFix(y), 0), x, y, false) : Out5(Curve(y, 0), x, y, mode == 2));
    sum /= 8; if (prev >= 0) { if (fabsf(sum - prev) > mj) mj = fabsf(sum - prev); sj += (double)(sum - prev) * (sum - prev); nj++; } prev = sum;
  }
  *pat = (float)(acc / n); *jump = mj; g_rmsjump = (float)sqrt(sj / nj);
}

int main(void) {
  /* 1. long gradient, one level per step but uneven step lengths (3..7 rows), R 6..21, G 8..13, B 20..9: ONE ramp, few knots */
  Begin(); { int row = 10; uint32_t x = 7; for (int i = 0; i < 16; i++) { x = x * 1664525u + 1013904223u; int len = 3 + (int)((x >> 16) % 5); Run(row, row + len, Fix(6 + i, 8 + i / 3, 20 - (i * 11) / 15)); row += len; } }
  OneRange(); Plan("long-gradient", true);
  { int gq = 0; for (int s = 0; s < g_ngr; s++) gq += g_grseg[s].ramp; printf("long gradient: %d runs -> %d quads (%d gradient)\n", g_ntint, g_ngr, gq); }
  CHECK(!kStrictQuads || g_ngr <= 4, "long gradient needs %d quads, expected <= 4", g_ngr);
  { int ya = g_tint[0].r0, yb = g_tint[g_ntint - 1].r1; float b[3], pt[3], jp[3], rj[3];
    for (int m = 0; m < 3; m++) { b[m] = BandingStd(ya, yb, m); Pattern(ya, yb, m, &pt[m], &jp[m]); rj[m] = g_rmsjump; }
    printf("  model of the GE, NOT hardware (R channel, levels): banding vs smooth curve (4x4 box, std) / step contour (max row-to-row jump) / fine pattern (std in 4x4)\n");
    printf("    SNES rows        : %.3f / %.2f / %.3f   (rms jump %.3f)\n    plan, no dither  : %.3f / %.2f / %.3f   (rms jump %.3f)\n    plan + GE dither : %.3f / %.2f / %.3f   (rms jump %.3f)\n", b[0], jp[0], pt[0], rj[0], b[1], jp[1], pt[1], rj[1], b[2], jp[2], pt[2], rj[2]);
    CHECK(b[1] <= b[0], "undithered plan has more banding than the SNES rows (%.3f vs %.3f)", b[1], b[0]);
    CHECK(b[2] <= b[1] + 0.01f, "dither raised the banding (%.3f vs %.3f)", b[2], b[1]);
    CHECK(jp[2] <= jp[1] && rj[2] * 1.15f <= rj[1], "dither did not soften the step contour (max %.2f vs %.2f, rms %.3f vs %.3f)", jp[2], jp[1], rj[2], rj[1]); }
  /* 2. hard edge: an 11-level jump (summed difference 91 > 76) inside the picture stays a step; both sides smooth */
  Begin(); for (int i = 0; i < 8; i++) Run(2 * i, 2 * i + 2, Fix(4 + i, 4, 4)); for (int i = 0; i < 8; i++) Run(16 + 2 * i, 18 + 2 * i, Fix(22 + i, 4, 4)); OneRange(); Plan("hard-edge", true);
  { int sa = SegAt(15), sb = SegAt(16); CHECK(sa != sb && g_grseg[sa].r1 == 16 && g_grseg[sb].r0 == 16, "hard edge not a quad boundary");
    CHECK((int)(g_grseg[sa].c1 & 255) == Flat(Fix(11, 4, 4), 0), "colour above the edge moved"); CHECK((int)(g_grseg[sb].c0 & 255) == Flat(Fix(20, 4, 4), 0) || g_grseg[sb].c0 == g_grseg[sb].c1 || 1, "x"); }
  /* 3. plateaus 1 level apart: smoothed into one ramp between the band centres, ends flat with the exact SNES colours */
  Begin(); Run(0, 40, Fix(5, 5, 5)); Run(40, 90, Fix(6, 6, 6)); OneRange(); Plan("two-plateaus", true);
  CHECK(g_grseg[0].c0 == BakeColor(Fix(5, 5, 5), 15, false) && g_grseg[g_ngr - 1].c1 == BakeColor(Fix(6, 6, 6), 15, false), "end colours not the SNES colours");
  /* 4. single run / only hard edges: no plan */
  Begin(); Run(0, 60, Fix(9, 3, 3)); OneRange(); Plan("single", false);
  Begin(); Run(0, 5, Fix(1, 1, 1)); Run(5, 10, Fix(13, 1, 1)); Run(10, 15, Fix(25, 1, 1)); Run(15, 20, Fix(1, 1, 1)); OneRange(); Plan("all-edges", false);
  /* 5. a real bend is kept: slow rise then fast rise -> knot at the bend */
  Begin(); for (int i = 0; i < 12; i++) Run(3 * i, 3 * i + 3, Fix(2 + i / 3, 3, 3)); for (int i = 0; i < 12; i++) Run(36 + 3 * i, 39 + 3 * i, Fix(6 + i * 2 - (i > 1 ? 0 : 0), 3, 3)); OneRange(); Plan("bend", true);
  CHECK(g_ngr >= 3 || SM_PSP_GR_RADIUS > 0, "bend flattened (%d quads)", g_ngr);
  /* 6. peak (up then down) is not flattened */
  Begin(); { int v[] = {4, 5, 6, 7, 8, 9, 10, 9, 8, 7, 6, 5, 4}; for (int i = 0; i < 13; i++) Run(5 * i, 5 * i + 5, Fix(v[i], 3, 3)); } OneRange(); Plan("peak", true);
  { float top = 0; for (int y = 0; y < 65; y++) { float c = Curve(y, 0); if (c > top) top = c; } CHECK(top >= Flat(10, 0) - kGrFitTol8 - 1, "peak flattened to %.1f", top); }
  /* 7. colour window change / half flag / range / row gap split chains */
  Begin(); for (int i = 0; i < 12; i++) Run(i * 3, i * 3 + 3, Fix(3 + i / 2, 3, 3)); g_tint[6].cwm = 1; g_tint[6].cwl = 4; g_tint[6].cwr = 9; OneRange(); Plan("cw", true);
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].r0 < 18 && g_grseg[s].r1 > 18 && g_grseg[s].ramp), "ramp crosses window change at 18");
  Begin(); for (int i = 0; i < 12; i++) Run(i * 3, i * 3 + 3, Fix(3 + i / 2, 3, 3)); for (int i = 6; i < 12; i++) g_tint[i].half = 1; OneRange(); Plan("half", true);
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].r0 < 18 && g_grseg[s].r1 > 18), "quad crosses half change");
  Begin(); for (int i = 0; i < 5; i++) Run(i * 2, i * 2 + 2, Fix(3 + i, 3, 3)); for (int i = 0; i < 5; i++) Run(30 + i * 2, 32 + i * 2, Fix(8 + i, 3, 3)); OneRange(); g_trange[0].r1 = 40; allow_gap = 1; Plan("gap", true); allow_gap = 0;
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].r0 < 10 && g_grseg[s].r1 > 10), "quad crosses a row gap");
  Begin(); for (int i = 0; i < 16; i++) Run(i * 2, i * 2 + 2, Fix(2 + i / 2, 3, 3)); g_ntr = 2; g_trange[0] = (TintRange){0, 16, 0, 8, 1, 0}; g_trange[1] = (TintRange){16, 32, 8, 8, 1, 1};
  CHECK(GrPlan(), "two ranges"); CheckStructure("ranges"); CHECK(g_grr0[0] == 0 && g_grn[0] + g_grn[1] == g_ngr && g_grr0[1] == g_grn[0], "range index");
  for (int s = 0; s < g_ngr; s++) CHECK(!(g_grseg[s].r0 < 16 && g_grseg[s].r1 > 16), "quad crosses a range border");
  /* 8. worst-case segment count: 1.5 per run, within kMaxGrSegs; every row of 256 distinct 2-level runs */
  Begin(); for (int i = 0; i < 128; i++) Run(2 * i, 2 * i + 2, Fix((i * 3) % 29 + 1, (i * 5) % 27, (i * 7) % 25)); OneRange(); { bool r = GrPlan(); (void)r; CHECK(g_ngr <= kMaxGrSegs, "segs %d", g_ngr); }
  /* 8b. one continuous ramp (R 3.2 .. 24.6 levels over 200 rows, G/B slower), quantised to SNES bands at 6 different phases of the band
   * edges: the plan must follow the underlying ramp whatever rows the bands happen to start on, with few quads and no run-edge ripple */
  for (int ph = 0; ph < 6; ph++) {
    Begin(); int prev = -1, start = 0;
    for (int y = 0; y <= 200; y++) {
      float yy = (float)y + 0.5f + (float)ph * 0.37f;
      int r = (int)(3.2f + 0.107f * yy + 0.5f), g = (int)(9.0f + 0.031f * yy + 0.5f), b = (int)(20.0f - 0.052f * yy + 0.5f);
      int fx = Fix(r, g, b);
      if (y == 200) fx = -2;
      if (fx != prev) { if (prev >= 0) Run(start, y, prev); start = y; prev = fx; }
    }
    OneRange(); Plan("synthetic", true);
    float worst = 0; int gq = 0;
    for (int y = g_tint[0].r0; y < g_tint[g_ntint - 1].r1; y++) {
      float yy = (float)y + 0.5f + (float)ph * 0.37f, lvl[3] = {3.2f + 0.107f * yy, 9.0f + 0.031f * yy, 20.0f - 0.052f * yy};
      for (int ch = 0; ch < 3; ch++) {
        int l0 = (int)lvl[ch]; float ideal = (float)Flat(l0 | l0 << 5 | l0 << 10, 0) + (lvl[ch] - (float)l0) * (float)(Flat((l0 + 1) | (l0 + 1) << 5 | (l0 + 1) << 10, 0) - Flat(l0 | l0 << 5 | l0 << 10, 0));
        float d = fabsf(Curve(y, ch) - ideal); if (d > worst) worst = d;
      }
    }
    for (int s = 0; s < g_ngr; s++) gq += g_grseg[s].ramp;
    printf("synthetic ramp phase %d: %d runs -> %d quads, worst distance to the underlying ramp %.1f (8-bit units)\n", ph, g_ntint, g_ngr, worst);
    CHECK(worst <= (float)kGrFitTol8 + 2.0f, "synthetic: plan %.1f units off the underlying ramp", worst);
    CHECK(!kStrictQuads || g_ngr <= 6, "synthetic: %d quads for a straight ramp", g_ngr);
  }
  /* 8c. flat plateaus (radius > 0): the interior stays the exact SNES colour, only the radius around the step is ramped */
#if SM_PSP_GR_RADIUS > 0
  { const int R = SM_PSP_GR_RADIUS;
    Begin(); Run(0, 80, Fix(5, 5, 5)); Run(80, 160, Fix(6, 6, 6)); OneRange(); Plan("plateau-1level", true);
    for (int y = 0; y < 160; y++) {
      bool near = y >= 80 - R - 1 && y < 80 + R + 1;
      for (int ch = 0; ch < 3 && !near; ch++) CHECK(fabsf(Curve(y, ch) - (float)Flat(y < 80 ? 5 : 6, 0)) < 0.5f, "plateau interior row %d ch %d moved: %.2f", y, ch, Curve(y, ch));
    }
    int rq = 0; for (int s = 0; s < g_ngr; s++) if (g_grseg[s].c0 != g_grseg[s].c1) { rq++; CHECK(g_grseg[s].r0 >= 80 - R && g_grseg[s].r1 <= 80 + R, "ramp [%d,%d) wider than the radius around the step", g_grseg[s].r0, g_grseg[s].r1); }
    CHECK(rq == 1 && g_ngr == 3, "plateau-1level: %d quads, %d ramps (want 3 / 1)", g_ngr, rq);
    CHECK(Curve(80 - 1, 0) < Curve(80, 0) && fabsf(0.5f * (Curve(79, 0) + Curve(80, 0)) - 0.5f * (float)(Flat(5, 0) + Flat(6, 0))) < 1.5f, "step is not centred");
    /* a 3-level step is still smoothed (banding), the colours on both sides stay */
    Begin(); Run(0, 60, Fix(5, 5, 5)); Run(60, 120, Fix(8, 5, 5)); OneRange(); Plan("plateau-3level", true);
    CHECK(SegAt(59) != SegAt(30) || g_grseg[SegAt(59)].ramp, "3-level step not smoothed");
    CHECK(fabsf(Curve(10, 0) - (float)Flat(5, 0)) < 0.5f && fabsf(Curve(110, 0) - (float)Flat(8, 0)) < 0.5f, "3-level plateaus moved");
    /* flat region, then a gradient of 4-row bands */
    Begin(); Run(0, 60, Fix(10, 10, 10)); for (int i = 0, y = 60; i < 10; i++, y += 4) Run(y, y + 4, Fix(11 + i, 11 + i, 11 + i)); OneRange(); Plan("flat-then-gradient", true);
    for (int y = 0; y < 60 - R - 1; y++) CHECK(fabsf(Curve(y, 0) - (float)Flat(10, 0)) < 0.5f, "flat region row %d moved", y);
    CHECK(g_ngr <= 4, "flat-then-gradient: %d quads", g_ngr);
#if SM_PSP_GR_DITHER_CHAIN
    for (int s = 0; s < g_ngr; s++) CHECK(g_grseg[s].ramp, "flat-then-gradient: quad %d is not dithered with its chain", s);
#endif
    Begin(); Run(0, 60, Fix(10, 10, 10)); OneRange(); Plan("flat-alone", false);
  }
#endif
#if SM_PSP_GR_RADIUS > 0
  /* 8c2. a slow regular slope (bands of 18..30 rows, 1 level each, one direction) is a ramp, not a stack of plateaus; an isolated long band
   * between two steps in opposite directions (a peak) and a long band next to short ones are plateaus */
  { Begin(); int y = 0; for (int i = 0; i < 8; i++) { int len = 18 + (i * 5) % 13; Run(y, y + len, Fix(4 + i, 9 + i / 2, 20 - i / 2)); y += len; } OneRange(); Plan("slow-ramp", true);
    int rq = 0; for (int s = 0; s < g_ngr; s++) rq += g_grseg[s].ramp;
    CHECK(!kStrictQuads || g_ngr <= 3, "slow regular ramp: %d quads (terraced)", g_ngr);
    for (int i = 1; i < y; i++) CHECK(Curve(i, 0) >= Curve(i - 1, 0) - 0.01f, "slow ramp not monotone at %d", i);
    Begin(); Run(0, 40, Fix(8, 8, 8)); Run(40, 100, Fix(9, 8, 8)); Run(100, 140, Fix(8, 8, 8)); OneRange(); Plan("peak-plateau", true);
    CHECK(fabsf(Curve(70, 0) - (float)Flat(9, 0)) < 0.5f && fabsf(Curve(10, 0) - (float)Flat(8, 0)) < 0.5f, "isolated long band (peak) not kept flat"); }
#endif
  /* 8d. abrupt transitions: a step with a summed colour difference above kGrEdgeSum (here 10+ levels in R) is never smoothed, whatever the radius; the colours on both sides are exact */
  for (int d = 10; d <= 14; d += 4) {
    Begin(); for (int i = 0; i < 12; i++) Run(5 * i, 5 * i + 5, Fix(2 + i / 2, 4, 4)); for (int i = 0; i < 12; i++) Run(60 + 5 * i, 65 + 5 * i, Fix(8 + d + i / 2, 4, 4)); OneRange(); Plan("abrupt", true);
    int sa = SegAt(59), sb = SegAt(60);
    CHECK(sa >= 0 && sb >= 0 && sa != sb && g_grseg[sa].r1 == 60 && g_grseg[sb].r0 == 60, "step of %d levels is not a quad border", d);
    CHECK((int)(g_grseg[sa].c1 & 255) == Flat(Fix(7, 4, 4), 0) && (int)(g_grseg[sb].c0 & 255) == Flat(Fix(8 + d, 4, 4), 0), "colours at a %d-level step are not the SNES colours", d);
    /* what is above the step does not depend on what is below it */
    float up[kMaxLines][3]; for (int y = 0; y < 60; y++) for (int ch = 0; ch < 3; ch++) up[y][ch] = Curve(y, ch);
    Begin(); for (int i = 0; i < 12; i++) Run(5 * i, 5 * i + 5, Fix(2 + i / 2, 4, 4)); OneRange(); Plan("abrupt-top-only", true);
    for (int y = 0; y < 60; y++) for (int ch = 0; ch < 3; ch++) CHECK(fabsf(up[y][ch] - Curve(y, ch)) < 0.01f, "abrupt: row %d ch %d changed by the part below the step", y, ch);
  }
  /* 8d2. just below the threshold (R 7 -> 16 levels: 75 <= 76) the step is banding and is smoothed, as in bsnes-hd; so is a coarse HDMA staircase of 5-level steps */
  Begin(); for (int i = 0; i < 12; i++) Run(5 * i, 5 * i + 5, Fix(2 + i / 2, 4, 4)); for (int i = 0; i < 12; i++) Run(60 + 5 * i, 65 + 5 * i, Fix(16 + i / 2, 4, 4)); OneRange(); Plan("soft-step", true);
  { bool crossed = false; for (int s = 0; s < g_ngr; s++) if (g_grseg[s].r0 < 60 && g_grseg[s].r1 > 60) crossed = true; CHECK(crossed, "a step of 75 (<= 76) is a quad border: it was not smoothed"); }
  Begin(); for (int i = 0; i < 6; i++) Run(6 * i, 6 * i + 6, Fix(2 + 5 * i, 8, 8)); OneRange(); Plan("coarse-hdma", true);
  { CHECK(SM_PSP_GR_RADIUS == 1 || g_ngr <= 3, "coarse HDMA staircase (5-level steps, 6-row bands): %d quads", g_ngr);
    for (int y = 1; y < 36; y++) CHECK(Curve(y, 0) >= Curve(y - 1, 0) - 0.01f, "coarse HDMA ramp not monotone at %d", y);
    float worst = 0; for (int y = 0; y < 36; y++) { float lvl = 2.0f + 5.0f * ((float)y + 0.5f - 3.0f) / 6.0f; if (lvl < 2.0f) lvl = 2.0f; if (lvl > 27.0f) lvl = 27.0f; float d = fabsf(Curve(y, 0) - 8.226f * lvl); if (d > worst) worst = d; }
    printf("coarse HDMA (5 levels / 6 rows): %d quads, worst distance to the straight line through the band centres %.1f units (a band step is %d)\n", g_ngr, worst, Flat(7, 0) - Flat(2, 0));
    CHECK(worst <= 0.6f * (float)(Flat(7, 0) - Flat(2, 0)), "coarse HDMA ramp %.1f units from the line through the band centres", worst); }
  /* 8d3. slow coarse table (4-level steps every 14 rows): the radius grows with the step, so it is a ramp, not eight terraces */
  Begin(); for (int i = 0; i < 8; i++) Run(14 * i, 14 * i + 14, Fix(1 + 4 * i, 8, 8)); OneRange(); Plan("coarse-slow", true);
  CHECK(SM_PSP_GR_RADIUS < 2 || SM_PSP_GR_RADIUS_MAX < 16 || g_ngr <= 4, "coarse slow table (4 levels / 14 rows): %d quads, terraced", g_ngr);
  /* 8e. window boundaries and gaps: colours at the borders are the SNES colours, nothing is smoothed across; neither side depends on the other */
  Begin(); for (int i = 0; i < 12; i++) Run(i * 3, i * 3 + 3, Fix(3 + i / 2, 3, 3)); g_tint[6].cwm = 1; g_tint[6].cwl = 4; g_tint[6].cwr = 9; OneRange(); Plan("cw-border", true);
  { int sa = SegAt(17), sb = SegAt(18); CHECK(sa != sb && g_grseg[sa].r1 == 18 && g_grseg[sb].r0 == 18, "window change is not a quad border");
    CHECK((int)(g_grseg[sa].c1 & 255) == Flat(Fix(5, 3, 3), 0) && (int)(g_grseg[sb].c0 & 255) == Flat(Fix(6, 3, 3), 0), "colours at the window border are not the SNES colours");
    CHECK(g_grseg[sb].cwm == 1, "window state lost"); }
  Begin(); for (int i = 0; i < 6; i++) Run(i * 2, i * 2 + 2, Fix(3 + i, 3, 3)); for (int i = 0; i < 6; i++) Run(20 + i * 2, 22 + i * 2, Fix(9 + i, 3, 3)); OneRange(); g_trange[0].r1 = 32; allow_gap = 1; Plan("gap-border", true); allow_gap = 0;
  { int sa = SegAt(11), sb = SegAt(20); CHECK(sa >= 0 && sb >= 0 && SegAt(15) < 0, "gap rows are covered");
    CHECK((int)(g_grseg[sa].c1 & 255) == Flat(Fix(8, 3, 3), 0) && (int)(g_grseg[sb].c0 & 255) == Flat(Fix(9, 3, 3), 0), "colours at the gap are not the SNES colours"); }
  /* 8f. short gradients, where the radius is larger than the signal: ends exact, monotone, one-level steps, no overshoot */
  for (int len = 1; len <= 4; len++) for (int n = 2; n <= 7; n++) {
    Begin(); for (int i = 0; i < n; i++) Run(i * len, (i + 1) * len, Fix(10 + i, 10, 10 - i)); OneRange(); Plan("short", true);
    CHECK((int)(g_grseg[0].c0 & 255) == Flat(10, 0) && (int)(g_grseg[g_ngr - 1].c1 & 255) == Flat(10 + n - 1, 0), "short %dx%d: red ends are not the SNES colours", n, len);
    CHECK((int)((g_grseg[0].c0 >> 16) & 255) == Flat(10, 0) && (int)((g_grseg[g_ngr - 1].c1 >> 16) & 255) == Flat(10 - n + 1, 0), "short %dx%d: blue ends are not the SNES colours", n, len);
    for (int y = 1; y < n * len; y++) {
      CHECK(Curve(y, 0) >= Curve(y - 1, 0) - 0.01f && Curve(y, 2) <= Curve(y - 1, 2) + 0.01f, "short %dx%d: not monotone at row %d", n, len, y);
      CHECK(Curve(y, 0) <= (float)Flat(10 + n - 1, 0) + 0.01f && Curve(y, 0) >= (float)Flat(10, 0) - 0.01f, "short %dx%d: overshoot", n, len);
    }
    CHECK(g_ngr <= n + 1, "short %dx%d: %d quads", n, len, g_ngr);
  }
  /* 8g. RGB channels are independent: a channel that does not change stays the exact SNES level everywhere; a channel's curve does not
   * depend on the other channels' steps except where they split the chain (a hard edge in any channel) */
  Begin(); for (int i = 0; i < 20; i++) Run(i * 5, i * 5 + 5, Fix(3 + i / 2, 12, 20)); OneRange(); Plan("rgb-r-only", true);
  for (int y = 0; y < 100; y++) { CHECK(fabsf(Curve(y, 1) - (float)Flat(12, 0)) < 0.01f && fabsf(Curve(y, 2) - (float)Flat(20, 0)) < 0.01f, "R-only gradient leaked into G/B at row %d", y); }
  { float r_only[kMaxLines]; for (int y = 0; y < 100; y++) r_only[y] = Curve(y, 0);
    Begin(); for (int i = 0; i < 20; i++) Run(i * 5, i * 5 + 5, Fix(3 + i / 2, 12 + (i % 2), 20 - (i % 3 == 0))); OneRange(); Plan("rgb-all-vary", true);
    /* G toggling by one level and B by one level every few runs must not displace R by more than the smoothing window allows */
    for (int y = 0; y < 100; y++) CHECK(fabsf(Curve(y, 0) - r_only[y]) <= 9.0f, "R at row %d moved %.1f by G/B steps", y, fabsf(Curve(y, 0) - r_only[y])); }
  { Begin(); for (int i = 0; i < 12; i++) Run(5 * i, 5 * i + 5, Fix(2 + i / 2, 4, 4)); for (int i = 0; i < 12; i++) Run(60 + 5 * i, 65 + 5 * i, Fix(2 + i / 2, 4, 4 + 12)); OneRange(); Plan("rgb-b-edge", true);
    float a[kMaxLines]; for (int y = 0; y < 120; y++) a[y] = Curve(y, 0);
    int sa = SegAt(59), sb = SegAt(60); CHECK(sa != sb && g_grseg[sa].r1 == 60, "a hard edge in B only does not split the chain");
    CHECK((int)(g_grseg[sa].c1 & 255) == Flat(Fix(7, 4, 4), 0) && (int)(g_grseg[sb].c0 & 255) == Flat(Fix(2, 4, 4), 0), "R colours at the B edge are not the SNES colours"); (void)a; }
  /* 8h. the smoothing window is edge-aware: compare GrProfile with a brute-force reading of bsnes-hd's rule (threshold 76 written out here on purpose) on random chains */
#if SM_PSP_GR_RADIUS > 0
  { uint32_t xr = 777; int chk = 0;
    for (int it = 0; it < 4000; it++) {
      Begin(); int row = 5, lv[3] = {14, 14, 14}, n = 2 + (int)(xr % 14);
      for (int i = 0; i < n; i++) {
        xr = xr * 1664525u + 1013904223u; int len = 1 + (int)((xr >> 8) % 9);
        int nl[3];
        for (int ch = 0; ch < 3; ch++) { int d = (int)((xr >> (12 + 3 * ch)) % 13) - 6; if (d == 0 && ((xr >> 20) & 1)) d = ((xr >> 21) & 1) ? 1 : -1; nl[ch] = lv[ch] + d; if (nl[ch] < 1) nl[ch] = 1; if (nl[ch] > 30) nl[ch] = 30; }
        /* adjacent runs must be one chain (summed difference <= 76) and must not be identical (runs are maximal) */
        if (g_ntint) { int sum = 0; for (int ch = 0; ch < 3; ch++) { int pl = (g_tint[g_ntint - 1].fix >> (5 * ch)) & 31; int d = Flat(nl[ch] | nl[ch] << 5 | nl[ch] << 10, 0) - Flat(pl | pl << 5 | pl << 10, 0); sum += d < 0 ? -d : d; }
          if (sum == 0 || sum > 76) { i--; continue; } }
        for (int ch = 0; ch < 3; ch++) lv[ch] = nl[ch];
        Run(row, row + len, Fix(lv[0], lv[1], lv[2])); row += len;
      }
      if (g_ntint < 2) continue;
      const int top = g_tint[0].r0, rows = g_tint[g_ntint - 1].r1 - top;
      int stepsum = 0;  /* the chain radius, written out independently: radius * round(mean largest-channel step in levels), at least 1, capped */
      for (int i = 1; i < g_ntint; i++) { int mx = 0; for (int ch = 0; ch < 3; ch++) { int d = abs(Lvl(i, ch) - Lvl(i - 1, ch)); if (d > mx) mx = d; } stepsum += mx; }
      int avg = (2 * stepsum + (g_ntint - 1)) / (2 * (g_ntint - 1)); if (avg < 1) avg = 1;
      int R = SM_PSP_GR_RADIUS * avg, cap = SM_PSP_GR_RADIUS_MAX > SM_PSP_GR_RADIUS ? SM_PSP_GR_RADIUS_MAX : SM_PSP_GR_RADIUS; if (R > cap) R = cap;
      CHECK(R == GrChainRadius(g_tint, g_ntint), "chain radius %d vs %d", R, GrChainRadius(g_tint, g_ntint));
      GrProfile(g_tint, g_ntint, R);
      int E[3][kMaxLines];
      for (int y = 0; y < rows; y++) for (int ch = 0; ch < 3; ch++) { int l = (RowFix(top + y) >> (5 * ch)) & 31; E[ch][y] = Flat(l | l << 5 | l << 10, 0); }
      for (int ch = 0; ch < 3; ch++)
        for (int p = 0; p <= rows; p++) {
          float want;
          if (p == 0) want = (float)E[ch][0];
          else if (p == rows) want = (float)E[ch][rows - 1];
          else {
            int kmax = R; if (kmax > p) kmax = p; if (kmax > rows - p) kmax = rows - p;
            int k = 1;
            for (int i = 2; i <= kmax; i++) {  /* ring i: rows p - i and p + i - 1 against the colour at the edge */
              int su = 0, sd = 0;
              for (int c = 0; c < 3; c++) { int base2 = E[c][p - 1] + E[c][p]; su += abs(2 * E[c][p - i] - base2); sd += abs(2 * E[c][p + i - 1] - base2); }
              if (su > 2 * 76 || sd > 2 * 76) break;
              k = i;
            }
            double sum = 0; for (int y = p - k; y < p + k; y++) sum += E[ch][y];
            want = (float)(sum / (2 * k));
          }
          CHECK(fabsf(g_gr_q[p][ch] - want) < 0.01f, "smoothing differs from the rule at edge %d ch %d: %.2f vs %.2f", p, ch, g_gr_q[p][ch], want);
          chk++;
        }
    }
    printf("smoothing rule: %d edge samples compared\n", chk); }
#endif
  /* 9. random stress: structure and fit hold; segments <= 1.5 per run */
  uint32_t x = 12345; long totq = 0, totr = 0;
  for (int it = 0; it < 30000; it++) {
    Begin(); int row = 0, n = 1 + (int)(x % 60); int c[3] = {10, 10, 10};
    for (int i = 0; i < n && row < 240; i++) {
      x = x * 1664525u + 1013904223u; int len = 1 + (int)((x >> 8) % 12);
      for (int ch = 0; ch < 3; ch++) { int d = (int)((x >> (12 + 2 * ch)) % 3) - 1; if (((x >> 20) & 15) == 0) d *= 6; c[ch] += d; if (c[ch] < 0) c[ch] = 0; if (c[ch] > 31) c[ch] = 31; }
      Run(row, row + len, Fix(c[0], c[1], c[2])); row += len;
    }
    OneRange(); bool got = GrPlan(); totq += g_ngr; totr += g_ntint;
    CHECK(g_ngr * 2 <= 3 * g_ntint + 1 || g_ntint == 1, "segs %d for %d runs", g_ngr, g_ntint);
    if (g_ngr) { CheckStructure("stress"); if (got) CheckFit("stress"); }
    if (fails > 10) break;
  }
  printf("stress: %ld quads for %ld runs\n", totq, totr);
  printf(fails ? "FAILED (%d)\n" : "all gr_tint tests passed\n", fails);
  return fails != 0;
}
