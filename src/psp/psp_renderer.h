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

// Optional GE effects (compile time, off by default). SM_PSP_GE_GLOW: selective glow (psp_renderer.c). SM_PSP_GE_HAZE_LIGHT: the
// Ceres haze colour gradient is replaced by GE point lights (sm_88.c writes no gradient, the renderer lights the picture).
#ifndef SM_PSP_GE_GLOW
#define SM_PSP_GE_GLOW 0
#endif
#ifndef SM_PSP_GE_HAZE_LIGHT
#define SM_PSP_GE_HAZE_LIGHT 0
#endif

// Glow emitters: a box in picture pixels (the 256 x 224 scene; x1 / y1 exclusive).
typedef struct PspGlowBox { int16_t x0, y0, x1, y1; } PspGlowBox;
// sm_84.c: boxes (picture pixels) of the door caps in view: coloured / opening-blue door PLMs by their current draw instruction, and the
// closed blue caps in the level data (type 0xC, BTS 0x40..0x43, with their 0xD / 0x5 continuation blocks). At most max; returns how many.
int PspGlow_DoorBoxes(PspGlowBox *out, int max);
// sm_84.c: boxes of the whitelist's things in view (src/psp/psp_glow_list.h): out[i], lights[i] = 1 (only coloured pixels) or 0 (all).
int PspGlow_ListBoxes(PspGlowBox *out, uint8_t *lights, int max);
// sm_88.c, every frame the Ceres haze object runs: fade level 0..16 and colour channel (0x80 blue, 0x20 red) of the haze.
void PspRenderer_SetHazeLight(unsigned level, unsigned mask);

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

// Instant door transition (SM_PSP_DOOR_SNAP / PSP_DOOR_SNAP=1): the game loads the destination room within one frame while the GE
// slides frozen snapshots of the old and the new room. All calls are no-ops (Ready/Hold return false) when it is compiled out.
void PspRenderer_DoorSnapArm(unsigned direction);                                  // door hit (game_state 9, normal door): snapshot this frame as the old room
bool PspRenderer_DoorSnapReady(void);                                // the old snapshot exists: the game may load the room instantly
void PspRenderer_DoorSnapAbort(void);                                // no snapshot / no instant load: drop everything, the game's own transition runs
bool PspRenderer_DoorSnapCanArm(void);                               // DoorSnapArm would take the snapshot this frame (the game then keeps Samus out of it)
void PspRenderer_DoorSnapBegin(unsigned direction, int align_px, int steps);  // room loaded: direction = door_direction & 3, align_px = old camera's distance to a 256 boundary across the door, steps = slide length (DsStepsForSpeed)
bool PspRenderer_DoorSnapHold(void);                                 // once per game frame: true = hold the game logic (the slide is running)

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
