/* Host test for Gate 5u. The tile addresses used by EnsureTileHash/EnsureTile7 under PSP_SWIZZLE are compared with the
 * PSPSDK sample swizzle (swizzle_fast: 16 byte x 8 row blocks) of the same linear atlas. Build and run: tools/test_psp_swizzle.sh */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PSPSDK samples/gu/common: width in BYTES, height in rows. */
static void swizzle_fast(uint8_t *out, const uint8_t *in, unsigned width, unsigned height) {
  unsigned width_blocks = width / 16, height_blocks = height / 8, src_pitch = (width - 16) / 4, src_row = width * 8;
  const uint8_t *ysrc = in;
  uint32_t *dst = (uint32_t *)out;
  for (unsigned by = 0; by < height_blocks; ++by) {
    const uint8_t *xsrc = ysrc;
    for (unsigned bx = 0; bx < width_blocks; ++bx) {
      const uint32_t *src = (const uint32_t *)xsrc;
      for (unsigned j = 0; j < 8; ++j) {
        *(dst++) = *(src++); *(dst++) = *(src++); *(dst++) = *(src++); *(dst++) = *(src++);
        src += src_pitch;
      }
      xsrc += 16;
    }
    ysrc += src_row;
  }
}

/* Keep in sync with psp_renderer.c (tools/test_psp_swizzle.sh greps the renderer for these exact expressions). */
#define ADDR_ATLAS4(pt)   ((((((pt) >> 5) << 3) + (((pt) & 31) >> 2)) << 7) + ((pt) & 3) * 4)
#define ADDR_ATLAS2(pt)   ((((((pt) >> 6) << 4) + (((pt) & 63) >> 2)) << 7) + ((pt) & 3) * 4)
#define ADDR_ATLAS7(t)    ((((((t) >> 4) << 3) + (((t) & 15) >> 1)) << 7) + ((t) & 1) * 8)
#define LIN_ATLAS4(pt)    ((pt >> 5) * 8 * 128 + (pt & 31) * 4)
#define LIN_ATLAS2(pt)    ((pt >> 6) * 8 * 256 + (pt & 63) * 4)
#define LIN_ATLAS7(t)     ((t >> 4) * 8 * 128 + (t & 15) * 8)

static int check(const char *name, int ntiles, int tilew, int linpitch, int rows, int (*lin)(int), int (*swz)(int)) {
  size_t bytes = (size_t)linpitch * rows;
  uint8_t *linear = calloc(bytes, 1), *ref = calloc(bytes, 1), *mine = calloc(bytes, 1);
  for (int t = 0; t < ntiles; t++)
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < tilew; x++)
        linear[lin(t) + y * linpitch + x] = (uint8_t)(1 + ((t * 131 + y * 17 + x * 5) % 250));  /* never 0: a missed byte shows */
  swizzle_fast(ref, linear, (unsigned)linpitch, (unsigned)rows);
  for (int t = 0; t < ntiles; t++)  /* what EnsureTile*() does: tilew bytes per row, rows 16 B apart */
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < tilew; x++)
        mine[swz(t) + y * 16 + x] = linear[lin(t) + y * linpitch + x];
  int bad = memcmp(ref, mine, bytes) != 0;
  /* all stores are 32-bit aligned and stay in the atlas */
  for (int t = 0; t < ntiles; t++)
    if ((swz(t) & 3) || (size_t)swz(t) + 7 * 16 + tilew > bytes) bad = 1;
  printf("%-28s %s\n", name, bad ? "FAIL" : "ok");
  free(linear); free(ref); free(mine);
  return bad;
}
static int l4(int t) { return LIN_ATLAS4(t); }
static int s4(int t) { return ADDR_ATLAS4(t); }
static int l2(int t) { return LIN_ATLAS2(t); }
static int s2(int t) { return ADDR_ATLAS2(t); }
static int l7(int t) { return LIN_ATLAS7(t); }
static int s7(int t) { return ADDR_ATLAS7(t); }
int main(void) {
  int bad = 0;
  bad |= check("atlas4 T4 256x512 (2048 t)", 2048, 4, 128, 512, l4, s4);
  bad |= check("atlas2 T4 512x512 (4096 t)", 4096, 4, 256, 512, l2, s2);
  bad |= check("atlas7 T8 128x128 (256 t)", 256, 8, 128, 128, l7, s7);
  return bad;
}
