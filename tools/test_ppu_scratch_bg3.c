#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "src/snes/ppu.h"
bool g_new_ppu = true;
/* Host test: ppu_drawLineScratch leaves BG3 (the HUD) out and restores the layer enables. Control: the normal line draw shows BG3. */
int main(void) {
  Ppu *p = ppu_init(NULL);
  ppu_reset(p);
  g_new_ppu = true;
  /* Mode 1, BG3 on main screen only (tm = 0x04), BG3 tilemap at word 0x1000 (reg 0x09 = 0x10), chr at 0 (reg 0x0c) */
  ppu_write(p, 0x00, 0x0f);            /* INIDISP: full brightness, no blank */
  ppu_write(p, 0x05, 0x01);            /* BGMODE 1 */
  ppu_write(p, 0x09, 0x10);            /* BG3SC */
  ppu_write(p, 0x0c, 0x00);            /* BG34NBA */
  ppu_write(p, 0x2c, 0x04);            /* TM: BG3 */
  ppu_write(p, 0x2d, 0x00);
  /* CGRAM: colour 1 of palette 0 = pure red */
  ppu_write(p, 0x21, 0x01); ppu_write(p, 0x22, 0x1f); ppu_write(p, 0x22, 0x00);
  /* VRAM: BG3 tile 1 (2bpp, 16 bytes) all plane0 set -> colour index 1; tilemap entry (0,0) = tile 1 */
  ppu_write(p, 0x15, 0x80);
  ppu_write(p, 0x16, 0x08); ppu_write(p, 0x17, 0x00);  /* word addr 8 = tile 1 (8 words/tile) */
  for (int i = 0; i < 8; i++) { ppu_write(p, 0x18, 0xff); ppu_write(p, 0x19, 0x00); }
  ppu_write(p, 0x16, 0x00); ppu_write(p, 0x17, 0x10);  /* tilemap word 0x1000 */
  for (int i = 0; i < 32 * 32; i++) { ppu_write(p, 0x18, 0x01); ppu_write(p, 0x19, 0x00); }
  uint8_t row[256 * 4], row2[256 * 4];
  const uint8_t se0 = p->screenEnabled[0], se1 = p->screenEnabled[1];
  const bool l2m = p->layer[2].mainScreenEnabled, l2s = p->layer[2].subScreenEnabled;
  memset(row, 0xAA, sizeof row);
  bool ok = ppu_drawLineScratch(p, 5, row);
  int nonzero = 0; for (int i = 0; i < 256 * 4; i++) if (row[i] != 0) nonzero++;
  printf("draw ok=%d, BG3 masked: non-black bytes=%d\n", ok, nonzero);
  int fails = !ok || nonzero != 0;
  fails |= p->screenEnabled[0] != se0 || p->screenEnabled[1] != se1 || p->layer[2].mainScreenEnabled != l2m || p->layer[2].subScreenEnabled != l2s;
  printf("state restored: %s\n", fails ? "NO" : "yes");
  /* control: the same line with BG3 enabled and unmasked must show red (draw via the normal path) */
  uint8_t *buf = calloc(256 * 4 * 224, 1);
  PpuBeginDrawing(p, buf, 256 * 4, 0);
  ppu_runLine(p, 5);
  int red = 0; for (int x = 0; x < 256; x++) if (buf[4 * 4 * 256 + x * 4] > 0x80) red++;
  printf("control (normal line draw, BG3 on): red pixels=%d\n", red);
  fails |= red == 0;
  puts(fails ? "FAILED" : "OK");
  return fails;
}
