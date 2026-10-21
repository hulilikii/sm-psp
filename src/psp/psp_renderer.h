#pragma once

#include <stdbool.h>

struct RendererFuncs;
struct Ppu;

void PspRenderer_Create(struct RendererFuncs *funcs);

// Called from ppu_runLine() (through g_ppu_line_hook) before each visible line is rendered. Only active while the GE path is enabled.
void PspRenderer_CaptureLine(const struct Ppu *ppu, int line);

// Switch between the software-PPU blit and the GE path at runtime. 
void PspRenderer_ToggleGe(void);
bool PspRenderer_GeActive(void);
