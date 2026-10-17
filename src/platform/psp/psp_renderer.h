#pragma once

typedef struct Ppu Ppu;
struct RendererFuncs;
void PspRenderer_Create(struct RendererFuncs *funcs);

void PspRenderer_BeginPpuFrame(Ppu *ppu);
void PspRenderer_OnPpuScanline(Ppu *ppu, int line);
Ppu *PspRenderer_GetActiveScanlinePpu(void);
int PspRenderer_GetActiveScanline(void);
void PspRenderer_Ensure4bppTile(int physical_tile);
void PspRenderer_Ensure2bppTile(int physical_tile);
void PspRenderer_FlushTileAtlases(void);
