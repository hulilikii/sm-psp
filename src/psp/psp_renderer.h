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

// Post-composition mosaic over the complete PSP frame (GE only). Size 1 disables it, 16 is the maximum.
void PspRenderer_SetMosaicTransition(unsigned size);
unsigned PspRenderer_GetMosaicTransition(void);

// Gameplay-only room transition (doors, not elevators). Direction = door_direction & 3: 0 right, 1 left, 2 down, 3 up.
// Live old room (drawn with the palette it had before the fade-out) -> frozen last frame -> the frozen frame follows the
// game camera off screen while the new room (target palette) is revealed -> normal rendering. No-ops unless
// SM_PSP_DOOR_GE and the GE path are active; the game logic is identical either way.
void PspRenderer_BeginDoorTransition(void);                                   // door hit: start following the old room
void PspRenderer_DoorTransitionFreeze(void);                                  // fade-out done: keep the last drawn frame
void PspRenderer_DoorTransitionRef(uint16_t layer1_x, uint16_t layer1_y, unsigned direction);  // camera at the start of the scroll
void PspRenderer_DoorTransitionArm(const uint16_t *dest_palettes);          // new room's tiles + palettes are loaded
void PspRenderer_DoorTransitionCamera(uint16_t layer1_x, uint16_t layer1_y);  // after every scroll step
void PspRenderer_EndDoorTransition(void);                                     // fade-in done: release everything
bool PspRenderer_DoorTransitionActive(void);                                  // Begin..End: a GE transition owns the screen
bool PspRenderer_DoorTransitionArmed(void);                                   // new room is on screen under the sliding old room

enum PspButtonIcon {
  PSP_BUTTON_ICON_CIRCLE,
  PSP_BUTTON_ICON_CROSS,
  PSP_BUTTON_ICON_SQUARE,
  PSP_BUTTON_ICON_TRIANGLE,
  PSP_BUTTON_ICON_L,
  PSP_BUTTON_ICON_R,
  PSP_BUTTON_ICON_SELECT,
  PSP_BUTTON_ICON_DPAD_UD,
  PSP_BUTTON_ICON_DPAD_LR,
  PSP_BUTTON_ICON_DPAD_LRUD,
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

// Native message overlay (final composition layer, both render paths). id: ROM message index 1-29.
void PspRenderer_SetMessageBox(unsigned id, unsigned selection);  /* selection: 0 = YES, 2 = NO */
void PspRenderer_SetMessageBoxSelection(unsigned selection);
void PspRenderer_SetMessageButton(int icon, const char *label);   /* icon: enum PspButtonIcon, -1 = show label */
void PspRenderer_SetMessageFade(unsigned fade);                   /* 0-255 */
void PspRenderer_SetMessageDimHold(bool hold);
void PspRenderer_ClearMessageBox(void);
