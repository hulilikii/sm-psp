/* Host test: ppu_drawLineScratch with bg3_ref_line. BG3's tilemap has the HUD in rows 0..3 (red) and FX (green, the water)
 * below. At the gameplay-line scroll, lines 1..32 would run into the HUD rows; pinned to line 33's row they show the FX row
 * instead. The register state is restored; the control (no pin) shows the HUD rows, i.e. the bug the pin fixes. */
#include <stdio.h>
#include <string.h>
#include "src/snes/ppu.h"
bool g_new_ppu = true;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void colour(const uint8_t *px, int x, int *r, int *g) { *r = px[x * 4 + 0]; *g = px[x * 4 + 1]; }

int main(void) {
  Ppu *p = ppu_init(NULL);
  ppu_reset(p);
  ppu_write(p, 0x00, 0x0f);  /* full brightness */
  ppu_write(p, 0x05, 0x01);  /* Mode 1 */
  ppu_write(p, 0x09, 0x10);  /* BG3SC: tilemap at word 0x1000 */
  ppu_write(p, 0x0c, 0x00);  /* BG3 chr at 0 */
  ppu_write(p, 0x2c, 0x04);  /* TM: BG3 only */
  ppu_write(p, 0x2d, 0x00);
  ppu_write(p, 0x21, 0x01);  /* CGRAM colour 1 = red, 2 = green */
  ppu_write(p, 0x22, 0x1f); ppu_write(p, 0x22, 0x00);
  ppu_write(p, 0x22, 0xe0); ppu_write(p, 0x22, 0x03);
  ppu_write(p, 0x15, 0x80);
  ppu_write(p, 0x16, 0x08); ppu_write(p, 0x17, 0x00);  /* tile 1: plane 0 -> colour 1 */
  for (int i = 0; i < 8; i++) { ppu_write(p, 0x18, 0xff); ppu_write(p, 0x19, 0x00); }
  ppu_write(p, 0x16, 0x10); ppu_write(p, 0x17, 0x00);  /* tile 2: plane 1 -> colour 2 */
  for (int i = 0; i < 8; i++) { ppu_write(p, 0x18, 0x00); ppu_write(p, 0x19, 0xff); }
  ppu_write(p, 0x16, 0x00); ppu_write(p, 0x17, 0x10);
  for (int row = 0; row < 32; row++)
    for (int col = 0; col < 32; col++) { ppu_write(p, 0x18, row < 4 ? 0x01 : 0x02); ppu_write(p, 0x19, 0x00); }

  const uint16_t v3 = p->bgLayer[2].vScroll;
  const uint8_t se0 = p->screenEnabled[0], se1 = p->screenEnabled[1];
  uint8_t ref[256 * 4], row[256 * 4];
  int r, g;

  /* line 33 as the game shows it: the FX (green) */
  CHECK(ppu_drawLineScratch(p, 33, ref, 0), "line 33");
  colour(ref, 10, &r, &g);
  CHECK(g > 0x80 && r < 0x40, "line 33 is the FX row (r %d g %d)", r, g);

  /* control: no pin, line 5 = the HUD tilemap rows (the bug) */
  CHECK(ppu_drawLineScratch(p, 5, row, 0), "line 5 unpinned");
  colour(row, 10, &r, &g);
  CHECK(r > 0x80 && g < 0x40, "control: unpinned line 5 shows the HUD rows (r %d g %d)", r, g);

  /* pinned: every band line is the FX row of line 33, none the HUD */
  for (int l = 1; l <= 32; l++) {
    CHECK(ppu_drawLineScratch(p, l, row, 33), "line %d pinned", l);
    CHECK(memcmp(row, ref, sizeof row) == 0, "pinned line %d differs from line 33's row", l);
  }
  CHECK(p->bgLayer[2].vScroll == v3 && p->screenEnabled[0] == se0 && p->screenEnabled[1] == se1, "state restored");

  /* a non-trivial gameplay scroll (FX scrolled by 5 rows + 3 px): still line 33's row on every band line */
  p->bgLayer[2].vScroll = (uint16_t)(v3 + 43);
  CHECK(ppu_drawLineScratch(p, 33, ref, 0), "scrolled line 33");
  for (int l = 1; l <= 32; l++) {
    CHECK(ppu_drawLineScratch(p, l, row, 33), "scrolled line %d", l);
    CHECK(memcmp(row, ref, sizeof row) == 0, "scrolled: pinned line %d differs from line 33's row", l);
  }
  p->bgLayer[2].vScroll = (uint16_t)(v3 + 43);
  CHECK(p->bgLayer[2].vScroll == (uint16_t)(v3 + 43), "scroll restored");

  puts(fails ? "FAILED" : "OK");
  return fails != 0;
}
