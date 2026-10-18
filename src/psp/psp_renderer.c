#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>

#include "src/types.h"
#include "src/util.h"
#include "src/snes/ppu.h"
#include "psp_renderer.h"

#define PSP_WIDTH 480
#define PSP_HEIGHT 272
#define GAME_WIDTH 256
#define GAME_HEIGHT 240
#define TEX_WIDTH 512
#define TEX_HEIGHT 256
#define GU_LIST_WORDS (64 * 1024)
#define PSP_TEXTURE_VRAM_OFFSET (512 * 272 * 6)
#define PSP_TEXTURE_VRAM_BYTES (TEX_WIDTH * TEX_HEIGHT * 2)
#define PSP_ATLAS4_VRAM_OFFSET \
  (PSP_TEXTURE_VRAM_OFFSET + PSP_TEXTURE_VRAM_BYTES)
#define PSP_ATLAS4_BYTES (256 * 512 / 2)
#define PSP_ATLAS2_VRAM_OFFSET (PSP_ATLAS4_VRAM_OFFSET + PSP_ATLAS4_BYTES)
#define PSP_ATLAS2_BYTES (512 * 512 / 2)

#ifdef PSP_PROFILE
extern void PspProfileLog(const char *message);

static uint64_t g_profile_convert_total_us;
static uint64_t g_profile_gu_total_us;
static uint32_t g_profile_convert_max_us;
static uint32_t g_profile_gu_max_us;
static uint32_t g_profile_render_count;
#endif

static uint32_t g_gu_list[GU_LIST_WORDS] __attribute__((aligned(16)));
static uint16_t *g_texture;
static uint8_t *g_atlas4;
static uint8_t *g_atlas2;
static uint8_t g_framebuffer[GAME_WIDTH * GAME_HEIGHT * 4] __attribute__((aligned(16)));

//Atlas metadata is PSP-renderer-only. The software PPU does not consult it. A tile is checked at most once per native-renderer frame.
static Ppu *g_tile_ppu;
static Ppu *g_active_scanline_ppu;
static int g_active_scanline = -1;

enum {
  PSP_CAPTURE_SCANLINES = 262,
  PSP_CAPTURE_PPU_COUNT = 2,
  PSP_PPU_WRITE_JOURNAL_CAPACITY = 8192,
};

typedef struct PspPpuCapture {
  Ppu *ppu;
  uint32_t frame;
  int activeLine;
  uint32_t vramVersion;
  uint32_t cgramVersion;
  uint32_t oamVersion;
  uint32_t vramWrites;
  uint32_t cgramWrites;
  uint32_t oamWrites;
  uint32_t journalCount;
  uint8_t journalOverflow;
  PspPpuMemorySnapshot memory;
  PspPpuWrite journal[PSP_PPU_WRITE_JOURNAL_CAPACITY];
  PspPpuScanlineState scanlines[PSP_CAPTURE_SCANLINES];
} PspPpuCapture;

static PspPpuCapture g_ppu_capture[PSP_CAPTURE_PPU_COUNT];

static PspPpuCapture *PspRenderer_FindPpuCapture(
    Ppu *ppu, bool create) {
  for (int i = 0; i < PSP_CAPTURE_PPU_COUNT; ++i) {
    if (g_ppu_capture[i].ppu == ppu)
      return &g_ppu_capture[i];
  }

  if (!create || ppu == NULL)
    return NULL;

  for (int i = 0; i < PSP_CAPTURE_PPU_COUNT; ++i) {
    if (g_ppu_capture[i].ppu == NULL) {
      memset(&g_ppu_capture[i], 0, sizeof(g_ppu_capture[i]));
      g_ppu_capture[i].ppu = ppu;
      return &g_ppu_capture[i];
    }
  }

  return NULL;
}
static uint32_t g_hash4[2048];
static uint32_t g_hash2[4096];
static uint16_t g_seen4[2048];
static uint16_t g_seen2[4096];
static uint16_t g_frame_stamp;
static bool g_atlas4_dirty;
static bool g_atlas2_dirty;

#ifdef PSP_ATLAS_VALIDATE
extern void PspProfileLog(const char *message);

static uint32_t g_atlas_validation_frames;
static uint32_t g_atlas_validation_mismatches;
static int g_atlas_validation_first_kind;
static int g_atlas_validation_first_tile;
static int g_atlas_validation_first_x;
static int g_atlas_validation_first_y;
static int g_atlas_validation_first_expected;
static int g_atlas_validation_first_actual;

static void PspRenderer_ValidateTileAtlases(void);
#endif

static inline uint16_t Rgb565(const uint8_t *p) {
  const uint8_t b = p[0];
  const uint8_t g = p[1];
  const uint8_t r = p[2];
  return (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

static void PspRenderer_Convert(void) {
  for (int y = 0; y < GAME_HEIGHT; ++y) {
    const uint8_t *src = g_framebuffer + y * GAME_WIDTH * 4;
    uint16_t *dst = g_texture + y * TEX_WIDTH;
    for (int x = 0; x < GAME_WIDTH; ++x)
      dst[x] = Rgb565(src + x * 4);
  }
}

/*
 * VRAM layout:
 *   [0, 2 * 512 * 272 * 2)       display buffers
 *   [2 * 512 * 272 * 2, ...)     depth buffer
 *   texture starts after the depth buffer
 *   tile atlases follow the presentation texture
 */
#define PSP_TEXTURE_VRAM_OFFSET (512 * 272 * 6)

static bool PspRenderer_Initialize(void) {
  if (PSP_ATLAS2_VRAM_OFFSET + PSP_ATLAS2_BYTES > sceGeEdramGetSize())
    Die("PSP VRAM is too small for the tile atlases");

  uintptr_t vram = (uintptr_t)sceGeEdramGetAddr() | 0x40000000U;
  g_texture = (uint16_t *)(
      vram + PSP_TEXTURE_VRAM_OFFSET);
  g_atlas4 = (uint8_t *)(vram + PSP_ATLAS4_VRAM_OFFSET);
  g_atlas2 = (uint8_t *)(vram + PSP_ATLAS2_VRAM_OFFSET);

  memset(g_hash4, 0xff, sizeof(g_hash4));
  memset(g_hash2, 0xff, sizeof(g_hash2));
  memset(g_seen4, 0, sizeof(g_seen4));
  memset(g_seen2, 0, sizeof(g_seen2));
  g_frame_stamp = 0;
  g_tile_ppu = NULL;
  g_atlas4_dirty = false;
  g_atlas2_dirty = false;

  sceGuInit();
  sceGuStart(GU_DIRECT, g_gu_list);

  sceGuDrawBuffer(GU_PSM_5650, (void *)0, 512);
  sceGuDispBuffer(PSP_WIDTH, PSP_HEIGHT, (void *)(512 * 272 * 2), 512);
  sceGuDepthBuffer((void *)(512 * 272 * 4), 512);

  sceGuOffset(2048 - PSP_WIDTH / 2, 2048 - PSP_HEIGHT / 2);
  sceGuViewport(2048, 2048, PSP_WIDTH, PSP_HEIGHT);
  sceGuScissor(0, 0, PSP_WIDTH, PSP_HEIGHT);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_CULL_FACE);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_DITHER);
  sceGuEnable(GU_TEXTURE_2D);

  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);

  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);
  return true;
}

static void PspRenderer_Destroy(void) {
  sceGuDisplay(GU_FALSE);
  sceGuTerm();
  g_tile_ppu = NULL;
  g_texture = NULL;
  g_atlas4 = NULL;
  g_atlas2 = NULL;
}

static uint32_t PspRenderer_HashWords(const uint16_t *src, int count) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < count; ++i)
    hash = (hash ^ src[i]) * 16777619u;
  return hash;
}

void PspRenderer_BeginPpuFrame(Ppu *ppu) {
  g_tile_ppu = ppu;

  if (++g_frame_stamp == 0) {
    memset(g_seen4, 0, sizeof(g_seen4));
    memset(g_seen2, 0, sizeof(g_seen2));
    g_frame_stamp = 1;
  }
  
#ifdef PSP_ATLAS_VALIDATE
  PspRenderer_ValidateTileAtlases();
#endif
}

//Called by the PPU at scanline execution time. Keep this separate from BeginPpuFrame(), which runs after emulation and selects the displayed PPU.
void PspRenderer_OnPpuScanline(Ppu *ppu, int line) {
  g_active_scanline_ppu = ppu;
  g_active_scanline = line;

  if (line < 0 || line >= PSP_CAPTURE_SCANLINES)
    return;

  PspPpuCapture *capture = PspRenderer_FindPpuCapture(ppu, true);
  if (capture == NULL)
    return;

    capture->activeLine = line;

  if (line == 0) {
    if (++capture->frame == 0)
      capture->frame = 1;

    capture->vramVersion = 0;
    capture->cgramVersion = 0;
    capture->oamVersion = 0;
    capture->vramWrites = 0;
    capture->cgramWrites = 0;
    capture->oamWrites = 0;

    capture->journalCount = 0;
    capture->journalOverflow = 0;

    capture->memory.frame = capture->frame;
    memcpy(capture->memory.vram, ppu->vram,
           sizeof(capture->memory.vram));
    memcpy(capture->memory.cgram, ppu->cgram,
           sizeof(capture->memory.cgram));
    memcpy(capture->memory.oam, ppu->oam,
           sizeof(capture->memory.oam));
    memcpy(capture->memory.highOam, ppu->highOam,
           sizeof(capture->memory.highOam));
  }

  PspPpuScanlineState *state = &capture->scanlines[line];
  state->frame = capture->frame;
  state->line = line;

  state->mode = ppu->mode;
  state->brightness = ppu->brightness;
  state->forcedBlank = ppu->forcedBlank;
  state->bg3priority = ppu->bg3priority;

  state->screenEnabled[0] = ppu->screenEnabled[0];
  state->screenEnabled[1] = ppu->screenEnabled[1];
  state->screenWindowed[0] = ppu->screenWindowed[0];
  state->screenWindowed[1] = ppu->screenWindowed[1];
  state->windowsel = ppu->windowsel;
  state->window1left = ppu->window1left;
  state->window1right = ppu->window1right;
  state->window2left = ppu->window2left;
  state->window2right = ppu->window2right;
  
  for (int i = 0; i < 6; ++i) {
    const WindowLayer *src = &ppu->windowLayer[i];

    state->windowLayer[i].flags =
        (src->window1enabled ? 1 : 0) |
        (src->window2enabled ? 2 : 0) |
        (src->window1inversed ? 4 : 0) |
        (src->window2inversed ? 8 : 0);
    state->windowLayer[i].maskLogic = src->maskLogic;
    state->mathEnabled[i] = ppu->mathEnabled[i];
  }

  state->clipMode = ppu->clipMode;
  state->preventMathMode = ppu->preventMathMode;
  state->addSubscreen = ppu->addSubscreen;
  state->subtractColor = ppu->subtractColor;
  state->halfColor = ppu->halfColor;
  state->fixedColorR = ppu->fixedColorR;
  state->fixedColorG = ppu->fixedColorG;
  state->fixedColorB = ppu->fixedColorB;

  state->mosaicSize = ppu->mosaicSize;
  state->mosaicStartLine = ppu->mosaicStartLine;
  state->mosaicEnabled = ppu->mosaicEnabled;
  
  state->pseudoHires = ppu->pseudoHires;
  state->overscan = ppu->overscan;
  state->frameOverscan = ppu->frameOverscan;
  state->interlace = ppu->interlace;
  state->frameInterlace = ppu->frameInterlace;
  state->directColor = ppu->directColor;

  for (int i = 0; i < 4; ++i) {
    const BgLayer *src = &ppu->bgLayer[i];

    state->bg[i].hScroll = src->hScroll;
    state->bg[i].vScroll = src->vScroll;
    state->bg[i].tilemapAdr = src->tilemapAdr;
    state->bg[i].tileAdr = src->tileAdr;
    state->bg[i].flags =
        (src->tilemapWider ? 1 : 0) |
        (src->tilemapHigher ? 2 : 0) |
        (src->bigTiles ? 4 : 0) |
        (src->mosaicEnabled ? 8 : 0);
  }

  memcpy(state->m7matrix, ppu->m7matrix, sizeof(state->m7matrix));
  state->m7largeField = ppu->m7largeField;
  state->m7charFill = ppu->m7charFill;
  state->m7xFlip = ppu->m7xFlip;
  state->m7yFlip = ppu->m7yFlip;
  state->m7extBg = ppu->m7extBg;

  state->vramVersion = capture->vramVersion;
  state->cgramVersion = capture->cgramVersion;
  state->oamVersion = capture->oamVersion;
  state->vramWrites = capture->vramWrites;
  state->cgramWrites = capture->cgramWrites;
  state->oamWrites = capture->oamWrites;
}

Ppu *PspRenderer_GetActiveScanlinePpu(void) {
  return g_active_scanline_ppu;
}

int PspRenderer_GetActiveScanline(void) {
  return g_active_scanline;
}

const PspPpuScanlineState *PspRenderer_GetScanlineState(
    Ppu *ppu, int line) {
  if (line < 0 || line >= PSP_CAPTURE_SCANLINES)
    return NULL;

  PspPpuCapture *capture = PspRenderer_FindPpuCapture(ppu, false);
  if (capture == NULL)
    return NULL;

  const PspPpuScanlineState *state = &capture->scanlines[line];
  if (state->frame != capture->frame || state->line != line)
    return NULL;

  return state;
}

void PspRenderer_RecordPpuMemoryWrite(
    Ppu *ppu, int region, uint16_t address, uint16_t value) {
  PspPpuCapture *capture = PspRenderer_FindPpuCapture(ppu, true);
  if (capture == NULL)
    return;

  switch (region) {
    case PSP_PPU_WRITE_VRAM:
      ++capture->vramVersion;
      ++capture->vramWrites;
      break;

    case PSP_PPU_WRITE_CGRAM:
      ++capture->cgramVersion;
      ++capture->cgramWrites;
      break;

    case PSP_PPU_WRITE_OAM:
      ++capture->oamVersion;
      ++capture->oamWrites;
      break;

    default:
      return;
  }

  /*
   * Writes before the first captured scanline are part of the baseline
   * captured at line 0, so they do not need journal entries.
   */
  if (capture->frame == 0 ||
      capture->activeLine < 0 ||
      capture->activeLine >= PSP_CAPTURE_SCANLINES) {
    return;
  }

  if (capture->journalCount >= PSP_PPU_WRITE_JOURNAL_CAPACITY) {
    capture->journalOverflow = 1;
    return;
  }

  PspPpuWrite *write = &capture->journal[capture->journalCount++];
  write->line = (uint16_t)capture->activeLine;
  write->address = address;
  write->value = value;
  write->region = (uint8_t)region;
  write->reserved = 0;
}

int PspRenderer_GetPpuMemoryHistory(
    Ppu *ppu, int line,
    const PspPpuMemorySnapshot **snapshot,
    const PspPpuWrite **writes,
    uint32_t *write_count) {
  if (snapshot != NULL)
    *snapshot = NULL;
  if (writes != NULL)
    *writes = NULL;
  if (write_count != NULL)
    *write_count = 0;

  if (line < 0 || line >= PSP_CAPTURE_SCANLINES)
    return 0;

  PspPpuCapture *capture = PspRenderer_FindPpuCapture(ppu, false);
  if (capture == NULL || capture->frame == 0 ||
      capture->journalOverflow) {
    return 0;
  }

  const PspPpuScanlineState *state = &capture->scanlines[line];
  if (state->frame != capture->frame || state->line != line ||
      capture->memory.frame != capture->frame) {
    return 0;
  }

  /*
   * Journal entries are in execution order. Only include writes from
   * scanlines strictly before the requested line.
   */
  uint32_t count = 0;
  while (count < capture->journalCount &&
         capture->journal[count].line < line) {
    ++count;
  }

  if (snapshot != NULL)
    *snapshot = &capture->memory;
  if (writes != NULL)
    *writes = capture->journal;
  if (write_count != NULL)
    *write_count = count;

  return 1;
}

void PspRenderer_Ensure4bppTile(int physical_tile) {
  if (g_tile_ppu == NULL || g_atlas4 == NULL || g_frame_stamp == 0)
    return;

  physical_tile &= 2047;
  if (g_seen4[physical_tile] == g_frame_stamp)
    return;
  g_seen4[physical_tile] = g_frame_stamp;

  const uint16_t *src = &g_tile_ppu->vram[physical_tile * 16];
  uint32_t hash = PspRenderer_HashWords(src, 16);
  if (g_hash4[physical_tile] == hash)
    return;
  g_hash4[physical_tile] = hash;

  int tile_x = physical_tile & 31;
  int tile_y = physical_tile >> 5;
  uint8_t *dst = g_atlas4 + tile_y * 8 * 128 + tile_x * 4;

  for (int y = 0; y < 8; ++y, dst += 128) {
    uint32_t planes = src[y] | ((uint32_t)src[y + 8] << 16);
    for (int x = 0; x < 8; ++x) {
      int shift = 7 - x;
      int pixel = ((planes >> shift) & 1) |
                  ((planes >> (shift + 7)) & 2) |
                  ((planes >> (shift + 14)) & 4) |
                  ((planes >> (shift + 21)) & 8);

      if (x & 1)
        dst[x >> 1] = (dst[x >> 1] & 0x0f) | (pixel << 4);
      else
        dst[x >> 1] = (dst[x >> 1] & 0xf0) | pixel;
    }
  }

  g_atlas4_dirty = true;
}

void PspRenderer_Ensure2bppTile(int physical_tile) {
  if (g_tile_ppu == NULL || g_atlas2 == NULL || g_frame_stamp == 0)
    return;

  physical_tile &= 4095;
  if (g_seen2[physical_tile] == g_frame_stamp)
    return;
  g_seen2[physical_tile] = g_frame_stamp;

  const uint16_t *src = &g_tile_ppu->vram[physical_tile * 8];
  uint32_t hash = PspRenderer_HashWords(src, 8);
  if (g_hash2[physical_tile] == hash)
    return;
  g_hash2[physical_tile] = hash;

  int tile_x = physical_tile & 63;
  int tile_y = physical_tile >> 6;
  uint8_t *dst = g_atlas2 + tile_y * 8 * 256 + tile_x * 4;

  for (int y = 0; y < 8; ++y, dst += 256) {
    uint16_t planes = src[y];
    for (int x = 0; x < 8; ++x) {
      int shift = 7 - x;
      int pixel = ((planes >> shift) & 1) |
                  ((planes >> (shift + 7)) & 2);

      if (x & 1)
        dst[x >> 1] = (dst[x >> 1] & 0x0f) | (pixel << 4);
      else
        dst[x >> 1] = (dst[x >> 1] & 0xf0) | pixel;
    }
  }

  g_atlas2_dirty = true;
}

void PspRenderer_FlushTileAtlases(void) {
  if (g_atlas4_dirty || g_atlas2_dirty) {
    sceGuTexFlush();
    g_atlas4_dirty = false;
    g_atlas2_dirty = false;
  }
}

#ifdef PSP_ATLAS_VALIDATE
static void PspRenderer_RecordAtlasMismatch(
    int kind, int tile, int x, int y, int expected, int actual) {
  if (g_atlas_validation_mismatches == 0) {
    g_atlas_validation_first_kind = kind;
    g_atlas_validation_first_tile = tile;
    g_atlas_validation_first_x = x;
    g_atlas_validation_first_y = y;
    g_atlas_validation_first_expected = expected;
    g_atlas_validation_first_actual = actual;
  }
  ++g_atlas_validation_mismatches;
}

static int PspRenderer_ReadAtlas4Pixel(int tile, int x, int y) {
  int tile_x = tile & 31;
  int tile_y = tile >> 5;
  const uint8_t *row =
      g_atlas4 + tile_y * 8 * 128 + y * 128 + tile_x * 4;
  uint8_t packed = row[x >> 1];
  return (x & 1) ? (packed >> 4) : (packed & 0x0f);
}

static int PspRenderer_ReadAtlas2Pixel(int tile, int x, int y) {
  int tile_x = tile & 63;
  int tile_y = tile >> 6;
  const uint8_t *row =
      g_atlas2 + tile_y * 8 * 256 + y * 256 + tile_x * 4;
  uint8_t packed = row[x >> 1];
  return (x & 1) ? (packed >> 4) : (packed & 0x0f);
}

static void PspRenderer_ValidateTileAtlases(void) {
  if (g_tile_ppu == NULL || g_atlas4 == NULL || g_atlas2 == NULL)
    return;

  /*
   * Populate each atlas using the normal atlas decoder, then independently
   * decode the source VRAM bitplanes and compare all 8x8 pixels.
   * This path is compiled out of normal builds.
   */
  for (int tile = 0; tile < 2048; ++tile)
    PspRenderer_Ensure4bppTile(tile);

  for (int tile = 0; tile < 4096; ++tile)
    PspRenderer_Ensure2bppTile(tile);

  for (int tile = 0; tile < 2048; ++tile) {
    const uint16_t *src = &g_tile_ppu->vram[tile * 16];

    for (int y = 0; y < 8; ++y) {
      uint16_t planes12 = src[y];
      uint16_t planes34 = src[y + 8];

      for (int x = 0; x < 8; ++x) {
        int bit = 7 - x;
        int expected =
            ((planes12 >> bit) & 1) |
            (((planes12 >> (bit + 8)) & 1) << 1) |
            (((planes34 >> bit) & 1) << 2) |
            (((planes34 >> (bit + 8)) & 1) << 3);
        int actual = PspRenderer_ReadAtlas4Pixel(tile, x, y);

        if (actual != expected)
          PspRenderer_RecordAtlasMismatch(
              4, tile, x, y, expected, actual);
      }
    }
  }

  for (int tile = 0; tile < 4096; ++tile) {
    const uint16_t *src = &g_tile_ppu->vram[tile * 8];

    for (int y = 0; y < 8; ++y) {
      uint16_t planes = src[y];

      for (int x = 0; x < 8; ++x) {
        int bit = 7 - x;
        int expected =
            ((planes >> bit) & 1) |
            (((planes >> (bit + 8)) & 1) << 1);
        int actual = PspRenderer_ReadAtlas2Pixel(tile, x, y);

        if (actual != expected)
          PspRenderer_RecordAtlasMismatch(
              2, tile, x, y, expected, actual);
      }
    }
  }

  if (++g_atlas_validation_frames == 60) {
    char line[192];

    if (g_atlas_validation_mismatches == 0) {
      snprintf(line, sizeof(line),
               "atlas_validation frames=60 mismatches=0");
    } else {
      snprintf(line, sizeof(line),
               "atlas_validation frames=60 mismatches=%u "
               "first_bpp=%d tile=%d x=%d y=%d expected=%d actual=%d",
               (unsigned int)g_atlas_validation_mismatches,
               g_atlas_validation_first_kind,
               g_atlas_validation_first_tile,
               g_atlas_validation_first_x,
               g_atlas_validation_first_y,
               g_atlas_validation_first_expected,
               g_atlas_validation_first_actual);
    }

    PspProfileLog(line);
    g_atlas_validation_frames = 0;
    g_atlas_validation_mismatches = 0;
  }
}
#endif

static void PspRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  if (width != GAME_WIDTH || height != GAME_HEIGHT)
    Die("PSP renderer requires a 256x240 framebuffer");
  *pixels = g_framebuffer;
  *pitch = GAME_WIDTH * 4;
}

typedef struct PspVertex {
  float u, v;
  float x, y, z;
} PspVertex;

static void PspRenderer_EndDraw(void) {
#ifdef PSP_PROFILE
  uint32_t profile_start_us = sceKernelGetSystemTimeLow();
#endif
  PspRenderer_Convert();
  #ifdef PSP_PROFILE
  uint32_t profile_convert_us =
      sceKernelGetSystemTimeLow() - profile_start_us;
  profile_start_us = sceKernelGetSystemTimeLow();
#endif


  sceKernelDcacheWritebackAll();
  sceGuStart(GU_DIRECT, g_gu_list);
  sceGuTexImage(0, TEX_WIDTH, TEX_HEIGHT, TEX_WIDTH, g_texture);

  const int left = (PSP_WIDTH - GAME_WIDTH) / 2;
  const int top = (PSP_HEIGHT - GAME_HEIGHT) / 2;

  PspVertex *v = (PspVertex *)sceGuGetMemory(2 * sizeof(PspVertex));
  v[0].u = 0;          v[0].v = 0;           v[0].x = left;          v[0].y = top;           v[0].z = 0;
  v[1].u = GAME_WIDTH; v[1].v = GAME_HEIGHT; v[1].x = left + GAME_WIDTH; v[1].y = top + GAME_HEIGHT; v[1].z = 0;

  sceGuDrawArray(GU_SPRITES,
                 GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                 2, NULL, v);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  #ifdef PSP_PROFILE
  uint32_t profile_gu_us =
      sceKernelGetSystemTimeLow() - profile_start_us;

  g_profile_convert_total_us += profile_convert_us;
  g_profile_gu_total_us += profile_gu_us;
  if (profile_convert_us > g_profile_convert_max_us)
    g_profile_convert_max_us = profile_convert_us;
  if (profile_gu_us > g_profile_gu_max_us)
    g_profile_gu_max_us = profile_gu_us;

  if (++g_profile_render_count == 60) {
    char profile_line[160];
    snprintf(profile_line, sizeof(profile_line),
             "render_frames=60 convert_avg_us=%u convert_max_us=%u "
             "gu_sync_avg_us=%u gu_sync_max_us=%u vblank=disabled",
             (unsigned int)(g_profile_convert_total_us / 60),
             (unsigned int)g_profile_convert_max_us,
             (unsigned int)(g_profile_gu_total_us / 60),
             (unsigned int)g_profile_gu_max_us);
    PspProfileLog(profile_line);
    g_profile_convert_total_us = 0;
    g_profile_gu_total_us = 0;
    g_profile_convert_max_us = 0;
    g_profile_gu_max_us = 0;
    g_profile_render_count = 0;
  }
#else
  sceDisplayWaitVblankStart();
  #endif
  sceGuSwapBuffers();
}

static const struct RendererFuncs kPspRendererFuncs = {
  &PspRenderer_Initialize,
  &PspRenderer_Destroy,
  &PspRenderer_BeginDraw,
  &PspRenderer_EndDraw,
};

void PspRenderer_Create(struct RendererFuncs *funcs) {
  *funcs = kPspRendererFuncs;
}
