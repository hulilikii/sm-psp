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

// After suspend/resume: VRAM atlases are rebuilt and the next frames are drawn by the software PPU.
void PspRenderer_Resume(void);

enum PspButtonIcon {
  PSP_BUTTON_ICON_CIRCLE,
  PSP_BUTTON_ICON_CROSS,
  PSP_BUTTON_ICON_SQUARE,
  PSP_BUTTON_ICON_TRIANGLE,
};

enum PspButtonIconEffect {
  PSP_BUTTON_ICON_EFFECT_NONE = 0,
  PSP_BUTTON_ICON_EFFECT_CLIP_BG2_WINDOW = 1,
};

// Button overlays use 256x224 source-frame coordinates.
void PspRenderer_SetButtonIcon(unsigned slot, enum PspButtonIcon icon,
                               int x, int y, int w, int h);
void PspRenderer_SetButtonIconEx(unsigned slot, enum PspButtonIcon icon,
                                 int x, int y, int w, int h,
                                 unsigned effects);
void PspRenderer_ClearButtonIcon(unsigned slot);
void PspRenderer_ClearButtonIcons(void);
