#ifndef PPU_TILE_CACHE_H
#define PPU_TILE_CACHE_H

#include <stdbool.h>
#include <stdint.h>

enum {
  kPpuTileCacheSlots = 256,
  kPpuTilePixels = 64,
};

typedef struct PpuTileCacheEntry {
  uint16_t base;
  bool valid;
  uint8_t pixels[kPpuTilePixels];
} PpuTileCacheEntry;

typedef struct PpuTileCacheStats {
  uint32_t hits2bpp;
  uint32_t misses2bpp;
  uint32_t decodes2bpp;
  uint32_t hits4bpp;
  uint32_t misses4bpp;
  uint32_t decodes4bpp;
  uint32_t invalidatedEntries;
} PpuTileCacheStats;

typedef struct PpuTileCache {
  PpuTileCacheEntry entries2bpp[kPpuTileCacheSlots];
  PpuTileCacheEntry entries4bpp[kPpuTileCacheSlots];
  PpuTileCacheStats stats;
} PpuTileCache;

void PpuTileCacheInit(PpuTileCache *cache);
void PpuTileCacheInvalidateAll(PpuTileCache *cache);
void PpuTileCacheInvalidateVramWord(PpuTileCache *cache, uint16_t address);
void PpuTileCacheResetStats(PpuTileCache *cache);

const uint8_t *PpuTileCacheGet2bpp(
    PpuTileCache *cache, const uint16_t *vram, uint16_t base);

const uint8_t *PpuTileCacheGet4bpp(
    PpuTileCache *cache, const uint16_t *vram, uint16_t base);

const PpuTileCacheStats *PpuTileCacheGetStats(
    const PpuTileCache *cache);

#endif
