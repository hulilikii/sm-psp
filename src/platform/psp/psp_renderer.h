#pragma once

typedef struct Ppu Ppu;
struct RendererFuncs;
void PspRenderer_Create(struct RendererFuncs *funcs);

void PspRenderer_BeginPpuFrame(Ppu *ppu);
void PspRenderer_OnPpuScanline(Ppu *ppu, int line);
Ppu *PspRenderer_GetActiveScanlinePpu(void);
int PspRenderer_GetActiveScanline(void);

enum {
  PSP_PPU_WRITE_VRAM = 0,
  PSP_PPU_WRITE_CGRAM = 1,
  PSP_PPU_WRITE_OAM = 2,
};

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

  uint8_t mosaicSize;
  uint8_t mosaicStartLine;
  uint8_t mosaicEnabled;

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
void PspRenderer_OnPpuMemoryWrite(Ppu *ppu, int region);

void PspRenderer_Ensure4bppTile(int physical_tile);
void PspRenderer_Ensure2bppTile(int physical_tile);
void PspRenderer_FlushTileAtlases(void);
