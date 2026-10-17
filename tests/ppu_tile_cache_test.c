
#include "../src/snes/ppu_tile_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_WORDS 0x8000

static uint16_t vram[VRAM_WORDS];

static void DecodeReference(
    const uint16_t *mem, uint16_t base, int bpp, uint8_t out[64]) {
  base &= 0x7fff;

  for (unsigned y = 0; y < 8; y++) {
    uint32_t bits = mem[(base + y) & 0x7fff];

    if (bpp == 4) {
      bits |= (uint32_t)mem[(base + y + 8) & 0x7fff] << 16;
    }

    for (unsigned x = 0; x < 8; x++) {
      uint8_t pixel = (bits >> x) & 1;
      pixel |= (bits >> (7 + x)) & 2;

      if (bpp == 4) {
        pixel |= (bits >> (14 + x)) & 4;
        pixel |= (bits >> (21 + x)) & 8;
      }

      out[y * 8 + x] = pixel;
    }
  }
}

static int CheckTile(
    PpuTileCache *cache, uint16_t base, int bpp) {
  uint8_t expected[64];

  DecodeReference(vram, base, bpp, expected);

  const uint8_t *actual = bpp == 2
      ? PpuTileCacheGet2bpp(cache, vram, base)
      : PpuTileCacheGet4bpp(cache, vram, base);

  if (memcmp(expected, actual, sizeof(expected)) != 0) {
    fprintf(stderr, "FAIL: %dbpp base=0x%04x\n", bpp, base);
    for (unsigned i = 0; i < 64; i++) {
      if (expected[i] != actual[i]) {
        fprintf(stderr,
            "  pixel %u: expected %u, got %u\n",
            i, expected[i], actual[i]);
        break;
      }
    }
    return 0;
  }

  return 1;
}

int main(void) {
  PpuTileCache cache;
  PpuTileCacheInit(&cache);

  /* Deterministic VRAM patterns. */
  uint32_t rng = 0x12345678;
  for (unsigned i = 0; i < VRAM_WORDS; i++) {
    rng = rng * 1664525u + 1013904223u;
    vram[i] = (uint16_t)(rng >> 16);
  }

  /* Exercise every possible VRAM base, including wrapping. */
  for (unsigned base = 0; base < VRAM_WORDS; base++) {
    if (!CheckTile(&cache, (uint16_t)base, 2) ||
        !CheckTile(&cache, (uint16_t)base, 4)) {
      return 1;
    }
  }

  /* Repeated lookup must hit the cache. */
  PpuTileCacheInit(&cache);
  if (!CheckTile(&cache, 0x1234, 4) ||
      !CheckTile(&cache, 0x1234, 4)) {
    return 1;
  }

  const PpuTileCacheStats *stats = PpuTileCacheGetStats(&cache);
  if (stats->misses4bpp != 1 || stats->hits4bpp != 1) {
    fprintf(stderr, "FAIL: repeated lookup was not a cache hit\n");
    return 1;
  }

  /* Modifying tile data must invalidate the cached pixels. */
  vram[0x1237] ^= 0xffff;
  PpuTileCacheInvalidateVramWord(&cache, 0x1237);

  if (!CheckTile(&cache, 0x1234, 4)) {
    return 1;
  }

  /* Verify invalidation across the end of VRAM. */
  PpuTileCacheInit(&cache);
  if (!CheckTile(&cache, 0x7ffc, 4)) {
    return 1;
  }

  vram[0x0002] ^= 0x55aa; /* Wrapped plane-0 word for base 0x7ffc. */
  PpuTileCacheInvalidateVramWord(&cache, 0x0002);

  if (!CheckTile(&cache, 0x7ffc, 4)) {
    return 1;
  }

  puts("PASS: tile cache correctness tests");
  return 0;
}

