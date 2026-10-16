#include "ppu_tile_cache.h"

#include <string.h>

enum {
  kVramWordMask = 0x7fff,
  kTile2bppWords = 8,
  kTile4bppWords = 16,
};

static unsigned PpuTileCacheSlot(uint16_t base) {
  return (base ^ (base >> 8)) & (kPpuTileCacheSlots - 1);
}

void PpuTileCacheInit(PpuTileCache *cache) {
  memset(cache, 0, sizeof(*cache));
}

void PpuTileCacheInvalidateAll(PpuTileCache *cache) {
  for (unsigned i = 0; i < kPpuTileCacheSlots; i++) {
    cache->entries2bpp[i].valid = false;
    cache->entries4bpp[i].valid = false;
  }
}

void PpuTileCacheInvalidateVramWord(
    PpuTileCache *cache, uint16_t address) {
  address &= kVramWordMask;

  for (unsigned i = 0; i < kPpuTileCacheSlots; i++) {
    PpuTileCacheEntry *entry = &cache->entries2bpp[i];
    if (entry->valid &&
        (((address - entry->base) & kVramWordMask) < kTile2bppWords)) {
      entry->valid = false;
      cache->stats.invalidatedEntries++;
    }

    entry = &cache->entries4bpp[i];
    if (entry->valid &&
        (((address - entry->base) & kVramWordMask) < kTile4bppWords)) {
      entry->valid = false;
      cache->stats.invalidatedEntries++;
    }
  }
}

void PpuTileCacheResetStats(PpuTileCache *cache) {
  memset(&cache->stats, 0, sizeof(cache->stats));
}

static const uint8_t *PpuTileCacheGet(
    PpuTileCache *cache, const uint16_t *vram, uint16_t base,
    bool four_bpp) {
  base &= kVramWordMask;
 
  PpuTileCacheEntry *entries =
      four_bpp ? cache->entries4bpp : cache->entries2bpp;
  PpuTileCacheEntry *entry = &entries[PpuTileCacheSlot(base)];
  uint32_t *hits = four_bpp
      ? &cache->stats.hits4bpp : &cache->stats.hits2bpp;
  uint32_t *misses = four_bpp
      ? &cache->stats.misses4bpp : &cache->stats.misses2bpp;
  uint32_t *decodes = four_bpp
      ? &cache->stats.decodes4bpp : &cache->stats.decodes2bpp;


  if (entry->valid && entry->base == base) {
    (*hits)++;
    return entry->pixels;
  }

  (*misses)++;
  (*decodes)++;

  entry->base = base;
  entry->valid = true;

  for (unsigned y = 0; y < 8; y++) {
    uint32_t bits;

    if (four_bpp) {
      bits = (uint32_t)vram[(base + y) & kVramWordMask] |
          ((uint32_t)vram[(base + y + 8) & kVramWordMask] << 16);

      for (unsigned x = 0; x < 8; x++) {
        entry->pixels[y * 8 + x] =
            ((bits >> x) & 1) |
            ((bits >> (7 + x)) & 2) |
            ((bits >> (14 + x)) & 4) |
            ((bits >> (21 + x)) & 8);
      }
    } else {
      bits = vram[(base + y) & kVramWordMask];

      for (unsigned x = 0; x < 8; x++) {
        entry->pixels[y * 8 + x] =
            ((bits >> x) & 1) |
            ((bits >> (7 + x)) & 2);
      }
    }
  }

  return entry->pixels;
}

const uint8_t *PpuTileCacheGet2bpp(
    PpuTileCache *cache, const uint16_t *vram, uint16_t base) {
  return PpuTileCacheGet(cache, vram, base, false);
}

const uint8_t *PpuTileCacheGet4bpp(
    PpuTileCache *cache, const uint16_t *vram, uint16_t base) {
  return PpuTileCacheGet(cache, vram, base, true);
}

const PpuTileCacheStats *PpuTileCacheGetStats(
    const PpuTileCache *cache) {
  return &cache->stats;
}
