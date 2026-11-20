/* Host test for Gate 5v: the one-pixel and the four-pixel loops of M7RenderRow (cut out of psp_renderer.c by
 * tools/test_m7_row4.sh and compiled twice) must give identical rows for random matrices, flags (large field, fill, flips,
 * mosaic), scroll/centre values and VRAM. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { kGameWidth = 256 };
typedef struct LineSnap { uint8_t mossize, mosstart, m7on, m7fl; int16_t m7m[8]; } LineSnap;
static uint16_t g_vram[0x8000];

#define SM_PSP_M7_ROW4 0
#define M7RenderRow M7RenderRowRef
#include "/tmp/m7row_extract.inc"
#undef M7RenderRow
#undef SM_PSP_M7_ROW4
#define SM_PSP_M7_ROW4 1
#define M7RenderRow M7RenderRowNew
#include "/tmp/m7row_extract.inc"
#undef M7RenderRow

static uint32_t rng = 0x9e3779b9u;
static uint32_t R(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int main(void) {
  long rows = 0, bad = 0, outside_rows = 0;
  for (int i = 0; i < 0x8000; i++) g_vram[i] = (uint16_t)R();
  for (long it = 0; it < 600000; it++) {
    LineSnap ls; memset(&ls, 0, sizeof ls);
    uint32_t k = R() % 5;
    for (int j = 0; j < 4; j++) ls.m7m[j] = (int16_t)(k == 0 ? ((int)(R() % 3) - 1) * 256 : k == 1 ? (int)(R() % 4097) - 2048 : (int)(R() % 1025) - 512);
    for (int j = 4; j < 8; j++) ls.m7m[j] = (int16_t)R();
    ls.m7fl = (uint8_t)(R() % 16 == 0 ? 0 : 1);
    if (R() % 3 == 0) ls.m7fl |= 2;
    if (R() % 3 == 0) ls.m7fl |= 4;
    if (R() % 3 == 0) ls.m7fl |= 8;
    if (R() % 5 == 0) ls.m7fl |= 32;
    ls.mossize = (uint8_t)(1 + R() % 16); ls.mosstart = (uint8_t)(R() % 224);
    int y = 1 + (int)(R() % 224);
    uint8_t a[256], b[256];
    memset(a, 0x55, 256); memset(b, 0xaa, 256);
    M7RenderRowRef(&ls, y, a);
    M7RenderRowNew(&ls, y, b);
    rows++;
    if (memcmp(a, b, 256)) { if (!bad) printf("mismatch: fl=%u y=%d\n", ls.m7fl, y); bad++; }
    for (int x = 0; x < 256; x++) if (a[x] == 0) { outside_rows++; break; }
  }
  printf("%ld rows, %ld mismatches (%ld rows contained index 0)\n", rows, bad, outside_rows);
  return bad != 0;
}
