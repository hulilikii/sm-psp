/* Host test for Gate 5t (mode 7 row reuse). The real M7RenderRow / M7KeyOf / M7PrepareImg text is cut out of
 * psp_renderer.c by tools/test_m7_reuse.sh and compiled here against small stubs. Random frame sequences (matrix and flag
 * changes on single rows, HDMA-style per-row matrices, mosaic, mode 7 rows appearing and disappearing, VRAM writes inside and
 * outside the mode 7 pages) must always leave g_m7img equal to a fresh render of every drawn row. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SM_PSP_LOG
#define SM_PSP_LOG 1
#endif
#if SM_PSP_LOG
#define PROF(...) __VA_ARGS__
#else
#define PROF(...)
#endif
enum { kGameWidth = 256, kMaxLines = 240, kM7Pages = 64, kVramPages = 128, kVramPageWords = 256 };
typedef struct LineSnap { uint8_t mossize, mosstart, m7on, m7fl; int16_t m7m[8]; } LineSnap;
static LineSnap g_lines[kMaxLines];
static int g_line_count;
static uint16_t g_vram[0x8000];
static uint8_t g_m7img[256 * 256] __attribute__((aligned(64)));
static bool g_m7_img;
static int g_m7_nrng;
static uint16_t g_m7rng[kMaxLines][2];
static uint32_t g_m7i_us;
#ifndef SM_PSP_M7_REUSE
#define SM_PSP_M7_REUSE 1
#endif
typedef struct M7RowKey { uint8_t on, fl, ms, mt; int16_t m[8]; } M7RowKey;
static M7RowKey g_m7ikey[kMaxLines];
static bool g_m7i_stale = true;
static bool g_m7i_reuse_ok = SM_PSP_M7_REUSE;
#if SM_PSP_LOG
static uint32_t g_m7i_built, g_m7i_kept, g_m7i_audits, g_m7i_bad, g_m7i_auditn, g_m7i_stalef;
#endif
static uint32_t g_dcache_bytes;
static uint32_t Now(void) { static uint32_t t; return t += 3; }
static void sceKernelDcacheWritebackRange(const void *p, unsigned n) { (void)p; g_dcache_bytes += n; }
#define PspLog(...) (printf("  log: " __VA_ARGS__), printf("\n"))

#include "/tmp/m7_extract.inc"

static uint32_t rng = 0x12345679u;
static uint32_t R(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void RandMatrix(int16_t *m) {
  static const int16_t base[4] = {256, 0, 0, 256};
  switch (R() % 4) {
    case 0: memcpy(m, base, sizeof(base)); break;  /* identity */
    case 1: for (int i = 0; i < 4; i++) m[i] = (int16_t)(R() % 1024 - 512); break;  /* any */
    case 2: m[0] = m[3] = (int16_t)(R() % 700); m[1] = (int16_t)(R() % 300); m[2] = (int16_t)-m[1]; break;  /* rotation */
    default: m[0] = (int16_t)(R() % 2 ? 128 : 512); m[1] = m[2] = 0; m[3] = m[0]; break;
  }
  m[4] = (int16_t)R(); m[5] = (int16_t)R(); m[6] = (int16_t)R(); m[7] = (int16_t)R();  /* 13-bit sign extension is done by the renderer */
}
static void RandRow(LineSnap *s, bool on) {
  memset(s, 0, sizeof(*s));
  s->m7on = on;
  s->mossize = (uint8_t)(1 + R() % 16);  /* kept random also when the mosaic flag is off: must not matter */
  s->mosstart = (uint8_t)(R() % 224);
  if (!on) return;
  s->m7fl = (uint8_t)(R() % 16 == 0 ? 0 : 1);  /* mostly large field, sometimes small */
  if (R() % 5 == 0) s->m7fl |= 2;
  if (R() % 6 == 0) s->m7fl |= 4;
  if (R() % 6 == 0) s->m7fl |= 8;
  if (R() % 6 == 0) s->m7fl |= 32;
  RandMatrix(s->m7m);
}

/* emulates SnapshotVram: the write reaches g_vram, and marks the stale flag as the renderer does (page < kM7Pages) */
static void VramWrite(int word, uint16_t v, bool mark) {
  g_vram[word] = v;
  if (mark && word / kVramPageWords < kM7Pages) g_m7i_stale = true;
}

static int Frame(int f, bool skip_mark) {
  /* per frame: keep most rows, change some */
  int nrows = R() % 8 == 0 ? (int)(R() % kMaxLines + 1) : 224;
  if (nrows > kMaxLines) nrows = kMaxLines;
  static bool first = true;
  if (first || R() % 20 == 0) {  /* new whole-frame state */
    int16_t m[8]; RandMatrix(m);
    for (int r = 0; r < kMaxLines; r++) { RandRow(&g_lines[r], R() % 10 != 0); memcpy(g_lines[r].m7m, m, sizeof(m)); }
    first = false;
  }
  for (int k = 0; k < (int)(R() % 4); k++) {  /* a few rows change */
    int r = (int)(R() % kMaxLines);
    RandRow(&g_lines[r], R() % 6 != 0);
  }
  if (R() % 3 == 0) {  /* HDMA-like: a band of rows takes another matrix */
    int r0 = (int)(R() % 200), r1 = r0 + (int)(R() % 40);
    int16_t m[8]; RandMatrix(m);
    for (int r = r0; r < r1 && r < kMaxLines; r++) if (g_lines[r].m7on) memcpy(g_lines[r].m7m, m, sizeof(m));
  }
  g_line_count = nrows;
  int nw = (int)(R() % 4 == 0 ? R() % 40 : 0);  /* VRAM writes */
  for (int i = 0; i < nw; i++) {
    int w = (int)(R() % 0x8000);
    bool mark = !skip_mark || (w / kVramPageWords >= kM7Pages);
    VramWrite(w, (uint16_t)R(), mark);
  }
  M7PrepareImg();
  /* every drawn row equals a fresh render; ranges cover exactly the m7on rows */
  int bad = 0, want_rows = 0, got_rows = 0;
  for (int r = 0; r < g_line_count; r++) {
    if (!g_lines[r].m7on) continue;
    want_rows++;
    uint8_t fresh[256];
    M7RenderRow(&g_lines[r], r + 1, fresh);
    if (memcmp(fresh, g_m7img + r * 256, 256) != 0) { if (!bad) printf("  frame %d: row %d differs\n", f, r); bad++; }
  }
  for (int i = 0; i < g_m7_nrng; i++) {
    got_rows += g_m7rng[i][1] - g_m7rng[i][0];
    for (int r = g_m7rng[i][0]; r < g_m7rng[i][1]; r++) if (!g_lines[r].m7on) bad++;
  }
  if (got_rows != want_rows || g_m7_img != (want_rows > 0)) bad++;
  return bad;
}

int main(int argc, char **argv) {
  bool negative = argc > 1 && !strcmp(argv[1], "--negative");  /* a VRAM write that skips the stale flag must be caught */
  for (int i = 0; i < 0x8000; i++) g_vram[i] = (uint16_t)R();
  int bad = 0;
  for (int f = 0; f < 4000; f++) bad += Frame(f, negative);
#if SM_PSP_LOG
  printf("frames=4000 rows built=%u kept=%u audits=%u audit_bad=%u reuse_ok=%d writeback=%u KB\n", g_m7i_built, g_m7i_kept,
         g_m7i_audits, g_m7i_bad, (int)g_m7i_reuse_ok, g_dcache_bytes >> 10);
#endif
  if (negative) { printf(bad ? "negative control: stale reuse detected (expected)\n" : "negative control: NOT detected -> test is blind\n"); return bad ? 0 : 1; }
  printf(bad ? "FAIL (%d)\n" : "ok\n", bad);
  return bad != 0;
}
