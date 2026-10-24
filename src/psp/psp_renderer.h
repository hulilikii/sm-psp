#pragma once

#include <stdbool.h>
#include <stdint.h>

struct RendererFuncs;
struct Ppu;

void PspRenderer_Create(struct RendererFuncs *funcs);

// The 256x240 8888 frame (pitch 1024) the renderer displays; the software PPU draws into it directly.
uint8_t *PspRenderer_FrameBuffer(void);

// Called from ppu_runLine() (through g_ppu_line_hook) before each visible line is rendered. Only active while the GE path is enabled.
void PspRenderer_CaptureLine(const struct Ppu *ppu, int line);

// Cycle at runtime: software PPU blit -> GE (with tint) -> GE without tint -> software.
void PspRenderer_ToggleGe(void);
bool PspRenderer_GeActive(void);
