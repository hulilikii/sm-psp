#pragma once

#include <stdint.h>

typedef struct Ppu Ppu;
struct RendererFuncs;
void PspRenderer_Create(struct RendererFuncs *funcs);

void PspRenderer_BeginPpuFrame(Ppu *ppu);
void PspRenderer_OnPpuScanline(Ppu *ppu, int line);

enum {
  PSP_PPU_PHASE_OTHER = 0,
  PSP_PPU_PHASE_CPU = 1,
  PSP_PPU_PHASE_POS = 2,
};
void PspRenderer_SetPpuWritePhase(int phase);
Ppu *PspRenderer_GetActiveScanlinePpu(void);
int PspRenderer_GetActiveScanline(void);

enum {
  PSP_PPU_WRITE_VRAM = 0,
  PSP_PPU_WRITE_CGRAM = 1,
  PSP_PPU_WRITE_OAM = 2,
};

typedef struct PspPpuMemorySnapshot {
  uint32_t frame;
  uint16_t vram[0x8000];
  uint16_t cgram[0x100];
  uint16_t oam[0x100];
  uint8_t highOam[0x20];
} PspPpuMemorySnapshot;

typedef struct PspPpuWrite {
  uint16_t line;
  uint16_t address;
  uint16_t value;
  uint8_t region;
  uint8_t reserved;
} PspPpuWrite;

typedef struct PspPpuScanlineState {
  uint32_t frame;
  int line;

  uint8_t mode;
  uint8_t brightness;
  uint8_t forcedBlank;
  uint8_t bg3priority;

  uint8_t screenEnabled[2];
  uint8_t screenWindowed[2];
  uint32_t windowsel;
  uint8_t window1left, window1right;
  uint8_t window2left, window2right;
  
  struct {
    uint8_t flags;
    uint8_t maskLogic;
  } windowLayer[6];

  uint8_t clipMode;
  uint8_t preventMathMode;
  uint8_t addSubscreen;
  uint8_t subtractColor;
  uint8_t halfColor;
  uint8_t mathEnabled[6];
  uint8_t fixedColorR;
  uint8_t fixedColorG;
  uint8_t fixedColorB;

  uint8_t mosaicSize;
  uint8_t mosaicStartLine;
  uint8_t mosaicEnabled;
  
  uint8_t pseudoHires;
  uint8_t overscan;
  uint8_t frameOverscan;
  uint8_t interlace;
  uint8_t frameInterlace;
  uint8_t directColor;

  struct {
    uint16_t hScroll;
    uint16_t vScroll;
    uint16_t tilemapAdr;
    uint16_t tileAdr;
    uint8_t flags;
  } bg[4];

  int16_t m7matrix[8];
  uint8_t m7largeField;
  uint8_t m7charFill;
  uint8_t m7xFlip;
  uint8_t m7yFlip;
  uint8_t m7extBg;

  uint32_t vramVersion;
  uint32_t cgramVersion;
  uint32_t oamVersion;
  uint32_t vramWrites;
  uint32_t cgramWrites;
  uint32_t oamWrites;
} PspPpuScanlineState;

const PspPpuScanlineState *PspRenderer_GetScanlineState(
    Ppu *ppu, int line);
void PspRenderer_RecordPpuMemoryWrite(
    Ppu *ppu, int region, uint16_t address, uint16_t value);

/*
 * Returns 1 when the requested scanline has valid memory history.
 * The returned pointers refer to renderer-owned memory and remain valid
 * only until that PPU's next frame capture overwrites them.
 *
 * The snapshot represents memory at the start of scanline 0.
 * The returned write list contains writes from earlier scanlines only.
 * A return value of 0 means the history is unavailable or overflowed.
 */
int PspRenderer_GetPpuMemoryHistory(
    Ppu *ppu, int line,
    const PspPpuMemorySnapshot **snapshot,
    const PspPpuWrite **writes,
    uint32_t *write_count);

void PspRenderer_Ensure4bppTile(int physical_tile);
void PspRenderer_Ensure2bppTile(int physical_tile);
void PspRenderer_FlushTileAtlases(void);
