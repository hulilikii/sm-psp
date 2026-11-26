#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>

#include "src/types.h"
#include "src/util.h"
#include "src/snes/ppu.h"
#include "src/psp/psp_renderer.h"
#include "src/psp/psp_log.h"
#include "src/psp/psp_audio_me.h"
#include "src/psp/psp_hud_icons.h"
#include "src/psp/psp_button_icons.h"
#include "src/psp/psp_message_font.h"
#include "src/psp/psp_message_text.h"
#include "src/psp/psp_gl_guard.h"  /* last: from here every emitting sceGu call is bounds-checked (see the header) */

#if SM_PSP_LOG
#define PROF(...) __VA_ARGS__
#else
#define PROF(...)
#endif

#ifndef SM_PSP_SW_PATH
#define SM_PSP_SW_PATH 1
#endif

/* Gate 5u: swizzled tile atlases. PSP_SWIZZLE is a bit mask: 1 = the BG/OBJ atlases (g_atlas2, g_atlas4), 2 = the mode 7
 * tile atlas (g_atlas7). A swizzled texture is stored in blocks of 16 bytes x 8 rows (128 B), blocks in row-major order, rows
 * inside a block consecutive: byte (xb, y) is at ((y >> 3) * (pitch / 16) + (xb >> 4)) * 128 + (y & 7) * 16 + (xb & 15).
 * Texture coordinates do not change, only where EnsureTile*() store a tile and the swizzle flag of sceGuTexMode. A tile is
 * 4 bytes (T4) or 8 bytes (T8) wide and 8 rows high, so it lies inside one block row and is still written with 32-bit stores,
 * and the GE finds an 8x8 tile in one 128 B block instead of 8 rows `pitch` bytes apart. Only the three atlases qualify:
 * they are written one tile at a time and never rewritten as a whole. Not swizzled: g_m7img and the layer images (CPU
 * rewritten row by row, sampled in horizontal runs), the mosaic/subscreen/frame buffers (render targets can't be swizzled)
 * and the small RAM icon/font textures. 0 (default): linear, as before. */
#ifndef SM_PSP_SWIZZLE
#define SM_PSP_SWIZZLE 0
#endif
#define SM_PSP_SWZ_BG (((SM_PSP_SWIZZLE) & 1) != 0)
#define SM_PSP_SWZ_M7 (((SM_PSP_SWIZZLE) & 2) != 0)
#define SWZ_FLAG(on) ((on) ? GU_TRUE : GU_FALSE)

#if !SM_PSP_LOG && SM_PSP_SW_PATH < 0
#error "SM_PSP_SW_PATH=-1 calibrates with the log clock (Now() is 0 without it): use PSP_LOG=1 or PSP_SW_PATH=0/1"
#endif

#ifndef SM_PSP_GE_STRIPS
#define SM_PSP_GE_STRIPS 0
#endif

#ifndef SM_PSP_GE_SMOOTH_TINT
#define SM_PSP_GE_SMOOTH_TINT 0  /* experimental, not verified on hardware: off unless the build asks for it (PSP_GE_SMOOTH_TINT=1) */
#endif


#ifndef SM_PSP_MOS_GE
#define SM_PSP_MOS_GE 1  /* Gate 5s: 1 = mosaic source image rendered by the GE, 0 = previous CPU-built image (MosBuild) */
#endif

#define PSP_VBLANK_GATE 1  /* wait for vblank only when running ahead of the panel */

#ifndef SM_PSP_BENCH_COMPOSE
#define SM_PSP_BENCH_COMPOSE 0  /* 1 (log builds): boot-time GE timing of the pieces of the composition pass */
#endif

#ifndef SM_PSP_COMPOSE_STRIP
#define SM_PSP_COMPOSE_STRIP 32  /* destination columns per sprite of the main composition quad (0: one 360 px sprite). Hardware: one wide scaled sprite 5.7 ms of GE time, 32 px columns 1.1 ms (cbench) */
#endif

#ifndef SM_PSP_VERIFY
#define SM_PSP_VERIFY 1  /* 0: play build, no verification or audit frames (the software PPU still draws until the GE streak is reached) */
#endif

#ifndef SM_PSP_VERIFY_GAP
#define SM_PSP_VERIFY_GAP 12  /* frames between two software-PPU verification frames in steady state (0: back to back, as before) */
#endif

#ifndef SM_PSP_ASYNC
#define SM_PSP_ASYNC 0  /* Present() queues a GE frame and returns; the next EndDraw waits for the GE and flips it. 1: scene list + compose list (g_gu_list2) queued back to back; 2: scene and compose in ONE list */
#endif

/* Glow (PSP_GE_GLOW=1, compile time, off by default; experimental, not verified on hardware): light spreading from selected parts of
 * the picture, made by the GE in the composition list (GlowPrep / ComposeGlowAdd), with no CPU pixel work. What emits (GlowCollect):
 * SM_PSP_GE_GLOW_SRC bit 0 = sprites (Samus, shots, enemies: their own visible pixels, found by redrawing them with the depth test
 * EQUAL, so what a foreground tile hides does not emit), bit 1 = door caps (their exact blocks), bit 2 = environment (lava / acid:
 * the layer-3 pixels; rain rooms: the whole picture for the lightning). Room tiles, the pause screens and the intro never emit.
 * THRESH: per channel level (0..254) below which a pixel does not emit; GAIN: percent of emitted light that is spread (1..400;
 * 100 = as bright as the source above THRESH). Works with PSP_ASYNC 0, 1 and 2 (ComposeBytesBound counts its commands). */
#ifndef SM_PSP_GE_GLOW
#define SM_PSP_GE_GLOW 0
#endif
#ifndef SM_PSP_GE_GLOW_THRESH
#define SM_PSP_GE_GLOW_THRESH 144
#endif
#ifndef SM_PSP_GE_GLOW_GAIN
#define SM_PSP_GE_GLOW_GAIN 300
#endif
#ifndef SM_PSP_GE_GLOW_SRC
#define SM_PSP_GE_GLOW_SRC 7
#endif
#if SM_PSP_GE_GLOW_THRESH < 0 || SM_PSP_GE_GLOW_THRESH > 254 || SM_PSP_GE_GLOW_GAIN < 1 || SM_PSP_GE_GLOW_GAIN > 400
#error "SM_PSP_GE_GLOW_THRESH is 0..254, SM_PSP_GE_GLOW_GAIN 1..400"
#endif
#if SM_PSP_GE_GLOW_SRC < 0 || SM_PSP_GE_GLOW_SRC > 7
#error "SM_PSP_GE_GLOW_SRC is a mask of 1 (sprites), 2 (doors), 4 (environment)"
#endif
/* Ceres haze light (PSP_GE_HAZE_LIGHT=1, SM_PSP_GE_HAZE_LIGHT in psp_renderer.h): the game no longer writes the haze colour gradient
 * (sm_88.c); ComposeHazeLight adds four GE point lights shining up from below the picture instead. GAIN: percent of the default strength. */
#ifndef SM_PSP_GE_HAZE_LIGHT_GAIN
#define SM_PSP_GE_HAZE_LIGHT_GAIN 100
#endif

/* SM_PSP_DOOR_SNAP=1 (PSP_DOOR_SNAP in the Makefile): instant door load, then the GE slides two frozen snapshots of the old and
 * the new room (see "Instant door transition" below). Needs the synchronous present: the snapshots are written into the layer
 * image slots, which the CPU rewrites after the transition, and the compose list is unaudited for the extra quads. */
#ifndef SM_PSP_DOOR_SNAP
#define SM_PSP_DOOR_SNAP 0
#endif
#if SM_PSP_DOOR_SNAP && SM_PSP_ASYNC
#error "PSP_DOOR_SNAP=1 needs PSP_ASYNC=0"
#endif
#if SM_PSP_DOOR_SNAP
#include "src/psp/psp_door_snap.h"
#endif

enum {
  kPspWidth = 480,
  kPspHeight = 272,
  kGameWidth = 256,
  kGameHeight = 240,
  kTexWidth = 512,
  kTexHeight = 256,
  kMaxLines = 240,
  kLayers = 3,
  kListWords = 128 * 1024,   /* 512 KB */
  kListBytes = kListWords * 4,
  kMaxTilesPerFrame = 12000, /* gate 5d: ceiling only; ListBytesNeeded() decides whether the frame fits the list */
  kMaxSpriteQuads = 2000,    /* 2000 * 2 * 16 B = 64 KB */
  kMaxObjBands = 4,          /* OBJ window: row bands of one window state per frame */
  kLeft = (kPspWidth - kGameWidth) / 2,
  kTop = (kPspHeight - kGameHeight) / 2,

// VRAM: [0,0x44000) draw, [0x44000,0x88000) disp, [0x88000,0xCC000) depth,
// [0xCC000,0x10C000) software-frame texture (565 512x256, or 8888 256x256), 512x512 T4 2bpp atlas, 256x512 T4 4bpp atlas.
  kVramTexture = 512 * 272 * 6,
  kVramAtlas2 = kVramTexture + kTexWidth * kTexHeight * 2,
  kAtlas2Bytes = 512 * 512 / 2,
  kVramAtlas4 = kVramAtlas2 + kAtlas2Bytes,
  kAtlas4Bytes = 256 * 512 / 2,
  kVramAtlas7 = kVramAtlas4 + kAtlas4Bytes,  /* mode 7: 256 tiles of 8x8, 8 bit, 16x16 tiles = 128x128 */
  kAtlas7Bytes = 128 * 128,
  kVramSub = kVramTexture,                    /* subscreen composite target: 256x256 8888; shares the software-frame texture (never both in one frame) */
  kSubBytes = 256 * 256 * 4,
  kTintTexW = 8, kTintTexH = 256, kTintTexBytes = kTintTexW * kTintTexH * 4,
  kVramSubZ = kVramAtlas7 + kAtlas7Bytes,     /* its depth buffer: 256x256 16 bit */
  kSubZBytes = 256 * 256 * 2,
  kVramNeeded = kVramSubZ + kSubZBytes,
  kVramBuf2 = kVramNeeded,                    /* third 5650 frame buffer (triple buffering) */
  kBufBytes = 512 * 272 * 2,
};

/* Behind the third frame buffer: the two layer images (gate 5l; BG2, BG3; 512x256 wide map or 256x512 high map) and the
 * smooth-tint strip. The strip has its own 8 KB, so it never depends on which frames use the subscreen target, the mosaic
 * images or the software-frame texture (kVramSub region). Written by the CPU before the list, read by the GE only. */
enum {
  kImgSlots = 2, kImgW = 512, kImgH = 256, kImgBytes = kImgW * kImgH,
  kVramImg0 = kVramBuf2 + kBufBytes, kVramImgEnd = kVramImg0 + kImgSlots * kImgBytes,
  kVramTint = kVramImgEnd, kVramTintEnd = kVramTint + kTintTexBytes,
};

/* Glow buffers (ComposeGlow), behind everything else: 8888 render targets of 128x128 (half resolution), 64x64 (quarter) and 32x32 (eighth).
 * 2 KB aligned like the frame buffers; the end must stay inside the 2 MB of eDRAM every PSP has (checked again at start-up against
 * sceGeEdramGetSize). */
enum {
  kGlowW1 = 128, kGlowW2 = 64, kGlowW3 = 32, kGlowS3 = 64,  /* kGlowS3: stride of the 32 px target (draw buffer widths are multiples of 64) */
  kVramGlow1 = kVramTintEnd, kVramGlow2 = kVramGlow1 + kGlowW1 * kGlowW1 * 4, kVramGlow3 = kVramGlow2 + kGlowW2 * kGlowW2 * 4,
  kVramGlowEnd = kVramGlow3 + kGlowS3 * kGlowW3 * 4,
};
_Static_assert(kVramGlow1 % 2048 == 0 && kVramGlow2 % 2048 == 0 && kVramGlow3 % 2048 == 0 && kVramGlowEnd <= 2 * 1024 * 1024,
               "glow buffers do not fit the eDRAM");

// Reasons a frame is handed back to the software renderer. 
enum {
  kReasonMode = 1, kReasonBigTile = 2, kReasonMosaic = 4, kReasonWindow = 8,
  kReasonMathClip = 16,   /* color window clip mode, or prevent mode 1/2 (window dependent) */
  kReasonMathTint = 32,   /* fixed colour or half colour together with subscreen layers */
  kReasonBg3Prio = 64,    /* unused since Gate 4a (BG3 high is level 15 as in ppu.c) */
  kReasonBudget = 128,
  kReasonSubObj = 256,    /* sprites enabled on the subscreen */
  kReasonSubBg = 512,     /* BG tiles visible on the subscreen where math applies */
  kReasonObjCfg = 1024,   /* OBJ interlace / config changes mid-frame / non-contiguous OBJ rows */
  kReasonObjLimit = 2048, /* a line exceeds 32 sprites or 34 tile slivers (hardware drops some) */
  kReasonSubWin = 4096,   /* contributing subscreen layer is windowed (TSW) */
  kReasonSubMulti = 8192, /* >1 contributing subscreen layer, or math config varies per line */
  kReasonMathBright = 16384, /* colour math in use while brightness < 15 (software clamps before brightness) */
  kReasonM7 = 32768,      /* mode 7 setup the GE path cannot draw (rotation, flips, charfill, varying matrix, ...) */
  kReasonCount = 16,
};

typedef struct BgSnap {
  uint16_t hscroll, vscroll, mapadr, tileadr;
  uint8_t flags;  /* 1 wider, 2 higher, 4 drawn on main, 8 enabled on subscreen */
  uint8_t wm, wl, wr;  /* main-screen window 1: wm 0 none, 1 visible only inside [wl,wr], 2 hidden inside [wl,wr] */
  uint8_t sm, sl, sr;  /* the same for the subscreen (TSW) */
} BgSnap;

typedef struct LineSnap {
  BgSnap bg[kLayers];
  uint16_t subfix;    /* gate 4t-c: fixed colour (5-5-5) added with a subscreen BG layer on this row, 0 = none */
  uint8_t subhalf;    /* gate 4t-d: this submath row uses half colour (BG3/OBJ subscreen, alpha-coded target) */
  uint8_t w2main;     /* gate 4u: a drawn main BG layer of this row is masked by window 2 */
  uint16_t cg0;       /* CGRAM entry 0 (backdrop); colour is baked per run in DrawGe */
  uint8_t forced;     /* forced blank: black */
  uint16_t reasons;
  uint8_t brightness, bg3prio, mathmask, submath;  /* submath: subscreen add may matter */
  uint8_t objon;                                   /* OBJ enabled on the main screen */
  uint8_t subneg, subwin;                          /* subtract mode; subscreen layers (bit l) windowed */
  uint8_t prevnoop;                                /* prevent-math mode set, but colour math has nothing to do (gate 4t-a) */
  uint8_t cwm, cwl, cwr;                           /* gate 4t-b: colour-window limit of the tint (as BgSnap.wm: 0 none, 1 math only inside [cwl,cwr], 2 only outside) */
  uint8_t tint, tinthalf;                          /* fixed-colour math on this row; result halved */
  uint16_t tintfix;                                /* fixed colour, 5-5-5 (R | G << 5 | B << 10) */
  uint8_t subobj;                                  /* OBJ on the subscreen and added by colour math */
  uint8_t owm, owl, owr;                           /* OBJ window on the main screen (as BgSnap.wm) */
  uint8_t mosmask, mossize, mosstart;              /* mosaic: BG1-3 mask (mode 1, size > 1), size, start line */
  uint8_t m7on, m7fl;                              /* mode 7 row with BG1 on; flags 1 large, 2 charfill, 4 xflip, 8 yflip, 16 extbg, 32 mosaic */
  int16_t m7m[8];                                  /* its matrix: a, b, c, d, centre x, y, scroll h, v */
} LineSnap;

typedef struct TileVert { float u, v; int16_t x, y, z; } TileVert;
typedef struct ColorVert { uint32_t color; int16_t x, y, z; } ColorVert;
typedef struct ColorVertF { uint32_t color; float x, y, z; } ColorVertF;
 CASSERT(sizeof(ColorVertF) == 16);
CASSERT(sizeof(TileVert) == 16);  /* u, v (2 x 4) + x, y, z (3 x 2) padded to the float alignment: the GE vertex size of kVtTile */
typedef struct Run { uint16_t r0, r1, first; } Run;
/* Gate 5h-c: BG and sprite quads (two vertices each) use a 10-byte vertex with 16-bit texture coordinates (texel units in
 * through mode, always integers here) and are written as five 32-bit words per quad instead of ten 16/32-bit stores
 * into the uncached display list. PSP_VTX16=0 restores the 16-byte float vertex. Mode 7, mosaic and composite quads
 * keep TileVert (fractional texture coordinates). */
#if SM_PSP_VTX16
typedef struct BgVert { uint16_t u, v; int16_t x, y, z; } BgVert;
#else
typedef struct BgVert { float u, v; int16_t x, y, z; } BgVert;
#endif
CASSERT(sizeof(BgVert) == (SM_PSP_VTX16 ? 10 : 16));
/* w must be 4-byte aligned: list and pool allocations are, and every emitter advances by whole quads (20 bytes). */
static inline void BgQuad(BgVert *w, int ul, int vt, int ur, int vb, int x0, int y0, int x1, int y1, int z) {
#if SM_PSP_VTX16
  uint32_t *p = (uint32_t *)w;  /* vertex 0: u v x y z, vertex 1: u v x y z = ten halfwords */
  p[0] = (uint32_t)(uint16_t)ul | (uint32_t)vt << 16;
  p[1] = (uint32_t)(uint16_t)x0 | (uint32_t)y0 << 16;
  p[2] = (uint32_t)(uint16_t)z | (uint32_t)ur << 16;
  p[3] = (uint32_t)(uint16_t)vb | (uint32_t)x1 << 16;
  p[4] = (uint32_t)(uint16_t)y1 | (uint32_t)z << 16;
#else
  w[0] = (BgVert){(float)ul, (float)vt, (int16_t)x0, (int16_t)y0, (int16_t)z};
  w[1] = (BgVert){(float)ur, (float)vb, (int16_t)x1, (int16_t)y1, (int16_t)z};
#endif
}
typedef struct Geom { BgVert *v; int n; } Geom;

static uint32_t g_gu_list[kListWords] __attribute__((aligned(16)));

/* Boot-time and benchmark lists (nothing recovers a frame there): a call the list cannot take is a Die before it writes,
 * never an overflow. Frame lists use GlStart + a recovery point instead (DrawGe, ComposeGeFrame, SwFrameList...). */
static void GuStartBoot(void) {
  if (GlStartFatal(g_gu_list, kListBytes) < 0)
    Die("sceGuStart failed");
}

#if SM_PSP_ASYNC
/* Async present. g_ge_pending: a frame is queued, so until GeWait() the GE may still read everything its two lists
 * reference (list memory of g_gu_list and g_gu_list2, CLUT arrays, atlases, layer images, g_m7img, g_texture) and
 * writes its draw buffer g_pend_off, which is flipped by GeWait(). The compose list of a queued frame is built
 * into g_gu_list2 while the GE still executes the scene list in g_gu_list. */
#if SM_PSP_ASYNC == 1
enum { kComposeListWords = 64 * 1024, kComposeListBytes = kComposeListWords * 4 };  /* 256 KB */
static uint32_t g_gu_list2[kComposeListWords] __attribute__((aligned(16)));
enum { kComposeCap = kComposeListBytes };
#else
enum { kComposeCap = kListBytes };  /* SM_PSP_ASYNC == 2: the compose commands are appended to the scene list */
#endif
static bool g_ge_pending;
static uint32_t g_pend_off;
/* Completion sentinel. The last draw of an async compose list writes a 4x1 marker into the unused columns 496..499 of the
 * 512 pixel wide draw buffer (the panel shows 480): GeWait() reads it back (uncached) before it flips the buffer. If the
 * GE sync returned before the compose list finished, the marker is missing: GeWait waits for it (bounded) and counts it. */
enum { kSentX = 496, kSentY = 0, kSentWaitUs = 4000 };
static uint16_t g_pend_sent;
static uint32_t g_sent_chk, g_sent_late, g_sent_fail;
#if SM_PSP_LOG
static uint32_t g_flip_d[4];  /* vblanks between the previous flip and this one (0, 1, 2, 3+): 0 = the pacing gate did not wait for a vblank */
#endif
static uint32_t g_frames_async;  /* frames queued */
static uint32_t g_async_nofit;   /* synchronous: the compose-list bound exceeds the list */
static uint32_t g_async_fail;    /* synchronous: sceGuStart could not enqueue the compose list */
static uint32_t g_async_viol;    /* compose lists that used more than their bound */
static bool g_async_ok = true;   /* cleared by such a list: every later frame is synchronous */
#if SM_PSP_LOG
static uint32_t g_comp_used_hw, g_comp_bound_hw, g_comp_call_hw;  /* log window: largest compose list / bound / implied command bytes per call */
#endif
#endif

/* Gate 5f: the big vertex buffers (tiles, sprites, mode 7) are built in this cached pool and written back once, instead
 * of inside the display list (sceGuStart maps the list uncached: every vertex there is four uncached stores).
 * Commands and the small colour buffers stay in the list. SM_PSP_VTX_POOL=0 restores the old behaviour. */
#ifndef SM_PSP_VTX_POOL
#define SM_PSP_VTX_POOL 0
#endif
enum { kVtxPoolBytes = 768 * 1024, kVtxPadBytes = 8 * 1024 };  /* pad: <= 99 allocations rounded up to 64 B */
#if SM_PSP_VTX_POOL
static uint8_t g_vtx_pool[kVtxPoolBytes] __attribute__((aligned(64)));
static uint32_t g_vtx_top;
static void *VtxAlloc(uint32_t bytes) {
  bytes = (bytes + 63u) & ~63u;
  if (g_vtx_top + bytes > (uint32_t)kVtxPoolBytes)
    Die("vertex pool overflow");  /* cannot happen: DrawGe checks the frame against the pool before the list is opened */
  void *p = g_vtx_pool + g_vtx_top;
  g_vtx_top += bytes;
  return p;
}
#define VTX_RESET() (g_vtx_top = 0)
#define VTX_FLUSH() do { if (g_vtx_top) sceKernelDcacheWritebackRange(g_vtx_pool, g_vtx_top); } while (0)
#else
#define VtxAlloc(bytes) sceGuGetMemory(bytes)
#define VTX_RESET() ((void)0)
#define VTX_FLUSH() ((void)0)
#endif
static uint8_t *g_atlas2, *g_atlas4;        /* uncached VRAM tile atlases */
static uint16_t *g_texture;                 /* uncached VRAM: 565 texture, or the 8888 copy target (256x256x4 = same bytes) */
/* Software-PPU frame, 256x256 8888 (rows 240-255 unused). */
static uint8_t g_framebuffer[kGameWidth * 256 * 4] __attribute__((aligned(64)));

/* Post-composition mosaic block size (1 = off), set by the menu transitions. */
static uint8_t g_mosaic_size = 1;

static LineSnap g_lines[kMaxLines];
static int g_line_count;
static uint16_t g_frame_reasons;
#if SM_PSP_LOG
/* Every distinct register signature that sends a frame to software is logged once (up to 48). */
extern uint8_t g_ram[0x20000];  /* sm_rtl.c; log only, read bytewise (no unaligned loads) */
static inline unsigned GameRam16(unsigned a) { return g_ram[a] | (unsigned)g_ram[a + 1] << 8; }
typedef struct Sig { uint32_t a, b, c, d; } Sig;  /* d: game state, so the same registers in another scene are logged again */
static Sig g_sigs[48];
static int g_nsigs;
static bool NewSig(const Ppu *p, uint16_t reasons) {
  uint32_t mm = 0;
  for (int i = 0; i < 6; i++)
    mm |= (uint32_t)(p->mathEnabled[i] ? 1 : 0) << i;
  Sig s;
  s.a = reasons | (uint32_t)p->mode << 16 | (uint32_t)p->screenEnabled[0] << 20;
  s.b = (uint32_t)p->screenEnabled[1] | (uint32_t)(p->addSubscreen ? 1 : 0) << 8 | (uint32_t)(p->halfColor ? 1 : 0) << 9 |
        (uint32_t)(p->subtractColor ? 1 : 0) << 10 | (uint32_t)p->preventMathMode << 11 | (uint32_t)p->clipMode << 13 |
        (uint32_t)p->screenWindowed[0] << 15 | (uint32_t)p->screenWindowed[1] << 23;
  s.c = p->windowsel | mm << 24;
  s.d = GameRam16(0x998);
  for (int i = 0; i < g_nsigs; i++)
    if (g_sigs[i].a == s.a && g_sigs[i].b == s.b && g_sigs[i].c == s.c && g_sigs[i].d == s.d)
      return false;
  if (g_nsigs == 48)
    return false;
  g_sigs[g_nsigs++] = s;
  return true;
}
#endif
static bool g_frame_dead;  /* a fallback reason is already known: stop capturing this frame */
static bool g_frame_subnoop;
static uint32_t g_frames_subnoop;
static Run g_subruns[kMaxLines];
#if SM_PSP_LOG
typedef struct Note { int line; unsigned bits, mode, tm, ts, mathmask, add, sub, half, fixed, clip, prevent, tmw, wsel, tsw, gs, room, area; } Note;
static Note g_note;
static bool g_note_pending;
#endif
static uint32_t g_capture_gaps;
/* Per-line capture reuse: a line whose PPU registers (Ppu.regGen) and OAM address equal the previous line's copies that
 * line's snapshot and re-reads only BG scroll and CGRAM entry 0. Log builds recompute every 16th frame and compare. */
static bool g_cap_reuse_ok = SM_PSP_CAP_REUSE;
static bool g_cap_win_ok = SM_PSP_CAP_WIN;  /* cleared if a window-edge-only line differs from a full capture */
static uint32_t g_cap_gen, g_cap_oamadr;
static uint8_t g_cap_win[4];  /* window 1 left/right, window 2 left/right as of the last full or window-refreshed capture */
#if SM_PSP_LOG
static uint32_t g_cap_n_reuse, g_cap_n_full, g_cap_n_chk, g_cap_n_bad, g_cap_n_win;
#endif
static uint16_t g_vram[0x8000] __attribute__((aligned(16)));
static uint16_t g_cgram[256] __attribute__((aligned(16)));
#if SM_PSP_DOOR_GE
/* Door transition palette: which CGRAM the GE path draws with. The game fades the real palette to black before it
 * loads the next room; the transition shows the old room with the palette it had (outgoing) and then the new room with
 * its target palette (dest), neither of which the software PPU sees, so GE frames are not compared with it meanwhile. */
enum { kDoorPalGame, kDoorPalCapture, kDoorPalOutgoing, kDoorPalDest };
static int g_door_pal_mode = kDoorPalGame;
static uint16_t g_door_pal_out[256] __attribute__((aligned(16)));
static const uint16_t *g_door_pal_dest;
#endif
static inline const uint16_t *CgramSrc(const Ppu *p) {
#if SM_PSP_DOOR_GE
  if (g_door_pal_mode == kDoorPalOutgoing)
    return g_door_pal_out;
  if (g_door_pal_mode == kDoorPalDest && g_door_pal_dest)
    return g_door_pal_dest;
#endif
  return p->cgram;
}

static uint16_t g_oam[256];
static uint8_t g_hoam[32];
static uint8_t g_obj_size, g_obj_start;
static uint16_t g_obj_adr1, g_obj_adr2;
static bool g_obj_rot;
typedef struct Sprite { int16_t x; uint8_t y, size; uint16_t attr; } Sprite;
typedef struct ObjSeg { uint8_t pal; uint16_t first, n; } ObjSeg;  /* first/n in vertices */
typedef struct ObjBand { uint16_t r0, r1; uint8_t wm, wl, wr; } ObjBand;  /* rows of one OBJ window state */
static Sprite g_spr[128];                 /* in precedence order (index k wins over k+1) */
static int g_nspr, g_spr_quads;
static ObjSeg g_objseg[128 * kMaxObjBands * 2];  /* runs of consecutive sprites with one palette (per band and span) */
static int g_nobjseg;
static ObjBand g_objband[kMaxObjBands];
static int g_nobjband;
static BgVert *g_objbase;
static uint32_t g_clutobj[16][8][16] __attribute__((aligned(64)));
static uint16_t g_clutobj_stamp[16];

static uint32_t g_hash2[4096], g_hash4[2048], g_hash7[256];
static uint16_t g_seen7[256];
static uint8_t g_empty7[256];
static uint8_t *g_atlas7;
static uint32_t g_clut7[256] __attribute__((aligned(64)));
static uint16_t g_clut7_stamp;
static uint8_t *g_vram_u;     /* uncached VRAM base */
static uint32_t g_draw_off;   /* VRAM offset of the buffer the GE draws into (toggles on every swap) */
static const uint32_t kBufOff[3] = {0, kBufBytes, kVramBuf2};
static bool g_triple;         /* three frame buffers in rotation */
static int g_draw_i, g_prev_i = 1;  /* buffer being drawn / the one queued by the previous swap (on screen by now) */

/* Triple buffering replaces sceGuSwapBuffers, so every list names its draw buffer itself. */
static void SelectDrawBuffer(void) {
  if (g_triple)
    sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)g_draw_off, 512);
}

void PspRenderer_SetMosaicTransition(unsigned size) {
  g_mosaic_size = (uint8_t)(size < 1 ? 1 : size > 16 ? 16 : size);
}

unsigned PspRenderer_GetMosaicTransition(void) {
  return g_mosaic_size;
}

/* Mode 7 (GE path): matrix and flags are snapshotted per row (LineSnap). Frames with one constant matrix, b = c = 0,
 * a, d > 0, large field only (no flips/charfill/mosaic) are drawn as tile quads (M7Prepare). Everything else
 * (rotation, HDMA matrix, flips, charfill, mosaic, small field) is sampled on the CPU into an 8-bit image (gate 4r). */
typedef struct M7Cfg { int16_t m[8]; uint8_t fl; } M7Cfg;
static M7Cfg g_m7c;           /* first mode 7 row of the frame */
static int g_m7_rows;         /* rows of this frame in mode 7 with BG1 on */
static bool g_m7_same;        /* all of them share g_m7c */
static bool g_m7q_ok = true;  /* cleared if the first verified tile-quad mode 7 frames differ from software */
static bool g_m7i_ok = true;  /* the same for the image path */
static int g_frame_mode = -1;        /* BG mode of this frame (first line that is not forced blank) */
static bool g_m7_ok = true;          /* cleared if the texture-coordinate self test fails */
/* Gate 5q: small-field mode 7 (the plane wraps) drawn as tile quads; own audit, falls back to the image path. */
static bool g_m7w_ok = true, g_m7w_frame;
static uint32_t g_m7w_vn, g_m7w_vbad, g_frames_m7w;
enum { kM7WVerify = 8 };
#ifndef SM_PSP_GE_WIN
#define SM_PSP_GE_WIN 1  /* 1: window-1 masks on BG1-3 / OBJ are drawn by the GE, 0: those frames stay in software */
#endif
static bool g_win_ok = SM_PSP_GE_WIN;  /* cleared if the first verified window frames differ from software */
static uint32_t g_win_vn, g_win_vbad, g_win_rows;
enum { kWinVerify = 8 };
/* Gate 4t-a: prevent-math modes 1/2 on rows where colour math has nothing to add are drawn by the GE. */
static bool g_prev_ok = true;  /* cleared if the first verified such frames differ from software */
static int g_prev_rows;        /* rows of this frame that rely on the no-op rule */
static uint32_t g_prev_vn, g_prev_vbad, g_frames_prev;
enum { kPrevVerify = 8 };
/* Gate 4t-b: a fixed-colour tint limited by one colour window (W1 or W2, prevent-math mode 1/2) is drawn by the GE as
 * one rectangle per row span. */
static bool g_cw_ok = true;  /* cleared if the first verified such frames differ from software */
static int g_cw_rows;        /* rows of this frame whose tint is limited by the colour window */
static uint32_t g_cw_vn, g_cw_vbad, g_frames_cw;
enum { kCwVerify = 8 };
/* Gate 4t-c: subscreen BG layer added together with a non-zero fixed colour (X-ray visor), drawn via the multisub target. */
static bool g_sf_ok = true;  /* cleared if the first verified such frames differ from software */
static bool g_sf_frame;      /* this frame is drawn by that path */
static uint32_t g_sf_vn, g_sf_vbad, g_frames_sf;
/* Gate 5b: subfix frames whose fixed colour is 0 (subscreen layer masked by window 2) have their own audit budget and
 * switch, so the first frames with a real fixed colour cannot use up their verification. */
static bool g_sf0_ok = true, g_sf_zero;
static uint32_t g_sf0_vn, g_sf0_vbad;
static int g_sf_r0, g_sf_r1, g_sf_masks;  /* log only */
static uint16_t g_sf_col;
enum { kSfVerify = 8 };
/* Gate 5e: subfix frames with OBJ on the subscreen (sprites composited in the multisub target over the fixed colour).
 * Own audit and switch: a failure only sends those frames back to software. */
static bool g_sfo_ok = true, g_sfo_frame;
static uint32_t g_sfo_vn, g_sfo_vbad, g_frames_sfo;
/* Gate 4t-d: X-ray visor with half colour: window 2 on BG1/BG2 per row, colour window (prevent mode 2), subscreen BG3
 * added half-and-half where it has a pixel, fixed colour added where it has none (alpha-coded target). */
static bool g_sh_ok = true;  /* cleared if the first verified such frames differ from software */
static bool g_sh_frame;      /* this frame is drawn by that path */
static uint32_t g_sh_vn, g_sh_vbad, g_frames_sh;
enum { kShVerify = 8 };
/* Gate 5c: the same in subtract mode (water-room visor): max(main - sub, 0) / 2 where BG3 has a pixel, main - fixed
 * elsewhere. Own audit and switch, so a failure keeps the add-mode visor path. */
static bool g_shn_ok = true, g_shn_frame;
static uint32_t g_shn_vn, g_shn_vbad, g_frames_shn;
/* Gate 4u: half colour with OBJ on the subscreen (own audit, so a failure keeps the visor path), and window 2 on
 * main BG layers outside the half-colour frames (per-row spans of gate 4t-d). */
static bool g_shobj_ok = true, g_shobj_frame;
static uint32_t g_sho_vn, g_sho_vbad, g_frames_sho;
static bool g_w2_ok = true, g_w2_frame;
static uint32_t g_w2_vn, g_w2_vbad, g_frames_w2;
enum { kW2Verify = 8 };
/* Gate 4p: lines with more than 32 sprites or 34 slivers are drawn on the GE with the hardware drop rule (ppu.c). */
static bool g_ovf_ok = true;  /* cleared if the first verified overflow frames differ from software */
static int g_ovf_rows;        /* rows of the sprite range this frame that exceed the limits */
static uint32_t g_ovf_vn, g_ovf_vbad, g_frames_ovf, g_ovf_watch, g_ovf_arms;
enum { kOvfVerify = 8, kOvfWatch = 12, kOvfArm = 3, kOvfArms = 3 };
static int16_t g_row_lastk[256];  /* per row: last kept sprite (index in g_spr), 32767 = row not limited */
static uint8_t g_row_lastc[256];  /* per row: slivers kept of that sprite, in screen column order */
static bool g_obj_dry;           /* EmitQuad8 only counts (ObjCountQuads) */
static int g_obj_dry_n;
/* Gate 4q: mosaic on BG1-3 (mode 1, main screen). The layer is sampled on the CPU at the block origins into a
 * 128x128 8888 image per tile priority; the GE stretches it (nearest) over each run. */
enum { kMosDim = 128, kMosVerify = 16, kMosMinBright = 6, kMosHiBright = 12, kMosHiVerify = 8,
       kMosImgBytes = kMosDim * kMosDim * 4 };
CASSERT(kVramTint >= kVramImgEnd && (kVramTint & 63) == 0);  /* the strip lies behind every other allocation, 64-byte aligned */
CASSERT(kVramTintEnd <= 2 * 1024 * 1024);                       /* inside the default 2 MB eDRAM */
typedef struct MosLayer { bool on, any[2]; int size, start; TileVert *v; } MosLayer;
static MosLayer g_mos[kLayers];
#if SM_PSP_MOS_GE
CASSERT(kLayers * kMosImgBytes <= kSubBytes);  /* one source image per layer fits the subscreen target's VRAM */
static const uint8_t kMosTag[2] = {0x40, 0xc0};  /* stencil (= alpha) tag of tile priority 0 / 1 in a source image */
#else
static uint32_t g_mosd[kLayers][2][kMosDim * kMosDim] __attribute__((aligned(64)));  /* 384 KB */
#endif
static bool g_mos_ok = true;  /* cleared if the first verified mosaic frames differ from software */
static bool g_mos_frame;      /* this frame has a mosaic layer on the GE */
static int g_mos_size;
static uint32_t g_mos_vn, g_mos_vnhi, g_mos_vbad, g_frames_mos;
/* Brightness of the middle row. A fade frame close to black cannot tell a wrong source image from a right one. */
static inline int MosAuditBright(void) {
  return g_line_count ? g_lines[g_line_count / 2].brightness : 0;
}
static float g_uvh[2] = {0.5f, 0.5f}; /* texture coordinate offset in pixels at the sample point (x, y) */
static bool g_verify_now, g_audit_now;
static bool g_multisub_ok = true;   /* cleared if the first verified multi-layer subscreen frames differ from software */
static bool g_frame_multisub, g_want_ms_verify;
/* Spacing of the software-PPU verification frames (see EndDraw): a verification frame costs 25-65 ms, and the first frames of
 * a new class (kM7BVerify = 24 for the Mode 7 block path, 8 for most others) used to run on every second frame, which is a
 * pause of about a second. */
static uint32_t g_verify_next, g_verify_armed, g_verify_deferred;
static uint32_t g_ms_vn, g_ms_vbad;
static uint32_t g_bright_rows, g_br_vn;  /* rows drawn through the brightness multiply this frame; verified frames that had some */
enum { kBrightVerify = 8 };
enum { kAuditFrames = 600 };  /* log builds: every this many frames the software PPU also draws, and the GE image is compared with it */
static uint32_t g_m7_vn, g_m7_vbad, g_m7_vnear;
static uint32_t g_m7_bigv, g_m7_bigbad;  /* verified frames above kM7BaseQuads, and their bad pixels */
static bool g_m7big_ok = true;           /* cleared if those frames differ from software */
enum { kM7Verify = 20, kM7MaxQuads = 7680, kM7BaseQuads = 4096, kM7BigVerify = 6 };
typedef struct M7Quad { int16_t x0, x1, y0, y1; float u0, u1, v0, v1; } M7Quad;
static M7Quad g_m7q[kM7MaxQuads];
static int g_m7_nq;
static uint32_t g_m7_yc;  /* Y0 of the frame (for the verification log) */
static TileVert *g_m7v;
/* Gate 4r image path: 8-bit plane colour index per screen pixel (row = screen row), 0 = transparent. */
static uint8_t g_m7img[256 * 256] __attribute__((aligned(64)));
static bool g_m7_img;              /* this frame's mode 7 comes from g_m7img */
static int g_m7_nrng;              /* row ranges drawn from it */
static uint16_t g_m7rng[kMaxLines][2];
static uint32_t g_m7i_vn, g_m7i_vbad, g_frames_m7i;
/* Gate 5w: mode 7 drawn from a swizzled plane with one texture window per block of rows (M7PrepareBlk). PSP_M7_BLK=0
 * (default) compiles none of the plane/prepare code; the CPU image path (g_m7img) takes every frame the block path does not. */
#ifndef SM_PSP_M7_BLK
#define SM_PSP_M7_BLK 0
#endif
static bool g_m7_blk;               /* this frame's mode 7 is drawn from the plane (g_m7_img is set too) */
static int g_m7_bnseg, g_m7_bnblk;  /* row segments and window blocks of this frame */
#if SM_PSP_M7_BLK
/* The plane is 1024 x 1024 plus 128 pixels of its own start repeated at the right and at the bottom (kM7PlaneP = 1152, 72
 * swizzle blocks of 16 B per block row), so a window of up to kM7WinMax pixels that crosses the edge of a wrapping plane reads
 * the repeated texels instead of needing a second bind. */
enum { kM7PlaneW = 1024, kM7PlaneP = 1024 + 128, kM7WinMax = 128, kM7MaxBlk = 256, kM7BVerify = 24, kM7PlFull = 32, kM7BlkBackoff = 8 };
typedef struct M7BSeg { float u0, v0, u1, v1; int16_t x0, x1, y, pad; } M7BSeg;
typedef struct M7BBlk { int16_t wx, wy; uint16_t first, n; } M7BBlk;
static uint8_t g_m7pl[kM7PlaneP * kM7PlaneP] __attribute__((aligned(64)));  /* swizzled in 16 x 8 blocks, pitch kM7PlaneP */
static bool g_m7pl_valid;
static uint8_t g_m7pl_dirty[64];  /* VRAM pages 0-63 written since the plane was last brought up to date */
/* The pixels of the 256 mode 7 tiles (the high bytes of the tile words), kept in step with g_vram so that a plane cell is a copy
 * of 64 bytes. g_m7pl_fast = false: cells are converted from g_vram word by word as before (set if a log build finds a difference). */
static uint8_t g_m7t8[256 * 64] __attribute__((aligned(16)));
static bool g_m7pl_fast = true;
/* The tile each map cell of the plane shows (the low bytes of g_vram, as of the last update), so that a written page only
 * rebuilds the cells whose tile number or tile pixels really changed. g_m7pl_incr = false: every cell of a written page and
 * every cell showing one of its tiles is rebuilt (set if a log build finds a difference). */
static uint8_t g_m7map[128 * 128];
static bool g_m7pl_incr = true;
#ifndef SM_PSP_VERIFY
#define SM_PSP_VERIFY 1
#endif
static M7BSeg g_m7bseg[kMaxLines * 8];  /* at most one segment per row and 32 pixels */
static M7BBlk g_m7bblk[kM7MaxBlk];
static bool g_m7b_ok = true;
static uint32_t g_m7b_vn, g_m7b_vbad, g_frames_m7b;
#if SM_PSP_LOG
enum { kBlkOk, kBlkOff, kBlkSame, kBlkFlags, kBlkWindow, kBlkReasons };
static uint32_t g_m7b_why[kBlkReasons], g_m7b_cfg[3];
static uint32_t g_m7b_t_prep, g_m7b_t_plane, g_m7b_t_vtx, g_m7b_t_draw, g_m7b_tn;  /* microseconds summed over g_m7b_tn block frames */
#define BLK_WHY(r) (g_m7b_why[r]++)
#else
#define BLK_WHY(r) ((void)0)
#endif
#endif
enum { kM7IVerify = 8 };
/* Gate 5t: row reuse for the mode 7 image. Row r of g_m7img is a pure function of the row number, the row's mode 7 state
 * (matrix, flags, and mosaic size/start when the mosaic flag is set) and g_vram words 0..0x3fff (tile map in the low
 * bytes, tile pixels in the high bytes; tile * 64 + 63 <= 0x3fff), which are the first kM7Pages VRAM pages. A row whose key
 * equals the one it was built with, with no write to those pages since (g_m7i_stale), keeps its pixels. Exact by
 * construction; log builds re-render every 16th reusing frame and compare. PSP_M7_REUSE=0 renders every row every frame. */
#ifndef SM_PSP_M7_REUSE
#define SM_PSP_M7_REUSE 1
#endif
enum { kM7Pages = 64 };
typedef struct M7RowKey { uint8_t on, fl, ms, mt; int16_t m[8]; } M7RowKey;
static M7RowKey g_m7ikey[kMaxLines];
static bool g_m7i_stale = true;  /* a page mode 7 reads was copied since the keys were stored: no stored row is valid */
static bool g_m7i_reuse_ok = SM_PSP_M7_REUSE;
/* VRAM pages the mode 7 plane reads were copied into g_vram: row reuse (5t) and the plane (5w) must catch up. */
static inline void M7VramDirty(int pg0, int pg1) {
  if (pg0 >= kM7Pages)
    return;
  g_m7i_stale = true;
#if SM_PSP_M7_BLK
  for (int pg = pg0; pg < pg1 && pg < kM7Pages; pg++)
    g_m7pl_dirty[pg] = 1;
#endif
}
#if SM_PSP_LOG
static uint32_t g_m7i_built, g_m7i_kept, g_m7i_audits, g_m7i_bad, g_m7i_auditn;
static uint32_t g_m7i_stalef;  /* image frames that had to drop every stored row because a mode 7 VRAM page was written */
/* PSP_M7_PROBE=1 (log builds): every 4th rendered row is rendered a second time into a scratch row straight away. The first
 * render is cold (its tile map and tile data come from RAM), the second warm (they are in the data cache), so
 * cold - warm is the time lost to cache misses and warm is the instruction cost. The probe is excluded from m7img cpu. */
#ifndef SM_PSP_M7_PROBE
#define SM_PSP_M7_PROBE 0
#endif
static uint32_t g_m7p_rows, g_m7p_cold, g_m7p_warm;
#endif
static uint16_t g_seen2[4096], g_seen4[2048];
static uint8_t g_empty2[4096], g_empty4[2048];
static uint16_t g_mask2[4096], g_mask4[2048];  /* bit i set: tile uses pixel value i */
static uint32_t g_ver2[4096], g_ver4[2048];    /* gate 5l: bumped whenever a tile's decoded data changes */
static uint16_t g_stamp;
static uint32_t g_clut2[16][8][8] __attribute__((aligned(64)));
static uint32_t g_clut4[16][8][16] __attribute__((aligned(64)));
static uint16_t g_clut2_stamp[16], g_clut4_stamp[16];
static uint32_t g_clut2h[8][8] __attribute__((aligned(64)));  /* gate 4t-d: BG3 on the half-colour target, opaque = alpha 0x80 */
static uint16_t g_clut2h_stamp;

static Run g_runs[kLayers][kMaxLines];
static int g_nruns[kLayers];
static int g_counts[kLayers][kMaxLines][2][8];
static Geom g_geom[kLayers][kMaxLines][2][8];

// One non-empty tile of a layer run: destination before clipping, atlas tile, palette and flags. Built once per run and shared
typedef struct TileEnt { int16_t x, y; uint16_t pt; uint8_t pal, fl; } TileEnt;  /* fl: 1 prio, 2 hflip, 4 vflip */
typedef struct TileList { uint16_t first, n; } TileList;
enum { kEntPool = 16384 };  /* gate 5d: a tile entry is 8 B; stays above kMaxTilesPerFrame */
static TileEnt g_ents[kEntPool];
static int g_ent_top;
/* Gate 4t-d: visible x spans of every row of a layer (window 2 with per-row HDMA edges), and per tile entry the
 * class (hidden / fully visible / split per row) found while counting. Both only used on half-colour visor frames. */
typedef struct RowWin { int16_t a[2], b[2]; uint8_t n; } RowWin;
static RowWin g_rw[kLayers][kMaxLines];
static uint8_t g_tkind[kEntPool];
/* Tile-row cache (gate 4k): per frame, the non-empty tiles of one tilemap row (x set, y left 0), keyed by everything
 * that selects them except vscroll. HDMA wave effects change vscroll every 2 rows, so short runs hit the same
 * rows over and over. */
typedef struct RowCache { uint16_t map, tile, hs, stamp, first, n; uint8_t l, wh, ty; } RowCache;
enum { kRowPool = 4096, kRowSlots = 256, kRowMaxRun = 16 };
static TileEnt g_rows[kRowPool];
static int g_rows_top;
static RowCache g_rc[kRowSlots];
static TileList g_mlist[kLayers][kMaxLines];  /* per main run */

// Subscreen blend: runs of the one contributing subscreen layer.
typedef struct BRun { uint8_t l; uint16_t first, r0, r1; TileList lst; } BRun;
static BRun g_bruns[kMaxLines + 1];
static int g_nbruns;
static int g_bcounts[kMaxLines + 1][2][8];
static Geom g_bgeom[kMaxLines + 1][2][8];
static int g_zbg[kLayers][2];  /* depth marker per BG layer/priority pass */
static uint32_t g_frames_blend, g_frames_tint;
/* Depth markers: z = SNES priority level * kZLevel (see DrawGe); sprites add 127-k; backdrop is below level 1. */
enum { kZLevel = 256, kZBackdrop = 64, kClutBright = 15 };  /* kClutBright: all CLUTs/backdrop are baked at 15; brightness is a final multiply */
/* Rows [r0,r1) whose main-screen pixels get the fixed colour added/subtracted (optionally halved), limited to the
 * colour-window state cwm/cwl/cwr (gate 4t-b). v0/vn: its ColorVerts in the tint vertex buffer. */
typedef struct TintRun { uint16_t r0, r1, fix, v0; uint8_t half, cwm, cwl, cwr, vn; } TintRun;
static TintRun g_tint[kMaxLines];
static int g_ntint;
/* Consecutive tint rows with the same layer mask and add/subtract mode (HDMA can change both inside a frame).
 * Each range gets its own overlay and its own restore pass, limited to its rows with the scissor. */
typedef struct TintRange { uint16_t r0, r1, first, n; uint8_t mask, neg; } TintRange;
enum { kMaxTintRanges = 6 };
static TintRange g_trange[kMaxTintRanges];
static int g_ntr;
static int g_scissor_mode;  /* from the boot self test: 0 unknown, 1 = (x, y, x2, y2), 2 = (x, y, w, h) */
static bool g_glow_ok;      /* built with PSP_GE_GLOW and the eDRAM holds the glow buffers (set at start-up) */
static int g_glow_obj_n;    /* sprites of this frame's scene in g_spr (0 when the scene has none); the glow emitters */
static bool g_tint_ok;      /* smooth tint enabled at build time and the eDRAM holds the strip region (set at start-up) */
static bool g_stint_frame;  /* this frame's tint is the smooth (dithered strip) one; GeVerify logs it but keeps the strict tolerance */
/* Gate 4s: rows of one subscreen colour-math mask. The blend is drawn once; each band restores its own layers. */
typedef struct BlendBand { uint16_t r0, r1; uint8_t mask; } BlendBand;
enum { kMaxBlendBands = 6, kBandVerify = 8 };
static BlendBand g_bband[kMaxBlendBands];
static int g_nbband;
static bool g_band_ok = true;  /* cleared if the first verified multi-band frames differ from software */
static uint32_t g_bnd_vn, g_bnd_vbad;

static bool g_ge_ok;
static bool g_ge_active;
static int g_ge_level;  /* 0 software, 1 GE, 2 GE without tint (tint frames go back to software) */
#ifndef SM_PSP_GE_DEFAULT
#define SM_PSP_GE_DEFAULT 1  /* 1: boot into GE + tint (when VRAM allows), 0: boot into software */
#endif
static unsigned g_vcount_last;
static uint32_t g_frame;
static int g_swpath = SM_PSP_SW_PATH;  /* -1 calibrating, 0 CPU 565 convert, 1 GE copy to VRAM */
enum { kSwPaths = 2, kSwCalFrames = 30, kSwCalSkip = 3 };
static uint32_t g_cal_frames, g_cal_sum[kSwPaths], g_cal_n[kSwPaths];
static uint32_t g_frames_ge, g_frames_sw, g_reason_counts[kReasonCount];

//profiling

#ifndef SM_PSP_SKIP_AFTER
#define SM_PSP_SKIP_AFTER 8
#endif
enum { kSkipAfter = SM_PSP_SKIP_AFTER };  /* consecutive GE frames before the software PPU is skipped (after a refusal the software PPU
                                           * draws every frame until then: about 35 ms each on a PSP-1000) */
static uint32_t g_streak;
static int g_last_reason;  /* mask of the last CountReasons call; cleared when DrawGe starts (log only) */
static uint32_t g_t_draw_start, g_t_list_start;
static uint32_t g_prep_us, g_build_us, g_gewait_us, g_vbwait_us;
#if SM_PSP_LOG
/* gewait split: the scene list (until the GE is idle after sceGuFinish), ComposeGeFrame (scale + HUD, own list and sync) and
 * ApplyMosaicTransition, summed over g_gws_n frames since the last log line. */
static uint32_t g_gws_scene, g_gws_compose, g_gws_mosaic, g_gws_n;
#endif

#if SM_PSP_LOG
enum { kLateUs = 17000, kMissUs = 33800, kHitchUs = 24000, kHitchPerWindow = 24 };

typedef struct Stat { uint32_t n, sum, max; } Stat;
typedef struct Prof {
  Stat game, ppu, prep, build, gewait, vbwait, period;
  Stat agewait, avbwait;                /* async: GE wait / vblank wait of the frame queued by the previous EndDraw (its completion, not this frame's work); GE frame bucket only */
  Stat cap, cpy, cls, bg, obj, emit;    /* capture hook (and its VRAM copy); prep split: classify, BG, OBJ; list vertices */
  Stat tseen, tdec, draws, clut, verts, m7i; /* per-frame counters; m7i: CPU time of the mode 7 image */
  uint32_t late, miss, hold;
} Prof;
static Prof g_prof[3];  /* 0: GE frame, 1: software frame while GE is on, 2: software mode */
static const char *const kProfName[3] = {"ge", "swfb", "off"};
static uint32_t g_ppu_us, g_ppu_t0, g_cap_us, g_cpy_us, g_m7i_us;  /* software PPU draw / capture / VRAM-copy / mode 7 image time this frame */
static uint32_t g_pc_class, g_pc_bg, g_pc_obj, g_pc_emit; /* prep phases this frame; emit: list start -> vertices written */
static uint32_t g_rc_hit, g_rc_miss;                      /* tile-row cache lookups since the last log line */
static uint32_t g_c_tseen, g_c_tdec, g_c_draws, g_c_clut, g_c_verts;
static uint32_t g_t_prev_end, g_holds;
static uint32_t g_hitch_n;
static bool g_hit_verified;  /* this frame was compared with the software image (audit frame) */

/* Frame-rate diagnostic: per-window and whole-run frame-time histogram, the time spent over one vblank by cause, and a summary
 * of every scene (game state, room, cinematic function). Log output only. */
enum { kDgBins = 6, kDgCause = 7, kVblankUs = 16683 };
typedef struct DgAcc {
  uint32_t n, max, bin[kDgBins];
  uint64_t sum, lost[kDgCause];  /* us; lost = time beyond one vblank, by the first matching cause */
} DgAcc;
typedef struct DgScene {
  uint32_t f0, n, worst, slow;
  uint64_t sum;
  unsigned gs, room, cin, area;
} DgScene;
static DgAcc g_dg_win, g_dg_all;
static DgScene g_dg_sc;
static uint8_t g_dg_why;     /* why the software PPU draws (or not) in the NEXT frame: 0 skipped, 1 streak, 2 overflow watch, 3 verify, 4 audit */
static uint16_t g_dg_vclass; /* which verification classes asked for this frame to be compared (0 with ver=1: another class) */

static inline uint32_t Now(void) {
  return sceKernelGetSystemTimeLow();
}

static void StatAdd(Stat *s, uint32_t v) {
  s->n++;
  s->sum += v;
  if (v > s->max)
    s->max = v;
}

static void ProfPpuBegin(void) { g_ppu_t0 = Now(); }
static void ProfPpuEnd(void) { g_ppu_us += Now() - g_ppu_t0; }
#else
static inline uint32_t Now(void) {
  return 0;
}
#endif

//color

static uint32_t BakeColor(uint16_t c, int brightness, bool transparent) {
  int r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
  uint32_t r = (uint32_t)(((r5 << 3) | (r5 >> 2)) * brightness / 15);
  uint32_t g = (uint32_t)(((g5 << 3) | (g5 >> 2)) * brightness / 15);
  uint32_t b = (uint32_t)(((b5 << 3) | (b5 >> 2)) * brightness / 15);
  return (transparent ? 0u : 0xff000000u) | b << 16 | g << 8 | r;
}

/* Texture state every scene pass (BG, OBJ, mode 7, restore passes) assumes; set at the top of the scene list. A pass that
 * changes any of it re-establishes all of it afterwards. Texture mode and image are bound by each pass itself. */
static void SceneTexState(void) {
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);
}

/* Gate 5t: smooth tint. The SNES fixed colour is 5 bits per channel and changes in steps (HDMA bands), and the main target
 * is RGB565, so a plain Gouraud ramp is quantised back to the same bands. Instead the ramp is built here as an 8x256 8888
 * strip: row y holds the fixed colour of tint row y, blended between neighbouring colour bands over min(len) rows around
 * each band boundary (plateaus stay exact), and ordered-dithered (4x4 Bayer) between the two nearest 5-bit levels. Every
 * texel is therefore a colour the PPU could have produced (BakeColor of an integer 5-5-5 value); flat bands are bit-exact.
 * The GE adds/subtracts the strip with the unchanged blend setup. A colour band is a span of consecutive rows with the
 * same fixed colour, whatever the colour window does to those rows (a run is split when its window edges change, a band is
 * not), and bands of one half-colour group within one range are blended with each other only. The strip is indexed by
 * row only; the colour window clips the sprites that sample it.
 * Called before the list is opened: the GE is idle, the strip is written through the uncached VRAM alias. */
static void TintStripBuild(void) {
  static const uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  static int logged, glogged;
  static uint16_t by0[kMaxLines], by1[kMaxLines], bfix[kMaxLines];  /* colour bands of one group */
  uint32_t *strip = (uint32_t *)(g_vram_u + kVramTint);
  int rows = 0, bands_total = 0, maxstep = 0;
  for (int k = 0; k < g_ntr; k++) {
    int end = g_trange[k].first + g_trange[k].n;
    for (int i = g_trange[k].first; i < end;) {
      int j = i + 1;
      while (j < end && g_tint[j].half == g_tint[i].half)
        j++;
      int nb = 0;  /* merge consecutive runs with the same fixed colour into bands */
      for (int q = i; q < j; q++) {
        const TintRun *c = &g_tint[q];
        if ((int)c->r1 <= (int)c->r0)
          continue;
        if (nb && by1[nb - 1] == c->r0 && bfix[nb - 1] == c->fix) {
          by1[nb - 1] = c->r1;
        } else if (nb < kMaxLines) {
          by0[nb] = c->r0;
          by1[nb] = c->r1;
          bfix[nb++] = c->fix;
        }
      }
      bands_total += nb;
      for (int q = 0; q < nb; q++) {
        int len = (int)by1[q] - (int)by0[q];
        int ra = by0[q], rb = by1[q];
        int y0 = ra < 0 ? 0 : ra, y1 = rb > kTintTexH ? kTintTexH : rb;  /* never write outside the 8x256 strip */
        if (len <= 0 || y1 <= y0)
          continue;
        int fx = bfix[q];
        int pfx = 0, nfx = 0;  /* neighbours blend only when they touch this band */
        int plen = q > 0 && by1[q - 1] == by0[q] ? (int)by1[q - 1] - (int)by0[q - 1] : 0;
        int nlen = q + 1 < nb && by0[q + 1] == by1[q] ? (int)by1[q + 1] - (int)by0[q + 1] : 0;
        if (plen > 0)
          pfx = bfix[q - 1];
        if (nlen > 0)
          nfx = bfix[q + 1];
        for (int sh = 0; sh < 15; sh += 5) {
          int d1 = plen > 0 ? (((fx >> sh) & 31) - ((pfx >> sh) & 31)) : 0;
          if (d1 < 0)
            d1 = -d1;
          if (d1 > maxstep)
            maxstep = d1;
        }
        int cr = fx & 31, cg = (fx >> 5) & 31, cb = (fx >> 10) & 31;
        for (int y = y0; y < y1; y++) {
          int pos2 = 2 * y + 1;  /* twice the row centre */
          int ofx = -1;
          int t = 256;           /* weight (of 256) of this band's colour */
          if (plen > 0) {
            int w = len < plen ? len : plen;
            int d = pos2 - 2 * ra;
            if (d < w) {
              ofx = pfx;
              t = (d + w) * 256 / (2 * w);
            }
          }
          if (ofx < 0 && nlen > 0) {
            int w = len < nlen ? len : nlen;
            int d = 2 * rb - pos2;
            if (d < w) {
              ofx = nfx;
              t = (d + w) * 256 / (2 * w);
            }
          }
          int lr = cr * 16, lg = cg * 16, lb = cb * 16;  /* Q4 levels */
          if (ofx >= 0) {
            lr = (cr * t + (ofx & 31) * (256 - t)) >> 4;
            lg = (cg * t + ((ofx >> 5) & 31) * (256 - t)) >> 4;
            lb = (cb * t + ((ofx >> 10) & 31) * (256 - t)) >> 4;
          }
          for (int x = 0; x < kTintTexW; x++) {
            int bay = kBayer[y & 3][x & 3];
            uint16_t col = (uint16_t)(((lr + bay) >> 4) | ((lg + bay) >> 4) << 5 | ((lb + bay) >> 4) << 10);
            strip[y * kTintTexW + x] = BakeColor(col, 15, false);
          }
          rows++;
        }
      }
      i = j;
    }
  }
  __sync_synchronize();  /* CPU writes to VRAM (uncached alias) complete before the GE is started */
  if ((bands_total >= 3 && glogged < 24 && (g_frame & 31) == 0) || logged < 2) {  /* gradient frames, not just the first flat ones */
    if (bands_total >= 3)
      glogged++;
    else
      logged++;
    PspLog("tintstrip f=%u: runs=%d bands=%d rows=%d ranges=%d scissor=%d cw=%u maxstep=%d(levels) first=%04x last=%04x",
           (unsigned)g_frame, g_ntint, bands_total, rows, g_ntr, g_scissor_mode, (unsigned)g_cw_rows, maxstep,
           g_ntint ? g_tint[0].fix : 0, g_ntint ? g_tint[g_ntint - 1].fix : 0);
   }
 }

//software patch
 
static inline uint16_t Rgb565(const uint8_t *p) {
  return (uint16_t)((p[0] >> 3) | (p[1] >> 2) << 5 | (p[2] >> 3) << 11);  /* PSP 5650: R in the low bits */
}

static void PspRenderer_Convert(void) {
  for (int y = 0; y < kGameHeight; y++) {
    const uint8_t *src = g_framebuffer + y * kGameWidth * 4;
    uint16_t *dst = g_texture + y * kTexWidth;
    for (int x = 0; x < kGameWidth; x++)
      dst[x] = Rgb565(src + x * 4);
  }
}

//capture

/* Gate 4t-c: subscreen BG layer added together with a non-zero fixed colour, no half colour. Such rows are drawn by
 * the GE through the multisub target; window masks of the subscreen layers are painted over that target. */
/* Gate 4v: a BG layer shown on the subscreen is masked by window 2 (TSW). */
static bool SubW2Layer(const Ppu *p) {
  for (int l = 0; l < kLayers; l++)
    if ((p->screenEnabled[1] & (1 << l)) && (p->screenWindowed[1] & (1 << l)) && ((p->windowsel >> (l * 4)) & 8))
      return true;
  return false;
}

static bool SubFixMath(const Ppu *p) {
  return g_sf_ok && p->mode == 1 && p->addSubscreen && !p->halfColor && p->preventMathMode != 3 &&
         ((p->fixedColorR | p->fixedColorG | p->fixedColorB) != 0 || (g_sf0_ok && SubW2Layer(p))) &&
         (p->screenEnabled[1] & 7) && (!(p->screenEnabled[1] & 0x10) || g_sfo_ok);  /* gate 5e: OBJ on the subscreen */
}

/* Gate 4t-d/5c: subscreen BG3 alone, added (or subtracted) with half colour. Every non-empty BG3 tile matters (a black
 * pixel still halves the main pixel), the fixed colour goes where BG3 has no pixel. Fixed colour may be 0. */
static bool SubHalfMath(const Ppu *p) {
  uint8_t ts = p->screenEnabled[1];  /* gate 4u: BG3 and/or OBJ on the subscreen, no BG1/BG2 */
  return g_sh_ok && p->mode == 1 && p->addSubscreen && p->halfColor && (!p->subtractColor || g_shn_ok) &&
         p->preventMathMode != 3 && (ts & 0x0b) == 0 && (ts & 0x14) != 0 && ((ts & 0x10) ? g_shobj_ok : true);
}

/* Window 1 mask of layer l (0-2 BG, 4 OBJ) on the main screen, for the GE path. wm: 0 no effect, 1 the layer is
 * visible only inside [wl,wr], 2 hidden inside [wl,wr]. As in ppu.c an empty window (left > right) masks
 * nothing, and an inverted empty window masks everything. Returns false for setups the GE path does not draw
 * (window 2, mode 7, or after the run-time check failed): the frame stays in software. */
static bool LayerWin(const Ppu *p, int l, int sub, uint8_t *wm, uint8_t *wl, uint8_t *wr) {
  *wm = *wl = *wr = 0;
  if (!(p->screenWindowed[sub] & (1 << l)))
    return true;
  uint32_t n = (p->windowsel >> (l * 4)) & 0xf;  /* 1 inverse 1, 2 enable 1, 4 inverse 2, 8 enable 2 */
  if (!(n & 0xa))
    return true;
  if (!g_win_ok || p->mode == 7 || (n & 0xa) == 0xa)  /* window 1 + 2 together (mask logic): not drawn */
    return false;
  bool w2 = (n & 8) != 0;
  if (w2 && !(sub ? (SubFixMath(p) || SubHalfMath(p)) : (SubHalfMath(p) || (g_w2_ok && l < 3))))  /* gates 4t-c/d/u: window 2 on the subscreen of a subfix/half row; on main BG layers via per-row spans */
    return false;
  int left = w2 ? p->window2left : p->window1left, right = w2 ? p->window2right : p->window1right;
  bool ena = left <= right;
  if (w2 ? (n & 4) : (n & 1)) {
    *wm = 1;
    *wl = ena ? left : 1;
    *wr = ena ? right : 0;
  } else if (ena) {
    *wm = 2;
    *wl = left;
    *wr = right;
  }
  return true;
}

/* Visible x spans [a,b) of a layer window state (see LayerWin): 0, 1 or 2 spans. */
static int WinSpans(int wm, int wl, int wr, int a[2], int b[2]) {
  int n = 0;
  if (wm == 0) {
    a[0] = 0;
    b[0] = kGameWidth;
    return 1;
  }
  if (wm == 1) {
    if (wl > wr)
      return 0;
    a[0] = wl;
    b[0] = wr + 1;
    return 1;
  }
  if (wl > 0) {
    a[n] = 0;
    b[n++] = wl;
  }
  if (wr + 1 < kGameWidth) {
    a[n] = wr + 1;
    b[n++] = kGameWidth;
  }
  return n;
}

/* True when colour math cannot change a pixel: fixed colour 0, no half colour, and no subscreen layer to add
 * (ppu.c no-math path). Prevent-math modes 1/2 only choose where math applies, so then they need no window. */
static bool MathHasNoEffect(const Ppu *p) {
bool fix = (p->fixedColorR | p->fixedColorG | p->fixedColorB) != 0;
  if (p->addSubscreen)
    return p->screenEnabled[1] == 0 && !fix;
  return !fix && !p->halfColor;
}

/* Colour-window limit of the math region for prevent-math mode 1 (math inside the window) or 2 (outside), for the GE
 * tint path. Same model as PpuWindows_Calc in ppu.c: a window counts only if enabled, inverse flips the region, an
 * empty (left > right) enabled window is empty, or everything when inverted. The result is a BgSnap-style state
 * (wm 0 no limit, 1 only inside [wl,wr], 2 only outside). Returns false for W1 + W2 together (ppu.c ORs them) or if
 * the audit failed. */
static bool ColorWinMath(const Ppu *p, uint8_t *wm, uint8_t *wl, uint8_t *wr) {
  enum { kEmpty, kAll, kIn, kOut };
  uint32_t n = (p->windowsel >> 20) & 0xf;  /* 1 inverse 1, 2 enable 1, 4 inverse 2, 8 enable 2 */
  bool e1 = (n & 2) != 0, e2 = (n & 8) != 0;
  int k = kEmpty, l = 0, r = 0;  /* k: the window region */
  *wm = *wl = *wr = 0;
  if (!g_cw_ok || (e1 && e2))
    return false;
  if (e1 || e2) {
    bool inv = e1 ? (n & 1) != 0 : (n & 4) != 0;
    l = e1 ? p->window1left : p->window2left;
    r = e1 ? p->window1right : p->window2right;
    k = l <= r ? (inv ? kOut : kIn) : (inv ? kAll : kEmpty);
  }
  if (p->preventMathMode == 2)  /* math outside the window */
    k = k == kEmpty ? kAll : k == kAll ? kEmpty : k == kIn ? kOut : kIn;
  if (k == kEmpty) {
    *wm = 1;  /* only inside an empty span */
    *wl = 1;
    *wr = 0;
  } else if (k != kAll) {
    *wm = k == kIn ? 1 : 2;
    *wl = (uint8_t)l;
    *wr = (uint8_t)r;
  }
  return true;
}

static uint16_t LineReasons(const Ppu *p, uint8_t mathmask, bool *submath, bool *tint, uint16_t *tintfix,
                           bool *tinthalf, uint8_t cw[3], uint16_t *subfix, bool *subhalf) {
  *submath = false;
  *subfix = 0;
  *subhalf = false;
  *tint = false;
  *tintfix = 0;
  *tinthalf = false;
  cw[0] = cw[1] = cw[2] = 0;
  if (p->forcedBlank)
    return 0;
  uint16_t r = 0;
  int nl = p->mode == 7 ? 1 : kLayers;  /* mode 7 has BG1 only */
  for (int l = 0; l < nl; l++) {
    if (!((p->screenEnabled[0] | p->screenEnabled[1]) & (1 << l)))
      continue;  /* shown on neither screen: tile size, mosaic and window cannot matter (X-ray HUD rows) */
    if (p->bgLayer[l].bigTiles)
      r |= kReasonBigTile;
    if (p->mode != 1 && p->mode != 7 && p->bgLayer[l].mosaicEnabled && p->mosaicSize > 1)  /* mode 1: MosPrepare, mode 7: image */
      r |= kReasonMosaic;
    uint8_t wm, wl, wr;
    if (!LayerWin(p, l, 0, &wm, &wl, &wr))
      r |= kReasonWindow;
  }
  if (p->clipMode != 0)
    r |= kReasonMathClip;
  if (mathmask != 0 && p->preventMathMode != 3) {
    if (p->mode == 7 && p->addSubscreen && p->screenEnabled[1] != 0)
      r |= kReasonM7;  /* subscreen layers in mode 7 are not drawn */
    uint16_t fix = (uint16_t)(p->fixedColorR | p->fixedColorG << 5 | p->fixedColorB << 10);
    if (p->addSubscreen && p->screenEnabled[1] != 0) {
      if (SubFixMath(p)) {
        *submath = true;
        *subfix = (uint16_t)(fix | 0x8000);  /* bit 15 marks a subfix row, also when the fixed colour is 0 (gate 4v) */
      } else if (SubHalfMath(p)) {
        *submath = true;
        *subfix = fix;
        *subhalf = true;
      } else if (fix || p->halfColor) {
        r |= kReasonMathTint;
      } else {
        /* fixed == 0 and no half: adding the subscreen only changes pixels where
         * a subscreen layer is opaque (ppu.c: empty subscreen pixel = fixed color
         * = 0, unhalved). Subscreen sprite bounds checked in DrawGe. */
        if (p->screenEnabled[1] & 7)
          *submath = true;
      }
    } else if (p->addSubscreen) {
      /* Empty subscreen: every pixel gets the fixed colour added, never halved (ppu.c). */
      if (fix) {
        *tint = true;
        *tintfix = fix;
      }
    } else if (fix || p->halfColor) {
      /* No subscreen add: fixed colour added/subtracted, result halved when half colour is set. */
      *tint = true;
      *tintfix = fix;
      *tinthalf = p->halfColor;
     }
    if (p->preventMathMode != 0 && !(g_prev_ok && MathHasNoEffect(p)) &&
        !((*tint || *subhalf) && ColorWinMath(p, &cw[0], &cw[1], &cw[2])))
      r |= kReasonMathClip;  /* prevent inside/outside the colour window: only a single-window fixed-colour tint is drawn (gate 4t-b) */
   }
  return r;
}

/* Mode 7 row (GE path): snapshot of matrix, flags and mosaic of every mode 7 row with BG1 on, so HUD + mode 7 frames
 * and per-line (HDMA) matrix changes need no fallback. Returns the fallback reasons. */
static uint16_t M7Line(const Ppu *p, LineSnap *s) {
  uint8_t fl = (uint8_t)((p->m7largeField ? 1 : 0) | (p->m7charFill ? 2 : 0) | (p->m7xFlip ? 4 : 0) |
                         (p->m7yFlip ? 8 : 0) | (p->m7extBg ? 16 : 0));
  uint16_t r = 0;
  if ((fl & 16) || !g_m7_ok || !g_m7i_ok)  /* extbg is not drawn by ppu.c either; self test or image audit failed */
    r |= kReasonM7;
  if (!(p->screenEnabled[0] & 1))
    return r;  /* BG1 off: no mode 7 pixels on this row */
  if (p->bgLayer[0].mosaicEnabled && p->mosaicSize > 1)
    fl |= 32;
  s->m7on = 1;
  s->m7fl = fl;
  memcpy(s->m7m, p->m7matrix, sizeof(s->m7m));
  if (g_m7_rows == 0) {
    memcpy(g_m7c.m, p->m7matrix, sizeof(g_m7c.m));
    g_m7c.fl = fl;
  } else if (fl != g_m7c.fl || memcmp(p->m7matrix, g_m7c.m, sizeof(g_m7c.m))) {
    g_m7_same = false;
  }
  g_m7_rows++;
  return r;
}

/* VRAM snapshot for the GE path: only the 512-byte pages written since the last one (ppu->vramDirty, set in
 * ppu_write). Every 64 frames the whole snapshot is compared with VRAM; a mismatch (a writer that bypasses
 * ppu_write) switches to full copies for good. */
enum { kVramPages = 128, kVramPageWords = 256 };
static bool g_vd_ok = SM_PSP_VRAM_DIRTY;
/* Persistent VRAM page generations (SM_PSP_TILE_GEN). A page's generation is bumped whenever SnapshotVram copies it (or
 * refreshes everything), so a tile whose recorded generation equals its page's has had unchanged source words in g_vram
 * since it was last hashed, however many frames ago: EnsureTile then skips the hash. 0 = unknown. When a page counter
 * would wrap, every tile record is cleared and all pages restart at 1. */
#ifndef SM_PSP_TILE_GEN
#define SM_PSP_TILE_GEN 1
#endif
#if SM_PSP_TILE_GEN
static uint16_t g_pgen[kVramPages];
static uint16_t g_tgen2[4096], g_tgen4[2048];
static bool g_tgen_ok = SM_PSP_TILE_GEN;
#if SM_PSP_LOG
static uint32_t g_tg_skip, g_tg_audit, g_tg_bad, g_tg_n;
#endif
static void VramGenBump(int pg0, int pg1) {
  for (int pg = pg0; pg < pg1; pg++)
    if (++g_pgen[pg] == 0xffffu) {
      memset(g_tgen2, 0, sizeof(g_tgen2));
      memset(g_tgen4, 0, sizeof(g_tgen4));
      for (int i = 0; i < kVramPages; i++)
        g_pgen[i] = 1;
    }
}
#endif

static void ImgMarkDirty(int pg0, int pg1);  /* gate 5m: layer images learn which pages were written */
static void ImgMarkAllDirty(void);
static void SnapshotVram(const Ppu *p) {
  uint8_t *dirty = (uint8_t *)p->vramDirty;  /* the renderer owns the clear; the Ppu object is not const */
  if (!g_vd_ok) {
    memcpy(g_vram, p->vram, sizeof(g_vram));
    memset(dirty, 0, kVramPages);
    ImgMarkAllDirty();
    M7VramDirty(0, kVramPages);
#if SM_PSP_TILE_GEN
    VramGenBump(0, kVramPages);
#endif
    return;
  }
  for (int pg = 0; pg < kVramPages;) {
    if (!dirty[pg]) {
      pg++;
      continue;
    }
    int e = pg + 1;
    while (e < kVramPages && dirty[e])
      e++;
    memcpy(&g_vram[pg * kVramPageWords], &p->vram[pg * kVramPageWords], (size_t)(e - pg) * kVramPageWords * 2);
    ImgMarkDirty(pg, e);
    M7VramDirty(pg, e);
#if SM_PSP_TILE_GEN
    VramGenBump(pg, e);
#endif
    memset(&dirty[pg], 0, (size_t)(e - pg));
    pg = e;
  }
  /* Audit one 1 KB slice per frame: all of VRAM is still compared every 64 frames, without the ~128 KB memcmp spike on every 64th. */
  CASSERT(kVramPages % 64 == 0 && sizeof(g_vram) == (size_t)kVramPages * kVramPageWords * 2);
  const size_t aw = (size_t)(g_frame & 63) * (kVramPages / 64) * kVramPageWords;
  if (memcmp(&g_vram[aw], &p->vram[aw], (size_t)(kVramPages / 64) * kVramPageWords * 2) != 0) {
    g_vd_ok = false;
    memcpy(g_vram, p->vram, sizeof(g_vram));
    ImgMarkAllDirty();
    M7VramDirty(0, kVramPages);
#if SM_PSP_TILE_GEN
    VramGenBump(0, kVramPages);
#endif
    PspLog("vramdirty: snapshot differs from VRAM, dirty tracking disabled");
  }
}

/* Snapshot of the previous line with the values that ppu_write's counter does not cover re-read. */
static void CaptureRefresh(const Ppu *p, LineSnap *d, const LineSnap *prev) {
  memcpy(d, prev, sizeof(*d));
  for (int l = 0; l < kLayers; l++) {
    d->bg[l].hscroll = p->bgLayer[l].hScroll & 0x3ff;
    d->bg[l].vscroll = p->bgLayer[l].vScroll & 0x3ff;
  }
  d->cg0 = CgramSrc(p)[0];
}

/* Window edges ($2126-$2129) as of the last full or window-refreshed capture. */
static inline bool CapWinSame(const Ppu *p) {
  return p->window1left == g_cap_win[0] && p->window1right == g_cap_win[1] && p->window2left == g_cap_win[2] &&
         p->window2right == g_cap_win[3];
}

static inline void CapWinStore(const Ppu *p) {
  g_cap_win[0] = p->window1left;
  g_cap_win[1] = p->window1right;
  g_cap_win[2] = p->window2left;
  g_cap_win[3] = p->window2right;
}

/* Gate 5k: a line whose registers equal the previous line's except for the window edges. The edges only reach wm/wl/wr,
 * sm/sl/sr, owm/owl/owr, cwm/cwl/cwr and w2main, so only those are derived again, with the same calls as CaptureImpl
 * and LineReasons (log builds compare the result with a full capture on every 16th frame). */
static void CaptureWinRefresh(const Ppu *p, LineSnap *s) {
  bool forced = p->forcedBlank;
  s->w2main = 0;
  for (int l = 0; l < kLayers; l++) {
    BgSnap *b = &s->bg[l];
    bool drawn = (b->flags & 4) != 0;
    b->wm = b->wl = b->wr = 0;
    if (drawn)
      LayerWin(p, l, 0, &b->wm, &b->wl, &b->wr);
    if (drawn && b->wm && ((p->windowsel >> (l * 4)) & 8))
      s->w2main = 1;
    b->sm = b->sl = b->sr = 0;
    if (!forced && p->mode == 1 && (p->screenEnabled[1] & (1 << l)) && !p->bgLayer[l].bigTiles)
      LayerWin(p, l, 1, &b->sm, &b->sl, &b->sr);
  }
  s->owm = s->owl = s->owr = 0;
  if (s->objon)
    LayerWin(p, 4, 0, &s->owm, &s->owl, &s->owr);
  s->cwm = s->cwl = s->cwr = 0;
  if (!forced && s->mathmask != 0 && p->preventMathMode != 3 && p->preventMathMode != 0 &&
      !(g_prev_ok && MathHasNoEffect(p)) && (s->tint || s->subhalf))
    ColorWinMath(p, &s->cwm, &s->cwl, &s->cwr);
}

static void CaptureImpl(const Ppu *p, int line) {
  if (!g_ge_active || line < 1 || line > kMaxLines)
    return;
  if (line == 1) {
    g_frame_dead = false;
    g_frame_mode = -1;
    g_m7_rows = 0;
    g_m7_same = true;
    g_obj_size = p->objSize;
    g_obj_adr1 = p->objTileAdr1;
    g_obj_adr2 = p->objTileAdr2;
    g_obj_rot = p->objPriority;
    g_obj_start = g_obj_rot ? (uint8_t)((p->oamAdr & 0xfe) >> 1) : 0;
    g_line_count = 0;
    g_frame_reasons = 0;
  } else if (g_frame_dead) {
    return;  /* DrawGe hands this frame back anyway; skip the per-line work */
  } else if (line != g_line_count + 1) {
    g_capture_gaps++;
    return;
  }
  LineSnap *s = &g_lines[line - 1];
  /* Same registers as the previous line: copy its snapshot. Log builds recompute every 16th frame and compare
   * (end of function). A frame that is already dead never gets here, so the copied reasons are 0. */
#if SM_PSP_LOG
  LineSnap cand;
  bool check = false;
#endif
  bool winsame = CapWinSame(p);
  if (g_cap_reuse_ok && line > 1 && p->regGen == g_cap_gen && p->oamAdr == g_cap_oamadr && (winsame || g_cap_win_ok)) {
#if SM_PSP_LOG
    if ((g_frame & 15) == 0) {
      check = true;
      CaptureRefresh(p, &cand, &g_lines[line - 2]);
      if (!winsame)
        CaptureWinRefresh(p, &cand);
    } else
#endif
    {
      CaptureRefresh(p, s, &g_lines[line - 2]);
      if (!winsame) {
        CaptureWinRefresh(p, s);
        CapWinStore(p);
        PROF(g_cap_n_win++;)
      }
      if (s->m7on)
        g_m7_rows++;  /* same matrix as the previous mode 7 row: g_m7c and g_m7_same already cover it */
      PROF(g_cap_n_reuse++;)
      g_line_count = line;
      return;
    }
  }
  g_cap_gen = p->regGen;
  g_cap_oamadr = p->oamAdr;
  CapWinStore(p);
  PROF(g_cap_n_full++;)
  bool forced = p->forcedBlank;
  s->w2main = 0;
  for (int l = 0; l < kLayers; l++) {
    const BgLayer *bg = &p->bgLayer[l];
    BgSnap *b = &s->bg[l];
    b->hscroll = bg->hScroll & 0x3ff;
    b->vscroll = bg->vScroll & 0x3ff;
    b->mapadr = bg->tilemapAdr;
    b->tileadr = bg->tileAdr;
    bool drawn = !forced && p->mode == 1 && (p->screenEnabled[0] & (1 << l)) && !bg->bigTiles;
    b->flags = (uint8_t)((bg->tilemapWider ? 1 : 0) | (bg->tilemapHigher ? 2 : 0) | (drawn ? 4 : 0) |
                         ((p->screenEnabled[1] & (1 << l)) ? 8 : 0));
     b->wm = b->wl = b->wr = 0;
    if (drawn)
      LayerWin(p, l, 0, &b->wm, &b->wl, &b->wr);  /* setups it cannot draw are flagged by LineReasons */
    if (drawn && b->wm && ((p->windowsel >> (l * 4)) & 8))
      s->w2main = 1;
    b->sm = b->sl = b->sr = 0;
    if (!forced && p->mode == 1 && (p->screenEnabled[1] & (1 << l)) && !bg->bigTiles)
      LayerWin(p, l, 1, &b->sm, &b->sl, &b->sr);  /* unsupported setups are flagged through subwin */
  }
  s->brightness = p->brightness & 15;
  s->bg3prio = p->bg3priority ? 1 : 0;
  s->cg0 = CgramSrc(p)[0];
  s->forced = forced ? 1 : 0;
  {  /* mosaic (mode 1): BG1-3 with the effect on; the GE path draws it from the layer geometry (MosPrepare) */
    uint8_t mos = 0;
    if (!forced && p->mode == 1 && p->mosaicSize > 1)
      for (int l = 0; l < kLayers; l++)
        if (p->bgLayer[l].mosaicEnabled)
          mos |= (uint8_t)(1 << l);
    s->mosmask = mos;
    s->mossize = p->mosaicSize;
    s->mosstart = p->mosaicStartLine;
  }
  uint8_t mm = 0;
  for (int i = 0; i < 6; i++)
    mm |= (uint8_t)((p->mathEnabled[i] ? 1 : 0) << i);
  s->mathmask = mm;
  bool submath, tint, tinthalf, subhalf;
  uint16_t tintfix;
  uint8_t cw[3];
  uint16_t subfix;
  s->reasons = LineReasons(p, mm, &submath, &tint, &tintfix, &tinthalf, cw, &subfix, &subhalf);
  s->subfix = subfix;
  s->subhalf = subhalf ? 1 : 0;
  s->cwm = cw[0];
  s->cwl = cw[1];
  s->cwr = cw[2];
  s->submath = submath;
  s->tint = tint;
  s->tinthalf = tinthalf;
  s->tintfix = tintfix;
  s->m7on = 0;
  s->m7fl = 0;
  if (!forced) {
    if (g_frame_mode < 0)
      g_frame_mode = p->mode;
    if (p->mode != 1 && p->mode != 7)  /* mixed mode 1 / mode 7 frames (HUD + mode 7) are drawn */
      s->reasons |= kReasonMode;
    if (p->mode == 7)
      s->reasons |= M7Line(p, s);
  }
  /* only matters where the subscreen is added to the main screen with the fixed colour at 0 */
  s->subobj = (!forced && mm != 0 && p->preventMathMode != 3 && p->addSubscreen && (p->screenEnabled[1] & 16)) ? 1 : 0;
  s->subneg = p->subtractColor ? 1 : 0;
  s->prevnoop = (!forced && mm != 0 && (p->preventMathMode == 1 || p->preventMathMode == 2) && g_prev_ok &&
                 MathHasNoEffect(p)) ? 1 : 0;
  {
    uint8_t sw = 0;
    for (int l = 0; l < kLayers; l++) {  /* only setups the GE path cannot draw (window 2, mode 7, audit failed) */
      uint8_t m, a, e;
      if (!LayerWin(p, l, 1, &m, &a, &e))
        sw |= (uint8_t)(1 << l);
    }
    if ((p->screenWindowed[1] & 16) && ((p->windowsel >> 16) & 0xa))
      sw |= 8;  /* OBJ on the subscreen is windowed */
    s->subwin = sw;
#if SM_PSP_LOG
    if (sw && submath && NewSig(p, 0))  /* gate 4v: which setup still makes a subscreen window unsupported */
      PspLog("SUBWIN line=%d sw=%02x TM=%02x TS=%02x tsw=%02x wsel=%06x w1=%d-%d w2=%d-%d math=%02x half=%d fixed=%04x neg=%d",
             line, sw, p->screenEnabled[0], p->screenEnabled[1], p->screenWindowed[1], (unsigned)p->windowsel,
             p->window1left, p->window1right, p->window2left, p->window2right, mm, p->halfColor ? 1 : 0,
             (unsigned)(p->fixedColorR | p->fixedColorG << 5 | p->fixedColorB << 10), p->subtractColor ? 1 : 0);
#endif
  }
  bool objon = !forced && (p->mode == 1 || p->mode == 7) && (p->screenEnabled[0] & 16);
  s->objon = objon;
  s->owm = s->owl = s->owr = 0;
  if (objon || s->subobj) {
    uint16_t r2 = 0;
    if (p->objInterlace || p->objSize != g_obj_size || p->objTileAdr1 != g_obj_adr1 ||
        p->objTileAdr2 != g_obj_adr2 || (bool)p->objPriority != g_obj_rot ||
        (g_obj_rot && (uint8_t)((p->oamAdr & 0xfe) >> 1) != g_obj_start))
      r2 |= kReasonObjCfg;
    if (objon && !LayerWin(p, 4, 0, &s->owm, &s->owl, &s->owr))
      r2 |= kReasonWindow;
    s->reasons |= r2;
  }
  g_frame_reasons |= s->reasons;
  if (line == 1 && !g_frame_reasons) {
    /* VRAM/CGRAM as the display saw them for this frame. One 64 KB copy per frame, never per line,
     * and only if the frame can still be drawn by the GE. */
    PROF(uint32_t t_cpy = Now();)
    SnapshotVram(p);
#if SM_PSP_DOOR_GE
    if (g_door_pal_mode == kDoorPalCapture) {  /* first frame of the door transition: the palette before the fade-out */
      memcpy(g_door_pal_out, p->cgram, sizeof(g_door_pal_out));
      g_door_pal_mode = kDoorPalOutgoing;
    }
#endif
    memcpy(g_cgram, CgramSrc(p), sizeof(g_cgram));
    memcpy(g_oam, p->oam, sizeof(g_oam));
    memcpy(g_hoam, p->highOam, sizeof(g_hoam));
    PROF(g_cpy_us = Now() - t_cpy;)
  }
  if (g_frame_reasons)
    g_frame_dead = true;
#if SM_PSP_LOG
  if (s->reasons && NewSig(p, s->reasons)) {  /* first time each register signature is seen: remember the state */
    g_note = (Note){line, s->reasons, p->mode, p->screenEnabled[0], p->screenEnabled[1], mm,
                    p->addSubscreen, p->subtractColor, p->halfColor,
                    p->fixedColorR | p->fixedColorG << 5 | p->fixedColorB << 10,
                    p->clipMode, p->preventMathMode, p->screenWindowed[0], p->windowsel, p->screenWindowed[1],
                    GameRam16(0x998), GameRam16(0x79B), g_ram[0x79F]};
    g_note_pending = true;
  }
#endif
#if SM_PSP_LOG
  if (check) {
    g_cap_n_chk++;
    if (!cand.m7on)
      memcpy(cand.m7m, s->m7m, sizeof(cand.m7m));  /* m7m is only written on mode 7 rows */
    if (memcmp(&cand, s, sizeof(cand)) != 0) {
      const uint8_t *ca = (const uint8_t *)&cand, *cs = (const uint8_t *)s;
      unsigned off = 0;
      while (off < sizeof(cand) && ca[off] == cs[off])
        off++;
      g_cap_n_bad++;
      if (winsame)
        g_cap_reuse_ok = false;
      else
        g_cap_win_ok = false;
      PspLog("capreuse: f=%u line=%d byte %u of %u differs, %s disabled", (unsigned)g_frame, line, off,
             (unsigned)sizeof(cand), winsame ? "reuse" : "window path");
    }
  }
#endif
  g_line_count = line;
}

#if SM_PSP_LOG
/* Mode 7 probe (log only, no effect on drawing): is the matrix constant over a frame or changed per
 * line (HDMA), how much of the 1024x1024 plane is visible, which options are in use. Runs for every
 * frame, whether or not the GE path is on. */
typedef struct M7Rec { int16_t m[8]; uint8_t fl; } M7Rec;  /* m: a, b, c, d, centre x, y, scroll h, v */
static struct {
  M7Rec first, cur;
  int lines, chg_abcd, chg_sc, chg_fl, other_lines;
  int amin, amax, dmin, dmax;
  bool rot;
} g_m7;
static struct {
  uint32_t n, constant, abcd, scroll, rot, large, charfill, flip, extbg, mosaic, obj, mixed;
  uint32_t tiles_sum, tiles_max;
} g_m7w;
static uint32_t g_m7_total;

static void M7Probe(const Ppu *p, int line) {
  if (line < 1 || line > kMaxLines)
    return;
  if (line == 1)
    memset(&g_m7, 0, sizeof(g_m7));
  if (p->forcedBlank || p->mode != 7) {
    g_m7.other_lines++;
    return;
  }
  M7Rec r;
  memcpy(r.m, p->m7matrix, sizeof(r.m));
  r.fl = (uint8_t)((p->m7largeField ? 1 : 0) | (p->m7charFill ? 2 : 0) | (p->m7xFlip ? 4 : 0) | (p->m7yFlip ? 8 : 0) |
                   (p->m7extBg ? 16 : 0) | ((p->bgLayer[0].mosaicEnabled && p->mosaicSize > 1) ? 32 : 0) |
                   ((p->screenEnabled[0] & 16) ? 64 : 0));
  if (g_m7.lines == 0) {
    g_m7.first = r;
    g_m7.amin = g_m7.amax = r.m[0];
    g_m7.dmin = g_m7.dmax = r.m[3];
  } else {
    if (memcmp(r.m, g_m7.cur.m, 4 * sizeof(int16_t)))
      g_m7.chg_abcd++;
    if (memcmp(r.m + 4, g_m7.cur.m + 4, 4 * sizeof(int16_t)))
      g_m7.chg_sc++;
    if (r.fl != g_m7.cur.fl)
      g_m7.chg_fl++;
    if (r.m[0] < g_m7.amin) g_m7.amin = r.m[0];
    if (r.m[0] > g_m7.amax) g_m7.amax = r.m[0];
    if (r.m[3] < g_m7.dmin) g_m7.dmin = r.m[3];
    if (r.m[3] > g_m7.dmax) g_m7.dmax = r.m[3];
  }
  if (r.m[1] || r.m[2])
    g_m7.rot = true;
  g_m7.cur = r;
  g_m7.lines++;
}

static void M7FrameEnd(void) {
  if (g_m7.lines == 0)
    return;
  g_m7_total++;
  g_m7w.n++;
  if (!g_m7.chg_abcd && !g_m7.chg_sc)
    g_m7w.constant++;
  else if (g_m7.chg_abcd)
    g_m7w.abcd++;
  else
    g_m7w.scroll++;
  uint8_t fl = g_m7.first.fl;
  g_m7w.rot += g_m7.rot;
  g_m7w.large += fl & 1;
  g_m7w.charfill += (fl >> 1) & 1;
  g_m7w.flip += (fl >> 2 & 3) != 0;
  g_m7w.extbg += (fl >> 4) & 1;
  g_m7w.mosaic += (fl >> 5) & 1;
  g_m7w.obj += (fl >> 6) & 1;
  g_m7w.mixed += g_m7.other_lines != 0;
  /* plane area covered by the screen (plane pixels): 256 x lines x |a*d - b*c| / 65536, in 8x8 tiles */
  const int16_t *m = g_m7.first.m;
  int64_t det = (int64_t)m[0] * m[3] - (int64_t)m[1] * m[2];
  if (det < 0)
    det = -det;
  uint32_t tiles = (uint32_t)((int64_t)256 * g_m7.lines * det / 65536 / 64);
  g_m7w.tiles_sum += tiles;
  if (tiles > g_m7w.tiles_max)
    g_m7w.tiles_max = tiles;
  if (g_m7_total <= 6 || g_m7_total % 600 == 0)
    PspLog("M7s f=%u a=%d b=%d c=%d d=%d cx=%d cy=%d hs=%d vs=%d fl=%02x | last a=%d d=%d | chg abcd=%d sc=%d fl=%d lines=%d other=%d | A %d..%d D %d..%d | tiles~%u",
           (unsigned)g_frame, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], fl, g_m7.cur.m[0], g_m7.cur.m[3],
           g_m7.chg_abcd, g_m7.chg_sc, g_m7.chg_fl, g_m7.lines, g_m7.other_lines, g_m7.amin, g_m7.amax, g_m7.dmin,
           g_m7.dmax, (unsigned)tiles);
}

static void M7Log(void) {
  if (g_m7w.n == 0)
    return;
  PspLog("M7 n=%u const=%u abcd-per-line=%u scroll-only=%u | rot=%u large=%u charfill=%u flip=%u extbg=%u mosaic=%u obj=%u mixed=%u | tiles avg=%u max=%u",
         (unsigned)g_m7w.n, (unsigned)g_m7w.constant, (unsigned)g_m7w.abcd, (unsigned)g_m7w.scroll, (unsigned)g_m7w.rot,
         (unsigned)g_m7w.large, (unsigned)g_m7w.charfill, (unsigned)g_m7w.flip, (unsigned)g_m7w.extbg,
         (unsigned)g_m7w.mosaic, (unsigned)g_m7w.obj, (unsigned)g_m7w.mixed, (unsigned)(g_m7w.tiles_sum / g_m7w.n),
         (unsigned)g_m7w.tiles_max);
  memset(&g_m7w, 0, sizeof(g_m7w));
}

/* Window probe (log only, no effect on drawing). Frames with a windowed layer go back to software
 * (kReasonWindow). This reports which layers are windowed (main / sub / colour window), which windows
 * they use, and whether the edges are a fixed box or change from line to line (HDMA). Every line,
 * whether or not the GE path is on. */
static struct {
  int rows, on, first, last, chg1, chg2, chgsel;
  int l1min, l1max, r1min, r1max, l2min, l2max, r2min, r2max;
  int pl1, pr1, pl2, pr2;
  uint32_t sel, psel, ml;
  uint8_t mon, son, cw, w1use, w2use, tm, ts, math, add;
} g_wp;
static uint32_t g_wp_total;

static void WinProbe(const Ppu *p, int line) {
  if (line < 1 || line > kMaxLines)
    return;
  if (line == 1)
    memset(&g_wp, 0, sizeof(g_wp));
  if (p->forcedBlank)
    return;
  g_wp.rows++;
  uint8_t m_on = 0, s_on = 0;
  for (int l = 0; l < 5; l++) {  /* BG1-4, OBJ: one windowsel nibble each (colour window is nibble 5) */
    if (!((p->windowsel >> (l * 4)) & 0xa))
      continue;
    if (p->screenWindowed[0] & (1 << l))
      m_on |= (uint8_t)(1 << l);
    if (p->screenWindowed[1] & (1 << l))
      s_on |= (uint8_t)(1 << l);
  }
  bool cw = (p->clipMode == 1 || p->clipMode == 2 || p->preventMathMode == 1 || p->preventMathMode == 2) &&
            ((p->windowsel >> 20) & 0xa);
  if (!m_on && !s_on && !cw)
    return;
  int l1 = p->window1left, r1 = p->window1right, l2 = p->window2left, r2 = p->window2right;
  if (g_wp.on == 0) {
    g_wp.first = line - 1;
    g_wp.l1min = g_wp.l1max = l1;
    g_wp.r1min = g_wp.r1max = r1;
    g_wp.l2min = g_wp.l2max = l2;
    g_wp.r2min = g_wp.r2max = r2;
    g_wp.sel = p->windowsel;
    for (int l = 0; l < 6; l++) {
      uint32_t n = (p->windowsel >> (l * 4)) & 0xf;
      if (n & 2)
        g_wp.w1use |= (uint8_t)(1 << l);
      if (n & 8)
        g_wp.w2use |= (uint8_t)(1 << l);
      g_wp.ml |= (uint32_t)p->windowLayer[l].maskLogic << (2 * l);
    }
    g_wp.tm = p->screenEnabled[0];
    g_wp.ts = p->screenEnabled[1];
    for (int i = 0; i < 6; i++)
      g_wp.math |= (uint8_t)((p->mathEnabled[i] ? 1 : 0) << i);
    g_wp.add = p->addSubscreen ? 1 : 0;
  } else {
    if (l1 != g_wp.pl1 || r1 != g_wp.pr1)
      g_wp.chg1++;
    if (l2 != g_wp.pl2 || r2 != g_wp.pr2)
      g_wp.chg2++;
    if (p->windowsel != g_wp.psel)
      g_wp.chgsel++;
    if (l1 < g_wp.l1min) g_wp.l1min = l1;
    if (l1 > g_wp.l1max) g_wp.l1max = l1;
    if (r1 < g_wp.r1min) g_wp.r1min = r1;
    if (r1 > g_wp.r1max) g_wp.r1max = r1;
    if (l2 < g_wp.l2min) g_wp.l2min = l2;
    if (l2 > g_wp.l2max) g_wp.l2max = l2;
    if (r2 < g_wp.r2min) g_wp.r2min = r2;
    if (r2 > g_wp.r2max) g_wp.r2max = r2;
  }
  g_wp.pl1 = l1; g_wp.pr1 = r1; g_wp.pl2 = l2; g_wp.pr2 = r2;
  g_wp.psel = p->windowsel;
  g_wp.mon |= m_on;
  g_wp.son |= s_on;
  g_wp.cw |= cw;
  g_wp.on++;
  g_wp.last = line - 1;
}

static void WinFrameEnd(void) {
  if (g_wp.on == 0)
    return;
  g_wp_total++;
  if (g_wp_total <= 4 || g_wp_total % 20 == 0)
    PspLog("WIN f=%u n=%u rows=%d/%d (%d..%d) main=%02x sub=%02x cw=%d | sel=%06x ml=%03x w1=%02x w2=%02x selchg=%d | w1 L%d-%d R%d-%d chg=%d | w2 L%d-%d R%d-%d chg=%d | TM=%02x TS=%02x math=%02x add=%d",
           (unsigned)g_frame, (unsigned)g_wp_total, g_wp.on, g_wp.rows, g_wp.first, g_wp.last, g_wp.mon, g_wp.son,
           g_wp.cw, (unsigned)g_wp.sel, (unsigned)g_wp.ml, g_wp.w1use, g_wp.w2use, g_wp.chgsel, g_wp.l1min,
           g_wp.l1max, g_wp.r1min, g_wp.r1max, g_wp.chg1, g_wp.l2min, g_wp.l2max, g_wp.r2min, g_wp.r2max,
           g_wp.chg2, g_wp.tm, g_wp.ts, g_wp.math, g_wp.add);
  g_wp.on = 0;
}
#endif

#if SM_PSP_LOG
/* Flicker probe (log only, no effect on drawing). The game may fake transparency by alternating something every
 * other frame. Per frame: a hash of the layer / colour-math / window-select registers over all rows (reg), of OAM
 * (oam) and of VRAM (vram). A hash that equals its value two frames ago but not last frame's is an A-B-A-B step. */
enum { kFlkReg, kFlkOam, kFlkVram, kFlkKinds, kFlkRun = 6, kFlkLogMax = 6 };
static struct {
  uint32_t cur[kFlkKinds], p1[kFlkKinds], p2[kFlkKinds];
  uint32_t run[kFlkKinds], frames[kFlkKinds], logged[kFlkKinds];
  uint32_t tm, ts, math, sel, fix;
  uint8_t add, half, neg;
  int lines;
} g_flk;

static inline uint32_t FlkMix(uint32_t h, uint32_t w) {
  return (h ^ w) * 16777619u;
}

static void FlickProbe(const Ppu *p, int line) {
  if (line < 1 || line > kMaxLines)
    return;
  uint32_t mm = 0;
  for (int i = 0; i < 6; i++)
    mm |= (uint32_t)(p->mathEnabled[i] ? 1 : 0) << i;
  uint32_t fix = (uint32_t)(p->fixedColorR | p->fixedColorG << 5 | p->fixedColorB << 10);
  uint32_t w0 = (uint32_t)p->screenEnabled[0] | (uint32_t)p->screenEnabled[1] << 8 | mm << 16 |
                (uint32_t)(p->addSubscreen ? 1 : 0) << 22 | (uint32_t)(p->halfColor ? 1 : 0) << 23 |
                (uint32_t)(p->subtractColor ? 1 : 0) << 24 | (uint32_t)p->preventMathMode << 25 |
                (uint32_t)p->clipMode << 27 | (uint32_t)(p->forcedBlank ? 1 : 0) << 29;
  uint32_t w1 = fix | (uint32_t)p->brightness << 16;
  uint32_t w3 = (uint32_t)p->screenWindowed[0] | (uint32_t)p->screenWindowed[1] << 8 | (uint32_t)p->mode << 16;
  if (line == 1) {
    g_flk.lines = 0;
    g_flk.cur[kFlkReg] = 2166136261u;
    uint32_t h = 2166136261u, w;
    for (int i = 0; i < 256; i += 2) {
      memcpy(&w, &p->oam[i], 4);
      h = FlkMix(h, w);
    }
    for (int i = 0; i < 32; i += 4) {
      memcpy(&w, &p->highOam[i], 4);
      h = FlkMix(h, w);
    }
    g_flk.cur[kFlkOam] = h;
    h = 2166136261u;
    for (int i = 0; i < 0x8000; i += 2) {
      memcpy(&w, &p->vram[i], 4);
      h = FlkMix(h, w);
    }
    g_flk.cur[kFlkVram] = h;
    g_flk.tm = p->screenEnabled[0];
    g_flk.ts = p->screenEnabled[1];
    g_flk.math = mm;
    g_flk.sel = p->windowsel;
    g_flk.fix = fix;
    g_flk.add = p->addSubscreen ? 1 : 0;
    g_flk.half = p->halfColor ? 1 : 0;
    g_flk.neg = p->subtractColor ? 1 : 0;
  }
  uint32_t h = g_flk.cur[kFlkReg];
  h = FlkMix(h, w0);
  h = FlkMix(h, w1);
  h = FlkMix(h, (uint32_t)p->windowsel);
  h = FlkMix(h, w3);
  g_flk.cur[kFlkReg] = h;
  g_flk.lines++;
}

static void FlickFrameEnd(void) {
  static const char *const kName[kFlkKinds] = {"reg", "oam", "vram"};
  if (g_flk.lines == 0)
    return;
  for (int k = 0; k < kFlkKinds; k++) {
    bool alt = g_flk.cur[k] == g_flk.p2[k] && g_flk.cur[k] != g_flk.p1[k];
    g_flk.run[k] = alt ? g_flk.run[k] + 1 : 0;
    if (g_flk.run[k] >= (uint32_t)kFlkRun) {
      g_flk.frames[k]++;
      if (g_flk.run[k] == (uint32_t)kFlkRun && g_flk.logged[k] < (uint32_t)kFlkLogMax) {
        g_flk.logged[k]++;
        PspLog("FLICKER f=%u kind=%s | row0 TM=%02x TS=%02x math=%02x add=%d half=%d neg=%d fix=%04x wsel=%06x lines=%d",
               (unsigned)g_frame, kName[k], (unsigned)g_flk.tm, (unsigned)g_flk.ts, (unsigned)g_flk.math,
               g_flk.add, g_flk.half, g_flk.neg, (unsigned)g_flk.fix, (unsigned)g_flk.sel, g_flk.lines);
      }
    }
    g_flk.p2[k] = g_flk.p1[k];
    g_flk.p1[k] = g_flk.cur[k];
  }
  g_flk.lines = 0;
}

static void FlickLog(void) {
  if (g_flk.frames[kFlkReg] | g_flk.frames[kFlkOam] | g_flk.frames[kFlkVram])
    PspLog("FLK frames in A-B alternation (of 300): reg=%u oam=%u vram=%u", (unsigned)g_flk.frames[kFlkReg],
           (unsigned)g_flk.frames[kFlkOam], (unsigned)g_flk.frames[kFlkVram]);
  memset(g_flk.frames, 0, sizeof(g_flk.frames));
}
#endif

/* Fullscreen source dimensions and HUD layout. */
enum {
  kSrcRows = 224,
  kCompX = 60, kCompY = 1, kCompW = 360, kCompH = 270,
  kHudRows = 31,          /* HUD IRQ ends at V=31 */
  kHudRowsMax = 32,       /* Gameplay begins at source row 32 */
  kHudBlock = 24,         /* Three rows of 8-pixel HUD tiles */
  kHudResvWord = 0x5820 + 8, /* hud_tilemap[8]: auto-reserve glyph */
};

// The immutable header row is copied by InitializeHud() from ROM $80988B to VRAM $5800. It identifies the HUD independently of the IRQ handler or room. The HUD itself is drawn during the opening 31/32 scanlines. Later BG3 activity and scroll changes belong to the gameplay area and must not invalidate the detected HUD interval.
extern const uint16 g_hud_top_row[32];
typedef struct HudCap {
  uint16_t run;            /* consecutive HUD rows from source row 0 */
  uint16_t nlines;         /* last line seen (224 normally, 239/240 for an overscan frame) */
  uint16_t hs, vs;         /* BG3 scroll on the first HUD row */
  uint16_t tm4;            /* BG3-only rows (mode 1, not blanked) that did not match the HUD map */
  uint16_t rsv;            /* hud_tilemap[8] as rendered this frame (auto-reserve glyph) */
  uint8_t brightness;      /*capture brightness*/
  bool open, gap, scroll_ok, marker_ok;
} HudCap;
static HudCap g_hud;
static const Ppu *g_hud_ppu;  /* for frameOverscan, which the PPU sets at V=225 of the frame being composed */

/* The HUD IRQ handlers (sm_80.c *_BeginHud) write CGWSEL = CGADSUB = 0.
 * Match every decoded bit so gameplay rows with the same BG3 configuration
 * cannot match merely because their layer colour-math enables are clear. */
static inline bool HudColorRegsZero(const Ppu *p) {
  if (p->directColor || p->addSubscreen || p->preventMathMode || p->clipMode ||
      p->subtractColor || p->halfColor)
    return false;
  for (int i = 0; i < 6; i++)
    if (p->mathEnabled[i])
      return false;
  return true;
}

static inline bool HudLineSig(const Ppu *p) {
  const BgLayer *b = &p->bgLayer[2];
  return !p->forcedBlank && p->mode == 1 && (p->screenEnabled[0] & 0x1f) == 4 && b->tilemapAdr == 0x5800 &&
         !b->tilemapWider && b->tilemapHigher && !b->bigTiles && HudColorRegsZero(p);
}

static bool HudTopRowSig(const Ppu *p) {
  for (int i = 0; i < 32; i++)
    if (p->vram[0x5800 + i] != g_hud_top_row[i])
      return false;
  return true;
}

static void HudCapture(const Ppu *p, int line) {
  if (line < 1 || line > kMaxLines)
    return;
  HudCap *h = &g_hud;
  g_hud_ppu = p;
  if (line == 1) {
    memset(h, 0, sizeof(*h));
    h->marker_ok = HudTopRowSig(p);
    h->open = h->marker_ok;
    h->scroll_ok = true;
  } else if (line != (int)h->nlines + 1) {
    h->gap = true;
  }
  h->nlines = (uint16_t)line;
  if (!HudLineSig(p)) {
    if (line <= kHudRowsMax)
      h->open = false;
    if (!p->forcedBlank && p->mode == 1 && (p->screenEnabled[0] & 0x1f) == 4 &&
        HudColorRegsZero(p))
      h->tm4++;
    return;
  }
  if (line > kHudRowsMax || !h->open)
    return;

  uint16_t hs = p->bgLayer[2].hScroll & 0x3ff, vs = p->bgLayer[2].vScroll & 0x3ff;
  if (h->run == 0) {
    h->hs = hs;
    h->vs = vs;
    h->rsv = p->vram[kHudResvWord];
    h->brightness = p->brightness & 0x0f;
  } else if (hs != h->hs || vs != h->vs) {
    h->scroll_ok = false;
  }
  h->run++;
  if (line == kHudRowsMax)
    h->open = false;
}

/* Layout decision. kHudOk: Layout A; everything else is Layout B (the full image) with the reason for the log. */
enum { kHudOk, kHudNone, kHudGap, kHudStray, kHudScroll, kHudLong, kHudGeom, kHudShort, kHudOverscan };

/* *off: BG3 pixel row b is on screen row b - *off (the SNES BG line is 1-based: off = 1 + vscroll). hud_tilemap row 0 is BG
 * tile row 1 ($5820; the 32-word row at $5800 is a separate header row), so it starts on screen row 8 - off. */
static int HudResolve(int *off) {
  const HudCap *h = &g_hud;
  if ((g_hud_ppu && g_hud_ppu->frameOverscan) || h->nlines > kSrcRows)
    return kHudOverscan;  /* 239/240 line frame: composed as Layout B from its first 224 rows */
  if (h->gap || h->nlines < kSrcRows)
    return kHudGap;
  if (!h->marker_ok || h->run == 0)
    return kHudNone;
  if (!h->scroll_ok || (h->hs & 0x3ff) != 0)
    return kHudScroll;
  if (h->run < kHudRows)
    return kHudShort;
  if (h->run > kHudRowsMax)
    return kHudLong;
  int vs = h->vs & 0x3ff;
  if (vs >= 0x200)
    vs -= 0x400;
  const int o = 1 + vs;
  if (8 - o < 0 || 8 - o + kHudBlock > (int)h->run)
    return kHudGeom;
  *off = o;
  return kHudOk;
}

#if SM_PSP_DOOR_SNAP
static void DsCaptureTop(const Ppu *p, int line);
#endif

void PspRenderer_CaptureLine(const Ppu *p, int line) {
  HudCapture(p, line);
#if SM_PSP_DOOR_SNAP
  DsCaptureTop(p, line);
#endif
#if SM_PSP_LOG
#if SM_PSP_PROBES
  WinProbe(p, line);
  FlickProbe(p, line);
#endif
  uint32_t t = Now();
#if SM_PSP_PROBES
  M7Probe(p, line);
#endif
  CaptureImpl(p, line);
  g_cap_us += Now() - t;
#else
  CaptureImpl(p, line);
#endif
}

//tile atlas

/* Decodes (if changed this frame) physical tile pt and returns true if empty.
 * bpp4: 16 words/tile, atlas 32 tiles wide. 2bpp: 8 words/tile, 64 wide.
 * One 32-bit store per tile row into the uncached VRAM mirror. */
static bool EnsureTileHash(bool bpp4, int pt) {
  uint16_t *seen = bpp4 ? g_seen4 : g_seen2;
  uint32_t *hash = bpp4 ? g_hash4 : g_hash2;
  uint8_t *empty = bpp4 ? g_empty4 : g_empty2;
  if (seen[pt] == g_stamp)
    return empty[pt];
  seen[pt] = g_stamp;
  PROF(g_c_tseen++;)
  int words = bpp4 ? 16 : 8;
  const uint16_t *src = &g_vram[pt * words];
  uint32_t h = 2166136261u;
  for (int i = 0; i < words; i++)
    h = (h ^ src[i]) * 16777619u;
  if (hash[pt] == h)
    return empty[pt];
  hash[pt] = h;
  (bpp4 ? g_ver4 : g_ver2)[pt]++;
  PROF(g_c_tdec++;)
  uint32_t *dst;
  int stride_words;
#if SM_PSP_SWZ_BG
// swizzled: atlas4 is 128 B wide (8 blocks per block row), atlas2 256 B (16); tile column (pt & 31 / 63) >> 2 picks the block, (pt & 3) * 4 the byte offset in the 16 B block row, the 8 tile rows are 16 B apart
  if (bpp4)
    dst = (uint32_t *)(g_atlas4 + ((((pt >> 5) << 3) + ((pt & 31) >> 2)) << 7) + (pt & 3) * 4);
  else
    dst = (uint32_t *)(g_atlas2 + ((((pt >> 6) << 4) + ((pt & 63) >> 2)) << 7) + (pt & 3) * 4);
  stride_words = 16 / 4;
#else
  if (bpp4) {
    dst = (uint32_t *)(g_atlas4 + (pt >> 5) * 8 * 128 + (pt & 31) * 4);
    stride_words = 128 / 4;
  } else {
    dst = (uint32_t *)(g_atlas2 + (pt >> 6) * 8 * 256 + (pt & 63) * 4);
    stride_words = 256 / 4;
  }
#endif
  uint32_t any = 0, used = 0;
  for (int y = 0; y < 8; y++, dst += stride_words) {
    uint32_t w0 = src[y], w1 = bpp4 ? src[8 + y] : 0, row = 0;
    for (int x = 0; x < 8; x++) {
      int s = 7 - x;
      uint32_t px = (w0 >> s & 1) | ((w0 >> (s + 7)) & 2);
      if (bpp4)
        px |= ((w1 >> s & 1) << 2) | ((w1 >> (s + 5)) & 8);
      row |= px << (4 * x);
      used |= 1u << px;
    }
    *dst = row;
    any |= row;
  }
  empty[pt] = any == 0;
  (bpp4 ? g_mask4 : g_mask2)[pt] = (uint16_t)used;
  return empty[pt];
}

#if SM_PSP_TILE_GEN
/* Gate: a tile whose page generation has not moved since the tile was last hashed needs no hash. */
static bool EnsureTile(bool bpp4, int pt) {
  uint16_t *seen = bpp4 ? g_seen4 : g_seen2;
  uint8_t *empty = bpp4 ? g_empty4 : g_empty2;
  if (seen[pt] == g_stamp)
    return empty[pt];
  uint16_t *tg = &(bpp4 ? g_tgen4 : g_tgen2)[pt];
  const uint16_t pgen = g_pgen[((unsigned)pt * (bpp4 ? 16u : 8u)) >> 8];
  bool audit = false;
  if (g_tgen_ok && *tg != 0 && *tg == pgen) {
#if SM_PSP_LOG
    audit = (++g_tg_n & 63) == 0;  /* every 64th skip is hashed anyway and must match */
#endif
    if (!audit) {
      seen[pt] = g_stamp;
      PROF(g_c_tseen++; g_tg_skip++;)
      return empty[pt];
    }
  }
#if SM_PSP_LOG
  const uint32_t ver0 = (bpp4 ? g_ver4 : g_ver2)[pt];
#endif
  const bool e = EnsureTileHash(bpp4, pt);
#if SM_PSP_LOG
  if (audit) {
    g_tg_audit++;
    if ((bpp4 ? g_ver4 : g_ver2)[pt] != ver0) {  /* the tile changed although its page generation did not */
      g_tgen_ok = false;
      g_tg_bad++;
      PspLog("tilegen: stale %s tile %d (page gen %u), tile generations disabled", bpp4 ? "4bpp" : "2bpp", pt, (unsigned)pgen);
    }
  }
#endif
  *tg = pgen;
  return e;
}
#else
#define EnsureTile EnsureTileHash
#endif

static void BuildClut2(int b) {
  if (g_clut2_stamp[b] == g_stamp)
    return;
  g_clut2_stamp[b] = g_stamp;
  for (int pal = 0; pal < 8; pal++)
    for (int j = 0; j < 8; j++)
      g_clut2[b][pal][j] = BakeColor(g_cgram[pal * 4 + (j & 3)], b, (j & 3) == 0);
}

static void BuildClut4(int b) {
  if (g_clut4_stamp[b] == g_stamp)
    return;
  g_clut4_stamp[b] = g_stamp;
  for (int pal = 0; pal < 8; pal++)
    for (int j = 0; j < 16; j++)
      g_clut4[b][pal][j] = BakeColor(g_cgram[pal * 16 + j], b, j == 0);
}

static void BuildClutObj(int b) {
  if (g_clutobj_stamp[b] == g_stamp)
    return;
  g_clutobj_stamp[b] = g_stamp;
  for (int pal = 0; pal < 8; pal++)
    for (int j = 0; j < 16; j++)
      g_clutobj[b][pal][j] = BakeColor(g_cgram[128 + pal * 16 + j], b, j == 0);
}

/* Gate 4t-d: BG3 palette for the half-colour subscreen target: index 0 transparent, every other entry opaque
 * (even black) with alpha 0x80, which the composite turns into the 1/2 factors. */
static void BuildClut2Half(void) {
  if (g_clut2h_stamp == g_stamp)
    return;
  g_clut2h_stamp = g_stamp;
  for (int pal = 0; pal < 8; pal++)
    for (int j = 0; j < 8; j++)
      g_clut2h[pal][j] = (j & 3) ? ((BakeColor(g_cgram[pal * 4 + (j & 3)], kClutBright, false) & 0x00ffffffu) | 0x80000000u)
                                 : 0u;
}

/* Mode 7 tiles: the pixel is the high byte of vram[tile * 64 + row * 8 + col]. Same scheme as EnsureTile.
 * Returns true if the tile is empty (all pixels 0 = transparent). */
static bool EnsureTile7(int t) {
  if (g_seen7[t] == g_stamp)
    return g_empty7[t];
  g_seen7[t] = g_stamp;
  const uint16_t *src = &g_vram[t * 64];
  uint32_t h = 2166136261u;
  for (int i = 0; i < 64; i++)
    h = (h ^ (src[i] >> 8)) * 16777619u;
  if (g_hash7[t] == h)
    return g_empty7[t];
  g_hash7[t] = h;
#if SM_PSP_SWZ_M7
// swizzled 128x128 T8: 8 blocks per block row; tile column (t & 15) >> 1 picks the block, (t & 1) * 8 the offset
  uint32_t *dst = (uint32_t *)(g_atlas7 + ((((t >> 4) << 3) + ((t & 15) >> 1)) << 7) + (t & 1) * 8);
  const int stride7 = 16 / 4;
#else
  uint32_t *dst = (uint32_t *)(g_atlas7 + (t >> 4) * 8 * 128 + (t & 15) * 8);
  const int stride7 = 128 / 4;
#endif
  uint32_t any = 0;
  for (int y = 0; y < 8; y++, dst += stride7) {
    uint32_t w0 = 0, w1 = 0;
    for (int x = 0; x < 4; x++) {
      w0 |= (uint32_t)(src[y * 8 + x] >> 8) << (8 * x);
      w1 |= (uint32_t)(src[y * 8 + 4 + x] >> 8) << (8 * x);
    }
    dst[0] = w0;
    dst[1] = w1;
    any |= w0 | w1;
  }
  g_empty7[t] = any == 0;
  return g_empty7[t];
}

static void BuildClut7(int b) {
  if (g_clut7_stamp == g_stamp)
    return;
  g_clut7_stamp = g_stamp;
  for (int i = 0; i < 256; i++)
    g_clut7[i] = BakeColor(g_cgram[i], b, i == 0);
}

/* Gate 5l: layer images. A BG2/BG3 layer split into many scroll runs (HDMA waves, ~100 runs of 2 rows) costs ~3.5k tile
 * quads per layer on the CPU. Instead the whole tilemap is kept as an 8 bit image in VRAM (512x256, index =
 * priority << 7 | palette << (4 or 2) | pixel, 0 = transparent) that is updated only where a tilemap word or a tile's
 * data changed, and every run is one textured strip (GU_REPEAT does the scroll wrap). The palette and the tile
 * priority come from two CLUTs (one per priority pass; entries of the other priority are transparent). */
enum { kImgCells = 64 * 32, kImgMinRuns = 12, kImgVerify = 8 };  /* image slots and VRAM offsets: see the VRAM layout at the top */

/* Final 4:3 composition (fullscreen.md). The GE scene is still drawn at (kLeft, kTop) of the draw buffer. Present() then
 * copies its 256x224 visible rows into the texture region (kVramTexture, 262144 bytes) as a 256x256 RGB565 image, and the
 * compositor samples that copy, so no render target is sampled while the GE writes it. No EDRAM is added: this region
 * holds the multisub target, the mosaic source images and the software-frame texture, which are all dead once the scene
 * list has been synced (each is rewritten before use in the next frame; layer images, atlases and frame buffers are
 * elsewhere). The image is described to the GE with its real size, so every texel the GE may address is inside the
 * allocation. Rows 224-255 hold stale data and are never sampled (UV inset, see ComposeQuad users).
 * Only the 224 visible SNES rows exist; an overscan frame is composed as Layout B from its first 224 rows. */

CASSERT(kGameWidth * 256 * 2 <= kSubBytes);  /* 256x256 RGB565 fits the 256 KB texture region */

typedef struct LayerImg { uint16_t map, tile; uint8_t wide, high, valid; uint16_t nhi, maxt; } LayerImg;  /* maxt: highest tile number + 1 */
static LayerImg g_img[kImgSlots];                       /* slot = layer - 1 (BG2 4bpp, BG3 2bpp) */
static uint16_t g_img_cw[kImgSlots][kImgCells];         /* tilemap word each image cell was built from */
static uint32_t g_img_cv[kImgSlots][kImgCells];         /* tile version it was built from, 0xffffffff = invalid */
static uint32_t g_climg[kImgSlots][2][256] __attribute__((aligned(64)));
static uint16_t g_climg_stamp[kImgSlots];
static uint8_t *g_img_tex[kImgSlots];                   /* uncached VRAM */
static bool g_img_ok = SM_PSP_LAYER_IMG;                /* cleared if the first verified frames differ from software */
static bool g_imgh_ok = SM_PSP_LAYER_IMG >= 2;          /* gate 5m: 32x64 tilemaps; cleared if the first verified such frames differ */
static bool g_img_skip_ok = SM_PSP_LAYER_IMG >= 3;      /* gate 5m: no written page in map or tiles since the last scan: skip it */
#if SM_PSP_DOOR_GE
// Door transition: old-room snapshot (256x192 RGB565) lives in layer-image slot 0 while the layer images are off.
// live: the snapshot is refreshed from every composed frame; held: the last live frame covers the scene; armed: it
// follows the game camera (new room loaded) and slides off.
static bool g_door_ok, g_door_reserved, g_door_live, g_door_snapshot_valid, g_door_armed;
static bool g_door_saved_img_ok, g_door_saved_imgh_ok, g_door_saved_img_skip_ok;
static unsigned g_door_direction;
static uint16_t g_door_ref_x, g_door_ref_y, g_door_cam_x, g_door_cam_y;
#endif
static uint8_t g_img_dirty[kImgSlots][kVramPages];      /* VRAM pages written since this image's last scan (SnapshotVram) */
static uint8_t g_img_tpg[kImgSlots][kVramPages];        /* gate 5n: VRAM pages holding tile data that a cell of the image uses (set by the scan) */
static bool g_img_on[kLayers], g_img_frame, g_imgh_frame;
static TileVert *g_imgv[kLayers][2];
static int g_imgn[kLayers][2], g_img_nv;
static uint32_t g_img_vn, g_img_vbad, g_frames_img, g_imgh_vn, g_imgh_vbad;
/* Gate 5p: a subscreen BG layer with many scroll runs (HDMA haze / ripple) is drawn from the same layer image.
 * One audit and switch per kind of frame: 0 plain subscreen blend, 1 subscreen + fixed colour, 2 half colour. */
#ifndef SM_PSP_SUB_IMG
#define SM_PSP_SUB_IMG 1  /* gate 5p: 0 = subscreen layers are always drawn from tile quads */
#endif
enum { kSimgKinds = 3, kSimgVerify = 8 };
static bool g_simg_ok[kSimgKinds] = {SM_PSP_SUB_IMG, SM_PSP_SUB_IMG, SM_PSP_SUB_IMG};
static bool g_simg_on[kLayers], g_simg_frame;
static int g_simg_kind;
static TileVert *g_simgv[kLayers][2];
static int g_simgn[kLayers][2];
static uint32_t g_simg_vn[kSimgKinds], g_simg_vbad[kSimgKinds], g_frames_simg;
static uint32_t g_climgh[kImgSlots][2][256] __attribute__((aligned(64)));  /* half-colour target: opaque = alpha 0x80 */
static uint16_t g_climgh_stamp[kImgSlots];
#if SM_PSP_LOG
static uint32_t g_li_n, g_li_upd, g_li_updmax, g_li_us, g_li_usmax, g_li_skip, g_li_blkm, g_li_blkt;
static uint32_t g_li_pg[kVramPages];  /* scans blocked by a dirty map page, per VRAM page */
#endif

static void ImgMarkDirty(int pg0, int pg1) {
  for (int s = 0; s < kImgSlots; s++)
    memset(&g_img_dirty[s][pg0], 1, (size_t)(pg1 - pg0));
}

static void ImgMarkAllDirty(void) {
  ImgMarkDirty(0, kVramPages);
}

static void DoorTransitionRelease(void) {
#if SM_PSP_DOOR_GE
  g_door_pal_mode = kDoorPalGame;
  g_door_pal_dest = NULL;
  if (!g_door_reserved)
    return;
  g_door_reserved = g_door_live = g_door_snapshot_valid = g_door_armed = false;
  g_img_ok = g_door_saved_img_ok;
  g_imgh_ok = g_door_saved_imgh_ok;
  g_img_skip_ok = g_door_saved_img_skip_ok;
  memset(g_img, 0, sizeof(g_img));  /* slot 0 held the snapshot: rebuild both images */
  ImgMarkAllDirty();
#endif
}


void PspRenderer_BeginDoorTransition(void) {
#if SM_PSP_DOOR_GE
  if (!g_door_ok || g_door_reserved)
    return;
  g_door_saved_img_ok = g_img_ok;
  g_door_saved_imgh_ok = g_imgh_ok;
  g_door_saved_img_skip_ok = g_img_skip_ok;
  g_img_ok = g_imgh_ok = g_img_skip_ok = false;  /* no layer image (main or subscreen) may use slot 0 */
  g_door_reserved = g_door_live = true;
  g_door_snapshot_valid = g_door_armed = false;
  g_door_pal_dest = NULL;
  g_door_pal_mode = kDoorPalCapture;
#endif
}

void PspRenderer_DoorTransitionFreeze(void) {
#if SM_PSP_DOOR_GE
  if (!g_door_reserved || !g_door_live)
    return;
  g_door_live = false;
  if (!g_door_snapshot_valid) {  /* nothing was captured: leave the transition to the game's own fade */
    PspLog("door: no snapshot at freeze");
    DoorTransitionRelease();
  }
#endif
}

void PspRenderer_DoorTransitionRef(uint16_t layer1_x, uint16_t layer1_y, unsigned direction) {
#if SM_PSP_DOOR_GE
  if (!g_door_reserved || g_door_live)
    return;
  g_door_ref_x = g_door_cam_x = layer1_x;
  g_door_ref_y = g_door_cam_y = layer1_y;
  g_door_direction = direction & 3;
#else
  (void)layer1_x, (void)layer1_y, (void)direction;
#endif
}

void PspRenderer_DoorTransitionArm(const uint16_t *dest_palettes) {
#if SM_PSP_DOOR_GE
  if (!g_door_reserved || g_door_live || !g_door_snapshot_valid || !dest_palettes)
    return;
  g_door_pal_dest = dest_palettes;
  g_door_pal_mode = kDoorPalDest;
  g_door_armed = true;
#else
  (void)dest_palettes;
#endif
}

void PspRenderer_DoorTransitionCamera(uint16_t layer1_x, uint16_t layer1_y) {
#if SM_PSP_DOOR_GE
  if (!g_door_reserved)
    return;
  g_door_cam_x = layer1_x;
  g_door_cam_y = layer1_y;
#else
  (void)layer1_x, (void)layer1_y;
#endif
}

void PspRenderer_EndDoorTransition(void) {
#if SM_PSP_DOOR_GE
  DoorTransitionRelease();
#endif
}

bool PspRenderer_DoorTransitionActive(void) {
#if SM_PSP_DOOR_GE
  return g_door_reserved;
#else
  return false;
#endif
}

bool PspRenderer_DoorTransitionArmed(void) {
#if SM_PSP_DOOR_GE
  return g_door_reserved && g_door_armed;
#else
  return false;
#endif
}

/* ---- Instant door transition (SM_PSP_DOOR_SNAP) -------------------------------------------------------------------------
 * The game loads the destination room within one frame (sm_82.c DoorSnap_Run); the GE slides two frozen 256x192 RGB565
 * snapshots of the gameplay rows (HUD rows are live) instead of the live scroll. Both live in the layer image slots (each
 * slot is exactly 256x256x2 bytes), which are switched off from Arm to release, so nothing else uses them:
 *   Arm (door hit, frame N-1)  layer images off; this frame's composition is copied to slot 0 (old room) -> HaveA
 *   Ready/Begin (frame N)      the game loaded the room; from now on the old snapshot covers the screen -> WaitB
 *   composition of frame N     the first valid frame (HUD layout resolved, GE scene) is copied to slot 1 -> Play
 *   Hold (frames N+1...N+n)    the game logic is held, the step counts the eased slide (step k on frame N+k, so step n, the exact
 *                              destination, is drawn); on frame N+n+1 everything is released and the live frames resume
 * Held frames keep the OAM and registers of frame N, so a snapshot that is refused on one frame is retried on the next
 * (kDsMaxWaitB), then the transition is dropped and the live destination shows (a cut, never a stuck lock). */
#if SM_PSP_DOOR_SNAP
enum { kDsIdle, kDsArmed, kDsHaveA, kDsWaitB, kDsPlay };
enum { kDsMaxWaitB = 8, kDsMaxHaveA = 4 };
CASSERT(kImgBytes >= 256 * 256 * 2);  /* a snapshot is described to the GE as 256x256 RGB565 and must fit its slot */
static uint8_t g_ds_state, g_ds_frames, g_ds_dir, g_ds_n;  /* g_ds_n: slide length in steps (DsStepsForSpeed) */
static uint16_t g_ds_step;
static int16_t g_ds_align;
static bool g_ds_ok, g_ds_saved_img_ok, g_ds_saved_imgh_ok, g_ds_saved_skip_ok;

/* The rows under the HUD. A vertical door shows the LOWER room's top 32 source rows (the HUD covers them in the live game: the
 * PPU draws BG3 only on those lines, so they are in no buffer) in the gap between the two images. They are drawn here by the
 * software PPU with the register state of the first gameplay line, once, on the frame the lower room is captured: a down door's
 * lower room is the new one (frame N), an up door's the old one (the armed frame). Rows are staged as RGB565 and copied into the
 * unused rows 192..223 of that room's slot; nothing else reads those rows. */
enum { kDsTopRows = 32, kDsTopRow0 = 192 };
CASSERT(kDsTopRow0 + kDsTopRows <= 256);
static uint16_t g_ds_top_stage[kGameWidth * kDsTopRows] __attribute__((aligned(64)));
/* Written (CPU) only on the frame that captures the lower room and read (GE copy) by that same frame's list; the next write is a
 * later transition's capture, so the stage needs no double buffering. */
static bool g_ds_top[2];       /* slot rows 192..223 hold the rows under the HUD */
static bool g_ds_top_ready;    /* the stage holds the rows of the frame being captured */
static uint8_t g_ds_arm_dir;
static uint32_t g_ds_top_us, g_ds_top_max_us, g_ds_top_n;

/* The slot whose room is the lower one of a vertical door and is captured from the frame being built, or -1. */
static int DsTopSlot(void) {
  if (g_ds_state == kDsArmed)
    return g_ds_arm_dir == 3 ? 0 : -1;  /* up door: the old room is below */
  if (g_ds_state == kDsWaitB)
    return g_ds_dir == 2 ? 1 : -1;      /* down door: the new room is below */
  return -1;
}

/* Called for every line before it is drawn. At the first gameplay line (33) the registers are the gameplay ones: lines 1..32 are
 * drawn with them (the BG rows simply continue upwards), which is what the PPU would show there without the HUD. */
static void DsCaptureTop(const Ppu *p, int line) {
  if (line == 1)
    g_ds_top_ready = false;
  if (line != kHudRowsMax + 1 || !g_ge_active || DsTopSlot() < 0)
    return;
  uint8_t row[kGameWidth * 4] __attribute__((aligned(16)));
  const uint32_t t0 = sceKernelGetSystemTimeLow();
  for (int l = 1; l <= kDsTopRows; l++) {
    if (!ppu_drawLineScratch((Ppu *)p, l, row)) {  /* the hook gets a const PPU; the scratch draw restores everything it touches */
      g_ds_top_ready = false;
      return;
    }
    uint16_t *d = &g_ds_top_stage[(l - 1) * kGameWidth];
    for (int x = 0; x < kGameWidth; x++)
      d[x] = Rgb565(row + x * 4);
  }
  g_ds_top_ready = true;
  const uint32_t us = sceKernelGetSystemTimeLow() - t0;
  g_ds_top_us += us;
  g_ds_top_n++;
  if (us > g_ds_top_max_us)
    g_ds_top_max_us = us;
  if (g_ds_top_n <= 16)
    PspLog("door snap: rows under the HUD drawn in %u us (slot %d)", (unsigned)us, DsTopSlot());
}

static void DoorSnapRelease(void) {
  if (g_ds_state == kDsIdle)
    return;
  g_ds_state = kDsIdle;
  g_ds_top[0] = g_ds_top[1] = g_ds_top_ready = false;
  g_img_ok = g_ds_saved_img_ok;
  g_imgh_ok = g_ds_saved_imgh_ok;
  g_img_skip_ok = g_ds_saved_skip_ok;
  memset(g_img, 0, sizeof(g_img));  /* both slots held snapshots: rebuild both images */
  ImgMarkAllDirty();
}
#endif

bool PspRenderer_DoorSnapCanArm(void) {
#if SM_PSP_DOOR_SNAP
#if SM_PSP_DOOR_GE
  if (g_door_reserved)
    return false;
#endif
  return g_ds_ok && g_ds_state == kDsIdle && g_ge_active;
#else
  return false;
#endif
}

void PspRenderer_DoorSnapArm(unsigned direction) {
#if SM_PSP_DOOR_SNAP
  if (!PspRenderer_DoorSnapCanArm())
    return;
  g_ds_arm_dir = (uint8_t)(direction & 3);
  g_ds_top[0] = g_ds_top[1] = g_ds_top_ready = false;
  g_ds_saved_img_ok = g_img_ok;
  g_ds_saved_imgh_ok = g_imgh_ok;
  g_ds_saved_skip_ok = g_img_skip_ok;
  g_img_ok = g_imgh_ok = g_img_skip_ok = false;  /* no layer image (main or subscreen) may use either slot */
  g_ds_state = kDsArmed;
  g_ds_frames = 0;
#else
  (void)direction;
#endif
}

bool PspRenderer_DoorSnapReady(void) {
#if SM_PSP_DOOR_SNAP
  return g_ds_state == kDsHaveA;
#else
  return false;
#endif
}

void PspRenderer_DoorSnapAbort(void) {
#if SM_PSP_DOOR_SNAP
  if (g_ds_state != kDsIdle)
    PspLog("door snap: dropped (state %d)", (int)g_ds_state);
  DoorSnapRelease();
#endif
}

void PspRenderer_DoorSnapBegin(unsigned direction, int align_px, int steps) {
#if SM_PSP_DOOR_SNAP
  if (g_ds_state != kDsHaveA)
    return;
  g_ds_dir = (uint8_t)(direction & 3);
  g_ds_n = (uint8_t)(steps < 1 ? DsSteps(g_ds_dir) : steps > 255 ? 255 : steps);
  g_ds_align = (int16_t)(align_px < -256 ? -256 : align_px > 256 ? 256 : align_px);
  g_ds_step = 0;
  g_ds_frames = 0;
  g_ds_state = kDsWaitB;
#else
  (void)direction, (void)align_px, (void)steps;
#endif
}

bool PspRenderer_DoorSnapHold(void) {
#if SM_PSP_DOOR_SNAP
  switch (g_ds_state) {
  case kDsArmed:
  case kDsHaveA:  /* the game commits (or aborts) within a frame or two */
    if (++g_ds_frames > kDsMaxHaveA) {
      PspLog("door snap: no commit");
      DoorSnapRelease();
    }
    return false;
  case kDsWaitB:
    if (++g_ds_frames > kDsMaxWaitB) {
      PspLog("door snap: no valid destination frame");
      DoorSnapRelease();
      return false;
    }
    return true;
  case kDsPlay:
    /* Frame N+k draws step k (the capture frame drew step 0), so steps 0..N are all shown and the last one is the exact
     * destination; the frame after it is the first live one. */
    if (g_ds_step >= (unsigned)g_ds_n) {
      DoorSnapRelease();
      return false;
    }
    g_ds_step++;
    return true;
  default:
    return false;
  }
#else
  return false;
#endif
}

/* True if a VRAM page of [word0, word0 + nwords) (modulo the 32 K words) was written since the last scan of slot. */
static bool ImgPagesDirty(int slot, int word0, int nwords) {
  int pg = (word0 >> 8) & (kVramPages - 1), n = (nwords + kVramPageWords - 1) / kVramPageWords;
  for (int i = 0; i < n; i++)
    if (g_img_dirty[slot][(pg + i) & (kVramPages - 1)])
      return true;
  return false;
}

/* True if a page of the tile data the image was built from was written since its last scan. */
static bool ImgTilePagesDirty(int slot) {
  for (int pg = 0; pg < kVramPages; pg++)
    if (g_img_tpg[slot][pg] && g_img_dirty[slot][pg])
      return true;
  return false;
}

#ifndef SM_PSP_VERIFY
#define SM_PSP_VERIFY 1
#endif
/* Planar to chunky for one image row. g_img_spread[hf][b][0 / 1] holds bit (hf ? x : 7 - x) of the byte b as byte x (0 or 1) of
 * pixels 0-3 / 4-7, so a row of 8 pixels is a few table loads and ORs. g_img_fast = false (a log build found a difference): the
 * pixel by pixel conversion below is used. */
static uint32_t g_img_spread[2][256][2];
static bool g_img_spread_ok, g_img_fast = true;

static void ImgSpreadInit(void) {
  for (int hf = 0; hf < 2; hf++)
    for (int v = 0; v < 256; v++) {
      uint32_t lo = 0, hi = 0;
      for (int x = 0; x < 8; x++) {
        const uint32_t bit = ((uint32_t)v >> (hf ? x : 7 - x)) & 1;
        if (x < 4)
          lo |= bit << (8 * x);
        else
          hi |= bit << (8 * (x - 4));
      }
      g_img_spread[hf][v][0] = lo;
      g_img_spread[hf][v][1] = hi;
    }
  g_img_spread_ok = true;
}

/* Row of 8 pixels: a = pixels 0-3, b = pixels 4-7, one byte each: 0 for a transparent pixel, else tag | pixel. */
static void ImgRowRef(uint32_t w0, uint32_t w1, bool bpp4, bool hf, uint32_t tag, uint32_t *oa, uint32_t *ob) {
  uint32_t a = 0, b = 0;
  for (int x = 0; x < 8; x++) {
    int s = hf ? x : 7 - x;
    uint32_t px = ((w0 >> s) & 1) | ((w0 >> (s + 7)) & 2);
    if (bpp4)
      px |= (((w1 >> s) & 1) << 2) | ((w1 >> (s + 5)) & 8);
    uint32_t v = px ? (tag | px) : 0;
    if (x < 4)
      a |= v << (8 * x);
    else
      b |= v << (8 * (x - 4));
  }
  *oa = a;
  *ob = b;
}

static inline void ImgRowFast(uint32_t w0, uint32_t w1, bool bpp4, bool hf, uint32_t tag, uint32_t *oa, uint32_t *ob) {
  const uint32_t *p0 = g_img_spread[hf][w0 & 0xff], *p1 = g_img_spread[hf][(w0 >> 8) & 0xff];
  uint32_t pa = p0[0] | p1[0] << 1, pb = p0[1] | p1[1] << 1;
  if (bpp4) {
    const uint32_t *p2 = g_img_spread[hf][w1 & 0xff], *p3 = g_img_spread[hf][(w1 >> 8) & 0xff];
    pa |= p2[0] << 2 | p3[0] << 3;
    pb |= p2[1] << 2 | p3[1] << 3;
  }
  const uint32_t tg = tag * 0x01010101u;
  uint32_t ma = pa, mb = pb;  /* 0xff in every byte whose pixel is not 0 */
  ma |= ma >> 1;
  ma |= ma >> 2;
  mb |= mb >> 1;
  mb |= mb >> 2;
  *oa = (pa | tg) & ((ma & 0x01010101u) * 0xffu);
  *ob = (pb | tg) & ((mb & 0x01010101u) * 0xffu);
}

/* One tilemap cell into the image (same plane decode as EnsureTile, flips applied here). stride: 512 (wide) or 256. */
static void ImgRenderCell(int slot, bool bpp4, int tx, int ty, unsigned t, int tileadr, int stride) {
  int base = tileadr + (int)(t & 0x3ff) * (bpp4 ? 16 : 8);
  unsigned tag = ((t >> 6) & 0x80) | (((t >> 10) & 7) << (bpp4 ? 4 : 2));
  bool hf = (t & 0x4000) != 0, vf = (t & 0x8000) != 0;
  uint32_t *d = (uint32_t *)(g_img_tex[slot] + ty * 8 * stride + tx * 8);
  if (!g_img_spread_ok)
    ImgSpreadInit();
  for (int y = 0; y < 8; y++, d += stride / 4) {
    int row = vf ? 7 - y : y;
    uint32_t w0 = g_vram[(base + row) & 0x7fff], w1 = bpp4 ? g_vram[(base + 8 + row) & 0x7fff] : 0;
    uint32_t a, b;
    if (g_img_fast)
      ImgRowFast(w0, w1, bpp4, hf, tag, &a, &b);
    else
      ImgRowRef(w0, w1, bpp4, hf, tag, &a, &b);
#if SM_PSP_LOG && SM_PSP_VERIFY
    {  /* the first rows of a log build are compared with the pixel by pixel conversion; a difference switches it back */
      static uint32_t nchk;
      if (g_img_fast && nchk < 200000) {
        nchk++;
        uint32_t ra, rb;
        ImgRowRef(w0, w1, bpp4, hf, tag, &ra, &rb);
        if (ra != a || rb != b) {
          PspLog("imgrow MISMATCH f=%u bpp4=%d hf=%d w0=%04x w1=%04x tag=%02x fast=%08x/%08x ref=%08x/%08x, pixel by pixel conversion restored",
                 (unsigned)g_frame, (int)bpp4, (int)hf, (unsigned)w0, (unsigned)w1, tag, (unsigned)a, (unsigned)b, (unsigned)ra, (unsigned)rb);
          g_img_fast = false;
          a = ra;
          b = rb;
        }
        if (nchk == 200000)
          PspLog("imgrow check: 200000 rows compared, %s", g_img_fast ? "no difference" : "restored");
      }
    }
#endif
    d[0] = a;  /* two 32 bit stores per row into the uncached VRAM mirror (the GE is idle here) */
    d[1] = b;
  }
}

static void ImgBuildClut(int slot, bool bpp4) {
  if (g_climg_stamp[slot] == g_stamp)
    return;
  g_climg_stamp[slot] = g_stamp;
  for (int pr = 0; pr < 2; pr++)
    for (int e = 0; e < 256; e++) {
      unsigned px = (unsigned)e & (bpp4 ? 15u : 3u);
      uint32_t c = 0;
      if (px && (e >> 7) == pr)
        c = BakeColor(g_cgram[bpp4 ? (e & 0x7f) : (e & 0x1f)], kClutBright, false);
      g_climg[slot][pr][e] = c;
    }
  sceKernelDcacheWritebackRange(g_climg[slot], sizeof(g_climg[slot]));
}

/* All runs of layer l must share the tilemap set-up (only scroll and window differ), and the map must be one or two
 * screens wide or high (not both; gate 5m). */
static bool ImgEligible(int l) {
  const BgSnap *b0 = &g_lines[g_runs[l][0].first].bg[l];
  if ((b0->flags & 3) == 3 || ((b0->flags & 2) && !g_imgh_ok))  /* 64x64: no image; 32x64 while its audit holds */
    return false;
  for (int i = 1; i < g_nruns[l]; i++) {
    const BgSnap *b = &g_lines[g_runs[l][i].first].bg[l];
    if (b->mapadr != b0->mapadr || b->tileadr != b0->tileadr || ((b->flags ^ b0->flags) & 3))
      return false;
  }
  return true;
}

/* Gate 5p: image CLUT of the half-colour subscreen target (as BuildClut2Half: every opaque pixel, even black, has alpha 0x80). */
static void ImgBuildClutHalf(int slot, bool bpp4) {
  if (g_climgh_stamp[slot] == g_stamp)
    return;
  g_climgh_stamp[slot] = g_stamp;
  for (int pr = 0; pr < 2; pr++)
    for (int e = 0; e < 256; e++) {
      unsigned px = (unsigned)e & (bpp4 ? 15u : 3u);
      uint32_t c = 0;
      if (px && (e >> 7) == pr)
        c = (BakeColor(g_cgram[bpp4 ? (e & 0x7f) : (e & 0x1f)], kClutBright, false) & 0x00ffffffu) | 0x80000000u;
      g_climgh[slot][pr][e] = c;
    }
  sceKernelDcacheWritebackRange(g_climgh[slot], sizeof(g_climgh[slot]));
}

/* True if the image of layer l was built for this tilemap set-up (a main and a subscreen user of one image must agree). */
static bool ImgCfgMatches(int l, const BgSnap *b) {
  const LayerImg *im = &g_img[l - 1];
  return im->valid && im->map == b->mapadr && im->tile == b->tileadr && im->wide == (uint8_t)(b->flags & 1) &&
         im->high == (uint8_t)((b->flags >> 1) & 1);
}

/* Gate 5p: the n subscreen runs in g_subruns of layer l (BG2/BG3) can be drawn from its layer image: many runs, one
 * tilemap set-up (only scroll and window differ), and the audit of this kind of frame has not failed. */
static bool SubImgEligible(int l, int n, int kind) {
  if (l < 1 || n < kImgMinRuns || !g_img_ok || !g_simg_ok[kind])
    return false;
  const BgSnap *b0 = &g_lines[g_subruns[0].first].bg[l];
  if ((b0->flags & 3) == 3 || ((b0->flags & 2) && !g_imgh_ok))  /* 64x64: no image; 32x64 while its audit holds */
    return false;
  for (int i = 1; i < n; i++) {
    const BgSnap *b = &g_lines[g_subruns[i].first].bg[l];
    if (b->mapadr != b0->mapadr || b->tileadr != b0->tileadr || ((b->flags ^ b0->flags) & 3))
      return false;
  }
  return true;
}

#if SM_PSP_LOG
/* Why a layer with many scroll runs is not drawn from an image (first few occurrences per layer). */
static void NoteImgNo(int l) {
  static unsigned n[kLayers];
  if (n[l]++ >= 6)
    return;
  const BgSnap *b0 = &g_lines[g_runs[l][0].first].bg[l];
  int diff = -1;
  for (int i = 1; i < g_nruns[l] && diff < 0; i++) {
    const BgSnap *b = &g_lines[g_runs[l][i].first].bg[l];
    if (b->mapadr != b0->mapadr || b->tileadr != b0->tileadr || ((b->flags ^ b0->flags) & 3))
      diff = i;
  }
  PspLog("imgno f=%u BG%d runs=%d fl=%02x map=%04x tile=%04x diffrun=%d high_ok=%d", (unsigned)g_frame, l + 1,
         g_nruns[l], b0->flags, b0->mapadr, b0->tileadr, diff, (int)g_imgh_ok);
}
#define NOTE_IMGNO(l) NoteImgNo(l)
#else
#define NOTE_IMGNO(l) ((void)0)
#endif

/* Brings the image of layer l up to date: a cell is rebuilt when its tilemap word or its tile's version changed.
 * Gate 5m: if no VRAM page of the tilemap or of the tiles it uses was written since the last scan, the scan is skipped
 * (log builds scan every 16th skip anyway and disable skipping if that finds anything). */
static void ImgUpdate(int l, const BgSnap *s) {
  int slot = l - 1;
  bool bpp4 = l < 2;
  LayerImg *im = &g_img[slot];
  bool wide = (s->flags & 1) != 0, high = (s->flags & 2) != 0, fresh = false;
  PROF(uint32_t t0 = Now();)
  if (!im->valid || im->map != s->mapadr || im->tile != s->tileadr || im->wide != (uint8_t)wide ||
      im->high != (uint8_t)high) {
    memset(g_img_cw[slot], 0, sizeof(g_img_cw[slot]));
    memset(g_img_cv[slot], 0xff, sizeof(g_img_cv[slot]));
    im->map = s->mapadr;
    im->tile = s->tileadr;
    im->wide = (uint8_t)wide;
    im->high = (uint8_t)high;
    im->nhi = 0;
    im->maxt = 0;
    im->valid = 1;
    fresh = true;
  }
  int words = bpp4 ? 16 : 8, shift = bpp4 ? 4 : 3, cols = wide ? 64 : 32, rows = high ? 64 : 32;
  int stride = wide ? 512 : 256;
  const uint32_t *ver = bpp4 ? g_ver4 : g_ver2;
  uint16_t *cw = g_img_cw[slot];
  uint32_t *cv = g_img_cv[slot];
  int nhi = im->nhi, nupd = 0, mx = 0;
  bool mapd = ImgPagesDirty(slot, s->mapadr, 1024 << ((wide ? 1 : 0) + (high ? 1 : 0))), tiled = ImgTilePagesDirty(slot);
  bool skip = !fresh && g_img_skip_ok && !mapd && !tiled;
#if SM_PSP_LOG
  if (mapd && !fresh) {  /* log only: which map pages hold the blocking writes */
    int pg0 = (s->mapadr >> 8) & (kVramPages - 1), np = (1024 << ((wide ? 1 : 0) + (high ? 1 : 0))) >> 8;
    for (int i = 0; i < np; i++)
      if (g_img_dirty[slot][(pg0 + i) & (kVramPages - 1)])
        g_li_pg[(pg0 + i) & (kVramPages - 1)]++;
  }
  static unsigned skips;
  int maxt0 = im->maxt;
  bool check = skip && (++skips & 15) == 0;
  if (check)
    skip = false;
#endif
  if (!skip) {
    memset(g_img_tpg[slot], 0, sizeof(g_img_tpg[slot]));
    for (int ty = 0; ty < rows; ty++) {
      int rowbase = s->mapadr + (ty & 31) * 32 + ((ty & 32) ? 0x400 : 0);
      for (int tx = 0; tx < cols; tx++) {
        int idx = ty * cols + tx;
        unsigned t = g_vram[(rowbase + (tx & 31) + ((tx & 32) ? 0x400 : 0)) & 0x7fff];
        if ((int)(t & 0x3ff) >= mx)
          mx = (int)(t & 0x3ff) + 1;
        int wd = (int)((s->tileadr + (t & 0x3ff) * words) & 0x7fff), pt = wd >> shift;
        g_img_tpg[slot][wd >> 8] = 1;  /* a tile is 8 or 16 aligned words: never crosses a 256 word page */
        EnsureTile(bpp4, pt);  /* decodes into the atlas if needed and bumps the version when the data changed */
        uint32_t v = ver[pt];
        if (cw[idx] == t && cv[idx] == v)
          continue;
        nhi += (int)((t >> 13) & 1) - (int)((cw[idx] >> 13) & 1);
        ImgRenderCell(slot, bpp4, tx, ty, t, s->tileadr, stride);
        cw[idx] = (uint16_t)t;
        cv[idx] = v;
        nupd++;
      }
    }
    im->nhi = (uint16_t)nhi;
    im->maxt = (uint16_t)mx;
    memset(g_img_dirty[slot], 0, sizeof(g_img_dirty[slot]));  /* the image now matches the snapshot */
  }
  ImgBuildClut(slot, bpp4);  /* CGRAM changes need no VRAM write */
#if SM_PSP_LOG
  if (check && (nupd || mx != maxt0)) {
    g_img_skip_ok = false;
    PspLog("imgskip: f=%u BG%d scan found %d changes (maxt %d -> %d), skipping disabled", (unsigned)g_frame, l + 1, nupd,
           maxt0, mx);
  }
#endif
  PROF(if (!fresh && g_img_skip_ok) { g_li_blkm += mapd ? 1u : 0u; g_li_blkt += tiled ? 1u : 0u; })
  PROF(uint32_t us = Now() - t0; g_li_n++; g_li_skip += skip ? 1u : 0u; g_li_upd += (uint32_t)nupd; if ((uint32_t)nupd > g_li_updmax) g_li_updmax = (uint32_t)nupd;
       g_li_us += us; if (us > g_li_usmax) g_li_usmax = us;)
  PROF(if (fresh) {  /* a new tilemap set-up: every cell is built */
    static unsigned nfresh;
    if (nfresh++ < 60)
      PspLog("imgfresh f=%u BG%d %dx%d cells=%d built=%d us=%u fast=%d", (unsigned)g_frame, l + 1, cols, rows, cols * rows, nupd, (unsigned)us,
             (int)g_img_fast);
  })
}

/* One strip per run and window span and priority pass. Screen row r shows image row (r + 1 + vscroll) & (height - 1) (the SNES
 * BG line is 1-based); the texture coordinates run past the image edge and GU_REPEAT wraps them (verified by the 5i-b
 * benchmark). iu/iv as in the mosaic and mode 7 quads. */
static void ImgBuildStrips(int l) {
  int slot = l - 1, W = g_img[slot].wide ? 512 : 256, H = g_img[slot].high ? 512 : 256;
  const float iu = 0.5f - g_uvh[0], iv = 0.5f - g_uvh[1];
  for (int pr = 0; pr < 2; pr++) {
    g_imgv[l][pr] = NULL;
    g_imgn[l][pr] = 0;
    if (pr && !g_img[slot].nhi)
      continue;
    TileVert *v = VtxAlloc(g_nruns[l] * 4 * sizeof(TileVert)), *w = v;
    for (int i = 0; i < g_nruns[l]; i++) {
      const Run *run = &g_runs[l][i];
      const BgSnap *b = &g_lines[run->first].bg[l];
      int wa[2], we[2];
      int ns = WinSpans(b->wm, b->wl, b->wr, wa, we);
      float vt = (float)((b->vscroll + run->r0 + 1) & (H - 1)) + iv;
      float vb = vt + (float)(run->r1 - run->r0);
      float ub = (float)(b->hscroll & (W - 1)) + iu;
      for (int k = 0; k < ns; k++) {
        *w++ = (TileVert){ub + (float)wa[k], vt, (int16_t)(kLeft + wa[k]), (int16_t)(kTop + run->r0), (int16_t)g_zbg[l][pr]};
        *w++ = (TileVert){ub + (float)we[k], vb, (int16_t)(kLeft + we[k]), (int16_t)(kTop + run->r1), (int16_t)g_zbg[l][pr]};
      }
    }
    g_imgv[l][pr] = v;
    g_imgn[l][pr] = (int)(w - v);
  }
}

/* Gate 5p: strips of the layer image for the subscreen runs of layer l (same mapping as ImgBuildStrips). Window spans as in
 * the tile path: none in fixed-colour / half-colour frames (the masks are painted over the target), else the layer's
 * subscreen window. (ox, oy): origin of the target (the screen when blending directly, 0 for the multisub target). */
static void SubImgBuildStrips(int l, bool full, int ox, int oy) {
  int slot = l - 1, W = g_img[slot].wide ? 512 : 256, H = g_img[slot].high ? 512 : 256;
  const float iu = 0.5f - g_uvh[0], iv = 0.5f - g_uvh[1];
  for (int pr = 0; pr < 2; pr++) {
    g_simgv[l][pr] = NULL;
    g_simgn[l][pr] = 0;
    if (pr && !g_img[slot].nhi)
      continue;
    TileVert *v = VtxAlloc(g_nbruns * 4 * sizeof(TileVert)), *w = v;
    for (int i = 0; i < g_nbruns; i++) {
      const BRun *br = &g_bruns[i];
      if (br->l != l)
        continue;
      const BgSnap *b = &g_lines[br->first].bg[l];
      int wa[2] = {0, 0}, we[2] = {kGameWidth, kGameWidth};
      int ns = full ? 1 : WinSpans(b->sm, b->sl, b->sr, wa, we);
      float vt = (float)((b->vscroll + br->r0 + 1) & (H - 1)) + iv;
      float vb = vt + (float)(br->r1 - br->r0);
      float ub = (float)(b->hscroll & (W - 1)) + iu;
      for (int k = 0; k < ns; k++) {
        *w++ = (TileVert){ub + (float)wa[k], vt, (int16_t)(ox + wa[k]), (int16_t)(oy + br->r0), (int16_t)g_zbg[l][pr]};
        *w++ = (TileVert){ub + (float)we[k], vb, (int16_t)(ox + we[k]), (int16_t)(oy + br->r1), (int16_t)g_zbg[l][pr]};
      }
    }
    g_simgv[l][pr] = v;
    g_simgn[l][pr] = (int)(w - v);
  }
}

//BG layers

static bool SameGeom(const LineSnap *a, const LineSnap *b, int l) {
  const BgSnap *x = &a->bg[l], *y = &b->bg[l];
  return x->hscroll == y->hscroll && x->vscroll == y->vscroll && x->mapadr == y->mapadr &&
         x->tileadr == y->tileadr && (x->flags & 7) == (y->flags & 7);
}

static bool SameBg(const LineSnap *a, const LineSnap *b, int l) {
  const BgSnap *x = &a->bg[l], *y = &b->bg[l];
  return SameGeom(a, b, l) && x->wm == y->wm && x->wl == y->wl && x->wr == y->wr;
}

static bool SameSub(const LineSnap *a, const LineSnap *b, int l) {
  const BgSnap *x = &a->bg[l], *y = &b->bg[l];
  return SameGeom(a, b, l) && x->flags == y->flags && x->sm == y->sm && x->sl == y->sl && x->sr == y->sr &&
         a->mathmask == b->mathmask && a->bg3prio == b->bg3prio;
}

/* Same as SameSub without the window state (gate 4t-c: windows are painted over the subscreen target per row). */
static bool SameSubNoWin(const LineSnap *a, const LineSnap *b, int l) {
  return SameGeom(a, b, l) && a->bg[l].flags == b->bg[l].flags && a->mathmask == b->mathmask &&
         a->bg3prio == b->bg3prio;
}

/* Hidden x spans (up to 2) of a layer window state (see LayerWin): the complement of WinSpans. */
static int HiddenSpans(int wm, int wl, int wr, int a[3], int b[3]) {
  int va[2], vb[2];
  int nv = WinSpans(wm, wl, wr, va, vb), n = 0, x = 0;
  for (int k = 0; k < nv; k++) {
    if (va[k] > x) {
      a[n] = x;
      b[n++] = va[k];
    }
    x = vb[k];
  }
  if (x < kGameWidth) {
    a[n] = x;
    b[n++] = kGameWidth;
  }
  return n;
}

/* Walks the tilemap of layer l once for rows [r0,r1) and appends every non-empty
 * tile to the pool (decoding it into the atlas on the way). Consecutive equal
 * tilemap words reuse the previous lookup. Returns false if the pool is full. */
static bool BuildTileListRaw(int l, const LineSnap *snap, int r0, int r1, TileList *out) {
  const BgSnap *s = &snap->bg[l];
  bool bpp4 = l < 2;
  int words = bpp4 ? 16 : 8, shift = bpp4 ? 4 : 3;
  /* SNES BG row for 0-based screen row r is r + 1 + vscroll (line is 1-based). */
  int ty0 = (s->vscroll + r0 + 1) >> 3, ty1 = (s->vscroll + r1) >> 3;
  int tx0 = s->hscroll >> 3, tx1 = (s->hscroll + kGameWidth - 1) >> 3;
  int wide = s->flags & 1, high = (s->flags >> 1) & 1;
  int start = g_ent_top, top = start;
  unsigned last_t = 0x10000;  /* no tilemap word equals this */
  int last_pt = 0;
  bool last_empty = true;
  uint8_t last_pal = 0, last_fl = 0;
  for (int ty = ty0; ty <= ty1; ty++) {
    int rowbase = s->mapadr + (ty & 31) * 32 + (((ty & 32) && high) ? (wide ? 0x800 : 0x400) : 0);
    int y0 = ty * 8 - s->vscroll - 1;
    for (int tx = tx0; tx <= tx1; tx++) {
      int addr = (rowbase + (tx & 31) + (((tx & 32) && wide) ? 0x400 : 0)) & 0x7fff;
      unsigned t = g_vram[addr];
      if (t != last_t) {
        last_t = t;
        last_pt = (int)((s->tileadr + (t & 0x3ff) * words) & 0x7fff) >> shift;
        last_pal = (uint8_t)((t >> 10) & 7);
        last_fl = (uint8_t)(((t >> 13) & 1) | ((t >> 13) & 2) | ((t >> 13) & 4));
        last_empty = EnsureTile(bpp4, last_pt);
      }
      if (last_empty)
        continue;
      if (top >= kEntPool)
        return false;
      TileEnt *e = &g_ents[top++];
      e->x = (int16_t)(tx * 8 - s->hscroll);
      e->y = (int16_t)y0;
      e->pt = (uint16_t)last_pt;
      e->pal = last_pal;
      e->fl = last_fl;
    }
  }
  g_ent_top = top;
  out->first = (uint16_t)start;
  out->n = (uint16_t)(top - start);
  return true;
}

/* Non-empty tiles of tilemap row ty of layer l (x set, y 0) written to dst. Returns the count, or -1 if more
 * than max would be needed. Same lookup as BuildTileListRaw. */
static int BuildRow(int l, const BgSnap *s, int ty, TileEnt *dst, int max) {
  bool bpp4 = l < 2;
  int words = bpp4 ? 16 : 8, shift = bpp4 ? 4 : 3;
  int tx0 = s->hscroll >> 3, tx1 = (s->hscroll + kGameWidth - 1) >> 3;
  int wide = s->flags & 1, high = (s->flags >> 1) & 1;
  int rowbase = s->mapadr + (ty & 31) * 32 + (((ty & 32) && high) ? (wide ? 0x800 : 0x400) : 0);
  int n = 0, last_pt = 0;
  unsigned last_t = 0x10000;  /* no tilemap word equals this */
  bool last_empty = true;
  uint8_t last_pal = 0, last_fl = 0;
  for (int tx = tx0; tx <= tx1; tx++) {
    int addr = (rowbase + (tx & 31) + (((tx & 32) && wide) ? 0x400 : 0)) & 0x7fff;
    unsigned t = g_vram[addr];
    if (t != last_t) {
      last_t = t;
      last_pt = (int)((s->tileadr + (t & 0x3ff) * words) & 0x7fff) >> shift;
      last_pal = (uint8_t)((t >> 10) & 7);
      last_fl = (uint8_t)(((t >> 13) & 1) | ((t >> 13) & 2) | ((t >> 13) & 4));
      last_empty = EnsureTile(bpp4, last_pt);
    }
    if (last_empty)
      continue;
    if (n >= max)
      return -1;
    TileEnt *e = &dst[n++];
    e->x = (int16_t)(tx * 8);  /* unscrolled: the caller subtracts hscroll, so rows are shared across fine scroll */
    e->y = 0;
    e->pt = (uint16_t)last_pt;
    e->pal = last_pal;
    e->fl = last_fl;
  }
  return n;
}

/* Cached tiles of row ty (built on first use this frame), or NULL if the row pool is full. The pool only grows
 * within a frame, so returned pointers stay valid. Entries from earlier frames never match (stamp). */
static const TileEnt *RowCached(int l, const BgSnap *s, int ty, int *count) {
  unsigned t6 = (unsigned)ty & 63, wh = s->flags & 3;
  unsigned hk = ((unsigned)(s->hscroll >> 3) << 1) | ((s->hscroll & 7) != 0);  /* tile column + (fine != 0: 32 vs 31 columns right) */
  unsigned h = (t6 * 5 + (unsigned)(s->hscroll >> 3) * 3 + (unsigned)l * 11 + (unsigned)(s->mapadr >> 10) * 7 +
                (unsigned)(s->tileadr >> 12) * 13 + wh + (hk & 1) * 17) & (kRowSlots - 1);
  RowCache *rc = &g_rc[h];
  if (rc->stamp == g_stamp && rc->l == l && rc->ty == t6 && rc->wh == wh && rc->map == s->mapadr &&
      rc->tile == s->tileadr && rc->hs == hk) {
    PROF(g_rc_hit++;)
    *count = rc->n;
    return &g_rows[rc->first];
  }
  int n = BuildRow(l, s, ty, &g_rows[g_rows_top], kRowPool - g_rows_top);
  if (n < 0)
    return NULL;
  PROF(g_rc_miss++;)
  *rc = (RowCache){s->mapadr, s->tileadr, (uint16_t)hk, g_stamp, (uint16_t)g_rows_top, (uint16_t)n,
                   (uint8_t)l, (uint8_t)wh, (uint8_t)t6};
  g_rows_top += n;
  *count = n;
  return &g_rows[rc->first];
}

/* Same result as BuildTileListRaw. Short runs (a few rows, e.g. HDMA waves) copy cached rows and only set y. */
static bool BuildTileList(int l, const LineSnap *snap, int r0, int r1, TileList *out) {
  if (r1 - r0 > kRowMaxRun)
    return BuildTileListRaw(l, snap, r0, r1, out);
  const BgSnap *s = &snap->bg[l];
  int ty0 = (s->vscroll + r0 + 1) >> 3, ty1 = (s->vscroll + r1) >> 3;
  int start = g_ent_top, top = start;
  for (int ty = ty0; ty <= ty1; ty++) {
    int y0 = ty * 8 - s->vscroll - 1, n = 0;
    const TileEnt *src = RowCached(l, s, ty, &n);
    TileEnt *dst = &g_ents[top];
    if (src) {
      if (top + n > kEntPool)
        return false;
      memcpy(dst, src, (size_t)n * sizeof(TileEnt));
    } else {
      n = BuildRow(l, s, ty, dst, kEntPool - top);
      if (n < 0)
        return false;
    }
    for (int k = 0; k < n; k++) {
      dst[k].y = (int16_t)y0;
      dst[k].x = (int16_t)(dst[k].x - s->hscroll);  /* cached and freshly built rows are both unscrolled */
    }
    top += n;
  }
  g_ent_top = top;
  out->first = (uint16_t)start;
  out->n = (uint16_t)(top - start);
  return true;
}

static void CountList(const TileList *lst, int counts[2][8]) {
  for (int i = 0; i < lst->n; i++) {
    const TileEnt *e = &g_ents[lst->first + i];
    counts[e->fl & 1][e->pal]++;
  }
}

/* Emits CPU-clipped quads for rows [r0,r1) into out[prio][pal] (no negative or
 * off-screen vertex ever reaches the GE) and for x in [vx0,vx1) (a window span). */
static void EmitList(int l, const TileList *lst, int r0, int r1, BgVert *out[2][8], int ox, int oy, int vx0,
                     int vx1) {
  int cols = l < 2 ? 32 : 64;
  for (int i = 0; i < lst->n; i++) {
    const TileEnt *e = &g_ents[lst->first + i];
    int x0 = e->x, y0 = e->y, prio = e->fl & 1, pal = e->pal, pt = e->pt;
    int cl = x0 < vx0 ? vx0 - x0 : 0, cr = x0 + 8 > vx1 ? x0 + 8 - vx1 : 0;
    int ct = y0 < r0 ? r0 - y0 : 0, cb = y0 + 8 > r1 ? y0 + 8 - r1 : 0;
    if (cl + cr >= 8 || ct + cb >= 8)
      continue;
    int u = (pt & (cols - 1)) * 8, v = (pt >> (cols == 32 ? 5 : 6)) * 8;
    int ul, ur, vt, vb;
    if (e->fl & 2) { ul = u + 8 - cl; ur = u + cr; } else { ul = u + cl; ur = u + 8 - cr; }
    if (e->fl & 4) { vt = v + 8 - ct; vb = v + cb; } else { vt = v + ct; vb = v + 8 - cb; }
    BgQuad(out[prio][pal], ul, vt, ur, vb, ox + x0 + cl, oy + y0 + ct, ox + x0 + 8 - cr, oy + y0 + 8 - cb,
           g_zbg[l][prio]);
    out[prio][pal] += 2;
  }
}

/* Gate 4t-d: main-screen window rows. On a half-colour visor frame runs are grouped by geometry only; the window
 * state of every row (HDMA edges) is applied while quads are emitted. g_rw holds the visible spans of each row. */
enum { kTkHidden = 0, kTkFull = 1, kTkSplit = 2 };

static void BuildRowWin(int l, int r0, int r1) {
  for (int r = r0; r < r1; r++) {
    const BgSnap *s = &g_lines[r].bg[l];
    int a[2] = {0, 0}, b[2] = {0, 0};
    int n = WinSpans(s->wm, s->wl, s->wr, a, b);
    RowWin *w = &g_rw[l][r];
    w->n = (uint8_t)n;
    for (int k = 0; k < 2; k++) {
      w->a[k] = (int16_t)a[k];
      w->b[k] = (int16_t)b[k];
    }
  }
}

/* One quad of tile e: x in [xl,xr), rows [ra,rb), clipped on the CPU (same maths as EmitList). */
static void PutTileQuad(BgVert *out[2][8], const TileEnt *e, int l, int ox, int oy, int xl, int xr, int ra, int rb) {
  int cols = l < 2 ? 32 : 64;
  int x0 = e->x, y0 = e->y;
  int cl = xl - x0, cr = x0 + 8 - xr, ct = ra - y0, cb = y0 + 8 - rb;
  int prio = e->fl & 1, pal = e->pal, pt = e->pt;
  int u = (pt & (cols - 1)) * 8, v = (pt >> (cols == 32 ? 5 : 6)) * 8;
  int ul, ur, vt, vb;
  if (e->fl & 2) { ul = u + 8 - cl; ur = u + cr; } else { ul = u + cl; ur = u + 8 - cr; }
  if (e->fl & 4) { vt = v + 8 - ct; vb = v + cb; } else { vt = v + ct; vb = v + 8 - cb; }
  BgQuad(out[prio][pal], ul, vt, ur, vb, ox + xl, oy + ra, ox + xr, oy + rb, g_zbg[l][prio]);
  out[prio][pal] += 2;
}

/* Quads of tile e for rows [r0,r1) under the per-row window table; consecutive rows with the same visible interval
 * merge. out == NULL only counts (identical control flow, so counts and emission always agree). */
static int TileRowsEmit(const TileEnt *e, int l, int r0, int r1, BgVert *out[2][8], int ox, int oy) {
  int x0 = e->x;
  int ra = e->y > r0 ? e->y : r0, rb = e->y + 8 < r1 ? e->y + 8 : r1;
  int nq = 0;
  int s0[2] = {0, 0}, s1[2] = {0, 0}, srow[2] = {-1, -1};
  for (int r = ra; r <= rb; r++) {
    int xl[2] = {0, 0}, xr[2] = {0, 0}, ni = 0;
    if (r < rb) {
      const RowWin *w = &g_rw[l][r];
      for (int k = 0; k < w->n; k++) {
        int a = w->a[k] > x0 ? w->a[k] : x0;
        int b = w->b[k] < x0 + 8 ? w->b[k] : x0 + 8;
        if (a < b) {
          xl[ni] = a;
          xr[ni] = b;
          ni++;
        }
      }
    }
    for (int j = 0; j < 2; j++) {
      bool have = j < ni;
      if (srow[j] >= 0 && !(have && xl[j] == s0[j] && xr[j] == s1[j])) {
        if (out)
          PutTileQuad(out, e, l, ox, oy, s0[j], s1[j], srow[j], r);
        nq++;
        srow[j] = -1;
      }
      if (have && srow[j] < 0) {
        s0[j] = xl[j];
        s1[j] = xr[j];
        srow[j] = r;
      }
    }
  }
  return nq;
}

static uint8_t TileWinKind(const TileEnt *e, int l, int r0, int r1) {
  int x0 = e->x;
  int ra = e->y > r0 ? e->y : r0, rb = e->y + 8 < r1 ? e->y + 8 : r1;
  bool full = true, any = false;
  for (int r = ra; r < rb; r++) {
    const RowWin *w = &g_rw[l][r];
    bool f = false;
    for (int k = 0; k < w->n; k++) {
      if (w->a[k] <= x0 && w->b[k] >= x0 + 8)
        f = true;
      if (w->a[k] < x0 + 8 && w->b[k] > x0)
        any = true;
    }
    if (!f)
      full = false;
  }
  if (ra >= rb || !any)
    return kTkHidden;
  return full ? kTkFull : kTkSplit;
}

static void CountListWin(int l, const TileList *lst, int r0, int r1, int counts[2][8]) {
  for (int i = 0; i < lst->n; i++) {
    const TileEnt *e = &g_ents[lst->first + i];
    uint8_t k = TileWinKind(e, l, r0, r1);
    g_tkind[lst->first + i] = k;
    if (k == kTkFull)
      counts[e->fl & 1][e->pal]++;
    else if (k == kTkSplit)
      counts[e->fl & 1][e->pal] += TileRowsEmit(e, l, r0, r1, NULL, 0, 0);
  }
}

static void EmitListWin(int l, const TileList *lst, int r0, int r1, BgVert *out[2][8], int ox, int oy) {
  for (int i = 0; i < lst->n; i++) {
    uint8_t k = g_tkind[lst->first + i];
    if (k == kTkFull) {
      TileList one = {(uint16_t)(lst->first + i), 1};
      EmitList(l, &one, r0, r1, out, ox, oy, 0, kGameWidth);
    } else if (k == kTkSplit) {
      TileRowsEmit(&g_ents[lst->first + i], l, r0, r1, out, ox, oy);
    }
  }
}

/* Composite quads of the half-colour subscreen target: rows [rlo,rhi) grouped by colour-window state, one quad per
 * span where colour math applies (gate 4t-b state). Returns the vertex count; out == NULL only counts. */
static int SubAddRows(int rlo, int rhi, TileVert *out) {
  int nv = 0;
  for (int r = rlo; r < rhi;) {
    const LineSnap *ls = &g_lines[r];
    int e = r + 1;
    while (e < rhi && g_lines[e].cwm == ls->cwm && g_lines[e].cwl == ls->cwl && g_lines[e].cwr == ls->cwr)
      e++;
    int wa[2], we[2];
    int ns = WinSpans(ls->cwm, ls->cwl, ls->cwr, wa, we);
    for (int k = 0; k < ns; k++, nv += 2) {
      if (out) {
        out[nv] = (TileVert){(float)wa[k], (float)r, (int16_t)(kLeft + wa[k]), (int16_t)(kTop + r), 0};
        out[nv + 1] = (TileVert){(float)we[k], (float)e, (int16_t)(kLeft + we[k]), (int16_t)(kTop + e), 0};
      }
    }
    r = e;
  }
  return nv;
}

#if !SM_PSP_MOS_GE
/* Gate 4q: samples layer l at the mosaic block origins into g_mosd[l][tile priority], as ppu.c does: column
+ * x - x % size, line y - (y - start) % size (1-based lines), then the scroll of the layer. */
static void MosBuild(int l, const BgSnap *s, int size, int start) {
  MosLayer *m = &g_mos[l];
  bool bpp4 = l < 2;
  int words = bpp4 ? 16 : 8;
  int ncols = (kGameWidth + size - 1) / size, nrows = (kGameHeight - start) / size + 1;
  int wide = s->flags & 1, high = (s->flags >> 1) & 1;
  uint32_t *img0 = g_mosd[l][0], *img1 = g_mosd[l][1];
  if (bpp4)
    BuildClut4(kClutBright);
  else
    BuildClut2(kClutBright);
  m->any[0] = m->any[1] = false;
  for (int i = 0; i < nrows; i++) {
    int by = start + i * size + s->vscroll, ty = by >> 3, row = by & 7;
    int rowbase = s->mapadr + (ty & 31) * 32 + (((ty & 32) && high) ? (wide ? 0x800 : 0x400) : 0);
    uint32_t *d0 = img0 + i * kMosDim, *d1 = img1 + i * kMosDim;
    for (int j = 0; j < ncols; j++) {
      int bx = j * size + s->hscroll;
      unsigned t = g_vram[(rowbase + ((bx >> 3) & 31) + (((bx & 0x100) && wide) ? 0x400 : 0)) & 0x7fff];
      int col = (t & 0x4000) ? (bx & 7) : 7 - (bx & 7);
      int base = (s->tileadr + (int)(t & 0x3ff) * words + ((t & 0x8000) ? 7 - row : row)) & 0x7fff;
      unsigned w0 = g_vram[base];
      int px = (int)((w0 >> col) & 1) | (int)(((w0 >> (8 + col)) & 1) << 1);
      if (bpp4) {
        unsigned w1 = g_vram[(base + 8) & 0x7fff];
        px |= (int)(((w1 >> col) & 1) << 2) | (int)(((w1 >> (8 + col)) & 1) << 3);
      }
      uint32_t c = 0;
      int hp = (t & 0x2000) != 0;
      if (px) {
        int pal = (t >> 10) & 7;
        c = bpp4 ? g_clut4[kClutBright][pal][px] : g_clut2[kClutBright][pal][px];
        m->any[hp] = true;
      }
      d0[j] = hp ? 0 : c;
      d1[j] = hp ? c : 0;
    }
  }
  sceKernelDcacheWritebackRange(img0, (unsigned)nrows * kMosDim * 4);
  sceKernelDcacheWritebackRange(img1, (unsigned)nrows * kMosDim * 4);
}
#endif

#if SM_PSP_MOS_GE
/* Gate 5s: GE-native mosaic. ppu.c shows, for screen column x and line y, the layer pixel at BG column
 * hscroll + x - x % size, BG line start + k * size + vscroll (k = (y - start) / size). Those samples form a small
 * image S[i][j] (ncols x nrows, 8888, 128 texels wide). The GE renders S: every non-empty tile is one sprite covering
 * the S cells whose sample point lies in the tile, with a texture step of `size` texels per S cell, so nearest
 * sampling at the cell centre lands exactly on the sampled texel. The stencil value written with each pixel (the
 * alpha of the 8888 target) tags the tile priority; DrawMosPass stretches S with an alpha test on that tag.
 * S lives in the subscreen target's VRAM (kVramSub): frames that use that target are not drawn this way. */
typedef struct MosSrc {
  int ncols, nrows, nq;
  TileList lst;
  int cnt[2][8];  /* quads per tile priority and palette */
  TileVert *sv[2][8];
  ColorVert *clr;
} MosSrc;
static MosSrc g_msrc[kLayers];

/* S cells [ja,jb) x [ia,ib) whose sample point lies inside tile e (e->x: screen x of the tile, e->y: screen row
 * of its top, so line e->y + 1 is its first row). False if there is none. */
static bool MosSpan(const MosLayer *m, const MosSrc *s, const TileEnt *e, int *ja, int *jb, int *ia, int *ib) {
  const int size = m->size, x0 = e->x, y0 = e->y + 1 - m->start;
  int a = x0 > 0 ? (x0 + size - 1) / size : 0;
  int b = x0 + 8 > 0 ? (x0 + 8 + size - 1) / size : 0;
  int c = y0 > 0 ? (y0 + size - 1) / size : 0;
  int d = y0 + 8 > 0 ? (y0 + 8 + size - 1) / size : 0;
  if (b > s->ncols)
    b = s->ncols;
  if (d > s->nrows)
    d = s->nrows;
  if (a >= b || c >= d)
    return false;
  *ja = a;
  *jb = b;
  *ia = c;
  *ib = d;
  return true;
}

/* One sprite (two vertices) of tile e over S cells [ja,jb) x [ia,ib). u/v are placed so that the sample of cell
 * (j, i) is the centre of texel (j * size - e->x, start + i * size - e->y - 1) of the tile (mirrored when flipped);
 * g_uvh says where in a pixel the GE samples. */
static void MosQuad(TileVert *w, int l, const TileEnt *e, int size, int start, int ja, int jb, int ia, int ib) {
  const int cols = l < 2 ? 32 : 64;
  const int u0 = (e->pt & (cols - 1)) * 8, v0 = (e->pt >> (cols == 32 ? 5 : 6)) * 8;
  const bool hf = (e->fl & 2) != 0, vf = (e->fl & 4) != 0;
  const int tc = ja * size - e->x, tr = start + ia * size - (e->y + 1);
  const float su = hf ? (float)-size : (float)size, sv = vf ? (float)-size : (float)size;
  const float uc = (float)(u0 + (hf ? 7 - tc : tc)) + 0.5f, vc = (float)(v0 + (vf ? 7 - tr : tr)) + 0.5f;
  const float ul = uc - g_uvh[0] * su, vt = vc - g_uvh[1] * sv;
  w[0] = (TileVert){ul, vt, (int16_t)ja, (int16_t)ia, 0};
  w[1] = (TileVert){ul + (float)(jb - ja) * su, vt + (float)(ib - ia) * sv, (int16_t)jb, (int16_t)ib, 0};
}

/* Pass A (before the list is opened): tile list of the lines the mosaic samples, decoded into the atlas, and the
 * quad counts per priority and palette. False if the tile pool is full. */
static bool MosPrepGe(int l, const LineSnap *first) {
  MosLayer *m = &g_mos[l];
  MosSrc *s = &g_msrc[l];
  s->ncols = (kGameWidth + m->size - 1) / m->size;
  s->nrows = (kGameHeight - m->start) / m->size + 1;
  s->nq = 0;
  memset(s->cnt, 0, sizeof(s->cnt));
  m->any[0] = m->any[1] = false;
  if (l < 2)
    BuildClut4(kClutBright);
  else
    BuildClut2(kClutBright);
  if (!BuildTileList(l, first, -1, m->start + (s->nrows - 1) * m->size, &s->lst))
    return false;
  for (int k = 0; k < s->lst.n; k++) {
    const TileEnt *e = &g_ents[s->lst.first + k];
    int ja, jb, ia, ib;
    if (!MosSpan(m, s, e, &ja, &jb, &ia, &ib))
      continue;
    s->cnt[e->fl & 1][e->pal]++;
    s->nq++;
    m->any[e->fl & 1] = true;
  }
  return true;
}

/* Pass B (list open): vertices of the source sprites and the clear rectangle of S. */
static void MosBuildSrc(int l) {
  const MosLayer *m = &g_mos[l];
  MosSrc *s = &g_msrc[l];
  TileVert *w[2][8];
  for (int pr = 0; pr < 2; pr++)
    for (int p = 0; p < 8; p++) {
      s->sv[pr][p] = s->cnt[pr][p] ? sceGuGetMemory(s->cnt[pr][p] * 2 * sizeof(TileVert)) : NULL;
      w[pr][p] = s->sv[pr][p];
    }
  for (int k = 0; k < s->lst.n; k++) {
    const TileEnt *e = &g_ents[s->lst.first + k];
    int ja, jb, ia, ib;
    if (!MosSpan(m, s, e, &ja, &jb, &ia, &ib))
      continue;
    TileVert **dst = &w[e->fl & 1][e->pal];
    MosQuad(*dst, l, e, m->size, m->start, ja, jb, ia, ib);
    *dst += 2;
  }
  s->clr = sceGuGetMemory(2 * sizeof(ColorVert));
  s->clr[0] = (ColorVert){0, 0, 0, 0};
  s->clr[1] = (ColorVert){0, (int16_t)s->ncols, (int16_t)s->nrows, 0};
}
#endif

/* 0: layer l has no mosaic this frame, 1: image built (g_mos[l].on), -1: a setup the GE path cannot draw
 * (geometry or mosaic changing inside the frame, mosaic on part of the rows, or the audit failed). */
static int MosPrepare(int l) {
  MosLayer *m = &g_mos[l];
  const LineSnap *first = NULL;
  int ndrawn = 0, nmos = 0;
  m->on = false;
  for (int r = 0; r < g_line_count; r++) {
    const LineSnap *ls = &g_lines[r];
    if (!(ls->bg[l].flags & 4))
      continue;
    ndrawn++;
    if (!(ls->mosmask & (1 << l)))
      continue;
    nmos++;
    if (!first)
      first = ls;
    else if (ls->mossize != first->mossize || ls->mosstart != first->mosstart || !SameGeom(first, ls, l))
      return -1;
  }
  if (nmos == 0)
    return 0;
  if (nmos != ndrawn || !g_mos_ok || first->mossize < 2 || first->mossize > 16 || first->mosstart > 1)
    return -1;
  m->size = first->mossize;
  m->start = first->mosstart;
#if SM_PSP_MOS_GE
  if (!MosPrepGe(l, first))
    return -1;
#else
  MosBuild(l, &first->bg[l], m->size, m->start);
#endif
  m->on = true;
  g_mos_frame = true;
  g_mos_size = m->size;
  return 1;
}

//GE Frame

static void CountReasons(int mask);

#if SM_PSP_LOG
/* Why a frame went back to software with kReasonSubMulti (first few occurrences only). */
static void NoteSubMulti(const char *what, int row, int x, int y) {
  static unsigned n;
  if (n++ < 12)
    PspLog("submulti f=%u %s row=%d %d %d (blend runs=%d tint runs=%d)", (unsigned)g_frame, what, row, x, y, g_nbruns,
           g_ntint);
}
#define NOTE_SUBMULTI(what, row, x, y) NoteSubMulti(what, row, x, y)
#else
#define NOTE_SUBMULTI(what, row, x, y) ((void)0)
#endif

#if SM_PSP_LOG
static void NoteBudget(int site, const char *what, int a, int b) {
  static unsigned n[10];  /* one counter per BUDGET site (0-8) */
  if (n[site]++ < 3)
    PspLog("budget f=%u %s %d/%d", (unsigned)g_frame, what, a, b);
}
#define BUDGET(site, what, a, b) do { NoteBudget(site, what, a, b); CountReasons(kReasonBudget); } while (0)
#else
#define BUDGET(site, what, a, b) CountReasons(kReasonBudget)
#endif

//sprites

static const uint8_t kSpriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static void EmitQuad8(BgVert **w, int ox, int oy, int x0, int y0, int r0, int r1, int pt, bool hf, bool vf, int z,
                      int vx0, int vx1) {
  int cl = x0 < vx0 ? vx0 - x0 : 0, cr = x0 + 8 > vx1 ? x0 + 8 - vx1 : 0;
  int ct = y0 < r0 ? r0 - y0 : 0, cb = y0 + 8 > r1 ? y0 + 8 - r1 : 0;
  if (cl + cr >= 8 || ct + cb >= 8)
    return;
  if (g_obj_dry) {
    g_obj_dry_n++;
    return;
  }
  int u = (pt & 31) * 8, v = (pt >> 5) * 8;
  int ul, ur, vt, vb;
  if (hf) { ul = u + 8 - cl; ur = u + cr; } else { ul = u + cl; ur = u + 8 - cr; }
  if (vf) { vt = v + 8 - ct; vb = v + cb; } else { vt = v + ct; vb = v + 8 - cb; }
  BgQuad(*w, ul, vt, ur, vb, ox + x0 + cl, oy + y0 + ct, ox + x0 + 8 - cr, oy + y0 + 8 - cb, z);
  *w += 2;
}

/* Visible 8-pixel columns of a sprite: the ones ppu.c counts as slivers (col + x in (-8, 256)). *first = first one. */
static int ObjVisCols(const Sprite *sp, int *first) {
  int n = sp->size >> 3, f = -1, c = 0;
  for (int tc = 0; tc < n; tc++) {
    int cx = tc * 8 + sp->x;
    if (cx > -8 && cx < 256) {
      if (f < 0)
        f = tc;
      c++;
    }
  }
  *first = f < 0 ? 0 : f;
  return c;
}

/* Sprite k (index in g_spr) still has its column vi (visible-column index) on row r. */
static inline bool ObjRowKeeps(int k, int r, int vi) {
  int lk = g_row_lastk[r];
  return lk > k || (lk == k && vi < (int)g_row_lastc[r]);
}

/* EmitQuad8 for the rows of the tile on which sprite k keeps this column (hardware sprite/sliver limit). */
static void EmitQuad8Rows(BgVert **w, int ox, int oy, int x0, int y0, int r0, int r1, int pt, bool hf, bool vf,
                          int z, int vx0, int vx1, int k, int vi) {
  int b = y0 + 8 < r1 ? y0 + 8 : r1;
  int r = y0 > r0 ? y0 : r0;
  while (r < b) {
    if (!ObjRowKeeps(k, r, vi)) {
      r++;
      continue;
    }
    int e = r + 1;
    while (e < b && ObjRowKeeps(k, e, vi))
      e++;
    EmitQuad8(w, ox, oy, x0, y0, r, e, pt, hf, vf, z, vx0, vx1);
    r = e;
  }
}

/* w == NULL: decode every tile the sprite uses (atlas fill, before the list is
 * open). Otherwise emit CPU-clipped quads for rows [o0,o1). Tile selection
 * mirrors ppu.c: 16x16-page wrap for the low nibble, row offset added to the
 * high nibble, hflip mirrors columns, vflip mirrors rows. */
static void WalkSprite(const Sprite *sp, int o0, int o1, BgVert **w, int z, int ox, int oy, int vx0, int vx1) {
  int n = sp->size >> 3;
  bool hf = (sp->attr & 0x4000) != 0, vf = (sp->attr & 0x8000) != 0;
  int adr = (sp->attr & 0x100) ? g_obj_adr2 : g_obj_adr1;
  int ycopies = (sp->y + sp->size > 256) ? 2 : 1;  /* rows wrap modulo 256 (uint8 row in ppu.c) */
  int k = (int)(sp - g_spr), tc0 = 0;
  if (g_ovf_rows && w)
    ObjVisCols(sp, &tc0);
  for (int tr = 0; tr < n; tr++) {
    for (int tc = 0; tc < n; tc++) {
      int sr = vf ? n - 1 - tr : tr, sc = hf ? n - 1 - tc : tc;
      int used = ((((sp->attr & 0xff) >> 4) + sr) << 4) | (((sp->attr & 0xf) + sc) & 0xf);
      int pt = ((adr + used * 16) & 0x7fff) >> 4;
      if (EnsureTile(true, pt))
        continue;
      if (w == NULL)
        continue;
      int x0 = sp->x + tc * 8;
      for (int c = 0; c < ycopies; c++) {
        int y0 = (c ? sp->y - 256 : sp->y) + tr * 8;
        if (g_ovf_rows)
          EmitQuad8Rows(w, ox, oy, x0, y0, o0, o1, pt, hf, vf, z, vx0, vx1, k, tc - tc0);
        else
          EmitQuad8(w, ox, oy, x0, y0, o0, o1, pt, hf, vf, z, vx0, vx1);
      }
    }
  }
}

/* Rows over 32 sprites / 34 slivers (ppu.c): sprites after the 32nd in-range one are dropped, and so is every
 * sliver after the 34th (counted per sprite in screen column order). Records, per such row, the last kept
 * sprite and how many of its slivers are kept. */
static void ObjLimitRows(int r0, int r1, const uint8_t *cs, const uint16_t *ct) {
  for (int r = 0; r < 256; r++) {
    g_row_lastk[r] = 32767;
    g_row_lastc[r] = 255;
  }
  for (int r = r0; r < r1; r++) {
    if (cs[r] <= 32 && ct[r] <= 34)
      continue;
    int nsp = 0, nt = 0, lk = -1, lc = 0;
    for (int k = 0; k < g_nspr; k++) {
      const Sprite *sp = &g_spr[k];
      if (((unsigned)(r - sp->y) & 255u) >= sp->size)
        continue;
      if (++nsp > 32)
        break;
      int first, vis = ObjVisCols(sp, &first);
      lk = k;
      if (nt + vis > 34) {
        lc = 34 - nt;
        break;
      }
      nt += vis;
      lc = vis;
    }
    g_row_lastk[r] = (int16_t)lk;
    g_row_lastc[r] = (uint8_t)lc;
  }
}

/* Builds the sprite list in precedence order and checks the hardware limits.
 * Returns false (reason counted) if the frame must fall back. */
static bool PrepareObj(int o0, int o1) {
  static uint8_t cs[256];
  static uint16_t ct[256];
  memset(cs, 0, sizeof(cs));
  memset(ct, 0, sizeof(ct));
  g_spr_quads = 0;
  g_nspr = 0;
  for (int k = 0; k < 128; k++) {
    int i = (g_obj_start + k) & 127;
    uint16_t w0 = g_oam[2 * i], w1 = g_oam[2 * i + 1];
    int hb = g_hoam[i >> 2] >> ((i & 3) * 2);
    int size = kSpriteSizes[g_obj_size][(hb >> 1) & 1];
    int x = (w0 & 0xff) | ((hb & 1) << 8);
    if (x > 255)
      x -= 512;
    int y = w0 >> 8;
    if (x <= -size)
      continue;
    int tiles = 0;
    for (int col = 0; col < size; col += 8)
      if (col + x > -8 && col + x < 256)
        tiles++;
    bool any_row = false;
    for (int r = 0; r < size; r++) {
      int row = (y + r) & 255;
      if (row >= o0 && row < o1) {
        cs[row]++;
        ct[row] = (uint16_t)(ct[row] + tiles);
        any_row = true;
      }
    }
    if (!any_row)
      continue;
    g_spr[g_nspr++] = (Sprite){(int16_t)x, (uint8_t)y, (uint8_t)size, w1};
    int q = (size >> 3) * (size >> 3) * ((y + size > 256) ? 2 : 1);
    g_spr_quads += q;
  }
  int nover = 0;
  for (int r = o0; r < o1; r++)
    if (cs[r] > 32 || ct[r] > 34)
      nover++;
  if (nover) {
    if (!g_ovf_ok) {
      CountReasons(kReasonObjLimit);
      return false;
    }
    ObjLimitRows(o0, o1, cs, ct);
    g_ovf_rows = nover;
  }
  if (g_spr_quads > kMaxSpriteQuads) {
    BUDGET(0, "sprquads", g_spr_quads, kMaxSpriteQuads);
    return false;
  }
  for (int i = 0; i < g_nspr; i++)
    WalkSprite(&g_spr[i], o0, o1, NULL, 0, 0, 0, 0, kGameWidth);
  return true;
}

static ObjSeg g_sobjseg[128];
static int g_nsobjseg;
static BgVert *g_sobjbase;

/* Number of quads BuildObjGeom will emit for these bands (limited rows split tiles, so g_spr_quads is no bound). */
static int ObjCountQuads(const ObjBand *bands, int nb) {
  int wa[2], we[2];
  BgVert *w = NULL;  /* never written: EmitQuad8 only counts */
  g_obj_dry = true;
  g_obj_dry_n = 0;
  for (int b = 0; b < nb; b++) {
    int ns = WinSpans(bands[b].wm, bands[b].wl, bands[b].wr, wa, we);
    for (int s = 0; s < ns; s++)
      for (int k = g_nspr - 1; k >= 0; k--)
        WalkSprite(&g_spr[k], bands[b].r0, bands[b].r1, &w, 0, 0, 0, wa[s], we[s]);
  }
  g_obj_dry = false;
  return g_obj_dry_n;
}

/* Emits the sprite quads of every window band at offset (ox,oy); returns the number of palette segments.
 * Per band the visible spans are clipped on the CPU. Bands and spans never overlap, so their order does not
 * matter; inside one the sprite with the lowest OAM index is still drawn last. */
static int BuildObjGeom(const ObjBand *bands, int nb, int ox, int oy, ObjSeg *segs, BgVert **base_out) {
  int nseg = 0, npairs = 0;
  int sa[kMaxObjBands][2], sb[kMaxObjBands][2], ns[kMaxObjBands];
  *base_out = NULL;
  if (g_spr_quads <= 0 || nb <= 0)
    return 0;
  for (int b = 0; b < nb; b++) {
    ns[b] = WinSpans(bands[b].wm, bands[b].wl, bands[b].wr, sa[b], sb[b]);
    npairs += ns[b];
  }
  if (npairs == 0)
    return 0;
  int cap = g_spr_quads * npairs;
  if (g_ovf_rows)
    cap = ObjCountQuads(bands, nb);  /* limited rows split tiles: count what is really emitted */
  if (cap <= 0)
    return 0;
  BgVert *base = VtxAlloc(cap * 2 * sizeof(BgVert));
  BgVert *w = base;
  *base_out = base;
  for (int b = 0; b < nb; b++) {
    for (int s = 0; s < ns[b]; s++) {
      for (int k = g_nspr - 1; k >= 0; k--) {
        const Sprite *sp = &g_spr[k];
        int pal = (sp->attr >> 9) & 7, prio = (sp->attr >> 12) & 3;
        BgVert *start = w;
        WalkSprite(sp, bands[b].r0, bands[b].r1, &w, (prio * 4 + 2) * kZLevel + 127 - k, ox, oy, sa[b][s],
                   sb[b][s]);
        if (w == start)
          continue;
        if (nseg == 0 || segs[nseg - 1].pal != pal)
          segs[nseg++] = (ObjSeg){(uint8_t)pal, (uint16_t)(start - base), 0};
        segs[nseg - 1].n = (uint16_t)(segs[nseg - 1].n + (w - start));
      }
    }
  }
  return nseg;
}

/* True if a sprite that is visible horizontally covers a row with subscreen OBJ + colour math. */
static bool SubObjHit(void) {
  static uint8_t row[256];
  bool any = false;
  memset(row, 0, sizeof(row));
  for (int r = 0; r < g_line_count; r++)
    if (g_lines[r].subobj) {
      row[r] = 1;
      any = true;
    }
  if (!any)
    return false;
  for (int i = 0; i < 128; i++) {
    int hb = g_hoam[i >> 2] >> ((i & 3) * 2);
    int size = kSpriteSizes[g_obj_size][(hb >> 1) & 1];
    int x = (g_oam[2 * i] & 0xff) | ((hb & 1) << 8);
    if (x > 255)
      x -= 512;
    if (x <= -size)
      continue;
    int y = g_oam[2 * i] >> 8;
    for (int r = 0; r < size; r++)  /* rows wrap modulo 256 like ppu.c */
      if (row[(y + r) & 255])
        return true;
  }
  return false;
}

/* One mode 7 row of the image path, computed as PpuDrawBackground_mode7 in ppu.c does (y = 1-based line):
 * an 8-bit colour index per pixel, 0 = transparent. */
/* Gate 5v: four pixels per step in the non-mosaic loop. Hardware probe (gate 5t-d, rows rendered twice back to back): the warm
 * render is within 2-5% of the cold one at 333 and 222 MHz, so M7RenderRow costs instructions and load latency (about 36 cycles
 * a pixel), not cache misses. With -fno-strict-aliasing the byte store to dst may alias g_vram, so the compiler cannot start the
 * next pixel's two dependent halfword loads before it. Here the four tile loads, then the four pixel loads, are issued before
 * the four stores. Same arithmetic, same results (tools/test_m7_row4.sh compares both loops on random rows); four positions
 * outside the field or inside it are told apart by one test, (a | b | c | d) > outside, exact because outside is a bit mask
 * (0x3ffff) or never true (0xffffffff). PSP_M7_ROW4=0 builds the one pixel loop. */
#ifndef SM_PSP_M7_ROW4
#define SM_PSP_M7_ROW4 1
#endif
#if SM_PSP_M7_ROW4
static inline unsigned M7Px(const uint16_t *vr, uint32_t xp, uint32_t yp, uint32_t outside, bool fill) {
  unsigned tile = 0;
  if ((xp | yp) > outside) {
    if (!fill)
      return 0;
  } else {
    tile = vr[(yp >> 11 & 0x7f) * 128 + (xp >> 11 & 0x7f)] & 0xff;
  }
  return vr[tile * 64 + (yp >> 8 & 7) * 8 + (xp >> 8 & 7)] >> 8;
}
#endif
static void M7RenderRow(const LineSnap *ls, int y, uint8_t *dst) {
  const int16_t *m = ls->m7m;
  unsigned fl = ls->m7fl;
  int xCenter = ((int16_t)(m[4] << 3)) >> 3, yCenter = ((int16_t)(m[5] << 3)) >> 3;
  int hScroll = ((int16_t)(m[6] << 3)) >> 3, vScroll = ((int16_t)(m[7] << 3)) >> 3;
  int clippedH = hScroll - xCenter, clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  int size = ls->mossize;
  bool mos = (fl & 32) && size > 1;
  if (mos)
    y -= (y - (int)ls->mosstart) % size;
  uint32_t ry = (fl & 8) ? (uint32_t)(255 - y) : (uint32_t)y;
  uint32_t a = (uint32_t)(int32_t)m[0], c = (uint32_t)(int32_t)m[2];
  uint32_t X0 = (uint32_t)((m[0] * clippedH) & ~63) + (uint32_t)((m[1] * (int)ry) & ~63) +
                (uint32_t)((m[1] * clippedV) & ~63) + ((uint32_t)xCenter << 8);
  uint32_t Y0 = (uint32_t)((m[2] * clippedH) & ~63) + (uint32_t)((m[3] * (int)ry) & ~63) +
                (uint32_t)((m[3] * clippedV) & ~63) + ((uint32_t)yCenter << 8);
  uint32_t outside = (fl & 1) ? 0x3ffffu : 0xffffffffu;
  bool fill = (fl & 2) != 0, xf = (fl & 4) != 0;
  uint32_t rx0 = xf ? 255u : 0u;
  uint32_t xpos = X0 + a * rx0, ypos = Y0 + c * rx0;
  uint32_t dx = xf ? 0u - a : a, dy = xf ? 0u - c : c;
  const uint16_t *vr = g_vram;
#if SM_PSP_M7_ROW4
  if (!mos) {
    for (int x = 0; x < kGameWidth; x += 4) {  /* kGameWidth is a multiple of 4 */
      const uint32_t x1 = xpos + dx, x2 = x1 + dx, x3 = x2 + dx, y1 = ypos + dy, y2 = y1 + dy, y3 = y2 + dy;
      if ((xpos | x1 | x2 | x3 | ypos | y1 | y2 | y3) > outside) {  /* some pixel is outside the field */
        dst[x] = (uint8_t)M7Px(vr, xpos, ypos, outside, fill);
        dst[x + 1] = (uint8_t)M7Px(vr, x1, y1, outside, fill);
        dst[x + 2] = (uint8_t)M7Px(vr, x2, y2, outside, fill);
        dst[x + 3] = (uint8_t)M7Px(vr, x3, y3, outside, fill);
      } else {
        const unsigned t0 = vr[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        const unsigned t1 = vr[(y1 >> 11 & 0x7f) * 128 + (x1 >> 11 & 0x7f)] & 0xff;
        const unsigned t2 = vr[(y2 >> 11 & 0x7f) * 128 + (x2 >> 11 & 0x7f)] & 0xff;
        const unsigned t3 = vr[(y3 >> 11 & 0x7f) * 128 + (x3 >> 11 & 0x7f)] & 0xff;
        const unsigned p0 = vr[t0 * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        const unsigned p1 = vr[t1 * 64 + (y1 >> 8 & 7) * 8 + (x1 >> 8 & 7)] >> 8;
        const unsigned p2 = vr[t2 * 64 + (y2 >> 8 & 7) * 8 + (x2 >> 8 & 7)] >> 8;
        const unsigned p3 = vr[t3 * 64 + (y3 >> 8 & 7) * 8 + (x3 >> 8 & 7)] >> 8;
        dst[x] = (uint8_t)p0;
        dst[x + 1] = (uint8_t)p1;
        dst[x + 2] = (uint8_t)p2;
        dst[x + 3] = (uint8_t)p3;
      }
      xpos = x3 + dx;
      ypos = y3 + dy;
    }
  } else {
#else
  if (!mos) {
    for (int x = 0; x < kGameWidth; x++, xpos += dx, ypos += dy) {
      unsigned tile = 0;
      if ((xpos | ypos) > outside) {
        if (!fill) {
          dst[x] = 0;
          continue;
        }
      } else {
        tile = vr[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
      }
      dst[x] = (uint8_t)(vr[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8);
    }
  } else {
#endif
    for (int x = 0; x < kGameWidth; x += size, xpos += dx * (uint32_t)size, ypos += dy * (uint32_t)size) {
      unsigned tile = 0, pixel = 0;
      if ((xpos | ypos) > outside) {
        if (fill)
          pixel = vr[(ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
      } else {
        tile = vr[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        pixel = vr[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
      }
      int w = kGameWidth - x < size ? kGameWidth - x : size;
      memset(dst + x, (int)pixel, (size_t)w);
    }
  }
}

/* The part of a row's state that M7RenderRow reads (see Gate 5t above). Rows without mode 7 all have the zero key, which
 * never equals the key of a mode 7 row (on = 1). */
static void M7KeyOf(const LineSnap *s, M7RowKey *k) {
  memset(k, 0, sizeof(*k));
  k->on = s->m7on;
  if (!s->m7on)
    return;
  k->fl = s->m7fl;
  if (s->m7fl & 32) {  /* the mosaic size and start line are only read with the mosaic flag */
    k->ms = s->mossize;
    k->mt = s->mosstart;
  }
  memcpy(k->m, s->m7m, sizeof(k->m));
}

/* Gate 4r: renders the mode 7 rows (BG1 on) into g_m7img and records the row ranges to draw. CPU only, before
 * the list is opened; the rows that were rendered are written back for the GE. Gate 5t: a row whose key is unchanged
 * and whose plane (VRAM pages 0..kM7Pages-1) was not written keeps the pixels of the last frame; the GE only reads
 * g_m7img, so nothing else needs doing for them. */
static void M7PrepareImg(void) {
  int nr = 0, bmin = -1, bmax = -1;
  PROF(uint32_t t0 = Now(); uint32_t taud = 0;)
  if (g_m7i_stale || !g_m7i_reuse_ok) {  /* no stored row is valid (rows that are not drawn now would be reused stale later) */
    memset(g_m7ikey, 0, sizeof(g_m7ikey));
#if SM_PSP_LOG
    if (g_m7i_stale)
      g_m7i_stalef++;
#endif
    g_m7i_stale = false;
  }
#if SM_PSP_LOG
  const bool audit = g_m7i_reuse_ok && (++g_m7i_auditn & 15) == 0;
#endif
  for (int r = 0; r < g_line_count; r++) {
    if (!g_lines[r].m7on)
      continue;
    M7RowKey k;
    M7KeyOf(&g_lines[r], &k);
    bool render = memcmp(&g_m7ikey[r], &k, sizeof(k)) != 0;
    if (render) {
#if SM_PSP_LOG && SM_PSP_M7_PROBE
      if ((r & 3) == 0) {
        uint8_t scratch[256] __attribute__((aligned(16)));
        uint32_t tc = Now();
        M7RenderRow(&g_lines[r], r + 1, g_m7img + r * 256);
        uint32_t tm = Now();
        M7RenderRow(&g_lines[r], r + 1, scratch);
        uint32_t tw = Now();
        g_m7p_rows++;
        g_m7p_cold += tm - tc;
        g_m7p_warm += tw - tm;
        taud += tw - tm;
      } else
#endif
      M7RenderRow(&g_lines[r], r + 1, g_m7img + r * 256);
      g_m7ikey[r] = k;
      PROF(g_m7i_built++;)
    } else {
      PROF(g_m7i_kept++;)
#if SM_PSP_LOG
      if (audit) {  /* log builds: the kept row must equal a fresh render */
        uint32_t ta = Now();
        uint8_t fresh[256] __attribute__((aligned(16)));
        M7RenderRow(&g_lines[r], r + 1, fresh);
        g_m7i_audits++;
        if (memcmp(fresh, g_m7img + r * 256, 256) != 0) {
          memcpy(g_m7img + r * 256, fresh, 256);
          render = true;
          g_m7i_bad++;
          g_m7i_reuse_ok = false;
          PspLog("m7reuse: row %d differs from a fresh render, row reuse disabled", r);
        }
        taud += Now() - ta;
      }
#endif
    }
    if (render) {
      if (bmin < 0)
        bmin = r;
      bmax = r;
    }
    if (nr && g_m7rng[nr - 1][1] == r) {
      g_m7rng[nr - 1][1] = (uint16_t)(r + 1);
    } else {
      g_m7rng[nr][0] = (uint16_t)r;
      g_m7rng[nr][1] = (uint16_t)(r + 1);
      nr++;
    }
  }
  if (bmin >= 0)  /* rows between bmin and bmax that were kept are clean: writing them back costs nothing visible */
    sceKernelDcacheWritebackRange(g_m7img + bmin * 256, (unsigned)(bmax - bmin + 1) * 256);
  g_m7_nrng = nr;
  g_m7_img = nr > 0;
  PROF(g_m7i_us = Now() - t0 - taud;)
}

#if SM_PSP_M7_BLK
#if SM_PSP_LOG
/* Frames the block path refused because no block size gave a window below kM7WinMax (the CPU image draws them: 13-16 ms of
 * prep). Log only: the matrix of the first refused frames, then every 64th, and per window the largest matrix terms. */
static int g_m7b_ext;  /* M7TryBlk: pixels of the first window that did not fit (-1: too many blocks) */
static uint32_t g_m7b_rf_n, g_m7b_rf_nlog;
static int g_m7b_rf_a, g_m7b_rf_b, g_m7b_rf_c, g_m7b_rf_d, g_m7b_rf_e8min = 1 << 30, g_m7b_rf_e8max;
static void M7BlkRefused(const int16_t *m, const int *ext) {
  const int a = m[0] < 0 ? -m[0] : m[0], b = m[1] < 0 ? -m[1] : m[1], c = m[2] < 0 ? -m[2] : m[2],
            d = m[3] < 0 ? -m[3] : m[3];
  g_m7b_rf_n++;
  if (a > g_m7b_rf_a) g_m7b_rf_a = a;
  if (b > g_m7b_rf_b) g_m7b_rf_b = b;
  if (c > g_m7b_rf_c) g_m7b_rf_c = c;
  if (d > g_m7b_rf_d) g_m7b_rf_d = d;
  if (ext[2] < g_m7b_rf_e8min) g_m7b_rf_e8min = ext[2];
  if (ext[2] > g_m7b_rf_e8max) g_m7b_rf_e8max = ext[2];
  if (g_m7b_rf_nlog < 40 || (g_m7b_rf_n & 63) == 0) {
    g_m7b_rf_nlog++;
    PspLog("m7blk refused f=%u m=%d,%d,%d,%d ctr=%d,%d scr=%d,%d fl=%u rows=%d lines=%d | first window too big (px, limit %d): 64x16=%d 32x16=%d 32x8=%d",
           (unsigned)g_frame, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], (unsigned)g_m7c.fl, g_m7_rows, g_line_count,
           (int)kM7WinMax, ext[0], ext[1], ext[2]);
  }
}
#endif

static int g_m7b_skip_n, g_m7b_skip_mag;  /* refusal backoff: frames still to refuse unseen, matrix size at the refusal */
#if SM_PSP_LOG
static uint32_t g_m7b_fast;  /* frames refused without trying a block size (backoff) */
#endif
/* M7BLK-BEGIN */
/* Gate 5w. Hardware bench (gate 5t-b/c, PSP-1000 222 MHz, 4 matrices): the CPU image costs 9.2-11.1 ms + 1.4 ms drawing it; a
 * swizzled 1024x1024 plane drawn with one bind per 64x16 block of rows, one GU_TRIANGLES quad per row and block, costs 2.1-3.1
 * ms on the GE and was pixel-exact (bad=0 in all 24 block configurations; one 512 window for the whole frame was not).
 * The plane is the 128x128 map of 8x8 tiles expanded to 8 bit pixels (the same pixel M7RenderRow reads: tile = low byte of
 * the map word, pixel = high byte of word tile * 64 + row * 8 + column) in PSP swizzle order, so it is kept up to date from
 * the VRAM pages the game writes. Taken when every row of the frame has the same matrix and the flags are at most large
 * field and y flip; anything else uses the CPU image as before. Gate 5w-b (first hardware run: 28% of the rotating frames
 * were taken, the rest failed the window size, the plane edge or the wrap edge): the block size is tried at 64x16, 32x16 and
 * 32x8; outside the plane (large field) every row segment is cut to the pixels ppu.c would draw; the wrapping plane has the
 * repeated margin. */
#define M7BMIN(a, b) ((a) < (b) ? (a) : (b))
#define M7BMAX(a, b) ((a) > (b) ? (a) : (b))

/* One 8x8 cell at (cx, cy), 0 <= cx, cy < 144, of the padded plane: the tile of map cell (cx & 127, cy & 127). Swizzled: 72
 * blocks of 16 B per block row, a cell is half a block (8 B) wide and one block row high. */
static void M7PlaneCell(int cx, int cy) {
  const unsigned tile = g_vram[(cy & 127) * 128 + (cx & 127)] & 0xff;
  uint32_t *dst = (uint32_t *)(g_m7pl + (((cy * (kM7PlaneP / 16)) + (cx >> 1)) << 7) + (cx & 1) * 8);
  if (g_m7pl_fast) {  /* little endian: the four pixels of dst[0] are the four bytes of the tile row */
    const uint32_t *src = (const uint32_t *)&g_m7t8[tile * 64];
    for (int y = 0; y < 8; y++, src += 2, dst += 4) {
      dst[0] = src[0];
      dst[1] = src[1];
    }
    return;
  }
  const uint16_t *src = &g_vram[tile * 64];
  for (int y = 0; y < 8; y++, src += 8, dst += 4) {
    dst[0] = (uint32_t)(src[0] >> 8) | (uint32_t)(src[1] >> 8) << 8 | (uint32_t)(src[2] >> 8) << 16 | (uint32_t)(src[3] >> 8) << 24;
    dst[1] = (uint32_t)(src[4] >> 8) | (uint32_t)(src[5] >> 8) << 8 | (uint32_t)(src[6] >> 8) << 16 | (uint32_t)(src[7] >> 8) << 24;
  }
}
/* Refreshes g_m7t8 for tiles t0 .. t0 + n - 1 from g_vram (tile t is the words t * 64 .. t * 64 + 63, VRAM page t >> 2). */
static void M7TilesRefresh(int t0, int n) {
  for (int i = t0 * 64; i < (t0 + n) * 64; i++)
    g_m7t8[i] = (uint8_t)(g_vram[i] >> 8);
}
#if SM_PSP_LOG && SM_PSP_VERIFY
/* Reference for one cell: the original word by word conversion straight from g_vram, compared with what is in the plane. */
static bool M7PlaneCellSame(int cx, int cy) {
  const unsigned tile = g_vram[(cy & 127) * 128 + (cx & 127)] & 0xff;
  const uint16_t *src = &g_vram[tile * 64];
  const uint32_t *dst = (const uint32_t *)(g_m7pl + (((cy * (kM7PlaneP / 16)) + (cx >> 1)) << 7) + (cx & 1) * 8);
  for (int y = 0; y < 8; y++, src += 8, dst += 4) {
    const uint32_t r0 = (uint32_t)(src[0] >> 8) | (uint32_t)(src[1] >> 8) << 8 | (uint32_t)(src[2] >> 8) << 16 | (uint32_t)(src[3] >> 8) << 24;
    const uint32_t r1 = (uint32_t)(src[4] >> 8) | (uint32_t)(src[5] >> 8) << 8 | (uint32_t)(src[6] >> 8) << 16 | (uint32_t)(src[7] >> 8) << 24;
    if (dst[0] != r0 || dst[1] != r1)
      return false;
  }
  return true;
}
#endif
/* Map cell (cx, cy), 0 <= cx, cy < 128, and its copies in the repeated margin (cells 128-143 repeat cells 0-15). */
static void M7PlaneCellAll(int cx, int cy) {
  M7PlaneCell(cx, cy);
  if (cx < 16)
    M7PlaneCell(cx + 128, cy);
  if (cy < 16) {
    M7PlaneCell(cx, cy + 128);
    if (cx < 16)
      M7PlaneCell(cx + 128, cy + 128);
  }
}

/* Like M7TilesRefresh for one tile; true if its pixels changed. */
static bool M7TileRefresh1(int t) {
  uint8_t *d = &g_m7t8[t * 64];
  const uint16_t *src = &g_vram[t * 64];
  unsigned diff = 0;
  for (int k = 0; k < 64; k++) {
    const uint8_t v = (uint8_t)(src[k] >> 8);
    diff |= (unsigned)(d[k] ^ v);
    d[k] = v;
  }
  return diff != 0;
}

/* The whole plane from g_vram. */
static void M7PlaneFull(void) {
  M7TilesRefresh(0, 256);
  for (int cell = 0; cell < 128 * 128; cell++)
    g_m7map[cell] = (uint8_t)(g_vram[cell] & 0xff);
  for (int cy = 0; cy < kM7PlaneP / 8; cy++)
    for (int cx = 0; cx < kM7PlaneP / 8; cx++)
      M7PlaneCell(cx, cy);
  g_m7pl_valid = true;
}

/* Brings the plane up to date with g_vram and writes it back for the GE (CPU only, before the list is opened, like g_m7img).
 * A written map word changes its own cell; a written tile word changes every cell that shows the tile (tile t is VRAM page
 * t >> 2). Many dirty pages, or no plane yet: rebuild everything. */
static void M7PlaneUpdate(void) {
  int nd = 0;
  unsigned nchg = 0, ncell = 0;  /* tiles whose pixels changed, cells rebuilt (partial update) */
  for (int pg = 0; pg < kM7Pages; pg++)
    nd += g_m7pl_dirty[pg];
  if (g_m7pl_valid && nd == 0)
    return;
#if SM_PSP_LOG
  const uint32_t t0 = Now();
  const bool full = !g_m7pl_valid || nd >= kM7PlFull;
#endif
  if (!g_m7pl_valid || nd >= kM7PlFull) {
    M7PlaneFull();
  } else if (g_m7pl_incr) {
    uint32_t tmask[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int pg = 0; pg < kM7Pages; pg++)
      if (g_m7pl_dirty[pg])
        for (int t = pg * 4; t < pg * 4 + 4; t++)
          if (M7TileRefresh1(t)) {
            tmask[t >> 5] |= 1u << (t & 31);
            nchg++;
          }
    for (int pg = 0; pg < kM7Pages; pg++) {
      if (!g_m7pl_dirty[pg])
        continue;
      for (int i = 0; i < kVramPageWords; i++) {
        const int cell = pg * kVramPageWords + i;
        const uint8_t t = (uint8_t)(g_vram[cell] & 0xff);
        if (t != g_m7map[cell]) {
          g_m7map[cell] = t;
          M7PlaneCellAll(cell & 127, cell >> 7);
          ncell++;
        }
      }
    }
    if (nchg)
      for (int cell = 0; cell < 128 * 128; cell++) {
        const unsigned t = g_m7map[cell];
        if (tmask[t >> 5] >> (t & 31) & 1) {
          M7PlaneCellAll(cell & 127, cell >> 7);
          ncell++;
        }
      }
  } else {
    uint32_t tmask[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int pg = 0; pg < kM7Pages; pg++)
      if (g_m7pl_dirty[pg])
        M7TilesRefresh(pg * 4, 4);
    for (int pg = 0; pg < kM7Pages; pg++) {
      if (!g_m7pl_dirty[pg])
        continue;
      for (int i = 0; i < kVramPageWords; i++) {
        const int cell = pg * kVramPageWords + i;
        g_m7map[cell] = (uint8_t)(g_vram[cell] & 0xff);
        M7PlaneCellAll(cell & 127, cell >> 7);
      }
      tmask[(pg * 4) >> 5] |= 0xfu << ((pg * 4) & 31);
    }
    for (int cell = 0; cell < 128 * 128; cell++) {
      const unsigned t = g_vram[cell] & 0xff;
      if (tmask[t >> 5] >> (t & 31) & 1)
        M7PlaneCellAll(cell & 127, cell >> 7);
    }
  }
  (void)nchg;
  (void)ncell;
  memset(g_m7pl_dirty, 0, sizeof(g_m7pl_dirty));
#if SM_PSP_LOG
  const uint32_t t_conv = Now() - t0;
#if SM_PSP_VERIFY
  {  /* the first full rebuilds are compared cell by cell with the word by word conversion; a difference switches it back */
    static unsigned nchkp;
    if (!full && g_m7pl_incr && nchkp < 12) {  /* the first partial updates: the whole plane against the word by word conversion */
      nchkp++;
      unsigned bad = 0;
      for (int cy = 0; cy < kM7PlaneP / 8; cy++)
        for (int cx = 0; cx < kM7PlaneP / 8; cx++)
          bad += !M7PlaneCellSame(cx, cy);
      if (bad || nchkp == 12)
        PspLog("m7plane check f=%u partial updates 1-%u: %u of %d cells differ%s", (unsigned)g_frame, nchkp, bad,
               (kM7PlaneP / 8) * (kM7PlaneP / 8), bad ? ", full rebuild every time restored" : "");
      if (bad) {
        g_m7pl_incr = false;
        M7PlaneFull();
      }
    }
    static unsigned nchk;
    if (full && g_m7pl_fast && nchk < 3) {
      nchk++;
      unsigned bad = 0;
      for (int cy = 0; cy < kM7PlaneP / 8; cy++)
        for (int cx = 0; cx < kM7PlaneP / 8; cx++)
          bad += !M7PlaneCellSame(cx, cy);
      PspLog("m7plane check f=%u full rebuild %u: %u of %d cells differ%s", (unsigned)g_frame, nchk, bad,
             (kM7PlaneP / 8) * (kM7PlaneP / 8), bad ? ", word by word conversion restored" : "");
      if (bad) {
        g_m7pl_fast = false;
        for (int cy = 0; cy < kM7PlaneP / 8; cy++)
          for (int cx = 0; cx < kM7PlaneP / 8; cx++)
            M7PlaneCell(cx, cy);
      }
    }
  }
#endif
  const uint32_t t1 = Now();
#endif
  sceKernelDcacheWritebackRange(g_m7pl, sizeof(g_m7pl));
#if SM_PSP_LOG
  {
    static unsigned nlog;
    const uint32_t t_wb = Now() - t1;
    if ((full || t_conv + t_wb > 2000) && nlog++ < 80)
      PspLog("m7plane f=%u %s dirty=%d conv=%uus writeback=%uus fast=%d incr=%d chg=%u cells=%u", (unsigned)g_frame, full ? "full" : "part", nd,
             (unsigned)t_conv, (unsigned)t_wb, (int)g_m7pl_fast, (int)g_m7pl_incr, nchg, ncell);
  }
#endif
}

/* Min and max of (r0 + k * step) & 255 over k < n for every start residue r0, without scanning the segment. offsets
 * o_k = (step * k) & 255 are marked present once per frame; nxt[t] = smallest present offset >= t (256: none), prv[t] =
 * largest present offset <= t (-1: none). Start r0 wraps for the offsets o >= 256 - r0: the smallest wrapped value is
 * r0 + nxt[256 - r0] - 256 (below r0, so below every unwrapped one) and the largest is max(r0 + omax - 256, r0 + the
 * largest unwrapped offset). A segment shorter than n has a subset of these residues: the range is then only wider. */
typedef struct M7Res { int16_t nxt[257], prv[256]; int omin, omax; } M7Res;
static void M7ResBuild(M7Res *t, int step, int n) {
  uint8_t present[256];
  memset(present, 0, sizeof(present));
  for (int k = 0; k < n; k++)
    present[(step * k) & 255] = 1;
  t->nxt[256] = 256;
  for (int i = 255; i >= 0; i--)
    t->nxt[i] = present[i] ? (int16_t)i : t->nxt[i + 1];
  int last = -1;
  for (int i = 0; i < 256; i++) {
    if (present[i])
      last = i;
    t->prv[i] = (int16_t)last;
  }
  t->omin = t->nxt[0];
  t->omax = t->prv[255];
}
static inline void M7ResGet(const M7Res *t, int r0, int *lo, int *hi) {
  const int w = 256 - r0;  /* offsets >= w wrap */
  if (t->omax < w) {
    *lo = r0 + t->omin;
    *hi = r0 + t->omax;
  } else {
    *lo = r0 + t->nxt[w] - 256;
    *hi = r0 + t->omax - 256;
    if (t->prv[w - 1] >= 0 && r0 + t->prv[w - 1] > *hi)
      *hi = r0 + t->prv[w - 1];
  }
}

/* Row starts exactly as M7RenderRow computes them (y is the row after the y flip). */
static inline void M7BRowStart(const int16_t *m, int y, int32_t *X0, int32_t *Y0) {
  const int xc = ((int16_t)(m[4] << 3)) >> 3, yc = ((int16_t)(m[5] << 3)) >> 3;
  const int hs = ((int16_t)(m[6] << 3)) >> 3, vs = ((int16_t)(m[7] << 3)) >> 3;
  int ch = hs - xc, cv = vs - yc;
  ch = (ch & 0x2000) ? (ch | ~1023) : (ch & 1023);
  cv = (cv & 0x2000) ? (cv | ~1023) : (cv & 1023);
  *X0 = ((m[0] * ch) & ~63) + ((m[1] * y) & ~63) + ((m[1] * cv) & ~63) + (xc << 8);
  *Y0 = ((m[2] * ch) & ~63) + ((m[3] * y) & ~63) + ((m[3] * cv) & ~63) + (yc << 8);
}

/* floor(n / d) and ceil(n / d) for d > 0 and any n. */
static inline int M7FloorDiv(int n, int d) { return n >= 0 ? n / d : -((-n + d - 1) / d); }
static inline int M7CeilDiv(int n, int d) { return -M7FloorDiv(-n, d); }

/* Narrows [*xs, *xe) to the x with 0 <= p0 + step * x <= 0x3ffff: the positions ppu.c does not treat as outside the large
 * field ((xpos | ypos) > 0x3ffff with xpos, ypos unsigned). */
static void M7Clip(int p0, int step, int *xs, int *xe) {
  int lo, hi;  /* x in [lo, hi] */
  if (step == 0) {
    if (p0 < 0 || p0 > 0x3ffff)
      *xe = *xs;
    return;
  }
  if (step > 0) {
    lo = M7CeilDiv(-p0, step);
    hi = M7FloorDiv(0x3ffff - p0, step);
  } else {
    lo = M7CeilDiv(p0 - 0x3ffff, -step);
    hi = M7FloorDiv(p0, -step);
  }
  if (lo > *xs)
    *xs = lo;
  if (hi + 1 < *xe)
    *xe = hi + 1;
  if (*xe < *xs)
    *xe = *xs;
}

/* Segments (one row of one block of bw pixels) and windows (one per block of bh rows x bw pixels). Nearest sampling takes
 * floor(u); the exact plane position of a pixel is a multiple of 1/256, so u is shifted by delta = (256 - m0 - m1) / 512
 * (middle of the room the residues m0..m1 of the segment leave) minus the half pixel the GE adds at the pixel centre (g_uvh),
 * and u, v are relative to the window origin so that they stay below kM7WinMax and the GE's interpolation stays exact.
 * Returns false when a window needs kM7WinMax pixels or more. */
static bool M7TryBlk(const int16_t *m, int bw, int bh, bool wrap, bool yflip, const M7Res *ru, const M7Res *rv, int *pnseg,
                     int *pnblk) {
  const int a = m[0], c = m[2];
  const float du = (float)a / 256.0f, dv = (float)c / 256.0f;
  int nseg = 0, nblk = 0;
  for (int by0 = 0; by0 < g_line_count; by0 += bh) {
    const int by1 = M7BMIN(by0 + bh, g_line_count);
    for (int bx0 = 0; bx0 < kGameWidth; bx0 += bw) {
      if (nblk >= kM7MaxBlk) {
        PROF(g_m7b_ext = -1;)
        return false;
      }
      const int first = nseg;
      int gminx = 1 << 30, gminy = 1 << 30, gmaxx = -(1 << 30), gmaxy = -(1 << 30);
      for (int r = by0; r < by1; r++) {
        if (!g_lines[r].m7on)
          continue;
        int32_t X0, Y0;
        M7BRowStart(m, yflip ? 255 - (r + 1) : r + 1, &X0, &Y0);
        int xs = bx0, xe = bx0 + bw;
        if (!wrap) {  /* outside the plane is transparent: the segment only covers the pixels ppu.c draws */
          M7Clip(X0, a, &xs, &xe);
          M7Clip(Y0, c, &xs, &xe);
          if (xs >= xe)
            continue;
        }
        const int32_t Xs = X0 + a * xs, Xe = X0 + a * xe, Ys = Y0 + c * xs, Ye = Y0 + c * xe;
        int mu0, mu1, mv0, mv1;
        M7ResGet(ru, Xs & 255, &mu0, &mu1);
        M7ResGet(rv, Ys & 255, &mv0, &mv1);
        const int minx = M7BMIN(Xs >> 8, Xe >> 8) - 1, maxx = M7BMAX(Xs >> 8, Xe >> 8) + 2;
        const int miny = M7BMIN(Ys >> 8, Ye >> 8) - 1, maxy = M7BMAX(Ys >> 8, Ye >> 8) + 2;
        if (minx < gminx) gminx = minx;
        if (miny < gminy) gminy = miny;
        if (maxx > gmaxx) gmaxx = maxx;
        if (maxy > gmaxy) gmaxy = maxy;
        const float su = (float)(256 - mu0 - mu1) / 512.0f - g_uvh[0] * du;
        const float sv = (float)(256 - mv0 - mv1) / 512.0f - g_uvh[1] * dv;
        M7BSeg *sg = &g_m7bseg[nseg++];
        sg->u0 = (float)Xs / 256.0f + su;  /* plane position, made window relative below */
        sg->u1 = (float)Xe / 256.0f + su;
        sg->v0 = (float)Ys / 256.0f + sv;
        sg->v1 = (float)Ye / 256.0f + sv;
        sg->x0 = (int16_t)xs;
        sg->x1 = (int16_t)xe;
        sg->y = (int16_t)r;
        sg->pad = 0;
      }
      if (nseg == first)
        continue;
      if (!wrap) {  /* the drawn pixels are inside the plane; the margin of the bounding box may not be */
        if (gminx < 0) gminx = 0;
        if (gminy < 0) gminy = 0;
      }
      const int wxu = gminx & ~15, wyu = gminy & ~7;  /* unwrapped window origin, aligned to the swizzle block */
      if (M7BMAX(gmaxx - wxu, gmaxy - wyu) >= kM7WinMax) {
        PROF(g_m7b_ext = M7BMAX(gmaxx - wxu, gmaxy - wyu);)
        return false;
      }
      /* wrapping plane: the window starts at the origin modulo 1024 and may run into the repeated margin */
      const int wx = wrap ? (wxu & (kM7PlaneW - 1)) : wxu, wy = wrap ? (wyu & (kM7PlaneW - 1)) : wyu;
      for (int i = first; i < nseg; i++) {
        g_m7bseg[i].u0 -= (float)wxu;
        g_m7bseg[i].u1 -= (float)wxu;
        g_m7bseg[i].v0 -= (float)wyu;
        g_m7bseg[i].v1 -= (float)wyu;
      }
      M7BBlk *bk = &g_m7bblk[nblk++];
      bk->wx = (int16_t)wx;
      bk->wy = (int16_t)wy;
      bk->first = (uint16_t)first;
      bk->n = (uint16_t)(nseg - first);
    }
  }
  *pnseg = nseg;
  *pnblk = nblk;
  return true;
}

static bool M7PrepareBlk(void) {
  g_m7_blk = false;
  g_m7_bnseg = g_m7_bnblk = 0;
  if (!g_m7b_ok) {
    BLK_WHY(kBlkOff);
    return false;
  }
  if (!g_m7_same || g_m7_rows == 0) {
    BLK_WHY(kBlkSame);
    return false;
  }
  const unsigned fl = g_m7c.fl;
  if (fl & ~9u) {  /* char fill, x flip, mosaic (extbg never gets here) */
    BLK_WHY(kBlkFlags);
    return false;
  }
  const int16_t *m = g_m7c.m;
  /* Backoff. A refused frame costs the three block-size attempts (about 5 ms) before the CPU image is built, and the zoom /
   * rotation sequences that are refused stay refused for many frames. After a refusal the next kM7BlkBackoff frames whose
   * matrix is not smaller (by more than 1/16) go straight to the CPU image; a smaller matrix, or the end of the backoff,
   * tries the block sizes again. The CPU image is the exact path: this only decides how much work is spent on finding out
   * that the block path does not fit. */
  int mag = 0;
  for (int i = 0; i < 4; i++) {
    const int v = m[i] < 0 ? -m[i] : m[i];
    mag = v > mag ? v : mag;
  }
  if (g_m7b_skip_n > 0 && mag * 16 >= g_m7b_skip_mag * 15) {
    g_m7b_skip_n--;
    BLK_WHY(kBlkWindow);
    PROF(g_m7b_fast++;)
    return false;
  }
  g_m7b_skip_n = 0;
  PROF(uint32_t t0 = Now();)
  static const int kCfg[3][2] = {{64, 16}, {32, 16}, {32, 8}};
  M7Res ru, rv;
  int resbw = 0, nseg = 0, nblk = 0, cfg = -1;
  PROF(int ext[3] = {0, 0, 0};)
  for (int k = 0; k < 3 && cfg < 0; k++) {
    if (kCfg[k][0] != resbw) {
      resbw = kCfg[k][0];
      M7ResBuild(&ru, m[0] & 255, resbw);
      M7ResBuild(&rv, m[2] & 255, resbw);
    }
    if (M7TryBlk(m, kCfg[k][0], kCfg[k][1], !(g_m7c.fl & 1), (g_m7c.fl & 8) != 0, &ru, &rv, &nseg, &nblk))
      cfg = k;
    PROF(else ext[k] = g_m7b_ext;)
  }
  if (cfg < 0 || nseg == 0) {
    BLK_WHY(kBlkWindow);
    PROF(M7BlkRefused(m, ext);)
    if (cfg < 0) {  /* a window that did not fit (nseg == 0: nothing of the frame is visible, no backoff) */
      g_m7b_skip_n = kM7BlkBackoff;
      g_m7b_skip_mag = mag;
    }
    return false;
  }
  PROF(uint32_t t1 = Now();)
  M7PlaneUpdate();
  PROF(g_m7b_t_prep += t1 - t0; g_m7b_t_plane += Now() - t1; g_m7b_tn++; g_m7b_cfg[cfg]++;)
  g_m7_bnseg = nseg;
  g_m7_bnblk = nblk;
  g_m7_blk = true;
  g_m7_img = true;  /* downstream code treats the frame like an image frame (z level, verification, statistics) */
  g_m7_nrng = 0;
  BLK_WHY(kBlkOk);
  return true;
}
/* M7BLK-END */
#endif

static void M7PrepareAny(void) {
#if SM_PSP_M7_BLK
  if (M7PrepareBlk())
    return;
#endif
  M7PrepareImg();
}

/* Mode 7 frame as textured quads. With b = c = 0 the screen maps to the plane by
 *   plane x = (X0 + a * x) >> 8                      (linear in x)
 *   plane y = (Y0 + ((d * ry) & ~63)) >> 8           (constant along a row; ry = line, 1-based)
 * exactly as ppu.c computes it. Rows are grouped by plane tile row when d is a multiple of 64 (the
 * mapping is then exactly linear and one quad covers a whole tile band), otherwise by plane row (the
 * quantisation to 64 makes the mapping stair-stepped) with a constant v. Texture coordinates are
 * placed so that nearest sampling picks the same texel ppu.c does: target + 1/512 stays clear of
 * texel boundaries, and g_uvh says whether the GE samples at the pixel centre or the corner. */
static bool M7Prepare(void) {
  g_m7_nq = 0;
  g_m7_img = false;
  g_m7_blk = false;
  g_m7_nrng = 0;
  if (g_m7_rows == 0)
    return true;
  const int16_t *m = g_m7c.m;
  /* fl 1: large field (outside the plane is transparent), fl 0: small field (the plane wraps, gate 5q) */
  const bool wrap = g_m7c.fl == 0;
  if (!(g_m7_same && g_m7q_ok && (g_m7c.fl == 1 || (wrap && g_m7w_ok)) && !m[1] && !m[2] && m[0] > 0 && m[3] > 0)) {
    M7PrepareAny();  /* rotation, per-line matrix, flips, charfill, mosaic */
    return true;
  }
  int a = m[0], d = m[3];
  int xCenter = ((int16_t)(m[4] << 3)) >> 3, yCenter = ((int16_t)(m[5] << 3)) >> 3;
  int hScroll = ((int16_t)(m[6] << 3)) >> 3, vScroll = ((int16_t)(m[7] << 3)) >> 3;
  int clippedH = hScroll - xCenter, clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  uint32_t X0 = (uint32_t)((a * clippedH) & ~63) + ((uint32_t)xCenter << 8);
  uint32_t Yc = (uint32_t)((d * clippedV) & ~63) + ((uint32_t)yCenter << 8);
  g_m7_yc = Yc;
  const float du = (float)a / 256.0f, dv = (float)d / 256.0f;

  struct { int xa, xb, tx, m0, m1; float u; } cs[160];
  int ncs = 0;
  for (int x = 0; x < kGameWidth; x++) {
    uint32_t X = X0 + (uint32_t)a * (uint32_t)x;
    if (!wrap && X > 0x3ffff)
      continue;
    int tx = (int)((X >> 11) & 0x7f);
    if (ncs && cs[ncs - 1].xb == x && cs[ncs - 1].tx == tx) {
      cs[ncs - 1].xb = x + 1;
    } else {
      if (ncs == 160) {
        g_m7_nq = 0;
        M7PrepareAny();
        return true;
      }
      cs[ncs].xa = x;
      cs[ncs].xb = x + 1;
      cs[ncs].tx = tx;
      cs[ncs].u = (float)(X & 0x7ffu) / 256.0f;
      cs[ncs].m0 = cs[ncs].m1 = (int)(X & 255);
      ncs++;
    }
    if ((int)(X & 255) < cs[ncs - 1].m0) cs[ncs - 1].m0 = (int)(X & 255);
    if ((int)(X & 255) > cs[ncs - 1].m1) cs[ncs - 1].m1 = (int)(X & 255);
  }
  /* Nearest sampling takes floor(u). The exact plane position of a pixel is a multiple of 1/256; shifting
   * it by delta keeps the right texel as long as 0 <= m/256 + delta < 1 for every residue m (0..255) that
   * occurs. Put delta in the middle of that range: the margin against GE rounding is then as large as the
   * residues allow (a fixed 1/512 failed on hardware for scales that put pixels exactly on texel edges). */
  for (int k = 0; k < ncs; k++)
    cs[k].u += (float)(256 - cs[k].m0 - cs[k].m1) / 512.0f;

  bool band = (d % 64) == 0;
  int r = 0;
  while (r < g_line_count) {
    uint32_t Y = (((uint32_t)d * (uint32_t)(r + 1)) & ~63u) + Yc;
    if (!g_lines[r].m7on || (!wrap && Y > 0x3ffff)) {
      r++;
      continue;
    }
    uint32_t key = band ? Y >> 11 : Y >> 8;
    int r1 = r + 1;
    while (r1 < g_line_count && g_lines[r1].m7on) {
      uint32_t Y1 = (((uint32_t)d * (uint32_t)(r1 + 1)) & ~63u) + Yc;
      if ((!wrap && Y1 > 0x3ffff) || (band ? Y1 >> 11 : Y1 >> 8) != key)
        break;
      r1++;
    }
    int ty = (int)((Y >> 11) & 0x7f);
    float v0, v1;
    if (band) {
      int m0 = (int)(Y & 255), m1 = m0;
      for (int rr = r + 1; rr < r1; rr++) {
        int m = (int)((((uint32_t)d * (uint32_t)(rr + 1)) & ~63u) + Yc) & 255;
        if (m < m0) m0 = m;
        if (m > m1) m1 = m;
      }
      float tv = (float)(Y & 0x7ffu) / 256.0f + (float)(256 - m0 - m1) / 512.0f;
      v0 = tv - g_uvh[1] * dv;
      v1 = v0 + (float)(r1 - r) * dv;
    } else {
      v0 = v1 = (float)((Y >> 8) & 7) + 0.5f;
    }
    for (int k = 0; k < ncs; k++) {
      int t = g_vram[ty * 128 + cs[k].tx] & 0xff;
      if (EnsureTile7(t))
        continue;
      if (g_m7_nq >= (g_m7big_ok ? kM7MaxQuads : kM7BaseQuads)) {
        g_m7_nq = 0;
        M7PrepareAny();  /* too many quads: draw the image instead of falling back to software */
        return true;
      }
      float ax = (float)((t & 15) * 8), ay = (float)((t >> 4) * 8);
      float u0 = ax + cs[k].u - g_uvh[0] * du;
      M7Quad *q = &g_m7q[g_m7_nq++];
      q->x0 = (int16_t)cs[k].xa;
      q->x1 = (int16_t)cs[k].xb;
      q->y0 = (int16_t)r;
      q->y1 = (int16_t)r1;
      q->u0 = u0;
      q->u1 = u0 + (float)(cs[k].xb - cs[k].xa) * du;
      q->v0 = ay + v0;
      q->v1 = ay + v1;
    }
    r = r1;
  }
  g_m7w_frame = wrap && g_m7_nq > 0;
  return true;
}

// Wait for the panel only if ahead
static void PaceWait(void) {
#if PSP_VBLANK_GATE
  if (sceDisplayGetVcount() == g_vcount_last)
    sceDisplayWaitVblankStart();
#else
  sceDisplayWaitVblankStart();
#endif
}

static void Pace(void) {
  uint32_t t = Now();
  PaceWait();
  g_vbwait_us = Now() - t;
}

#if SM_PSP_ASYNC
/* Completes the frame queued by Present(): waits for the GE (both lists), waits for the panel only if ahead, queues the
 * finished buffer. Returns true if it completed one (EndDraw has then already paced). Both lists are finished before
 * this returns, so every buffer they reference may be rewritten afterwards. The waits belong to the frame that queued
 * the work: they go to the async profile fields, not to g_gewait_us / g_vbwait_us of the frame being drawn now. */
static void SentinelCheck(void) {
  const volatile uint16_t *p = (const volatile uint16_t *)(g_vram_u + g_pend_off) + kSentY * 512 + kSentX;
  g_sent_chk++;
  if (p[0] == g_pend_sent && p[3] == g_pend_sent)
    return;
  const uint32_t t0 = Now();
  while (Now() - t0 < (uint32_t)kSentWaitUs)
    if (p[0] == g_pend_sent && p[3] == g_pend_sent) {
      g_sent_late++;
      if (g_sent_late <= 8)
        PspLog("async: compose marker appeared %u us after the GE sync (f=%u)", (unsigned)(Now() - t0), (unsigned)g_frame);
      return;
    }
  g_sent_fail++;
  if (g_sent_fail <= 8)
    PspLog("async: compose marker missing at flip (f=%u got=%04x,%04x want=%04x)", (unsigned)g_frame, (unsigned)p[0],
           (unsigned)p[3], (unsigned)g_pend_sent);
}

static bool GeWait(void) {
  if (!g_ge_pending)
    return false;
  PROF(uint32_t t0 = Now();)
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  SentinelCheck();
  g_ge_pending = false;
  PROF(uint32_t t1 = Now();)
  PaceWait();
  PROF(uint32_t t2 = Now(); StatAdd(&g_prof[0].agewait, t1 - t0); StatAdd(&g_prof[0].avbwait, t2 - t1);)
#if SM_PSP_LOG
  {
    const uint32_t d = sceDisplayGetVcount() - g_vcount_last;
    g_flip_d[d > 3 ? 3 : d]++;
  }
#endif
  sceDisplaySetFrameBuf((void *)((uintptr_t)sceGeEdramGetAddr() + g_pend_off), 512, PSP_DISPLAY_PIXEL_FORMAT_565,
                        PSP_DISPLAY_SETBUF_NEXTFRAME);
  g_vcount_last = sceDisplayGetVcount();
  return true;
}
#else
static inline bool GeWait(void) {
  return false;
}
#endif

/* Compares the GE image of a mode 7 frame with the software image (the software PPU drew this frame too).
 * Only the first kM7Verify mode 7 frames; reads the draw buffer through the uncached VRAM mirror. */
static void GeVerify(void) {
  const uint16_t *ge = (const uint16_t *)(g_vram_u + g_draw_off);
  uint32_t exact = 0, near = 0, bad = 0;
  int bx = -1, by = -1;
  uint16_t bsw = 0, bge = 0;
  static uint16_t rowbad[kMaxLines];
  memset(rowbad, 0, sizeof(rowbad));
  for (int y = 0; y < g_line_count; y++) {
    const int tr = 1, tg = 2;
    for (int x = 0; x < kGameWidth; x++) {
      uint16_t sw = Rgb565(g_framebuffer + (y * kGameWidth + x) * 4), g = ge[(kTop + y) * 512 + kLeft + x];
      if (sw == g) {
        exact++;
      } else {
        int dr = (sw & 31) - (g & 31), dg = ((sw >> 5) & 63) - ((g >> 5) & 63), db = (sw >> 11) - (g >> 11);
        if (dr >= -tr && dr <= tr && dg >= -tg && dg <= tg && db >= -tr && db <= tr) {
          near++;
        } else {
          rowbad[y]++;
          if (!bad) {
            bx = x;
            by = y;
            bsw = sw;
            bge = g;
          }
          bad++;
        }
      }
    }
  }
  if (g_stint_frame)  /* smooth tint blends colour bands on purpose: this shows how far it departs from the strict comparison */
    PspLog("geaudit: smooth tint f=%u exact=%u near=%u bad=%u", (unsigned)g_frame, (unsigned)exact, (unsigned)near, (unsigned)bad);
  bool m7 = g_m7_nq > 0;
  if (m7 && !g_m7w_frame) {
    g_m7_vn++;
    g_m7_vbad += bad;
    g_m7_vnear += near;
  }
#if SM_PSP_M7_BLK
  if (g_m7_blk) {  /* gate 5w: its own, strict gate (the image path tolerates 600 bad pixels a frame, this path should have none) */
    g_m7b_vn++;
    g_m7b_vbad += bad;
    if (bad > 8)
      PspLog("m7verify: block mode 7 frame has %d bad pixels (first %d,%d)", bad, bx, by);
    if (g_m7b_vn >= 3 && g_m7b_vbad / g_m7b_vn > 8) {
      g_m7b_ok = false;
      PspLog("m7verify: block mode 7 differs, GE block path disabled (CPU image from now on)");
    }
  } else
#endif
  if (g_m7_img) {
    g_m7i_vn++;
    g_m7i_vbad += bad;
    if (g_m7i_vn >= 3 && g_m7i_vbad / g_m7i_vn > 600) {
      g_m7i_ok = false;
      PspLog("m7verify: image mode 7 differs, GE image path disabled");
    }
  }
  if (m7 && !g_m7w_frame && g_m7_nq > kM7BaseQuads) {
    g_m7_bigv++;
    g_m7_bigbad += bad;
    if (g_m7_bigv >= 3 && g_m7_bigbad / g_m7_bigv > 600) {
      g_m7big_ok = false;
      PspLog("m7verify: large mode 7 frames differ, quad cap back to %d", (int)kM7BaseQuads);
    }
  }
  if (g_m7w_frame) {  /* gate 5q: small-field mode 7 quads have their own audit */
    g_m7w_vn++;
    g_m7w_vbad += bad;
    PspLog("m7wrap f=%u quads=%d bad=%u", (unsigned)g_frame, g_m7_nq, (unsigned)bad);
    if (g_m7w_vn >= 3 && g_m7w_vbad / g_m7w_vn > 600) {
      g_m7w_ok = false;
      PspLog("m7verify: small-field mode 7 quads differ, wrap path disabled (image path takes over)");
    }
  }
  if (g_bright_rows)
    g_br_vn++;
  if (g_img_frame) {  /* gates 5l/5m: layer images; frames with a 32x64 map have their own audit */
    uint32_t *vn = g_imgh_frame ? &g_imgh_vn : &g_img_vn;
    uint32_t *vbad = g_imgh_frame ? &g_imgh_vbad : &g_img_vbad;
    (*vn)++;
    *vbad += bad;
    PspLog("imgaudit f=%u layers=%x high=%d hi=%u,%u bad=%u", (unsigned)g_frame,
           (unsigned)((g_img_on[1] ? 1 : 0) | (g_img_on[2] ? 2 : 0)), g_imgh_frame ? 1 : 0, (unsigned)g_img[0].nhi,
           (unsigned)g_img[1].nhi, (unsigned)bad);
    if ((*vn >= 3 && *vbad / *vn > 600) || bad > 8192) {
      if (g_imgh_frame)
        g_imgh_ok = false;
      else
        g_img_ok = false;
      PspLog("geaudit: layer-image frames differ too much, GE layer images disabled (high=%d)", g_imgh_frame ? 1 : 0);
    }
  }
  if (g_simg_frame) {  /* gate 5p: subscreen layer images, one audit per kind of frame */
    int k = g_simg_kind;
    g_simg_vn[k]++;
    g_simg_vbad[k] += bad;
    PspLog("simgaudit f=%u kind=%d bruns=%d bad=%u", (unsigned)g_frame, k, g_nbruns, (unsigned)bad);
    if ((g_simg_vn[k] >= 3 && g_simg_vbad[k] / g_simg_vn[k] > 600) || bad > 8192) {
      g_simg_ok[k] = false;
      PspLog("geaudit: subscreen layer-image frames differ too much, kind %d disabled", k);
    }
  }
  if (g_win_rows && !g_sh_frame && !g_w2_frame) {  /* half-colour and window-2 frames have their own audits (gates 4t-d/u) */
    g_win_vn++;
    g_win_vbad += bad;
    if (g_win_vn >= 3 && g_win_vbad / g_win_vn > 600) {
      g_win_ok = false;
      PspLog("geaudit: window frames differ too much, GE windows disabled");
    }
  }
  if (g_prev_rows) {
    g_prev_vn++;
    g_prev_vbad += bad;
    if (g_prev_vn >= 3 && g_prev_vbad / g_prev_vn > 600) {
      g_prev_ok = false;
      PspLog("geaudit: prevent-math no-op frames differ too much, GE no-op rule disabled");
    }
  }
  if (g_cw_rows) {
    g_cw_vn++;
    g_cw_vbad += bad;
    if (g_cw_vn >= 3 && g_cw_vbad / g_cw_vn > 600) {
      g_cw_ok = false;
      PspLog("geaudit: colour-window tint frames differ too much, GE colour window disabled");
    }
  }
  if (g_sf_frame) {
    uint32_t *vn = g_sfo_frame ? &g_sfo_vn : g_sf_zero ? &g_sf0_vn : &g_sf_vn;
    uint32_t *vbad = g_sfo_frame ? &g_sfo_vbad : g_sf_zero ? &g_sf0_vbad : &g_sf_vbad;
    const char *nm = g_sfo_frame ? "subfixo" : g_sf_zero ? "subfix0" : "subfix";
    (*vn)++;
    *vbad += bad;
    PspLog("%s f=%u rows=%d..%d fix=%04x bruns=%d masks=%d bad=%u", nm, (unsigned)g_frame, g_sf_r0, g_sf_r1 - 1,
           (unsigned)g_sf_col, g_nbruns, g_sf_masks, (unsigned)bad);
    if (*vn >= 3 && *vbad / *vn > 600) {
      if (g_sfo_frame)
        g_sfo_ok = false;
      else if (g_sf_zero)
        g_sf0_ok = false;
      else
        g_sf_ok = false;
      PspLog("geaudit: subscreen + fixed colour frames differ too much, GE %s disabled", nm);
    }
  }
  if (g_sh_frame) {
    static const char *const kShName[3] = {"subhalf", "subhobj", "subhneg"};
    int k = g_shn_frame ? 2 : g_shobj_frame ? 1 : 0;
    uint32_t *vn = k == 2 ? &g_shn_vn : k == 1 ? &g_sho_vn : &g_sh_vn;
    uint32_t *vbad = k == 2 ? &g_shn_vbad : k == 1 ? &g_sho_vbad : &g_sh_vbad;
    (*vn)++;
    *vbad += bad;
    PspLog("%s f=%u rows=%d..%d fix=%04x bruns=%d masks=%d winrows=%u bad=%u", kShName[k],
           (unsigned)g_frame, g_sf_r0, g_sf_r1 - 1, (unsigned)g_sf_col, g_nbruns, g_sf_masks, (unsigned)g_win_rows,
           (unsigned)bad);
    if ((*vn >= 3 && *vbad / *vn > 600) || bad > 8192) {
      if (k == 2)
        g_shn_ok = false;
      else if (k == 1)
        g_shobj_ok = false;
      else
        g_sh_ok = false;
      PspLog("geaudit: half-colour frames differ too much, GE %s disabled", kShName[k]);
     }
   }
  if (g_w2_frame) {
    g_w2_vn++;
    g_w2_vbad += bad;
    PspLog("w2rows f=%u winrows=%u cw=%u bad=%u", (unsigned)g_frame, (unsigned)g_win_rows, (unsigned)g_cw_rows,
           (unsigned)bad);
    if ((g_w2_vn >= 3 && g_w2_vbad / g_w2_vn > 600) || bad > 8192) {
      g_w2_ok = false;
      PspLog("geaudit: window-2 frames differ too much, GE window 2 disabled");
    }
  }
  if (g_ovf_rows) {
    g_ovf_vn++;
    g_ovf_vbad += bad;
    if (g_ovf_vn >= 3 && g_ovf_vbad / g_ovf_vn > 64) {
      g_ovf_ok = false;
      PspLog("geaudit: sprite-limit frames differ, GE sprite limit disabled");
    }
  }
  if (g_mos_frame) {
    int mb = MosAuditBright();
    PspLog("mosaudit f=%u size=%d bright=%d bad=%u", (unsigned)g_frame, g_mos_size, mb, (unsigned)bad);
    if (mb >= kMosMinBright) {  /* dim frames cannot tell a wrong source image from a right one: neither count nor judge them */
      g_mos_vn++;
      if (mb >= kMosHiBright)
        g_mos_vnhi++;
      g_mos_vbad += bad;
      if (g_mos_vn >= 3 && g_mos_vbad / g_mos_vn > 600) {
        g_mos_ok = false;
        PspLog("geaudit: mosaic frames differ too much, GE mosaic disabled");
      }
    }
  }
  if (g_nbband > 1) {
    g_bnd_vn++;
    g_bnd_vbad += bad;
    if (g_bnd_vn >= 3 && g_bnd_vbad / g_bnd_vn > 600) {
      g_band_ok = false;
      PspLog("geaudit: multi-band subscreen blend differs too much, disabled");
    }
  }
  PspLog("%s f=%u exact=%u near=%u bad=%u (mode=%d quads=%d tint=%d blend=%d bright=%u win=%u ovf=%d mos=%d img=%d m7rows=%d bands=%d prev=%u cw=%u) first bad (%d,%d) sw=%04x ge=%04x", (m7 || g_m7_img) ? "m7verify" : "geaudit",
         (unsigned)g_frame, (unsigned)exact, (unsigned)near, (unsigned)bad, g_frame_mode, g_m7_nq, g_ntint, g_nbruns,
         (unsigned)g_bright_rows, (unsigned)g_win_rows, g_ovf_rows, g_mos_frame ? g_mos_size : 0, g_m7_img ? 1 : 0,
         g_m7_rows, g_nbband, (unsigned)g_prev_rows, (unsigned)g_cw_rows, bx, by, bsw, bge);
  (void)bx; (void)by; (void)bsw; (void)bge;
  if (bad) {
    int shown = 0;
    for (int y = 0; y < g_line_count && shown < 8; y++)
      if (rowbad[y]) {
        shown++;
        if (m7) {
          uint32_t Y = (((uint32_t)g_m7c.m[3] * (uint32_t)(y + 1)) & ~63u) + g_m7_yc;
          (void)Y;
          PspLog("  badrow y=%d n=%u py=%u forced=%d bright=%d a=%d d=%d c=(%d,%d) s=(%d,%d)", y, (unsigned)rowbad[y], (unsigned)(Y >> 8),
                 g_lines[y].forced, g_lines[y].brightness, g_m7c.m[0], g_m7c.m[3], g_m7c.m[4], g_m7c.m[5], g_m7c.m[6], g_m7c.m[7]);
        } else {
          PspLog("  badrow y=%d n=%u forced=%d bright=%d cg0=%04x", y, (unsigned)rowbad[y], g_lines[y].forced, g_lines[y].brightness,
                 g_lines[y].cg0);
        }
      }
  }
  if (g_frame_multisub) {
    g_ms_vn++;
    g_ms_vbad += bad;
    if (g_ms_vn >= 3 && g_ms_vbad / g_ms_vn > 600) {
      g_multisub_ok = false;
      PspLog("geaudit: multi-layer subscreen differs too much, disabled");
    }
  }
  if (m7 && g_m7_vn >= 6 && g_m7_vbad / g_m7_vn > 600) {
    g_m7q_ok = false;
    PspLog("m7verify: too many differences, GE mode 7 tile quads disabled (image path takes over)");
  }
}

static void ComposeGeFrame(void);
static void ApplyMosaicTransition(void);
#if SM_PSP_ASYNC
static bool ComposeGeAsync(void);
#endif

static void Present(bool ge_scene) {
  uint32_t t = Now();
  g_build_us = t - g_t_list_start;
#if SM_PSP_ASYNC == 2
  /* One list: the compose commands follow the scene commands in the same list (ComposeGeAsync appends them while the
   * list is still open, or returns false having emitted nothing). */
  const bool queued = ge_scene && g_triple && !g_verify_now && g_mosaic_size <= 1 && ComposeGeAsync();
#endif
  sceGuFinish();
#if SM_PSP_ASYNC
  /* The scene list is now with the GE. A GE scene frame queues its compose list behind it (SM_PSP_ASYNC == 1) or has it
   * in the same list (== 2) and returns; GeWait() at the next EndDraw completes it. Frames that read the scene on the
   * CPU (verify, audit), mosaic frames and software-path frames stay synchronous, and so does any frame whose compose
   * commands cannot be queued (ComposeGeAsync returns false without touching the buffers): they continue below with
   * the scene list already finished. */
#if SM_PSP_ASYNC == 1
  const bool queued = ge_scene && g_triple && !g_verify_now && g_mosaic_size <= 1 && ComposeGeAsync();
#endif
  if (queued) {
    g_build_us = Now() - g_t_list_start;
    g_ge_pending = true;
    g_pend_off = g_draw_off;
    g_frames_async++;
    const int next = 3 - g_draw_i - g_prev_i;  /* as the synchronous path; the flip itself is GeWait() */
    g_prev_i = g_draw_i;
    g_draw_i = next;
    g_draw_off = kBufOff[next];
    return;
  }
#endif
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  g_gewait_us = Now() - t;
  PROF(g_gws_scene += g_gewait_us; g_gws_n++;)
  if (g_verify_now) {
    g_verify_now = false;
#if SM_PSP_DOOR_GE
    if (g_door_pal_mode != kDoorPalGame && g_door_pal_mode != kDoorPalCapture) {
      /* the software image uses the game's palette: a difference here is the transition, not a GE fault */
    } else
#endif
    {
      PROF(g_hit_verified = true;)
      GeVerify();
    }
  }
  if (ge_scene) {  /* the scene is complete in the draw buffer: scale it and place the HUD, then swap */
    uint32_t t_c = Now();
    ComposeGeFrame();
    g_gewait_us += Now() - t_c;
    PROF(g_gws_compose += Now() - t_c;)
  }
  if (g_mosaic_size > 1) {  /* the compose list is synced: the draw buffer is complete and idle */
    uint32_t t_m = Now();
    ApplyMosaicTransition();
    g_gewait_us += Now() - t_m;
    PROF(g_gws_mosaic += Now() - t_m;)
  }
  Pace();
  if (g_triple) {
    /* The queued buffer goes on screen at the next vblank. Pace() waited for a vblank since the previous swap only
     * if the game ran ahead, so draw the next frame into the one buffer that is neither queued nor on screen. */
    sceDisplaySetFrameBuf((void *)((uintptr_t)sceGeEdramGetAddr() + g_draw_off), 512, PSP_DISPLAY_PIXEL_FORMAT_565,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    int next = 3 - g_draw_i - g_prev_i;
    g_prev_i = g_draw_i;
    g_draw_i = next;
    g_draw_off = kBufOff[next];
  } else {
    sceGuSwapBuffers();
    g_draw_off ^= 0x44000;
  }
  g_vcount_last = sceDisplayGetVcount();
}

// Pixel values (bit i = value i, i >= 1) of palette pal whose colour is not black. 
static uint16_t PalNonBlackMask(int l, int pal) {
  int n = l < 2 ? 16 : 4, base = pal * n;
  uint16_t m = 0;
  for (int i = 1; i < n; i++)
    if (g_cgram[(base + i) & 0xff])
      m |= (uint16_t)(1u << i);
  return m;
}

#if SM_PSP_LOG
static uint32_t g_subbg_diag, g_subbg_frame, g_subbg_n;
#endif

/* Classifies layer l, shown on the subscreen over rows [r0,r1). A subscreen
 * pixel adds cgram[index] (ppu.c), so a tile only changes the picture if it uses
 * a pixel value whose colour is not black. BG3 HUD tiles (high priority, also on
 * the main screen with math off) are covered by main-screen BG3 pixels and never
 * matter. Returns -1 if the tile pool is full, 1 if the layer contributes (its
 * tile list is kept in *keep for the blend pass), 0 if not (list discarded). */
static int SubRunContributes(int l, const LineSnap *ls, int r0, int r1, bool all, TileList *keep) {
#if SM_PSP_LOG
  const BgSnap *s = &ls->bg[l];
#endif
  bool bpp4 = l < 2;
  int mark = g_ent_top;
  TileList lst;
  if (!BuildTileList(l, ls, r0, r1, &lst))
    return -1;
  if (all) {  /* gate 4t-c: target is cleared to the fixed colour, so every non-empty tile matters (black ones too) */
    if (lst.n == 0) {
      g_ent_top = mark;
      return 0;
    }
    *keep = lst;
    return 1;
  }
  bool harmless_hi = l == 2 && (ls->bg[2].flags & 4) && !(ls->mathmask & 4);  /* BG3 high is level 15 */
  uint16_t pm[8];
  uint8_t pm_ok = 0;
  int contrib = 0;
#if SM_PSP_LOG
  int shown = 0;
  struct { int tx, ty; uint16_t pt, mask, pmask; uint8_t pal, fl; } ex[4];
#endif
  for (int i = 0; i < lst.n; i++) {
    const TileEnt *e = &g_ents[lst.first + i];
    if (harmless_hi && (e->fl & 1))
      continue;
    if (!(pm_ok & (1 << e->pal))) {
      pm[e->pal] = PalNonBlackMask(l, e->pal);
      pm_ok |= (uint8_t)(1 << e->pal);
    }
    uint16_t used = (bpp4 ? g_mask4 : g_mask2)[e->pt];
    if (used & pm[e->pal]) {
#if SM_PSP_LOG
      if (shown < 4)
        ex[shown++] = (typeof(ex[0])){((e->x + s->hscroll) >> 3) & 63, ((e->y + 1 + s->vscroll) >> 3) & 63,
                                      e->pt, used, pm[e->pal], e->pal, e->fl};
#endif
      contrib++;
    }
  }
#if SM_PSP_LOG
  bool subbg_want = false;
  if (contrib) {
    subbg_want = g_subbg_diag < 3;
    if (!subbg_want && (g_frame % 3000) == 0) {
      if (g_subbg_frame != g_frame) { g_subbg_frame = g_frame; g_subbg_n = 0; }
      subbg_want = g_subbg_n++ < 4;
    }
  }
  if (subbg_want) {
    g_subbg_diag++;
    PspLog("SUBBG f=%u l=%d rows=%d-%d map=%04x tiles=%04x hs=%d vs=%d fl=%02x b3p=%d math=%02x nonempty=%d contrib=%d",
           (unsigned)g_frame, l, r0, r1, s->mapadr, s->tileadr, s->hscroll, s->vscroll, s->flags,
           ls->bg3prio, ls->mathmask, (int)lst.n, contrib);
    for (int i = 0; i < shown; i++)
      PspLog("  tile tx=%d ty=%d pt=%u pal=%u fl=%u used=%04x nonblack=%04x", ex[i].tx, ex[i].ty, ex[i].pt,
             ex[i].pal, ex[i].fl, ex[i].mask, ex[i].pmask);
  }
#endif
  if (contrib) {
    *keep = lst;
    return 1;
  }
  g_ent_top = mark;
  return 0;
}

static void CountReasons(int mask) {
  g_last_reason = mask;
  for (int i = 0; i < kReasonCount; i++)
    if (mask & (1 << i))
      g_reason_counts[i]++;
}

/* Depth levels (ppu.c z values, level * kZLevel): BG3 low 1, OBJ prio0 2, OBJ1 6, BG2 low 7, BG1 low 8,
 * OBJ2 10, BG2 high 11, BG1 high 12, OBJ3 14, BG3 high 15. ppu.c always uses 15 for BG3 high (it never
 * looks at the BG3-priority bit). Sprites: (prio*4+2)*kZLevel + 127-k, k = precedence index. */

enum { kVtTile = GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
       kVtBg = (SM_PSP_VTX16 ? GU_TEXTURE_16BIT : GU_TEXTURE_32BITF) | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
       kVtColor = GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
       kVtColorF = GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
       kVtTexF = GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D };

static inline void GeDraw(int vtype, int n, const void *v) {
  PROF(g_c_draws++; g_c_verts += (uint32_t)n;)
  sceGuDrawArray(GU_SPRITES, vtype, n, NULL, v);
}

enum { kPspButtonIconSlots = 8 };

typedef struct ButtonOverlay {
  const uint32_t *pixels;
  int tex_w, tex_h;
  int x, y, w, h;
  unsigned effects;
} ButtonOverlay;

static ButtonOverlay g_button_overlays[kPspButtonIconSlots];

void PspRenderer_ClearButtonIcon(unsigned slot) {
  if (slot < kPspButtonIconSlots)
    memset(&g_button_overlays[slot], 0, sizeof(g_button_overlays[slot]));
}

void PspRenderer_ClearButtonIcons(void) {
  memset(g_button_overlays, 0, sizeof(g_button_overlays));
}

void PspRenderer_SetButtonIconEx(unsigned slot, enum PspButtonIcon icon,
                                 int x, int y, int w, int h,
                                 unsigned effects) {
  if (slot >= kPspButtonIconSlots || x < 0 || y < 0 ||
      w <= 0 || h <= 0 || w > 256 || h > 224 ||
      x > 256 - w || y > 224 - h ||
      (effects & ~(unsigned)PSP_BUTTON_ICON_EFFECT_CLIP_BG2_WINDOW) != 0) {
    if (slot < kPspButtonIconSlots)
      PspRenderer_ClearButtonIcon(slot);
    return;
  }

  const uint32_t *pixels;
  int tex_w = 16, tex_h = 16;
  switch (icon) {
  case PSP_BUTTON_ICON_CIRCLE:    pixels = g_psp_button_circle; break;
  case PSP_BUTTON_ICON_CROSS:     pixels = g_psp_button_cross; break;
  case PSP_BUTTON_ICON_SQUARE:    pixels = g_psp_button_square; break;
  case PSP_BUTTON_ICON_TRIANGLE:  pixels = g_psp_button_triangle; break;
  case PSP_BUTTON_ICON_L:         pixels = g_psp_button_l; tex_w = 32; break;
  case PSP_BUTTON_ICON_R:         pixels = g_psp_button_r; tex_w = 32; break;
  case PSP_BUTTON_ICON_SELECT:    pixels = g_psp_button_select; tex_w = 32; break;
  case PSP_BUTTON_ICON_DPAD_UD:   pixels = g_psp_button_dpad_ud; break;
  case PSP_BUTTON_ICON_DPAD_LR:   pixels = g_psp_button_dpad_lr; break;
  case PSP_BUTTON_ICON_DPAD_LRUD: pixels = g_psp_button_dpad_lrud; break;
  default:
    PspRenderer_ClearButtonIcon(slot);
    return;
  }

  g_button_overlays[slot] = (ButtonOverlay){pixels, tex_w, tex_h, x, y, w, h, effects};
}

void PspRenderer_SetButtonIcon(unsigned slot, enum PspButtonIcon icon,
                               int x, int y, int w, int h) {
  PspRenderer_SetButtonIconEx(slot, icon, x, y, w, h,
                              PSP_BUTTON_ICON_EFFECT_NONE);
}

// Draw state of overlay b on source line y. False: not drawn there (forced blank, brightness 0, or the clip layer, BG2, is not drawn on that line, so its window state is meaningless). wm/wl/wr: BG2 window state as WinSpans takes it.
static bool OverlayLine(const ButtonOverlay *b, int y, int *bright, int *wm, int *wl, int *wr) {
  const LineSnap *ls = &g_lines[y];
  *bright = ls->brightness;
  *wm = *wl = *wr = 0;
  if (ls->forced || *bright == 0)
    return false;
  if (b->effects & PSP_BUTTON_ICON_EFFECT_CLIP_BG2_WINDOW) {
    const BgSnap *w = &ls->bg[1];
    if (!(w->flags & 4))
      return false;
    *wm = w->wm;
    *wl = w->wl;
    *wr = w->wr;
  }
  return true;
}

/* ---- Final composition: one post-pass for the GE scene and for the software frame ------------------------------- */
typedef struct ComposeSrc { const void *ptr; int psm, w, h, stride; } ComposeSrc;

/* Original HUD elements as rendered (BG3 pixels, nearest, 1:1) and where they go in the 60 px side panels.
 * x, w: source columns (tilemap column * 8); bg0, h: BG3 pixel rows (hud_tilemap row 0 = BG row 8); px, py: panel position.
 * Left panel: energy, [auto-reserve icon], missiles, super missiles, power bombs, grapple. Right panel: minimap (with its
 * frame), [speedbooster icon], x-ray visor. The two bracketed icons are the new artwork below, not game pixels. */
typedef struct HudRect { int16_t x, w, bg0, h, px, py; } HudRect;
#ifndef SM_PSP_HUD_MAP_FRAME
#define SM_PSP_HUD_MAP_FRAME 1  /* 0: minimap without its frame (40 px, as the fullscreen.md table) */
#endif
enum {
  kHudResvX = 14, kHudResvY = 34,      /* auto-reserve icon 32x16: under the energy block */
  kHudSpeedX = 431, kHudSpeedY = 245,  /* speedbooster icon 16x16: right panel */
};
static const HudRect kHudRects[] = {
  {   8, 56,  8, 24,   2,   8 },  /* energy tanks, label, digits (cols 1-7); the label is x 8-40 = screen x 2-34 */
  {  80, 24,  8, 24,  18, 238 },  /* missiles (cols 10-12) */
  { 112, 16,  8, 24,  22, 209 },  /* super missiles (cols 14-15) */
  { 136, 16,  8, 24,  22, 180 },  /* power bombs (cols 17-18) */
  { 160, 16,  8, 16,  22, 158 },  /* grapple beam (cols 20-21) */
#if SM_PSP_HUD_MAP_FRAME
  { 208, 48,  0, 32, 430,   4 },  /* minimap: cols 26-30 plus its frame: BG tile row 0 ($2c1d/$2c1c) and col 31 ($2c1e), non-blank tiles in kHudTilemaps */
#else
  { 208, 40,  8, 24, 430,   4 },  /* minimap exactly as the fullscreen.md table */
#endif
  { 184, 16,  8, 16, 453, 245 },  /* x-ray visor (cols 23-24), below the speedbooster icon */
};
#define kHudRectN ((int)(sizeof(kHudRects) / sizeof(kHudRects[0])))

/* Auto-reserve state from the glyph the game put in hud_tilemap[8] (HandleHudTilemap, sm_80.c): tile $33 is shown only in
 * auto mode; palette 7 ($3c33...) is used while the reserve tank has energy, palette 3 ($2c33...) while it is empty.
 * 0: not shown, 1: auto, reserve empty (deactivated icon), 2: auto, reserve has energy (activated icon). */
static int HudReserveState(void) {
  const unsigned w = g_hud.rsv;
  if ((w & 0x3ff) != 0x33)
    return 0;
  return ((w >> 10) & 7) == 7 ? 2 : 1;
}

/* Speedbooster state: 0 hidden (Speed Booster not equipped in the pause item menu, which also covers "not collected"),
 * 1 equipped but the run/dash toggle is off (unselected icon), 2 equipped and the toggle is on, so the boost can be used
 * (selected icon). equipped_items is $7E:09A2 (variables.h), the Speed Booster is bit $2000 (Samus_HandleExtraRunspeedX,
 * sm_90.c). The toggle is Samus_IsRunButtonActive() (sm_91.c): true only in run mode, false when off or in x-ray mode. */
extern uint8_t g_ram[0x20000];
extern uint8_t Samus_IsRunButtonActive(void);
static int HudSpeedState(void) {
  const unsigned eq = g_ram[0x9a2] | (unsigned)g_ram[0x9a3] << 8;
  if (!(eq & 0x2000))
    return 0;
  return Samus_IsRunButtonActive() ? 2 : 1;
}

/* Sprite (two vertices) whose pixel i of j samples the source at (su0 + i * stepx, sv0 + j * stepy), the steps chosen so that
 * the first and last pixel hit su0/su1 and sv0/sv1 (texel centres). Samples never leave [su0, su1] x [sv0, sv1], so bilinear
 * filtering stays inside the rows/columns the caller names. g_uvh: where the GE samples inside a pixel (boot self test). */
static void ComposeQuad(TileVert *w, float su0, float sv0, float su1, float sv1, int dx, int dy, int dw, int dh) {
  const float sx = dw > 1 ? (su1 - su0) / (float)(dw - 1) : 0.0f;
  const float sy = dh > 1 ? (sv1 - sv0) / (float)(dh - 1) : 0.0f;
  const float u0 = su0 - g_uvh[0] * sx, v0 = sv0 - g_uvh[1] * sy;
  w[0] = (TileVert){u0, v0, (int16_t)dx, (int16_t)dy, 0};
  w[1] = (TileVert){u0 + (float)dw * sx, v0 + (float)dh * sy, (int16_t)(dx + dw), (int16_t)(dy + dh), 0};
}

/* Number of sprites ComposeQuadStrips writes for a dw px wide destination. */
#define COMPOSE_STRIPS(dw) ((SM_PSP_COMPOSE_STRIP) > 0 ? ((dw) + (SM_PSP_COMPOSE_STRIP) - 1) / (SM_PSP_COMPOSE_STRIP) : 1)
enum { kComposeStrips = COMPOSE_STRIPS(kCompW) };

/* ComposeQuad split into columns of SM_PSP_COMPOSE_STRIP destination pixels. Pixel i of the whole quad samples the same
 * (su0 + i * stepx, sv0 + j * stepy) as ComposeQuad gives it, so the picture is unchanged; only the GE's texture cache sees
 * narrower sprites. Writes 2 * COMPOSE_STRIPS(dw) vertices, returns how many. */
static int ComposeQuadStrips(TileVert *w, float su0, float sv0, float su1, float sv1, int dx, int dy, int dw, int dh) {
  const int strip = SM_PSP_COMPOSE_STRIP;
  if (strip <= 0 || dw <= strip) {
    ComposeQuad(w, su0, sv0, su1, sv1, dx, dy, dw, dh);
    return 2;
  }
  const float sx = dw > 1 ? (su1 - su0) / (float)(dw - 1) : 0.0f;
  const float sy = dh > 1 ? (sv1 - sv0) / (float)(dh - 1) : 0.0f;
  const float v0 = sv0 - g_uvh[1] * sy, v1 = v0 + (float)dh * sy;
  const int n = (dw + strip - 1) / strip;
  for (int k = 0; k < n; k++) {
    const int i0 = k * strip, cw = dw - i0 < strip ? dw - i0 : strip;
    const float u0 = su0 + ((float)i0 - g_uvh[0]) * sx;
    w[2 * k] = (TileVert){u0, v0, (int16_t)(dx + i0), (int16_t)dy, 0};
    w[2 * k + 1] = (TileVert){u0 + (float)cw * sx, v1, (int16_t)(dx + i0 + cw), (int16_t)(dy + dh), 0};
  }
  return 2 * n;
}

#if SM_PSP_DOOR_GE
// Old gameplay image (256x192 RGB565 in slot 0) drawn over the live new room. While armed it is displaced by the opposite
// of the game camera's movement since the scroll started, so it stays glued to the new room whatever the easing. The rect
// is clipped on the CPU to the gameplay viewport (no scissor needed); the image maps 256x192 -> 360x270 exactly like the
// normal composition. Where the displaced image no longer covers the old room (the 32 source rows the HUD hides on an up
// door, or the camera offset across the door) its nearest edge row/column is repeated, so tubes and walls continue.
#ifndef SM_PSP_DOOR_UP_LIVE
#define SM_PSP_DOOR_UP_LIVE 1  /* 1 = upward doors: no edge repeat above the old image, the live new room shows there (diagnostic) */
#endif

static int DoorDispPx(int src_px) {  /* source pixels -> viewport pixels (360/256 = 270/192 = 45/32), rounded */
  return (src_px * 45 + (src_px >= 0 ? 16 : -16)) / 32;
}

static void DoorQuad(float su0, float sv0, float su1, float sv1, int x0, int y0, int x1, int y1) {
  if (x1 <= x0 || y1 <= y0)
    return;
  TileVert *v = sceGuGetMemory(2 * (int)sizeof(TileVert));
  ComposeQuad(v, su0, sv0, su1, sv1, x0, y0, x1 - x0, y1 - y0);
  GeDraw(kVtTile, 2, v);
}

static void DrawDoorTransition(const ComposeSrc *s) {
  enum { kDoorRows = 192, kHiddenRows = 45 /* 32 source rows */ };
  const int sdx = g_door_armed ? -(int)(int16_t)(uint16_t)(g_door_cam_x - g_door_ref_x) : 0;
  const int sdy = g_door_armed ? -(int)(int16_t)(uint16_t)(g_door_cam_y - g_door_ref_y) : 0;
  const int dx = kCompX + DoorDispPx(sdx), dy = kCompY + DoorDispPx(sdy);  /* where the old image's top-left corner is */
  const int vx0 = kCompX, vx1 = kCompX + kCompW, vy0 = kCompY, vy1 = kCompY + kCompH;
  const int x0 = dx > vx0 ? dx : vx0, x1 = dx + kCompW < vx1 ? dx + kCompW : vx1;
  const int y0 = dy > vy0 ? dy : vy0, y1 = dy + kCompH < vy1 ? dy + kCompH : vy1;
  const float ku = (float)(kGameWidth - 1) / (float)(kCompW - 1), kv = (float)(kDoorRows - 1) / (float)(kCompH - 1);
  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
  sceGuTexImage(0, 256, 256, 256, g_img_tex[0]);
  sceGuTexFlush();
  if (x1 > x0 && y1 > y0) {
    const float u0 = 0.5f + (float)(x0 - dx) * ku, u1 = 0.5f + (float)(x1 - 1 - dx) * ku;
    const float v0 = 0.5f + (float)(y0 - dy) * kv, v1 = 0.5f + (float)(y1 - 1 - dy) * kv;
    DoorQuad(u0, v0, u1, v1, x0, y0, x1, y1);
    if (g_door_armed) {
      const bool horiz = g_door_direction < 2;  /* horizontal door: the camera offset across it is vertical */
      const float top = 0.5f, bottom = (float)kDoorRows - 0.5f, left = 0.5f, right = (float)kGameWidth - 0.5f;
      if (dy > vy0 && g_door_direction != 2 && !(SM_PSP_DOOR_UP_LIVE && g_door_direction == 3)) {  /* above the image: row 0 repeated */
        int h = dy - vy0;
        if (!horiz && h > kHiddenRows)
          h = kHiddenRows;
        DoorQuad(u0, top, u1, top, x0, dy - h, x1, dy);
      }
      if (horiz && dy + kCompH < vy1)  /* below the image */
        DoorQuad(u0, bottom, u1, bottom, x0, dy + kCompH, x1, vy1);
      if (!horiz) {
        if (dx > vx0)  /* left of the image */
          DoorQuad(left, v0, left, v1, vx0, y0, dx, y1);
        if (dx + kCompW < vx1)  /* right of the image */
          DoorQuad(right, v0, right, v1, dx + kCompW, y0, vx1, y1);
      }
    }
  }
  /* the HUD pass samples the composition source again */
  sceGuTexMode(s->psm, 0, 0, GU_FALSE);
  sceGuTexImage(0, s->w, s->h, s->stride, s->ptr);
  sceGuTexFlush();
}
#endif

#if SM_PSP_DOOR_SNAP
/* The two snapshots (slot 0 old room, slot 1 new room; 256x192 valid rows) slid over the gameplay viewport. Each image is
 * mapped 256x192 -> 360x270 exactly like the normal composition and displaced by DsOffsets(). The image that leads (left /
 * upper: old for right and down doors, new for left and up doors) owns the viewport up to its trailing edge, the other image
 * owns the rest, so there is no seam or gap whatever the rounding. Where an image does not reach a pixel of its region (the
 * camera offset across the door, and the 32 source rows the HUD hides on vertical doors) its nearest edge row/column is
 * repeated by drawing constant-coordinate quads, so tubes and walls continue. The 32 source rows the HUD hides are real picture
 * for the lower room of a vertical door (g_ds_top: software-PPU rows in slot rows 192..223, drawn in the gap above its image); without
 * them (software PPU unavailable) the edge row is repeated there. Only rows 0..191 and, with g_ds_top, 192..223 are ever sampled.
 * The rectangles are clipped on the CPU (no scissor state is touched). */
static void DsQuad(float su0, float sv0, float su1, float sv1, int x0, int y0, int x1, int y1) {
  TileVert *v = sceGuGetMemory(2 * (int)sizeof(TileVert));
  ComposeQuad(v, su0, sv0, su1, sv1, x0, y0, x1 - x0, y1 - y0);
  GeDraw(kVtTile, 2, v);
}

static int DsClamp(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/* Draws the part [rx0, rx1) x [ry0, ry1) of the viewport from the image tex whose top-left corner is at (dx, dy). With `top` the
 * image also has the 32 rows above its row 0 (slot rows 192..223): they fill the 45 viewport rows above dy, and the edge row
 * replicated above that is the first of them. */
static void DsImage(const void *tex, int dx, int dy, int rx0, int ry0, int rx1, int ry1, bool top) {
  enum { kRows = 192 };
  if (rx1 <= rx0 || ry1 <= ry0)
    return;
  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
  sceGuTexImage(0, 256, 256, 256, tex);
  sceGuTexFlush();
  const float ku = (float)(kGameWidth - 1) / (float)(kCompW - 1), kv = (float)(kRows - 1) / (float)(kCompH - 1);
  const int tb = top ? DsDispPx(kDsTopRows) : 0;  /* viewport rows of the band above the image */
  const float kvt = top ? (float)(kDsTopRows - 1) / (float)(tb - 1) : 0.0f;
  const float vt0 = (float)kDsTopRow0 + 0.5f;  /* centre of the band's first row */
  const int xs[4] = {rx0, DsClamp(dx, rx0, rx1), DsClamp(dx + kCompW, rx0, rx1), rx1};
  const int ys[5] = {ry0, DsClamp(dy - tb, ry0, ry1), DsClamp(dy, ry0, ry1), DsClamp(dy + kCompH, ry0, ry1), ry1};
  for (int j = 0; j < 4; j++) {
    const int y0 = ys[j], y1 = ys[j + 1];
    if (y1 <= y0)
      continue;
    float v0, v1;
    if (j == 0) {  /* above everything: the nearest edge row */
      v0 = v1 = top ? vt0 : 0.5f;
    } else if (j == 1) {  /* the rows under the HUD (empty unless top) */
      v0 = vt0 + (float)(y0 - (dy - tb)) * kvt;
      v1 = vt0 + (float)(y1 - 1 - (dy - tb)) * kvt;
    } else if (j == 2) {
      v0 = 0.5f + (float)(y0 - dy) * kv;
      v1 = 0.5f + (float)(y1 - 1 - dy) * kv;
    } else {
      v0 = v1 = (float)kRows - 0.5f;
    }
    for (int i = 0; i < 3; i++) {
      const int x0 = xs[i], x1 = xs[i + 1];
      if (x1 <= x0)
        continue;
      const float u0 = i == 0 ? 0.5f : i == 2 ? (float)kGameWidth - 0.5f : 0.5f + (float)(x0 - dx) * ku;
      const float u1 = i == 0 ? 0.5f : i == 2 ? (float)kGameWidth - 0.5f : 0.5f + (float)(x1 - 1 - dx) * ku;
      DsQuad(u0, v0, u1, v1, x0, y0, x1, y1);
    }
  }
}

static void DrawDoorSnap(const ComposeSrc *s) {
  const int step = g_ds_state == kDsPlay ? (int)g_ds_step : 0;  /* until the new snapshot exists the old one stays put */
  int o[4];
  DsOffsetsN(g_ds_dir, g_ds_n, step, g_ds_align, o);
  const int vx0 = kCompX, vx1 = kCompX + kCompW, vy0 = kCompY, vy1 = kCompY + kCompH;
  const int oldx = kCompX + DsDispPx(o[0]), oldy = kCompY + DsDispPx(o[1]);
  const int newx = kCompX + DsDispPx(o[2]), newy = kCompY + DsDispPx(o[3]);
  const void *const tex_old = g_img_tex[0], *const tex_new = g_img_tex[1];
  const bool top_old = g_ds_top[0], top_new = g_ds_top[1];
  if (g_ds_state != kDsPlay) {  /* waiting for the new snapshot: the old image alone covers the screen */
    DsImage(tex_old, oldx, oldy, vx0, vy0, vx1, vy1, top_old);
  } else if (!(g_ds_dir & 2)) {  /* horizontal door: the leading image is on the left */
    const bool old_first = g_ds_dir == 0;
    const int seam = DsClamp((old_first ? oldx : newx) + kCompW, vx0, vx1);
    if (old_first) {
      DsImage(tex_old, oldx, oldy, vx0, vy0, seam, vy1, top_old);
      DsImage(tex_new, newx, newy, seam, vy0, vx1, vy1, top_new);
    } else {
      DsImage(tex_new, newx, newy, vx0, vy0, seam, vy1, top_new);
      DsImage(tex_old, oldx, oldy, seam, vy0, vx1, vy1, top_old);
    }
  } else {  /* vertical door: the leading image is on top */
    const bool old_first = g_ds_dir == 2;
    const int seam = DsClamp((old_first ? oldy : newy) + kCompH, vy0, vy1);
    if (old_first) {
      DsImage(tex_old, oldx, oldy, vx0, vy0, vx1, seam, top_old);
      DsImage(tex_new, newx, newy, vx0, seam, vx1, vy1, top_new);
    } else {
      DsImage(tex_new, newx, newy, vx0, vy0, vx1, seam, top_new);
      DsImage(tex_old, oldx, oldy, vx0, seam, vx1, vy1, top_old);
    }
  }
  /* the HUD pass samples the composition source again */
  sceGuTexMode(s->psm, 0, 0, GU_FALSE);
  sceGuTexImage(0, s->w, s->h, s->stride, s->ptr);
  sceGuTexFlush();
}

/* Display-list use of the snapshot code (compose list, PSP_ASYNC=0 only). Every libgu call is one or a few 4 byte commands
 * (pspsdk/gu sources): sceGuTexMode 2 + flush 1, sceGuTexImage 3 + flush 1, sceGuTexFlush 1, sceGuDrawArray 4 (vertex type,
 * base, address, prim), sceGuCopyImage 8, sceGuTexSync 1; sceGuGetMemory(n) takes n rounded up to 4, plus an 8 byte jump.
 * DrawDoorSnap changes only the texture mode / image (restored at the end) and draws sprites: no other GE state.
 *   a DsImage call: <= 4 x 3 = 12 quads (9 without the rows under the HUD); only the lower room of a vertical door has them, so
 *   DrawDoorSnap makes <= 12 + 9 = 21 (one image while the new snapshot is awaited)
 *   per image TexMode + TexImage + TexFlush = 12 + 16 + 4 = 32 B; per quad GuMem(2 TileVert) + 16 B draw; restore 32 B;
 *   capture (CopyImage + TexSync) 32 + 4 B, plus the rows under the HUD (a second CopyImage) 32 B = 68 B. Calls: 2 x 3 + 21 + 3 + 3 = 33.
 *   => exactly <= 2*32 + 21*(40 + 16) + 32 + 68 = 1340 B (tools/test_door_snap_draw.sh measures it with these costs).
 * The compose bound (ComposeBytesBound) charges the generous kGuCallMax per call instead: kDsMaxCalls calls + kDsMaxMem. */
enum { kDsMaxQuads = 21, kDsMaxCalls = 33, kDsExactBytes = 1340,
       kDsMaxMem = kDsMaxQuads * (((2 * (int)sizeof(TileVert) + 3) / 4) * 4 + 8) };
#endif

/* ---- Native message overlay ---------------------------------------------------------------------------------
 * Drawn last in ComposeDraw, so the GE and software paths share it. Text: psp_message_text.h (from messages.txt),
 * glyph metrics: psp_message_font.h. Textures are RGBA8888 in RAM, written back once (ComposeDraw), never modified.
 * The dim covers the gameplay rectangle only; the HUD side panels are not touched. */
extern const uint8_t psp_msg_font_rgba[], psp_msg_grapple_rgba[], psp_msg_missile_rgba[], psp_msg_powerbomb_rgba[],
    psp_msg_supermissile_rgba[], psp_msg_xray_rgba[];

enum { kMsgPassGlyphs, kMsgPassIcons, kMsgPassMeasure,
       kMsgPassCountGlyphs,  /* *nv += vertices kMsgPassGlyphs would write (same predicate, nothing written) */
       kMsgPassCountIcons,   /* *nv += icon draws kMsgPassIcons would make */
       kMsgMaxLines = 12, kMsgMaxW = 320, kMsgIconH = 16, kMsgIconPad = 2, kMsgDimAlpha = 0xa0 };
static const float kMsgTitleScale = 0.5f, kMsgBodyScale = 0.25f,
                   kMsgTextScale = 0.5f, kMsgOptionScale = 0.5f,
                   kMsgTitleGap = 6.0f, kMsgOptGap = 12.0f;

static uint8_t g_msg_id, g_msg_sel, g_msg_fade;
static bool g_msg_dim_hold;
static const uint32_t *g_msg_btn_px;
static int g_msg_btn_tw, g_msg_btn_th, g_msg_btn_iw, g_msg_btn_ih;
static const char *g_msg_btn_label;

void PspRenderer_SetMessageBox(unsigned id, unsigned selection) {
  g_msg_id = (id >= 1 && id <= 29) ? (uint8_t)id : 0;
  g_msg_sel = selection == 2 ? 2 : 0;
  g_msg_fade = 0;
  g_msg_btn_px = NULL;
  g_msg_btn_tw = g_msg_btn_th = g_msg_btn_iw = g_msg_btn_ih = 0;
  g_msg_btn_label = NULL;
}

void PspRenderer_SetMessageBoxSelection(unsigned selection) {
  g_msg_sel = selection == 2 ? 2 : 0;
}

void PspRenderer_SetMessageFade(unsigned fade) {
  g_msg_fade = fade > 255 ? 255 : (uint8_t)fade;
}

void PspRenderer_SetMessageDimHold(bool hold) {
  g_msg_dim_hold = hold;
}

void PspRenderer_ClearMessageBox(void) {
  g_msg_id = g_msg_sel = g_msg_fade = 0;
  g_msg_btn_px = NULL;
  g_msg_btn_tw = g_msg_btn_th = g_msg_btn_iw = g_msg_btn_ih = 0;
  g_msg_btn_label = NULL;
}

void PspRenderer_SetMessageButton(int icon, const char *label) {
  g_msg_btn_px = NULL;
  g_msg_btn_tw = g_msg_btn_th = g_msg_btn_iw = g_msg_btn_ih = 0;
  g_msg_btn_label = NULL;
  switch (icon) {
  case PSP_BUTTON_ICON_CIRCLE:   g_msg_btn_px = g_psp_button_circle; g_msg_btn_tw = g_msg_btn_th = g_msg_btn_iw = g_msg_btn_ih = 16; break;
  case PSP_BUTTON_ICON_CROSS:    g_msg_btn_px = g_psp_button_cross; g_msg_btn_tw = g_msg_btn_th = g_msg_btn_iw = g_msg_btn_ih = 16; break;
  case PSP_BUTTON_ICON_SQUARE:   g_msg_btn_px = g_psp_button_square; g_msg_btn_tw = g_msg_btn_th = g_msg_btn_iw = g_msg_btn_ih = 16; break;
  case PSP_BUTTON_ICON_TRIANGLE: g_msg_btn_px = g_psp_button_triangle; g_msg_btn_tw = g_msg_btn_th = g_msg_btn_iw = g_msg_btn_ih = 16; break;
  case PSP_BUTTON_ICON_L:       g_msg_btn_px = g_psp_button_l; g_msg_btn_tw = g_msg_btn_iw = 32; g_msg_btn_th = g_msg_btn_ih = 16; break;
  case PSP_BUTTON_ICON_R:       g_msg_btn_px = g_psp_button_r; g_msg_btn_tw = g_msg_btn_iw = 32; g_msg_btn_th = g_msg_btn_ih = 16; break;
  case PSP_BUTTON_ICON_SELECT:  g_msg_btn_px = g_psp_button_select; g_msg_btn_tw = g_msg_btn_iw = 32; g_msg_btn_th = g_msg_btn_ih = 16; break;
  default:
    if (label && label[0])
      g_msg_btn_label = label;
    break;
  }
}

typedef struct MsgTok { const void *px; int tw, th, iw, ih; const char *label; } MsgTok;
static const struct { const char *name; const void *px; int tw, th, iw, ih; } kMsgItem[] = {
  {"item:missile",       psp_msg_missile_rgba,      32, 16, 24, 16},
  {"item:super_missile", psp_msg_supermissile_rgba, 16, 16, 16, 16},
  {"item:power_bomb",    psp_msg_powerbomb_rgba,    16, 16, 16, 16},
  {"item:grapple_beam",  psp_msg_grapple_rgba,      16, 16, 16, 16},
  {"item:xray_scope",    psp_msg_xray_rgba,         16, 16, 16, 16},
  {"item:speedbooster",  g_hud_ico_speed_sel,       16, 16, 16, 16},
};

static bool MsgTokIs(const char *p, int n, const char *s) {
  return (int)strlen(s) == n && memcmp(p, s, (size_t)n) == 0;
}

static bool MsgResolve(const char *p, int n, MsgTok *t) {
  memset(t, 0, sizeof(*t));
  for (unsigned i = 0; i < sizeof(kMsgItem) / sizeof(kMsgItem[0]); i++)
    if (MsgTokIs(p, n, kMsgItem[i].name)) {
      *t = (MsgTok){kMsgItem[i].px, kMsgItem[i].tw, kMsgItem[i].th, kMsgItem[i].iw, kMsgItem[i].ih, NULL};
      return true;
    }
  if (MsgTokIs(p, n, "button:shoot") || MsgTokIs(p, n, "button:run")) {  /* each message has one button: the game's current binding */
    if (g_msg_btn_px) {
      *t = (MsgTok){g_msg_btn_px, g_msg_btn_tw, g_msg_btn_th, g_msg_btn_iw, g_msg_btn_ih, NULL};
      return true;
    }
    if (g_msg_btn_label) {
      t->label = g_msg_btn_label;
      return true;
    }
  }
  return false;
}

// Sprite over dest rect (dx,dy,dw,dh) showing source rect (su,sv,sw,sh), texel-centre mapping as in ComposeQuad: the first and last pixel sample the centres of the first and last texel (su + 0.5, su + sw - 0.5), so bilinear filtering never reads outside the rect (the missile art is 24 px inside a 32 px texture). g_uvh: where the GE samples in a pixel. 
static void MsgQuad(TileVert *w, float su, float sv, float sw, float sh, int dx, int dy, int dw, int dh) {
  const float stx = dw > 1 ? (sw - 1.0f) / (float)(dw - 1) : 0.0f;
  const float sty = dh > 1 ? (sh - 1.0f) / (float)(dh - 1) : 0.0f;
  const float u0 = su + 0.5f - g_uvh[0] * stx;
  const float v0 = sv + 0.5f - g_uvh[1] * sty;
  w[0] = (TileVert){u0, v0, (int16_t)dx, (int16_t)dy, 0};
  w[1] = (TileVert){u0 + (float)dw * stx, v0 + (float)dh * sty,
                    (int16_t)(dx + dw), (int16_t)(dy + dh), 0};
}

static void MsgDrawImg(const MsgTok *t, int x, int y, int w, int h, unsigned a) {
  sceGuColor(((uint32_t)a << 24) | 0x00ffffffu);
  sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
  sceGuTexImage(0, t->tw, t->th, t->tw, t->px);
  sceGuTexFlush();
  TileVert *v = sceGuGetMemory(2 * (int)sizeof(TileVert));
  MsgQuad(v, 0.0f, 0.0f, (float)t->iw, (float)t->ih, x, y, w, h);
  GeDraw(kVtTile, 2, v);
}

/* One walker for all three jobs (same advance maths, so measure and draw always agree): glyph quads into gv, icon
 * draws, or width only. A run holds plain text and {tokens}; a label token expands to its glyphs. */
static void MsgWalk(const char *s, const char *e, float *cx, float y, float sc, int pass, TileVert *gv, int *nv,
                    unsigned a) {
  for (const char *p = s; p < e;) {
    if (*p == '{') {
      const char *c = memchr(p, '}', (size_t)(e - p));
      if (c) {
        MsgTok t;
        if (MsgResolve(p + 1, (int)(c - p - 1), &t)) {
          if (t.label) {
            MsgWalk(t.label, t.label + strlen(t.label), cx, y, sc, pass, gv, nv, a);
          } else {
            const int w = t.iw * kMsgIconH / t.ih;
            if (pass == kMsgPassIcons)
              MsgDrawImg(&t, (int)(*cx + 0.5f) + kMsgIconPad, (int)(y + 30.5f * sc) - kMsgIconH / 2, w, kMsgIconH, a);
            else if (pass == kMsgPassCountIcons)
              ++*nv;
            *cx += (float)(w + 2 * kMsgIconPad);
          }
        }
        p = c + 1;
        continue;
      }
    }
    unsigned char ch = (unsigned char)*p++;
    if (ch < 32 || ch > 126)
      ch = '?';
    const PspMsgGlyph *g = &g_psp_msg_glyphs[ch - 32];
    if (pass == kMsgPassGlyphs && ch != ' ' && g->w && g->h) {
      int dw = (int)((float)g->w * sc + 0.5f), dh = (int)((float)g->h * sc + 0.5f);
      if (dw < 1) dw = 1;
      if (dh < 1) dh = 1;
      const int baseline = (int)(y + (float)PSP_MSG_FONT_BASE * sc + 0.5f);
      const int gy = baseline - (int)((float)(PSP_MSG_FONT_BASE - g->yoff) * sc + 0.5f);
      MsgQuad(&gv[*nv], (float)g->x, (float)g->y, (float)g->w, (float)g->h,
              (int)(*cx + (float)g->xoff * sc + 0.5f), gy, dw, dh);
      *nv += 2;
    } else if (pass == kMsgPassCountGlyphs && ch != ' ' && g->w && g->h) {
      *nv += 2;
    }
    *cx += (float)g->advance * sc;
  }
}

static float MsgWidth(const char *s, const char *e, float sc) {
  float w = 0.0f;
  MsgWalk(s, e, &w, 0.0f, sc, kMsgPassMeasure, NULL, NULL, 0);
  return w;
}

static void MsgDrawRun(const char *s, const char *e, float x, float y, float sc, uint32_t rgb, unsigned a) {
  if (e <= s)
    return;
  /* A label token expands to more glyphs than its source text, so the vertex buffer is sized by counting with the walker
   * that writes it (kMsgPassCountGlyphs: same predicate, nothing written). */
  int ng = 0;
  float cx = x;
  MsgWalk(s, e, &cx, y, sc, kMsgPassCountGlyphs, NULL, &ng, a);
  if (ng) {
    TileVert *gv = sceGuGetMemory(ng * (int)sizeof(TileVert));
    int nv = 0;
    cx = x;
    MsgWalk(s, e, &cx, y, sc, kMsgPassGlyphs, gv, &nv, a);
    sceGuColor(((uint32_t)a << 24) | rgb);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, PSP_MSG_FONT_W, PSP_MSG_FONT_H, PSP_MSG_FONT_W, psp_msg_font_rgba);
    sceGuTexFlush();
    GeDraw(kVtTile, nv, gv);
  }
  cx = x;
  MsgWalk(s, e, &cx, y, sc, kMsgPassIcons, NULL, NULL, a);
}

typedef struct MsgLine { const char *s, *e; float scale; bool gap; } MsgLine;

/* Greedy word wrap of every message line to kMsgMaxW. Returns the line count. */
static int MsgLayout(const PspMessageText *m, MsgLine *out) {
  int n = 0;
  for (int i = 0; i < m->n; i++) {
    const char *s = m->line[i];
    const bool title = (m->flags & PSP_MSG_TITLE) && i == 0;
    const float sc = (m->flags & PSP_MSG_TITLE)
                         ? (i == 0 ? kMsgTitleScale : kMsgBodyScale)
                         : kMsgTextScale;
    if (!*s) {
      if (n < kMsgMaxLines)
        out[n++] = (MsgLine){s, s, sc, false};
      continue;
    }
    const float sp = (float)g_psp_msg_glyphs[0].advance * sc;
    const char *ls = s, *last = s, *p = s;
    float lw = 0.0f;
    while (*p) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;
      const char *q = p;
      while (*q && *q != ' ') {
        if (*q == '{') {
          const char *c = strchr(q, '}');
          q = c ? c + 1 : q + 1;
        } else {
          q++;
        }
      }
      const float pw = MsgWidth(p, q, sc);
      if (lw > 0.0f && lw + sp + pw > (float)kMsgMaxW) {
        if (n < kMsgMaxLines)
          out[n++] = (MsgLine){ls, last, sc, false};
        ls = p;
        lw = pw;
      } else {
        lw = lw > 0.0f ? lw + sp + pw : pw;
      }
      last = q;
      p = q;
    }
    if (n < kMsgMaxLines)
      out[n++] = (MsgLine){ls, last, sc, title};
  }
  return n;
}

static void MsgDrawStr(const char *s, float x, float y, float sc, uint32_t rgb, unsigned a) {
  MsgDrawRun(s, s + strlen(s), x, y, sc, rgb, a);
}

static float MsgStrWidth(const char *s, float sc) {
  return MsgWidth(s, s + strlen(s), sc);
}

static void DrawNativeMessage(void) {
  if ((!g_msg_id || !g_msg_fade) && !g_msg_dim_hold)
    return;
  const unsigned a = g_msg_fade;

  /* dim: gameplay rectangle only */
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
  const unsigned dim_alpha = g_msg_dim_hold ? kMsgDimAlpha : kMsgDimAlpha * a / 255u;
  const uint32_t dim = (uint32_t)dim_alpha << 24;
  ColorVert *dv = sceGuGetMemory(2 * (int)sizeof(ColorVert));
  dv[0] = (ColorVert){dim, kCompX, kCompY, 0};
  dv[1] = (ColorVert){dim, kCompX + kCompW, kCompY + kCompH, 0};
  GeDraw(kVtColor, 2, dv);

  sceGuEnable(GU_TEXTURE_2D);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuAlphaFunc(GU_GREATER, 0, 0xff);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);

  if (!g_msg_id || !g_msg_fade)
    return;

  const PspMessageText *m = &g_psp_message_text[g_msg_id - 1];
  if (!m->n)
    return;

  MsgLine ln[kMsgMaxLines];
  const int nl = MsgLayout(m, ln);
  const bool save = (m->flags & PSP_MSG_SAVE) != 0;
  const bool gold = (g_msg_id >= 20 && g_msg_id <= 24) || g_msg_id == 28;
  const uint32_t text_rgb = gold ? 0x0002b0f8u : 0x00ffffffu;
  const float lh_opt = (float)PSP_MSG_FONT_LINE_HEIGHT * kMsgOptionScale;
  float total = 0.0f;
  for (int i = 0; i < nl; i++) {
    total += (float)PSP_MSG_FONT_LINE_HEIGHT * ln[i].scale;
    if (ln[i].gap)
      total += kMsgTitleGap;
  }
  if (save)
    total += kMsgOptGap + lh_opt;

  const float cx = (float)kCompX + (float)kCompW * 0.5f;
  float y = (float)kCompY + ((float)kCompH - total) * 0.5f;
  for (int i = 0; i < nl; i++) {
    const float w = MsgWidth(ln[i].s, ln[i].e, ln[i].scale);
    const uint32_t rgb =
        ((m->flags & PSP_MSG_TITLE) && ln[i].scale == kMsgTitleScale)
            ? 0x008c3ed4u : text_rgb;
    MsgDrawRun(ln[i].s, ln[i].e, cx - w * 0.5f, y, ln[i].scale, rgb, a);
    y += (float)PSP_MSG_FONT_LINE_HEIGHT * ln[i].scale;
    if (ln[i].gap)
      y += kMsgTitleGap;
  }
  if (save) {  /* current game-owned selection: 0 = YES, 2 = NO */
    y += kMsgOptGap;
    const float wy = MsgStrWidth("YES", kMsgOptionScale), wn = MsgStrWidth("NO", kMsgOptionScale), gap = 64.0f;
    const float yes_x = cx - (wy + gap + wn) * 0.5f, no_x = yes_x + wy + gap;
    const bool yes = g_msg_sel == 0;
    MsgDrawStr(">", (yes ? yes_x : no_x) - 20.0f, y, kMsgOptionScale, text_rgb, a);
    MsgDrawStr("YES", yes_x, y, kMsgOptionScale, yes ? text_rgb : 0x00909090u, a);
    MsgDrawStr("NO", no_x, y, kMsgOptionScale, yes ? 0x00909090u : text_rgb, a);
  }
}

#if SM_PSP_GE_HAZE_LIGHT
/* Ceres haze light. The game gives the haze its fade level (0..16) and colour channel (0x80 blue while Ridley lives, 0x20 red after:
 * the escape) every frame it runs the haze object (PspRenderer_SetHazeLight, sm_88.c). Four GE point lights below the gameplay
 * rectangle shine up on a flat quad mesh (normals towards the viewer); the GE lights it per vertex and the triangle strips
 * interpolate. Light height 140, 10 px below the picture, attenuation 1 / (1 + d^2 / 10000): measured on the model, the brightness
 * up the middle of the picture is 100%, 93%, 81%, 67%, 54%, 43%, 33%, 26%, 21%, 17% every 30 px from the bottom edge, so it reaches
 * the top of the picture as the original gradient did (which ran the full height), with 91% at the bottom between two lights. The peak
 * (bottom edge) is 0.40 of full scale at GAIN 100 (the original gradient peaked at 15/31 = 0.48 and fell to 0 at the top). The red light ebbs like an emergency light
 * (HazePulse). Drawn twice: SM_PSP_GE_HAZE_GLOW percent into the glow chain (so it blooms; PSP_GE_GLOW) and the rest on the picture
 * with the GE's ordered dither on (no banding from the 5-bit blue). The projection maps the picture rectangle onto the target
 * (frame buffer or the half-resolution glow image); lighting, lights, dither and flat shading are put back. */
#ifndef SM_PSP_GE_HAZE_GLOW
#define SM_PSP_GE_HAZE_GLOW 35
#endif
#ifndef SM_PSP_GE_HAZE_PULSE_FRAMES
#define SM_PSP_GE_HAZE_PULSE_FRAMES 112 /* the red light's ebb: one full cycle, frames (60 per second) */
#endif
static unsigned g_haze_level, g_haze_mask;
static uint32_t g_haze_frame;
static bool g_haze_via_glow;  /* this frame the glow chain carries SM_PSP_GE_HAZE_GLOW percent of the haze light */

void PspRenderer_SetHazeLight(unsigned level, unsigned mask) {
  g_haze_level = level > 16 ? 16 : level;
  g_haze_mask = mask;
  g_haze_frame = g_frame;  /* g_frame++ opens EndDraw, after the game logic that calls this */
}

enum { kHazeCols = 12, kHazeRows = 9, kHazeCell = 30, kHazeVerts = kHazeRows * 2 * (kHazeCols + 1), kHazeCalls = 80 };
typedef struct LitVert { float nx, ny, nz, x, y, z; } LitVert;

static inline bool HazeLightOn(void) { return g_ge_ok && g_haze_level && (uint32_t)(g_frame - g_haze_frame) <= 1u; }

/* 0.15 .. 1: smoothstep up and down over SM_PSP_GE_HAZE_PULSE_FRAMES for the red (escape) light, steady for the blue one */
static float HazePulse(void) {
  if (g_haze_mask != 0x20)
    return 1.0f;
  const unsigned half = SM_PSP_GE_HAZE_PULSE_FRAMES / 2, p = g_frame % (2 * half);
  const float t = (float)(p < half ? p : 2 * half - p) / (float)half;
  return 0.15f + 0.85f * (t * t * (3.0f - 2.0f * t));
}

/* ABGR light colour; scale 1 = the full haze light */
static unsigned HazeColor(float scale) {
  static const uint8_t kBlue[3] = {40, 90, 255}, kRed[3] = {255, 80, 32};
  const uint8_t *base = g_haze_mask == 0x20 ? kRed : kBlue;
  const float k = (115.0f / 255.0f) * (float)g_haze_level * (1.0f / 16.0f) * ((float)SM_PSP_GE_HAZE_LIGHT_GAIN * 0.01f) * HazePulse() * scale;
  unsigned c[3];
  for (int i = 0; i < 3; i++) {
    const float f = (float)base[i] * k + 0.5f;
    c[i] = f > 255.0f ? 255u : (unsigned)f;
  }
  return 0xff000000u | c[2] << 16 | c[1] << 8 | c[0];
}

/* Draws the lit mesh additively. The picture rectangle (kCompX.. + kCompW, kCompY.. + kCompH) lands on the rectangle of size tw x th
 * whose top-left corner is the target pixel (ox, oy): (0, 0) in the glow targets, (kCompX, kCompY) in the 480 x 272 frame buffer.
 * The target, scissor and blend source are the caller's; the blend is set here. */
static void HazeDrawMesh(unsigned col, float tw, float th, float ox, float oy, bool dither) {
  LitVert *v = sceGuGetMemory((int)(kHazeVerts * sizeof(LitVert)));
  const float x0 = (float)kCompX, y_bot = (float)(kCompY + kCompH);
  int n = 0;
  for (int r = 0; r < kHazeRows; r++) {
    const float ya = y_bot - (float)(r * kHazeCell), yb = ya - (float)kHazeCell;
    for (int k = 0; k <= kHazeCols; k++) {
      const float x = x0 + (float)(k * kHazeCell);
      v[n++] = (LitVert){0.0f, 0.0f, 1.0f, x, ya, 0.0f};
      v[n++] = (LitVert){0.0f, 0.0f, 1.0f, x, yb, 0.0f};
    }
  }
  /* world pixel (x, y) -> target pixel (ox + (x - kCompX) * sx, oy + (y - kCompY) * sy); the GE maps NDC to pixels with 240 / -136
   * about the centre of the 480 x 272 viewport, so the matrix is that mapping inverted (column-major, translation in 12..14).
   * ox = kCompX, oy = kCompY, sx = sy = 1 is glOrtho(0, 480, 272, 0, -1, 1), what the first version drew with. */
  const float sx = tw / (float)kCompW, sy = th / (float)kCompH;
  float proj[16] __attribute__((aligned(16))) = {0}, ident[16] __attribute__((aligned(16))) = {0};
  proj[0] = sx / 240.0f;
  proj[5] = -sy / 136.0f;
  proj[10] = -1.0f;
  proj[12] = (ox - (float)kCompX * sx) / 240.0f - 1.0f;
  proj[13] = 1.0f + ((float)kCompY * sy - oy) / 136.0f;
  proj[15] = 1.0f;
  ident[0] = ident[5] = ident[10] = ident[15] = 1.0f;
  sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)proj);
  sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)ident);
  sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)ident);

  sceGuDisable(GU_TEXTURE_2D);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00ffffffu, 0x00ffffffu);  /* dst + lit colour */
  if (dither) {
    static const int kDither[16] __attribute__((aligned(16))) = {-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2};
    sceGuSetDither((const ScePspIMatrix4 *)kDither);
    sceGuEnable(GU_DITHER);
  }
  sceGuShadeModel(GU_SMOOTH);
  sceGuColorMaterial(0);
  sceGuMaterial(GU_AMBIENT | GU_DIFFUSE, 0xffffffffu);
  sceGuAmbient(0xff000000u);
  sceGuLightMode(GU_SINGLE_COLOR);
  for (int i = 0; i < 4; i++) {
    const ScePspFVector3 pos = {x0 + (float)(45 + 90 * i), y_bot + 10.0f, 140.0f};
    sceGuLight(i, GU_POINTLIGHT, GU_DIFFUSE, &pos);
    sceGuLightColor(i, GU_AMBIENT, 0);
    sceGuLightColor(i, GU_DIFFUSE, col);
    sceGuLightAtt(i, 1.0f, 0.0f, 1.0f / 10000.0f);
    sceGuEnable(GU_LIGHT0 + i);
  }
  sceGuEnable(GU_LIGHTING);
  for (int r = 0; r < kHazeRows; r++)
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 2 * (kHazeCols + 1), 0,
                   v + r * 2 * (kHazeCols + 1));
  sceGuDisable(GU_LIGHTING);
  for (int i = 0; i < 4; i++)
    sceGuDisable(GU_LIGHT0 + i);
  sceGuShadeModel(GU_FLAT);
  if (dither)
    sceGuDisable(GU_DITHER);
  sceGuDisable(GU_BLEND);
}

/* on the picture, after the scaled image */
static void ComposeHazeLight(void) {
  HazeDrawMesh(HazeColor(g_haze_via_glow ? 1.0f - (float)SM_PSP_GE_HAZE_GLOW * 0.01f : 1.0f), (float)kCompW, (float)kCompH, (float)kCompX, (float)kCompY, true);
  sceGuEnable(GU_TEXTURE_2D);
}
#endif

#if SM_PSP_GE_GLOW
/* Glow. What emits is decided here, from this frame's state, and drawn from what the scene pass left behind:
 *  - sprites (Samus, shots, enemies; SRC bit 0) and, in lava / acid rooms, the layer-3 pixels (SRC bit 2): redrawn with the depth
 *    test EQUAL against the scene's depth buffer (the way RestoreLayers works), so only the pixels where that layer won survive: the
 *    silhouette is the object's own, and what a foreground tile hides does not emit;
 *  - door shells (SRC bit 1): the exact blocks of the cap (PspGlow_DoorBoxes), taken from the scene copy;
 *  - rain rooms (SRC bit 2): the whole picture (lightning);
 *  - the haze light (PSP_GE_HAZE_LIGHT): SM_PSP_GE_HAZE_GLOW percent of it.
 * Only in main gameplay (state 8): not in the pause screens, the intro, menus or door scrolls. */
enum { kGlowMaxBoxes = 16, kGlowDoorMax = 8, kGlowMargin = 24, kGlowCalls = 320 };
#ifndef SM_PSP_GE_GLOW_CORE
#define SM_PSP_GE_GLOW_CORE 35 /* percent of the half-resolution emitter image added back at screen resolution, hugging the shape */
#endif
typedef struct GlowBox { int16_t x0, y0, x1, y1; } GlowBox;
static GlowBox g_glow_box[kGlowMaxBoxes];
static int g_glow_nbox, g_glow_ux0, g_glow_uy0, g_glow_ux1, g_glow_uy1;
static bool g_glow_obj, g_glow_bg3, g_glow_live;
static TileVert *g_glow_vc;  /* the two additive quads of the last step, built in GlowPrep (list memory) */

static void DrawBgPass(int l, int pr);
static void DrawObjSegs(const ObjSeg *segs, int n, const BgVert *base, int bright, bool only_excluded, int mathmask);

static inline bool GlowStateOk(void) { return g_glow_ok && GameRam16(0x998) == 8; /* kGameState_8_MainGameplay */ }

static void GlowBBox(int x0, int y0, int x1, int y1, int srow0) {
  x0 = x0 < 0 ? 0 : x0;
  y0 = y0 < srow0 ? srow0 : y0;
  x1 = x1 > kGameWidth ? kGameWidth : x1;
  y1 = y1 > kSrcRows ? kSrcRows : y1;
  if (x1 <= x0 || y1 <= y0)
    return;
  g_glow_ux0 = x0 < g_glow_ux0 ? x0 : g_glow_ux0;
  g_glow_uy0 = y0 < g_glow_uy0 ? y0 : g_glow_uy0;
  g_glow_ux1 = x1 > g_glow_ux1 ? x1 : g_glow_ux1;
  g_glow_uy1 = y1 > g_glow_uy1 ? y1 : g_glow_uy1;
}

static void GlowAddBox(int x0, int y0, int x1, int y1, int srow0) {
  x0 = x0 < 0 ? 0 : x0;
  y0 = y0 < srow0 ? srow0 : y0;
  x1 = x1 > kGameWidth ? kGameWidth : x1;
  y1 = y1 > kSrcRows ? kSrcRows : y1;
  if (x1 <= x0 || y1 <= y0 || g_glow_nbox >= kGlowMaxBoxes)
    return;
  g_glow_box[g_glow_nbox++] = (GlowBox){(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1};
  GlowBBox(x0, y0, x1, y1, srow0);
}

#if SM_PSP_LOG
/* Why a frame has (or has no) glow, once per change of room / emitter set (first 64 lines): lets a room that should glow and does not
 * (Maridia) be told apart: state or start-up (why = 0), no emitter found (1), or emitters present (2). */
static void GlowLog(int why, int srow0) {
  static unsigned last, nlog;
  const unsigned room = GameRam16(0x79B);
  const unsigned sig = room ^ (unsigned)why << 16 ^ (unsigned)g_glow_nbox << 20 ^ (g_glow_obj ? 1u << 24 : 0) ^ (g_glow_bg3 ? 1u << 25 : 0);
  if (sig == last || nlog >= 64)
    return;
  last = sig;
  nlog++;
  PspLog("glow: why=%d room=%04x area=%02x gs=%04x fx=%04x ok=%d boxes=%d obj=%d(n=%d seg=%d) bg3=%d srow0=%d f=%u", why, room, (unsigned)g_ram[0x79F],
         (unsigned)GameRam16(0x998), (unsigned)GameRam16(0x196E), (int)g_glow_ok, g_glow_nbox, (int)g_glow_obj, g_glow_obj_n, g_nobjseg,
         (int)g_glow_bg3, srow0, (unsigned)g_frame);
}
#endif

static bool GlowCollect(int srow0) {
  g_glow_nbox = 0;
  g_glow_obj = g_glow_bg3 = false;
#if SM_PSP_GE_HAZE_LIGHT
  g_haze_via_glow = false;
#endif
  g_glow_ux0 = kGameWidth;
  g_glow_uy0 = kSrcRows;
  g_glow_ux1 = g_glow_uy1 = 0;
  if (!GlowStateOk()) {
#if SM_PSP_LOG
    GlowLog(0, srow0);
#endif
    return false;
  }
  const int cx = (int)GameRam16(0x911), cy = (int)GameRam16(0x915);  /* layer 1 position: level pixel of picture column / row 0 */
  (void)cx;
  (void)cy;
#if SM_PSP_GE_GLOW_SRC & 2
  {
    PspGlowBox db[kGlowDoorMax];
    const int n = PspGlow_DoorBoxes(db, kGlowDoorMax);
    for (int i = 0; i < n; i++)
      GlowAddBox(db[i].x0, db[i].y0, db[i].x1, db[i].y1, srow0);
#if SM_PSP_LOG
    static int last_n = -1;
    static unsigned nlog;
    if (n != last_n && nlog < 48) {
      nlog++;
      PspLog("glow: door boxes=%d room=%04x f=%u", n, (unsigned)GameRam16(0x79B), (unsigned)g_frame);
    }
    last_n = n;
#endif
  }
#endif
#if SM_PSP_GE_GLOW_SRC & 4
  {
    const unsigned fx = GameRam16(0x196E), surf = GameRam16(0x1962);  /* fx_type, lava_acid_y_pos */
    if (fx == 0x0A) {  /* rain: the lightning flashes the whole picture */
      if (g_glow_nbox < kGlowMaxBoxes)
        GlowAddBox(0, srow0, kGameWidth, kSrcRows, srow0);
    } else if ((fx == 0x02 || fx == 0x04) && surf < 0x4000 && !g_img_on[2] && !g_mos[2].on) {  /* lava, acid: the layer-3 pixels */
      g_glow_bg3 = true;
      GlowBBox(0, (int)surf - cy > 32 ? (int)surf - cy : 32, kGameWidth, kSrcRows, srow0);
    }
  }
#endif
#if SM_PSP_GE_GLOW_SRC & 1
  if (g_nobjseg > 0) {
    g_glow_obj = true;
    for (int i = 0; i < g_glow_obj_n; i++) {
      const Sprite *sp = &g_spr[i];
      GlowBBox(sp->x, sp->y, sp->x + sp->size, sp->y + sp->size, srow0);
      if (sp->y + sp->size > 256)  /* rows wrap modulo 256 */
        GlowBBox(sp->x, sp->y - 256, sp->x + sp->size, sp->y - 256 + sp->size, srow0);
    }
  }
#endif
#if SM_PSP_GE_HAZE_LIGHT && SM_PSP_GE_HAZE_GLOW > 0
  g_haze_via_glow = HazeLightOn();
  if (g_haze_via_glow)
    GlowBBox(0, srow0, kGameWidth, kSrcRows, srow0);
#endif
  const bool found = g_glow_ux1 > g_glow_ux0 && g_glow_uy1 > g_glow_uy0;
#if SM_PSP_LOG
  GlowLog(found ? 2 : 1, srow0);
#endif
  return found;
}

static void GlowScissor(int x0, int y0, int x1, int y1) {
  if (g_scissor_mode == 2)
    sceGuScissor(x0, y0, x1 - x0, y1 - y0);
  else
    sceGuScissor(x0, y0, x1, y1);
}

static void ComposeGlowTaps(TileVert *v, int n, int rows, int src_w, uint32_t weight) {
  sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, weight * 0x010101u, 0x00ffffffu);
  for (int t = 0; t < 4; t++) {
    const float tx = (t & 1) ? 1.0f : -1.0f, ty = (t & 2) ? 1.0f : -1.0f;
    const int nv = ComposeQuadStrips(v, 1.0f + tx, 1.0f + ty, (float)(src_w - 1) + tx, 1.0f + ty + 2.0f * (float)(rows - 1), 0, 0, n, rows);
    GeDraw(kVtTile, nv, v);
    v += nv;
  }
}

/* Before the picture is composed (the draw buffer still holds the scene, its depth buffer the layer markers; the scene copy has been
 * taken): emitters, bright pass, blurs. All GE; nothing is read back.
 *  0. the emitter image in the scene rectangle of the draw buffer (scratch: the composition overwrites it right after).
 *  1. half-resolution bright pass: a black 128x128 target, the emitter image averaged 2x2 into it (bilinear sample on the texel
 *     boundary), THRESH subtracted per channel (reverse-subtract sprite; hue is kept), the haze light added on top.
 *  2. quarter-resolution blur: four additive taps, each a bilinear sample on a texel corner (a 4x4 box average, 1/4 weight each).
 *  3. eighth-resolution blur: the same four taps with the gain (GAIN / 4 of their colour each).
 * ComposeGlowAdd, after the picture, adds the result. Nothing reads what it writes. */
static bool GlowPrep(const ComposeSrc *s, int srow0) {
  enum { kThr = SM_PSP_GE_GLOW_THRESH, kGain = (SM_PSP_GE_GLOW_GAIN * 255 + 200) / 400, kStrips = COMPOSE_STRIPS(kCompW) };
  g_glow_live = false;
  if (!GlowCollect(srow0))
    return false;
  const int nr = (kSrcRows - srow0) / 2;  /* half-resolution rows covering the source rows the picture shows */
  const int nr2 = (nr + 1) / 2;           /* quarter-resolution rows */
  const int nr3 = (nr2 + 1) / 2;          /* eighth-resolution rows */
  int nvb = 0;
  for (int b = 0; b < g_glow_nbox; b++)
    nvb += 2 * COMPOSE_STRIPS(g_glow_box[b].x1 - g_glow_box[b].x0);
  const int nvq = 2 * COMPOSE_STRIPS(kGlowW1);
  TileVert *tv = sceGuGetMemory((int)((nvb + nvq + 4 * 2 * COMPOSE_STRIPS(kGlowW2) + 4 * 2 * COMPOSE_STRIPS(kGlowW3) + 4 * kStrips + 4) *
                                      sizeof(TileVert)));
  ColorVert *cv = sceGuGetMemory((int)(8 * sizeof(ColorVert)));
  TileVert *vb = tv, *vq = vb + nvb, *v2 = vq + nvq, *v3 = v2 + 4 * 2 * COMPOSE_STRIPS(kGlowW2), *vc = v3 + 4 * 2 * COMPOSE_STRIPS(kGlowW3);
  g_glow_vc = vc;
  const uint32_t thr = 0xff000000u | (uint32_t)kThr * 0x010101u;
  cv[0] = (ColorVert){0xff000000u, 0, 0, 0};
  cv[1] = (ColorVert){0xff000000u, kGlowW1, kGlowW1, 0};
  cv[2] = (ColorVert){thr, 0, 0, 0};
  cv[3] = (ColorVert){thr, kGlowW1, (int16_t)nr, 0};
  cv[4] = (ColorVert){0xff000000u, 0, 0, 0};
  cv[5] = (ColorVert){0xff000000u, kGlowW2, kGlowW2, 0};
  cv[6] = (ColorVert){0xff000000u, 0, 0, 0};
  cv[7] = (ColorVert){0xff000000u, kGlowW3, kGlowW3, 0};

  /* 0. emitter image */
  sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)g_draw_off, 512);
  sceGuDepthBuffer((void *)(512 * 272 * 4), 512);
  GlowScissor(kLeft, kTop, kLeft + kGameWidth, kTop + kSrcRows);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);  /* colour only: the depth markers stay */
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);
  if (g_glow_obj || g_glow_bg3) {
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xff);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_EQUAL);
    sceGuDepthMask(GU_TRUE);  /* GU_TRUE masks depth writes */
    if (g_glow_obj)
      DrawObjSegs(g_objseg, g_nobjseg, g_objbase, kClutBright, false, 0);
    if (g_glow_bg3) {  /* rows 32.. only: the HUD is layer 3 too */
      GlowScissor(kLeft, kTop + 32, kLeft + kGameWidth, kTop + kSrcRows);
      DrawBgPass(2, 0);
      DrawBgPass(2, 1);
      GlowScissor(kLeft, kTop, kLeft + kGameWidth, kTop + kSrcRows);
    }
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
  }
  if (g_glow_nbox) {  /* door caps, rain: the exact blocks of the scene copy, 1:1 */
    int nv = 0;
    for (int b = 0; b < g_glow_nbox; b++) {
      const GlowBox *bx = &g_glow_box[b];
      nv += ComposeQuadStrips(vb + nv, (float)bx->x0 + 0.5f, (float)bx->y0 + 0.5f, (float)bx->x1 - 0.5f, (float)bx->y1 - 0.5f,
                              kLeft + bx->x0, kTop + bx->y0, bx->x1 - bx->x0, bx->y1 - bx->y0);
    }
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexMode(s->psm, 0, 0, GU_FALSE);
    sceGuTexImage(0, s->w, s->h, s->stride, s->ptr);
    sceGuTexFlush();
    GeDraw(kVtTile, nv, vb);
  }

  /* 1. bright pass: half resolution */
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_TEXTURE_2D);
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDrawBufferList(GU_PSM_8888, (void *)(uintptr_t)kVramGlow1, kGlowW1);
  GeDraw(kVtColor, 2, &cv[0]);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
  sceGuTexImage(0, 512, 512, 512, g_vram_u + g_draw_off);  /* the emitter image: the draw buffer, which is not drawn to now */
  sceGuTexFlush();
  sceGuTexSync();  /* the emitter image has been rendered by the GE */
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
  GeDraw(kVtTile,
         ComposeQuadStrips(vq, (float)(kLeft + 1), (float)(kTop + srow0 + 1), (float)(kLeft + 255), (float)(kTop + srow0 + 2 * nr - 1), 0, 0,
                           kGlowW1, nr),
         vq);
  sceGuDisable(GU_TEXTURE_2D);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0x00ffffffu, 0x00ffffffu);  /* dst - THRESH, as the subtractive tint */
  GeDraw(kVtColor, 2, &cv[2]);
#if SM_PSP_GE_HAZE_LIGHT && SM_PSP_GE_HAZE_GLOW > 0
  if (g_haze_via_glow)  /* the glow chain multiplies by GAIN / 100: feed the share divided by it */
    HazeDrawMesh(HazeColor((float)SM_PSP_GE_HAZE_GLOW / (float)SM_PSP_GE_GLOW_GAIN), (float)kGlowW1, (float)nr, 0.0f, 0.0f, false);
#endif

  /* 2. quarter-resolution blur */
  sceGuDisable(GU_BLEND);
  sceGuDrawBufferList(GU_PSM_8888, (void *)(uintptr_t)kVramGlow2, kGlowW2);
  GeDraw(kVtColor, 2, &cv[4]);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
  sceGuTexImage(0, kGlowW1, kGlowW1, kGlowW1, g_vram_u + kVramGlow1);
  sceGuTexFlush();
  sceGuTexSync();  /* the half-resolution target has been rendered by the GE */
  sceGuEnable(GU_BLEND);
  ComposeGlowTaps(v2, kGlowW2, nr2, kGlowW1, 0x40);

  /* 3. eighth-resolution blur, with the gain */
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDrawBufferList(GU_PSM_8888, (void *)(uintptr_t)kVramGlow3, kGlowS3);
  GeDraw(kVtColor, 2, &cv[6]);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexImage(0, kGlowW2, kGlowW2, kGlowW2, g_vram_u + kVramGlow2);
  sceGuTexFlush();
  sceGuTexSync();  /* the quarter-resolution target has been rendered by the GE */
  sceGuEnable(GU_BLEND);
  ComposeGlowTaps(v3, kGlowW3, nr3, kGlowW2, kGain);

  /* back to the frame buffer for the composition, which sets everything else itself */
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)g_draw_off, 512);
  sceGuTexSync();  /* the eighth-resolution target has been rendered by the GE */

  /* the two additive quads of ComposeGlowAdd: the blur (8x), then the half-resolution image itself (3.75x, hugging the shape) */
  const int nbl = ComposeQuadStrips(vc, 0.5f / 8.0f, 0.5f / 8.0f, 255.5f / 8.0f, ((float)(kSrcRows - srow0) - 0.5f) / 8.0f, kCompX, kCompY,
                                    kCompW, kCompH);
  ComposeQuadStrips(vc + nbl, 0.5f / 2.0f, 0.5f / 2.0f, 255.5f / 2.0f, ((float)(kSrcRows - srow0) - 0.5f) / 2.0f, kCompX, kCompY, kCompW,
                    kCompH);
  g_glow_live = true;
  return true;
}

/* After the picture (and the haze light): add the glow inside the emitters' bounding box + margin. */
static void ComposeGlowAdd(const ComposeSrc *s, int srow0) {
  if (!g_glow_live)
    return;
  const int nbl = 2 * COMPOSE_STRIPS(kCompW);
  const int sh = kSrcRows - srow0;
  int sx0 = kCompX + ((g_glow_ux0 - kGlowMargin) * kCompW) / kGameWidth;
  int sx1 = kCompX + ((g_glow_ux1 + kGlowMargin) * kCompW + kGameWidth - 1) / kGameWidth;
  int sy0 = kCompY + ((g_glow_uy0 - kGlowMargin - srow0) * kCompH) / sh;
  int sy1 = kCompY + (((g_glow_uy1 + kGlowMargin - srow0) * kCompH + sh - 1) / sh);
  sx0 = sx0 < kCompX ? kCompX : sx0;
  sy0 = sy0 < kCompY ? kCompY : sy0;
  sx1 = sx1 > kCompX + kCompW ? kCompX + kCompW : sx1;
  sy1 = sy1 > kCompY + kCompH ? kCompY + kCompH : sy1;
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
  sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
  sceGuTexImage(0, kGlowW3, kGlowW3, kGlowS3, g_vram_u + kVramGlow3);
  sceGuTexFlush();
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00ffffffu, 0x00ffffffu);
  GlowScissor(sx0, sy0, sx1, sy1);
  GeDraw(kVtTile, nbl, g_glow_vc);
#if SM_PSP_GE_GLOW_CORE > 0
  sceGuTexImage(0, kGlowW1, kGlowW1, kGlowW1, g_vram_u + kVramGlow1);
  sceGuTexFlush();
  sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, (uint32_t)(SM_PSP_GE_GLOW_CORE * 255 / 100) * 0x010101u, 0x00ffffffu);
  GeDraw(kVtTile, nbl, g_glow_vc + nbl);
#endif
  sceGuScissor(0, 0, kPspWidth, kPspHeight);

  /* state for what follows (the HUD samples the source image again) */
  sceGuDisable(GU_BLEND);
  sceGuTexMode(s->psm, 0, 0, GU_FALSE);
  sceGuTexImage(0, s->w, s->h, s->stride, s->ptr);
  sceGuTexFlush();
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
}
#endif

/* Appends the composition of the source image s (256 wide, rows 0-223 valid) to the open list: black panel, then Layout A
 * (HUD present: source rows 32-223 -> 360x270, original HUD elements in the side panels) or Layout B (rows 0-223 -> 360x270,
 * empty margins). The list is open and the source is complete; exactly one Present() follows. */
#if SM_PSP_DOOR_SNAP
static bool DsListFits(void);
#endif

static void ComposeDraw(const ComposeSrc *s) {
  static bool icons_flushed;
  if (!icons_flushed) {  /* the icon textures are read by the GE from RAM: write the CPU's copy back once (never written again) */
    icons_flushed = true;
    sceKernelDcacheWritebackRange(g_hud_ico_reserve_on, sizeof(g_hud_ico_reserve_on));
    sceKernelDcacheWritebackRange(g_hud_ico_reserve_off, sizeof(g_hud_ico_reserve_off));
    sceKernelDcacheWritebackRange(g_hud_ico_speed_sel, sizeof(g_hud_ico_speed_sel));
    sceKernelDcacheWritebackRange(g_hud_ico_speed_uns, sizeof(g_hud_ico_speed_uns));
    sceKernelDcacheWritebackRange((void *)g_psp_button_circle, sizeof(g_psp_button_circle));
    sceKernelDcacheWritebackRange((void *)g_psp_button_cross, sizeof(g_psp_button_cross));
    sceKernelDcacheWritebackRange((void *)g_psp_button_square, sizeof(g_psp_button_square));
    sceKernelDcacheWritebackRange((void *)g_psp_button_triangle, sizeof(g_psp_button_triangle));
    sceKernelDcacheWritebackRange((void *)g_psp_button_l, sizeof(g_psp_button_l));
    sceKernelDcacheWritebackRange((void *)g_psp_button_r, sizeof(g_psp_button_r));
    sceKernelDcacheWritebackRange((void *)g_psp_button_select, sizeof(g_psp_button_select));
    sceKernelDcacheWritebackRange((void *)g_psp_button_dpad_ud, sizeof(g_psp_button_dpad_ud));
    sceKernelDcacheWritebackRange((void *)g_psp_button_dpad_lr, sizeof(g_psp_button_dpad_lr));
    sceKernelDcacheWritebackRange((void *)g_psp_button_dpad_lrud, sizeof(g_psp_button_dpad_lrud));
    sceKernelDcacheWritebackRange((void *)psp_msg_font_rgba, PSP_MSG_FONT_W * PSP_MSG_FONT_H * 4);
    sceKernelDcacheWritebackRange((void *)psp_msg_grapple_rgba, 16 * 16 * 4);
    sceKernelDcacheWritebackRange((void *)psp_msg_missile_rgba, 32 * 16 * 4);
    sceKernelDcacheWritebackRange((void *)psp_msg_powerbomb_rgba, 16 * 16 * 4);
    sceKernelDcacheWritebackRange((void *)psp_msg_supermissile_rgba, 16 * 16 * 4);
    sceKernelDcacheWritebackRange((void *)psp_msg_xray_rgba, 16 * 16 * 4);
  }
  int off = 0;
  const int why = HudResolve(&off);
  const bool hud = why == kHudOk;
  const int srow0 = hud ? 32 : 0;
#if SM_PSP_LOG
  {  /* why: 0 HUD, 1 none, 2 gap/short frame, 3 reserved, 4 scroll, 5 run too long, 6 geometry, 7 run too short, 8 overscan */
    static int last_why = -1;
    static unsigned nlog;
    if (why != last_why && nlog < 64) {
      nlog++;
      PspLog("layout f=%u %s why=%d run=%u rows=%u os=%d hs=%u vs=%u gap=%d marker=%d tm4=%u", (unsigned)g_frame,
             hud ? "HUD" : "FULL", why, (unsigned)g_hud.run, (unsigned)g_hud.nlines,
             g_hud_ppu ? (int)g_hud_ppu->frameOverscan : -1, (unsigned)g_hud.hs, (unsigned)g_hud.vs, (int)g_hud.gap,
             (int)g_hud.marker_ok, (unsigned)g_hud.tm4);
    }
    last_why = why;
  }
#endif
  /* every state the composition depends on is set here; none is inherited from the scene pass */
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_STENCIL_TEST);
  sceGuDisable(GU_DITHER);
  sceGuDisable(GU_CULL_FACE);
  sceGuDisable(GU_TEXTURE_2D);
#if SM_PSP_GE_GLOW
  GlowPrep(s, srow0);  /* while the draw buffer still holds the scene and its depth markers; leaves this state as it found it */
#endif
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);  /* the 60 px panels, and whatever the scene left outside the gameplay rectangle */
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexMode(s->psm, 0, 0, GU_FALSE);
  sceGuTexImage(0, s->w, s->h, s->stride, s->ptr);
  sceGuTexFlush();
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);

#if SM_PSP_DOOR_GE
  if (g_door_live && hud && s->psm == GU_PSM_5650) {
    /* gameplay rows 32-223 -> slot 0 (VRAM to VRAM), every frame until the fade-out is over; sync before it is sampled */
    sceGuCopyImage(GU_PSM_5650, 0, 32, kGameWidth, 192, s->stride,
                   (void *)(0x04000000u | ((uintptr_t)s->ptr & 0x001fffffu)), 0, 0, 256,
                   (void *)(uintptr_t)(0x04000000u | (uint32_t)kVramImg0));
    sceGuTexSync();
    g_door_snapshot_valid = true;
  }
#endif

#if SM_PSP_DOOR_SNAP
  /* The compose list is checked, not assumed, before the snapshots add to it (see DsListFits); a transition that does not fit
   * is dropped: the live destination frame shows (a cut) and nothing is emitted for it. */
  if (g_ds_state != kDsIdle && !DsListFits()) {
    PspLog("door snap: compose list too full, transition dropped");
    DoorSnapRelease();
  }
  /* A snapshot is only taken from a GE scene frame whose HUD layout resolved (gameplay rows 32-223 of the 256x256 composition
   * source, which lives in VRAM), copied VRAM to VRAM and synced before it is sampled; rows 192-255 of a slot are never read. */
  if ((g_ds_state == kDsArmed || g_ds_state == kDsWaitB) && hud && s->psm == GU_PSM_5650 && s->ptr == (const void *)g_texture) {
    const int slot = g_ds_state == kDsArmed ? 0 : 1;
    sceGuCopyImage(GU_PSM_5650, 0, 32, kGameWidth, 192, s->stride,
                   (void *)(0x04000000u | ((uintptr_t)s->ptr & 0x001fffffu)), 0, 0, 256,
                   (void *)(uintptr_t)(0x04000000u | (uint32_t)(kVramImg0 + slot * kImgBytes)));
    g_ds_top[slot] = false;
    if (g_ds_top_ready && DsTopSlot() == slot) {  /* the lower room of a vertical door: the rows under the HUD (RAM -> slot rows 192..223) */
      sceKernelDcacheWritebackRange(g_ds_top_stage, sizeof(g_ds_top_stage));
      sceGuCopyImage(GU_PSM_5650, 0, 0, kGameWidth, kDsTopRows, kGameWidth, g_ds_top_stage, 0, kDsTopRow0, 256,
                     (void *)(uintptr_t)(0x04000000u | (uint32_t)(kVramImg0 + slot * kImgBytes)));
      g_ds_top[slot] = true;
    }
    g_ds_top_ready = false;
    sceGuTexSync();
    if (slot == 0) {
      g_ds_state = kDsHaveA;
    } else {
      g_ds_state = kDsPlay;
      g_ds_step = 0;
    }
  }
#endif

  TileVert *base = sceGuGetMemory((int)((kComposeStrips + kHudRectN + 2) * 2 * sizeof(TileVert)));
  const int nbase = ComposeQuadStrips(base, 0.5f, (float)srow0 + 0.5f, (float)kGameWidth - 0.5f, (float)kSrcRows - 0.5f, kCompX,
                                      kCompY, kCompW, kCompH);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
  GeDraw(kVtTile, nbase, base);
#if SM_PSP_DOOR_GE
  if (g_door_reserved && g_door_snapshot_valid && !g_door_live)
    DrawDoorTransition(s);
#endif
#if SM_PSP_GE_HAZE_LIGHT
  if (HazeLightOn())
    ComposeHazeLight();
#endif
#if SM_PSP_GE_GLOW
  ComposeGlowAdd(s, srow0);
#endif
#if SM_PSP_DOOR_SNAP
  if (g_ds_state == kDsWaitB || g_ds_state == kDsPlay)
    DrawDoorSnap(s);
#endif
  if (hud) {
    TileVert *w = base + nbase;
    int nv = 0;
    for (int k = 0; k < kHudRectN; k++) {
      const HudRect *r = &kHudRects[k];
      int sy = r->bg0 - off, h = r->h, py = r->py;
      if (sy < 0) {
        py -= sy;
        h += sy;
        sy = 0;
      }
      if (h <= 0 || sy + h > (int)g_hud.run)
        continue;
      ComposeQuad(&w[nv], (float)r->x + 0.5f, (float)sy + 0.5f,
                  (float)(r->x + r->w) - 0.5f, (float)(sy + h) - 0.5f,
                  r->px, py, r->w, h);
      nv += 2;
    }

    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    if (nv)
      GeDraw(kVtTile, nv, w);

    const int rs = HudReserveState(), ss = HudSpeedState();
    const struct {
      const uint32_t *px;
      int w, h, dx, dy;
      bool show;
    } ic[2] = {
      {rs == 2 ? g_hud_ico_reserve_on : g_hud_ico_reserve_off,
       32, 16, kHudResvX, kHudResvY, rs != 0},
      {ss == 2 ? g_hud_ico_speed_sel : g_hud_ico_speed_uns,
       16, 16, kHudSpeedX, kHudSpeedY, ss != 0},
    };

    TileVert *iv = base + nbase + 2 * kHudRectN;
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_NOTEQUAL, 0, 0xff);
    const unsigned brightness = (unsigned)g_hud.brightness * 17;
    const unsigned hud_tint = 0xff000000u |
                              (brightness << 16) |
                              (brightness << 8) |
                              brightness;
    sceGuColor(hud_tint);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    for (int i = 0; i < 2; i++) {
      if (!ic[i].show || !brightness)
        continue;
      sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
      sceGuTexImage(0, ic[i].w, ic[i].h, ic[i].w, ic[i].px);
      sceGuTexFlush();
      ComposeQuad(&iv[2 * i], 0.5f, 0.5f,
                  (float)ic[i].w - 0.5f, (float)ic[i].h - 0.5f,
                  ic[i].dx, ic[i].dy, ic[i].w, ic[i].h);
      GeDraw(kVtTile, 2, &iv[2 * i]);
    }
    sceGuColor(0xffffffff);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_ALPHA_TEST);
  }

  /*
   * Face-button overlays. These are drawn after the source composition and
   * HUD, so they work for both HUD and non-HUD frames.
   */
  const int source_rows = kSrcRows - srow0;

  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuAlphaFunc(GU_GREATER, 0, 0xff);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);

  for (int i = 0; i < kPspButtonIconSlots; i++) {
    const ButtonOverlay *b = &g_button_overlays[i];
    if (!b->pixels || b->y < srow0 || b->y + b->h > kSrcRows)
      continue;

    const int yend = b->y + b->h < g_line_count ? b->y + b->h : g_line_count;
    if (yend <= b->y)
      continue;

    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexImage(0, b->tex_w, b->tex_h, b->tex_w, b->pixels);
    sceGuTexFlush();

    /* Texel-centre sampling supports both 16x16 and 32x16 source textures. */
    const float uscale = b->w > 1 ? (float)(b->tex_w - 1) / (float)(b->w - 1) : 0.0f;
    const float vscale = b->h > 1 ? (float)(b->tex_h - 1) / (float)(b->h - 1) : 0.0f;

    int y = b->y;
    while (y < yend) {
      int bright, wm, wl, wr;
      if (!OverlayLine(b, y, &bright, &wm, &wl, &wr)) {
        ++y;
        continue;
      }
      int run_end = y + 1;  /* lines of equal brightness and window state share one draw */
      while (run_end < yend) {
        int b2, m2, l2, r2;
        if (!OverlayLine(b, run_end, &b2, &m2, &l2, &r2) || b2 != bright || m2 != wm || l2 != wl || r2 != wr)
          break;
        ++run_end;
      }

      int sa[2], se[2];
      const int ns = WinSpans(wm, wl, wr, sa, se);
      const int dy0 = kCompY + ((y - srow0) * kCompH + source_rows / 2) / source_rows;
      const int dy1 = kCompY + ((run_end - srow0) * kCompH + source_rows / 2) / source_rows;
      TileVert verts[4];
      int nv = 0;
      if (dy1 > dy0) {
        for (int k = 0; k < ns; k++) {
          const int x0 = sa[k] > b->x ? sa[k] : b->x;
          const int x1 = se[k] < b->x + b->w ? se[k] : b->x + b->w;
          if (x1 <= x0)
            continue;
          const int dx0 = kCompX + (x0 * kCompW + kGameWidth / 2) / kGameWidth;
          const int dx1 = kCompX + (x1 * kCompW + kGameWidth / 2) / kGameWidth;
          if (dx1 <= dx0)
            continue;
          ComposeQuad(&verts[nv], 0.5f + (float)(x0 - b->x) * uscale, 0.5f + (float)(y - b->y) * vscale,
                      0.5f + (float)(x1 - 1 - b->x) * uscale, 0.5f + (float)(run_end - 1 - b->y) * vscale,
                      dx0, dy0, dx1 - dx0, dy1 - dy0);
          nv += 2;
        }
      }
      if (nv) {  /* own vertex memory per run: earlier draws may still reference the list */
        TileVert *dst = sceGuGetMemory(nv * (int)sizeof(TileVert));
        memcpy(dst, verts, (size_t)nv * sizeof(TileVert));
        const unsigned br = (unsigned)bright * 17;
        sceGuColor(0xff000000u | (br << 16) | (br << 8) | br);
        GeDraw(kVtTile, nv, dst);
      }
      y = run_end;
    }
  }

  DrawNativeMessage();

  sceGuColor(0xffffffff);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_BLEND);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
}

static uint32_t g_gl_aborts, g_gl_start_fail;  /* frame lists abandoned after a refused call / that could not start (log) */

/* Starts a frame list; false (logged, counted) if the GE would not take it: the caller draws nothing. */
static bool FrameListStart(void *list, uint32_t cap, const char *what) {
  if (GlStart(list, cap) >= 0)
    return true;
  if (g_gl_start_fail++ < 16)
    PspLog("gl: %s: sceGuStart failed, nothing drawn f=%u", what, (unsigned)g_frame);
  return false;
}

/* A frame list refused a call (never expected: the sizes are checked before the frame is built, see GlTrip): it is finished
 * and the GE waited for, so the caller can fall back with the GE idle. Logged (first 16) and counted. */
static void GlAborted(const char *what) {
  GlAbortList();
  if (g_gl_aborts++ < 16)
    PspLog("gl: %s list refused a call (trips %u, cap %u B), frame abandoned f=%u", what, (unsigned)g_gl_trips,
           (unsigned)g_gl_cap, (unsigned)g_frame);
}

/* ComposeGeAsync was abandoned after the copy (scene -> texture region) went into the list the GE then ran: the draw buffer
 * may hold part of the composition, so ComposeGeFrame composes from that copy instead of copying the draw buffer. */
static bool g_compose_copied, g_compose_resume;

/* GE scene: it is complete in the draw buffer (previous list synced, so the multisub / mosaic users of the texture region are
 * done). Copy its 256x224 to the texture region, wait for the copy (TexSync), then compose into the same draw buffer from the
 * copy. Own short list; g_gu_list is idle here. */
static void ComposeGeFrame(void) {
  const bool resume = g_compose_resume;
  g_compose_resume = false;
  if (!FrameListStart(g_gu_list, kListBytes, "compose"))
    return;
  if (setjmp(g_gl_jb)) {
    GlAborted("compose");  /* the draw buffer shows what the GE drew: one frame, not flipped back */
    return;
  }
  GL_ARM();
  SelectDrawBuffer();
  if (!resume) {
    sceGuCopyImage(GU_PSM_5650, kLeft, kTop, kGameWidth, kSrcRows, 512, (void *)(uintptr_t)(0x04000000u | g_draw_off), 0, 0,
                   kGameWidth, (void *)(uintptr_t)(0x04000000u | (uint32_t)kVramTexture));
    sceGuTexSync();
  }
  const ComposeSrc src = {g_texture, GU_PSM_5650, kGameWidth, 256, kGameWidth};
  ComposeDraw(&src);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

// Post-composition mosaic, GE only (no CPU pixels, no frame-buffer readback). Both compose lists are synced, so the draw buffer is complete and idle. Pass A renders pixels into the texture region (dead after compose), sampling the draw buffer at the block origins. Pass B stretches that image over the draw buffer (nearest). 5650 -> 5650 is exact (dither off). Pass A writes elsewhere while it reads the draw buffer, and pass B reads the texture region while it writes the draw buffer: no feedback loop.
static void ApplyMosaicTransition(void) {
  const int size = g_mosaic_size;
  const int cw = (kPspWidth + size - 1) / size, ch = (kPspHeight + size - 1) / size;
  const float inv = 1.0f / (float)size;

  if (!FrameListStart(g_gu_list, kListBytes, "mosaic"))
    return;
  if (setjmp(g_gl_jb)) {
    GlAborted("mosaic");
    return;
  }
  GL_ARM();
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_STENCIL_TEST);
  sceGuDisable(GU_DITHER);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);

  TileVert *v = sceGuGetMemory(4 * (int)sizeof(TileVert));
// Pass A: pixel j of the small image samples texel j * size (ComposeQuad: step = (su1 - su0) / (dw - 1) = size).
  sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)kVramTexture, 256);
  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
  sceGuTexImage(0, 512, 512, 512, g_vram_u + g_draw_off);
  sceGuTexFlush();
  ComposeQuad(&v[0], 0.5f, 0.5f, 0.5f + (float)((cw - 1) * size), 0.5f + (float)((ch - 1) * size), 0, 0, cw, ch);
  GeDraw(kVtTile, 2, &v[0]);
  
// Pass B: destination pixel x shows texel floor(x / size) (sampled at (x + 0.5) / size; g_uvh as in DrawMosPass).
  const float u0 = (0.5f - g_uvh[0]) * inv, v0 = (0.5f - g_uvh[1]) * inv;
  v[2] = (TileVert){u0, v0, 0, 0, 0};
  v[3] = (TileVert){u0 + (float)kPspWidth * inv, v0 + (float)kPspHeight * inv, (int16_t)kPspWidth, (int16_t)kPspHeight, 0};
  sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)g_draw_off, 512);
  sceGuTexImage(0, 256, 256, 256, g_vram_u + kVramTexture);
  sceGuTexFlush();
  sceGuTexSync();  /* pass A has been rendered by the GE */
  GeDraw(kVtTile, 2, &v[2]);

  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

#if SM_PSP_ASYNC || SM_PSP_DOOR_SNAP  /* the bound is also what PSP_DOOR_SNAP checks the (synchronous) compose list against */
/* Compose-list bound. sceGuGetMemory and sceGuCopyImage..sceGuFinish never check the list end, so ComposeDraw only
 * runs on g_gu_list2 if ComposeBytesBound() <= kComposeAsyncMax (half of the list). The bound is mem + calls *
 * kGuCallMax, from a complete audit of what ComposeGeAsync, ComposeDraw, DrawDoorTransition and DrawNativeMessage emit:
 *
 * mem (exact, from sizeof; every sceGuGetMemory is GuMem: size rounded up to 4 plus an 8 byte header):
 *   ComposeDraw: source quad (kComposeStrips sprites) + HUD rects + icons ((kComposeStrips + kHudRectN + 2) * 2 TileVert); DrawDoorTransition: 2 TileVert;
 *   DrawNativeMessage: dim quad (2 ColorVert); sceGuClear (libgu): kGuClearBytes;
 *   button overlay: one run per source row at most (a run is >= 1 row), a run has <= 2 spans = 4 TileVert;
 *   message: per MsgDrawRun / MsgDrawStr the glyph vertices, per icon token 2 TileVert. Both are counted by the
 *   walker that writes them (kMsgPassCountGlyphs / kMsgPassCountIcons) on the layout DrawNativeMessage uses, so
 *   they are exact, label expansion included.
 * calls (libgu calls, state / texture / draw / copy / clear; counted from the code):
 *   ComposeGeAsync 5 (start, draw buffer, copy, sync, finish) + marker 6 (3 states, scissor x2, draw; mem 2 ColorVert);
 *   ComposeDraw preamble 19, door capture 2, base quad 2,
 *   door transition 7, HUD 17, button header 7, trailer 5; DrawNativeMessage fixed part 12 (71 + 5 = 76);
 *   SM_PSP_DOOR_SNAP (synchronous only): kDsMaxCalls + kDsMaxMem, see DrawDoorSnap;
 *   button overlay: 3 set-up + 2 per run (colour, draw); message: 5 per glyph run (colour, mode, image, flush, draw)
 *   and 5 per icon (the same, its vertices counted in mem).
 *   kComposeEvolutionCalls (20) is head-room for edits of ComposeDraw; ComposeCheckUsed catches an edit that exceeds it.
 * Assumed of libgu, not provable from this source: every call above emits <= kGuCallMax (128) bytes (a draw ~20 B,
 * a texture image ~16 B, sceGuCopyImage ~40 B, sceGuClear ~64 B) and sceGuClear allocates <= kGuClearBytes.
 * ComposeCheckUsed measures the real list after every compose; if it ever exceeds the bound (an audit or libgu
 * assumption is wrong) async is switched off for good. That is a detector. The guard is psp_gl_guard.h, which refuses any
 * call that would not fit before it writes (and ComposeGeAsync then falls back to the synchronous compose). */
enum { kGuCallMax = 128, kGuClearBytes = 16 * 16, kComposeFixedCalls = 82, kComposeEvolutionCalls = 20,
       kComposeTail = 1024   /* head-room behind the compose commands: FINISH / END and the checks' own allocations */
};
#if SM_PSP_ASYNC
enum { kComposeAsyncMax = kComposeCap / 2 };
CASSERT(kComposeAsyncMax + kComposeTail <= kComposeCap);
#endif
#if SM_PSP_DOOR_SNAP
/* the generous per-call charge covers the measured worst case, and the snapshots' charge is small beside the list */
CASSERT(kDsExactBytes <= kDsMaxMem + kDsMaxCalls * kGuCallMax);
CASSERT(kDsMaxMem + kDsMaxCalls * kGuCallMax <= 8 * 1024);
CASSERT(kDsMaxMem + kDsMaxCalls * kGuCallMax + kComposeTail < kListBytes / 32);
#endif

static inline uint32_t GuMem(uint32_t n) {
  return ((n + 3u) & ~3u) + 8u;
}

/* One MsgDrawRun / MsgDrawStr over [s,e): its allocations and calls. */
static void MsgRunBound(const char *s, const char *e, uint32_t *mem, uint32_t *calls) {
  if (e <= s)
    return;
  float cx = 0.0f;
  int ng = 0, ni = 0;
  MsgWalk(s, e, &cx, 0.0f, 1.0f, kMsgPassCountGlyphs, NULL, &ng, 0);
  cx = 0.0f;
  MsgWalk(s, e, &cx, 0.0f, 1.0f, kMsgPassCountIcons, NULL, &ni, 0);
  if (ng) {
    *mem += GuMem((uint32_t)ng * sizeof(TileVert));
    *calls += 5;
  }
  *mem += (uint32_t)ni * GuMem(2 * sizeof(TileVert));
  *calls += (uint32_t)ni * 5;
}

static uint32_t ComposeBytesBound(uint32_t *mem_out, uint32_t *calls_out) {
  uint32_t mem = GuMem((kComposeStrips + kHudRectN + 2) * 2 * sizeof(TileVert)) + GuMem(2 * sizeof(TileVert)) +
                 GuMem(2 * sizeof(ColorVert)) + GuMem(2 * sizeof(ColorVert)) + GuMem(kGuClearBytes);
  uint32_t calls = kComposeFixedCalls + kComposeEvolutionCalls;
#if SM_PSP_DOOR_SNAP
  mem += kDsMaxMem;
  calls += kDsMaxCalls;
#endif
#if SM_PSP_GE_GLOW
  if (GlowStateOk()) {  /* worst case of GlowPrep + ComposeGlowAdd: the box list is only known inside ComposeDraw */
    mem += GuMem((uint32_t)(kGlowMaxBoxes * 2 * COMPOSE_STRIPS(kGameWidth) + 2 * COMPOSE_STRIPS(kGlowW1) + 4 * 2 * COMPOSE_STRIPS(kGlowW2) +
                            4 * 2 * COMPOSE_STRIPS(kGlowW3) + 4 * kComposeStrips + 4) * sizeof(TileVert)) + GuMem(8 * sizeof(ColorVert));
    calls += kGlowCalls;
#if SM_PSP_GE_GLOW_SRC & 1
    calls += 6u * (uint32_t)g_nobjseg;  /* DrawObjSegs again: per segment clut mode, clut load, texture mode, texture image, draw */
#endif
#if SM_PSP_GE_GLOW_SRC & 4
    for (int i = 0; i < g_nruns[2]; i++)  /* DrawBgPass(2, 0 / 1) again (GlowCollect skips an image / mosaic layer 3): bind + draw per cell */
      for (int pr = 0; pr < 2; pr++)
        for (int p = 0; p < 8; p++)
          if (g_geom[2][i][pr][p].n)
            calls += 6u;
#endif
  }
#endif
#if SM_PSP_GE_HAZE_LIGHT
  if (HazeLightOn()) {  /* the picture's mesh, and the glow chain's */
    mem += 2 * GuMem(kHazeVerts * sizeof(LitVert));
    calls += 2 * kHazeCalls;
  }
#endif
  for (int i = 0; i < kPspButtonIconSlots; i++) {
    const ButtonOverlay *b = &g_button_overlays[i];
    if (!b->pixels)
      continue;
    const uint32_t runs = (uint32_t)b->h;
    mem += runs * GuMem(4 * sizeof(TileVert));
    calls += 3 + runs * 2;
  }
  if (g_msg_id && g_msg_fade) {
    const PspMessageText *m = &g_psp_message_text[g_msg_id - 1];
    if (m->n) {
      MsgLine ln[kMsgMaxLines];
      const int nl = MsgLayout(m, ln);
      for (int i = 0; i < nl; i++)
        MsgRunBound(ln[i].s, ln[i].e, &mem, &calls);
      if (m->flags & PSP_MSG_SAVE) {
        MsgRunBound(">", ">" + 1, &mem, &calls);
        MsgRunBound("YES", "YES" + 3, &mem, &calls);
        MsgRunBound("NO", "NO" + 2, &mem, &calls);
      }
    }
  }
  *mem_out = mem;
  *calls_out = calls;
  return mem + calls * kGuCallMax;
}

/* Bytes in the open list so far (sceGuCheckList: current - start; reads only, writes nothing). */
static uint32_t CurListUsed(void) {
  const int u = sceGuCheckList();
  return u > 0 ? (uint32_t)u : 0u;
}

#if SM_PSP_DOOR_SNAP
/* Synchronous compose with snapshots. Both callers of ComposeDraw (ComposeGeFrame and the software path) start g_gu_list right
 * before it, so CurListUsed(g_gu_list) is what the compose has emitted so far. The rest of the composition is charged with the
 * whole bound again (the commands already emitted are counted twice: conservative), snapshots included. If that plus the tail
 * does not fit the list, the transition is dropped before it adds a single command. The snapshots' share of the bound is
 * 29 * 128 + 720 = 4432 B of the 512 KB list; this never trips in practice, it is the guard, not the budget. */
static bool DsListFits(void) {
  uint32_t mem, calls;
  const uint32_t bound = ComposeBytesBound(&mem, &calls);
  return CurListUsed() + bound + kComposeTail <= (uint32_t)kListBytes;
}
#endif

#if SM_PSP_ASYNC
/* After the compose commands are built (every build): the compose commands took CurListUsed() - base bytes (base: what the
 * list held before them, 0 for a list of their own). Detector of a wrong estimate above, not a guard: the guard is the
 * check psp_gl_guard.h makes before every call. */
static void ComposeCheckUsed(uint32_t bound, uint32_t mem, uint32_t calls, uint32_t base) {
  const uint32_t total = CurListUsed();
  const uint32_t used = total > base ? total - base : 0u;
  (void)mem;
  (void)calls;
  PROF(if (used > g_comp_used_hw) g_comp_used_hw = used;
       if (calls) {
         const uint32_t pc = (used > mem ? used - mem : 0) / calls;
         if (pc > g_comp_call_hw)
           g_comp_call_hw = pc;
       })
  if (used > bound) {
    g_async_ok = false;
    g_async_viol++;
    PspLog("async: compose list used %u > bound %u (mem %u, calls %u): async disabled", (unsigned)used, (unsigned)bound,
           (unsigned)mem, (unsigned)calls);
  }
}

/* The completion marker: the last draw of the compose commands. A 4x1 sprite of a per-frame colour at x = 496 (the panel
 * shows 480 columns of the 512 wide buffer), the scissor widened for it and put back. The colour is chosen so that the
 * RGB565 value is exactly c (low bits clear, nothing to round); c is odd, so never the 0 of a cleared buffer, and differs
 * from the marker the same buffer held three frames earlier. GeWait() compares it after the GE sync. */
static void ComposeMarker(void) {
  const uint16_t c = (uint16_t)((g_frame * 7919u) | 1u);
  const uint32_t col = 0xff000000u | (uint32_t)(((c >> 11) & 31u) << 3) << 16 | (uint32_t)(((c >> 5) & 63u) << 2) << 8 |
                       (uint32_t)((c & 31u) << 3);
  ColorVert *v = sceGuGetMemory(2 * (int)sizeof(ColorVert));
  v[0] = (ColorVert){col, kSentX, kSentY, 0};
  v[1] = (ColorVert){col, kSentX + 4, kSentY + 1, 0};
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuScissor(0, 0, 512, kPspHeight);
  GeDraw(kVtColor, 2, v);
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  g_pend_sent = c;
}

/* Builds the compose commands (same as ComposeGeFrame). SM_PSP_ASYNC == 1: into g_gu_list2 behind the scene list the GE
 * already has (own sceGuStart / sceGuFinish). SM_PSP_ASYNC == 2: appended to the scene list, which is still open
 * (Present finishes it afterwards). Returns false, with nothing emitted and no pending / buffer state touched, if async
 * is off, the bound does not fit or (== 1) the list cannot be enqueued; the caller then finishes the frame
 * synchronously (sceGuStart of the next list re-initialises the GU). */
static bool ComposeGeAsync(void) {
  if (!g_async_ok || g_gl_finish_fail)  /* g_gl_finish_fail: a sceGuFinish failed once, stay synchronous */
    return false;
  uint32_t mem, calls;
  const uint32_t bound = ComposeBytesBound(&mem, &calls);
  PROF(if (bound > g_comp_bound_hw) g_comp_bound_hw = bound;)
#if SM_PSP_ASYNC == 1
  const uint32_t base = 0;
  if (bound > (uint32_t)kComposeAsyncMax) {
    g_async_nofit++;
    return false;
  }
  if (GlStart(g_gu_list2, kComposeListBytes) < 0) {
    g_async_fail++;
    return false;
  }
#else
  if (!g_gl_open)
    return false;
  const uint32_t base = CurListUsed();
  if (base + bound + kComposeTail > (uint32_t)kListBytes) {
    g_async_nofit++;
    return false;
  }
#endif
  if (setjmp(g_gl_jb)) {
    /* A call was refused before it wrote (the estimate above was wrong). The list is finished and the GE waited for; the
     * caller finishes the frame synchronously, composing from the copy if it was already in the list. */
    GlAborted("compose-async");
    g_compose_resume = g_compose_copied;
    g_async_ok = false;
    g_async_viol++;
    return false;
  }
  GL_ARM();
  g_compose_copied = false;
  SelectDrawBuffer();
  sceGuCopyImage(GU_PSM_5650, kLeft, kTop, kGameWidth, kSrcRows, 512, (void *)(uintptr_t)(0x04000000u | g_draw_off), 0, 0,
                 kGameWidth, (void *)(uintptr_t)(0x04000000u | (uint32_t)kVramTexture));
  sceGuTexSync();
  g_compose_copied = true;
  const ComposeSrc src = {g_texture, GU_PSM_5650, kGameWidth, 256, kGameWidth};
  ComposeDraw(&src);
  ComposeMarker();
  ComposeCheckUsed(bound, mem, calls, base);
  GL_DISARM();
#if SM_PSP_ASYNC == 1
  sceGuFinish();
#endif
  return true;
}
#endif  /* SM_PSP_ASYNC */
#endif  /* SM_PSP_ASYNC || SM_PSP_DOOR_SNAP */

/* Splits [x0,x1) at multiples of 64 framebuffer pixels. PPSSPP's GE notes say
 * 2D images are cheaper to draw in 64-pixel-wide vertical strips on 16-bit
 * targets. Returns the strip count; edges[0..n] are the boundaries. */
static int StripEdges(int x0, int x1, int *edges) {
#if SM_PSP_GE_STRIPS
  int n = 0;
  edges[n++] = x0;
  for (int x = (x0 / 64 + 1) * 64; x < x1; x += 64)
    edges[n++] = x;
  edges[n++] = x1;
  return n - 1;
#else
  edges[0] = x0;
  edges[1] = x1;
  return 1;
#endif
}

static void BindBg(int l, int b, int p) {
  PROF(g_c_clut++;)
  sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
  if (l < 2) {
    sceGuClutLoad(2, g_clut4[b][p]);
    sceGuTexMode(GU_PSM_T4, 0, 0, SWZ_FLAG(SM_PSP_SWZ_BG));
    sceGuTexImage(0, 256, 512, 256, g_atlas4);
  } else {
    sceGuClutLoad(1, g_clut2[b][p]);
    sceGuTexMode(GU_PSM_T4, 0, 0, SWZ_FLAG(SM_PSP_SWZ_BG));
    sceGuTexImage(0, 512, 512, 512, g_atlas2);
  }
}

/* Gate 4t-d: BG3 with the alpha 0x80 palette (half-colour subscreen target). */
static void BindBgHalf(int p) {
  PROF(g_c_clut++;)
  sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
  sceGuClutLoad(1, g_clut2h[p]);
  sceGuTexMode(GU_PSM_T4, 0, 0, SWZ_FLAG(SM_PSP_SWZ_BG));
  sceGuTexImage(0, 512, 512, 512, g_atlas2);
}

#if SM_PSP_MOS_GE
/* Gate 5s: renders the source image S of every mosaic layer into its slot of the subscreen target's VRAM. Depth is
 * not written (mask), the stencil value of each pixel (alpha of the 8888 target) is the priority tag kMosTag. */
static void DrawMosSrc(void) {
  bool any = false;
  for (int l = 0; l < kLayers; l++)
    any |= g_mos[l].on;
  if (!any)
    return;
  sceGuEnable(GU_DEPTH_TEST);
  sceGuDepthMask(GU_TRUE);
  sceGuDepthFunc(GU_ALWAYS);
  sceGuEnable(GU_STENCIL_TEST);
  for (int l = 0; l < kLayers; l++) {
    const MosSrc *s = &g_msrc[l];
    if (!g_mos[l].on)
      continue;
    sceGuDrawBufferList(GU_PSM_8888, (void *)(uintptr_t)(kVramSub + (uint32_t)l * kMosImgBytes), kMosDim);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
    sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
    GeDraw(kVtColor, 2, s->clr);  /* transparent: tag 0 */
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    for (int pr = 0; pr < 2; pr++) {
      sceGuStencilFunc(GU_ALWAYS, kMosTag[pr], 0xff);
      for (int p = 0; p < 8; p++)
        if (s->cnt[pr][p]) {
          BindBg(l, kClutBright, p);
          GeDraw(kVtTile, s->cnt[pr][p] * 2, s->sv[pr][p]);
        }
    }
  }
  sceGuDisable(GU_STENCIL_TEST);  /* the 5650 main target has no stencil bits */
  sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)g_draw_off, 512);
  sceGuDepthMask(GU_FALSE);
  sceGuDepthFunc(GU_GEQUAL);
}
#endif

/* Mosaic layer: the stretched image of this priority, one nearest-sampled quad per run and window span. */
static void DrawMosPass(int l, int pr) {
  const MosLayer *m = &g_mos[l];
  if (!m->any[pr])
    return;
  sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
#if SM_PSP_MOS_GE
  sceGuTexImage(0, kMosDim, kMosDim, kMosDim, g_vram_u + kVramSub + (uint32_t)l * kMosImgBytes);
  sceGuTexFlush();  /* the image was just rendered by the GE */
  sceGuTexSync();
  sceGuAlphaFunc(GU_EQUAL, kMosTag[pr], 0xff);  /* this priority's pixels only */
#else
  sceGuTexImage(0, kMosDim, kMosDim, kMosDim, g_mosd[l][pr]);
  sceGuTexFlush();
#endif
  for (int i = 0; i < g_nruns[l]; i++) {
    const BgSnap *wb = &g_lines[g_runs[l][i].first].bg[l];
    int wa[2], we[2];
    int ns = WinSpans(wb->wm, wb->wl, wb->wr, wa, we);
    for (int k = 0; k < ns; k++)
      GeDraw(kVtTile, 2, m->v + ((i * 2 + k) * 2 + pr) * 2);
  }
#if SM_PSP_MOS_GE
  sceGuAlphaFunc(GU_NOTEQUAL, 0, 0xff);
#endif
}

/* Gate 5l: strips of a layer image for one tile priority (the CLUT hides the other priority's pixels). */
static void DrawImgPass(int l, int pr) {
  if (g_imgn[l][pr] == 0)
    return;
  int slot = l - 1, iw = g_img[slot].wide ? 512 : 256, ih = g_img[slot].high ? 512 : 256;
  PROF(g_c_clut++;)
  sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
  sceGuClutLoad(32, g_climg[slot][pr]);
  sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
  sceGuTexImage(0, iw, ih, iw, g_img_tex[slot]);
  sceGuTexFlush();  /* the image is updated by the CPU between frames */
  sceGuTexWrap(GU_REPEAT, GU_REPEAT);
  GeDraw(kVtTile, g_imgn[l][pr], g_imgv[l][pr]);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

static void DrawBgPass(int l, int pr) {
  if (g_img_on[l]) {
    DrawImgPass(l, pr);
    return;
  }
  if (g_mos[l].on) {
    DrawMosPass(l, pr);
    return;
  }
  /* One draw per palette: the vertices of a palette are contiguous across runs (see the geometry pass), so
   * consecutive runs with the same brightness (same CLUT) merge. Runs cover disjoint rows: order is irrelevant. */
  int nr = g_nruns[l];
  for (int p = 0; p < 8; p++) {
    for (int i = 0; i < nr;) {
      const Geom *g = &g_geom[l][i][pr][p];
      if (g->n == 0) {
        i++;
        continue;
      }
      const BgVert *v = g->v;
      int n = g->n, j = i + 1;
      for (; j < nr; j++) {
        const Geom *h = &g_geom[l][j][pr][p];
        if (h->n == 0)
          continue;
        if (h->v != v + n)
          break;
        n += h->n;
      }
      BindBg(l, kClutBright, p);
      GeDraw(kVtBg, n, v);
      i = j;
    }
  }
}

/* Gate 5p: strips of a layer image on the subscreen target (half: the alpha 0x80 CLUT of the half-colour target). */
static void DrawSubImgPass(int l, int pr, bool half) {
  if (g_simgn[l][pr] == 0)
    return;
  int slot = l - 1, iw = g_img[slot].wide ? 512 : 256, ih = g_img[slot].high ? 512 : 256;
  PROF(g_c_clut++;)
  sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
  sceGuClutLoad(32, half ? g_climgh[slot][pr] : g_climg[slot][pr]);
  sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
  sceGuTexImage(0, iw, ih, iw, g_img_tex[slot]);
  sceGuTexFlush();  /* the image is updated by the CPU between frames */
  sceGuTexWrap(GU_REPEAT, GU_REPEAT);
  GeDraw(kVtTile, g_simgn[l][pr], g_simgv[l][pr]);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

/* Same merging for the subscreen blend runs (grouped by layer; depth markers or disjoint rows decide overlaps). */
static void DrawBlendBgs(bool half) {
  for (int l = 0; l < kLayers; l++) {
    if (g_simg_on[l]) {  /* gate 5p: the layer comes from its image, one strip per run */
      for (int pr = 0; pr < 2; pr++)
        DrawSubImgPass(l, pr, half && l == 2);
      continue;
    }
    for (int pr = 0; pr < 2; pr++) {
      for (int p = 0; p < 8; p++) {
        for (int i = 0; i < g_nbruns;) {
          const Geom *g = &g_bgeom[i][pr][p];
          if (g_bruns[i].l != l || g->n == 0) {
            i++;
            continue;
          }
          const BgVert *v = g->v;
          int n = g->n, j = i + 1;
          for (; j < g_nbruns; j++) {
            const Geom *h = &g_bgeom[j][pr][p];
            if (g_bruns[j].l != l || h->n == 0)
              continue;
            if (h->v != v + n)
              break;
            n += h->n;
          }
          if (half && l == 2)
            BindBgHalf(p);
          else
            BindBg(l, kClutBright, p);
          GeDraw(kVtBg, n, v);
          i = j;
        }
      }
    }
  }
}

/* only_excluded: draw just the sprites that never take part in colour math
 * (OBJ palettes 0-3 always; 4-7 when the OBJ math bit is off). */
static void DrawObjSegs(const ObjSeg *segs, int n, const BgVert *base, int bright, bool only_excluded, int mathmask) {
  for (int i = 0; i < n; i++) {
    const ObjSeg *sg = &segs[i];
    if (only_excluded && !(sg->pal < 4 || !(mathmask & 0x10)))
      continue;
    PROF(g_c_clut++;)
    sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
    sceGuClutLoad(2, g_clutobj[bright][sg->pal]);
    sceGuTexMode(GU_PSM_T4, 0, 0, SWZ_FLAG(SM_PSP_SWZ_BG));
    sceGuTexImage(0, 256, 512, 256, g_atlas4);
    GeDraw(kVtBg, sg->n, base + sg->first);
  }
}

static void DrawM7(void) {
#if SM_PSP_M7_BLK
  if (g_m7_blk) {  /* gate 5w: a bind of the block's window of the swizzled plane and one triangle draw per block */
    PROF(g_c_clut++;)
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, g_clut7);
    sceGuTexMode(GU_PSM_T8, 0, 0, GU_TRUE);
    PROF(uint32_t td0 = Now();)
    for (int b = 0; b < g_m7_bnblk; b++) {
      const M7BBlk *bk = &g_m7bblk[b];
      /* swizzled: block row wy / 8 is 8 * kM7PlaneP B, block column wx / 16 is 128 B (wx % 16 == wy % 8 == 0) */
      sceGuTexImage(0, kM7WinMax, kM7WinMax, kM7PlaneP, g_m7pl + (bk->wy >> 3) * (kM7PlaneP * 8) + (bk->wx >> 4) * 128);
      PROF(g_c_draws++; g_c_verts += (uint32_t)bk->n * 6;)
      sceGuDrawArray(GU_TRIANGLES, kVtTile, bk->n * 6, NULL, g_m7v + bk->first * 6);
    }
    PROF(g_m7b_t_draw += Now() - td0;)
    return;
  }
#endif
  if (g_m7_img) {
    PROF(g_c_clut++;)
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, g_clut7);
    sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
    sceGuTexImage(0, 256, 256, 256, g_m7img);
    sceGuTexFlush();  /* the image is rewritten every frame */
    GeDraw(kVtTile, g_m7_nrng * 2, g_m7v);
    return;
  }
  if (g_m7_nq == 0)
    return;
  PROF(g_c_clut++;)
  sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
  sceGuClutLoad(32, g_clut7);
  sceGuTexMode(GU_PSM_T8, 0, 0, SWZ_FLAG(SM_PSP_SWZ_M7));
  sceGuTexImage(0, 128, 128, 128, g_atlas7);
  GeDraw(kVtTile, g_m7_nq * 2, g_m7v);
}

static void ScissorRows(int r0, int r1) {
  int y0 = kTop + r0, y1 = kTop + r1;
  if (g_scissor_mode == 2)
    sceGuScissor(0, y0, kPspWidth, y1 - y0);
  else
    sceGuScissor(0, y0, kPspWidth, y1);
}

/* Restore the layers whose colour-math bit is off: redraw them where their depth marker is still in the
 * buffer (the overlay neither tests nor writes depth). */
static void RestoreLayers(int mask, int obj_bright, const ColorVert *bd, int nbd) {
  sceGuEnable(GU_DEPTH_TEST);
  sceGuDepthFunc(GU_EQUAL);
  sceGuDepthMask(GU_TRUE);  /* GU_TRUE masks depth writes */
  if (!(mask & 0x20)) {
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    GeDraw(kVtColor, nbd, bd);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
  }
  if (!(mask & 1))
    DrawM7();
  for (int l = 0; l < kLayers; l++)
    if (!(mask & (1 << l)))
      for (int pr = 0; pr < 2; pr++)
        DrawBgPass(l, pr);
  DrawObjSegs(g_objseg, g_nobjseg, g_objbase, obj_bright, true, mask);
  sceGuDepthMask(GU_FALSE);
  sceGuDisable(GU_DEPTH_TEST);
}

/* Gate 5d: display-list guard. sceGuGetMemory() carves vertex data out of the display list and never checks the
 * list end, so the frame must be known to fit before the list is opened. This is an upper bound of everything one frame
 * writes: every vertex buffer (all sizes are known after pass A), the commands of every draw (a texture bind plus a
 * DrawArray is at most ~16 words, 96 B is used), the state set-up, and the 8 B header of each sceGuGetMemory. */
#if SM_PSP_GE_STRIPS
enum { kMaxStrips = 5 };
#else
enum { kMaxStrips = 1 };
#endif
enum { kDrawCmdBytes = 96, kListFixedBytes = 8 * 1024, kListAllocBytes = 160 * 16, kListSlackBytes = 16 * 1024 };
static uint32_t g_peak_tiles, g_peak_list, g_peak_vtx;  /* since the last log line */

static uint32_t ListBytesNeeded(int total, int obj_quads, int obj_np, bool blend, bool tint, bool sobj, uint32_t *vtx) {
  uint32_t big = ((uint32_t)total + (uint32_t)obj_quads) * 2 * sizeof(BgVert) +
                 (uint32_t)(g_m7_nq + g_m7_nrng + g_m7_bnseg * 3 + g_img_nv / 2) * 2 * sizeof(TileVert);  /* tiles, sprites, mode 7 (quads, image rows, 6 vertices per block segment), layer image strips */
  uint32_t geom = 0;
  uint32_t mos_draws = 0;
  for (int l = 0; l < kLayers; l++) {
    if (!g_mos[l].on)
      continue;
    geom += (uint32_t)g_nruns[l] * 8 * sizeof(TileVert);
    mos_draws += (uint32_t)g_nruns[l] * 4;
#if SM_PSP_MOS_GE
    geom += (uint32_t)g_msrc[l].nq * 2 * sizeof(TileVert) + 2 * sizeof(ColorVert);  /* source sprites + clear */
    mos_draws += 24;  /* source draws: clear + up to 16 priority/palette draws (also counted per pass: an upper bound) */
#endif
  }
#if SM_PSP_VTX_POOL
  *vtx = big + kVtxPadBytes;  /* built in the cached pool, not in the list */
#else
  *vtx = 0;
  geom += big;
#endif
  geom += (uint32_t)g_line_count * (kMaxStrips * 2 + 2) * sizeof(ColorVert);  /* backdrop + brightness rects */
  if (blend)  /* subscreen composite quads + subscreen window masks, per row */
    geom += (uint32_t)g_line_count * (4 * sizeof(TileVert) + 18 * sizeof(ColorVert));
#if SM_PSP_GE_SMOOTH_TINT
  geom += (uint32_t)g_ntint * 4 * sizeof(TileVert);  /* one sprite per window span of a run: at most 2 spans = 4 vertices */
#else
  geom += (uint32_t)g_ntint * 4 * sizeof(ColorVert);
#endif
  uint32_t passes = 1 + (blend ? (g_nbband > 1 ? (uint32_t)g_nbband : 1u) : 0u) + (tint ? (uint32_t)g_ntr : 0u);
  uint32_t per_pass = 2 * kLayers * 8 + 2 + mos_draws + (uint32_t)g_nspr * (uint32_t)obj_np + (uint32_t)g_m7_bnblk;  /* BG, M7 + backdrop (+ a bind and a draw per block), OBJ segments */
  uint32_t draws = passes * per_pass + 48 + (sobj ? (uint32_t)g_nspr : 0u) + (uint32_t)g_ntint + 16;
  return geom + draws * kDrawCmdBytes + kListFixedBytes + kListAllocBytes + kListSlackBytes;
}

#if SM_PSP_S5I
/* Gate 5i-a (log only, nothing here changes drawing): per drawn GE frame, the quads, runs and thin runs (2 rows or
 * fewer) of each BG layer, of the subscreen blend and of OBJ, and three probes for a cached layer image: scrollonly
 * (several runs that differ only in scroll: HDMA waves), mapsame (tilemap words equal to the previous frame) and
 * mapstatic (mapsame, CGRAM unchanged and no tile decoded this frame). */
enum { kS5Sub = kLayers, kS5Obj = kLayers + 1, kS5Cols = kLayers + 2, kS5Heavy = 5000, kS5HeavyMax = 12 };
typedef struct S5Col { uint32_t n, runs, rows, thin, quads, qmax, scrollonly, mapsame, mapstatic; } S5Col;
static struct {
  S5Col c[kS5Cols];
  uint32_t frames, qsum, qmax, tdec0, cgsame, blend, tint, bands, bandmax, heavy, submask;
} g_s5;
static uint32_t g_s5_hash[kLayers], g_s5_cg;
static bool g_s5_hash_ok[kLayers];

/* Tilemap words of layer l (1, 2 or 4 screens) plus map/tile base and size flags. */
static uint32_t S5iMapHash(const BgSnap *s) {
  uint32_t h = 2166136261u;
  h = (h ^ s->mapadr) * 16777619u;
  h = (h ^ s->tileadr) * 16777619u;
  h = (h ^ (uint32_t)(s->flags & 3)) * 16777619u;
  int n = 1024 << ((s->flags & 1) + ((s->flags >> 1) & 1));
  for (int i = 0; i < n; i++)
    h = (h ^ g_vram[(s->mapadr + i) & 0x7fff]) * 16777619u;
  return h;
}

static void S5iAdd(S5Col *c, uint32_t runs, uint32_t rows, uint32_t thin, uint32_t quads) {
  c->n++;
  c->runs += runs;
  c->rows += rows;
  c->thin += thin;
  c->quads += quads;
  if (quads > c->qmax)
    c->qmax = quads;
}

static void S5iFrame(int total, int obj_quads, bool blend, bool tint) {
  uint32_t cg = 2166136261u;
  for (int i = 0; i < 256; i++)
    cg = (cg ^ g_cgram[i]) * 16777619u;
  bool cgsame = cg == g_s5_cg;
  g_s5_cg = cg;
  bool tdec0 = g_c_tdec == 0;
  uint32_t lrows[kLayers] = {0}, lthin[kLayers] = {0}, lquads[kLayers] = {0};
  bool lso[kLayers] = {false}, lms[kLayers] = {false};
  for (int l = 0; l < kLayers; l++) {
    int nr = g_nruns[l];
    if (nr == 0) {
      g_s5_hash_ok[l] = false;
      continue;
    }
    const BgSnap *b0 = &g_lines[g_runs[l][0].first].bg[l];
    bool same = true;
    for (int i = 0; i < nr; i++) {
      const Run *r = &g_runs[l][i];
      const BgSnap *b = &g_lines[r->first].bg[l];
      int h = r->r1 - r->r0;
      lrows[l] += (uint32_t)h;
      if (h <= 2)
        lthin[l]++;
      for (int pr = 0; pr < 2; pr++)
        for (int p = 0; p < 8; p++)
          lquads[l] += (uint32_t)g_counts[l][i][pr][p];
      if (b->mapadr != b0->mapadr || b->tileadr != b0->tileadr || ((b->flags ^ b0->flags) & 7))
        same = false;
    }
    uint32_t mh = S5iMapHash(b0);
    lso[l] = nr > 1 && same;
    lms[l] = g_s5_hash_ok[l] && g_s5_hash[l] == mh;
    g_s5_hash[l] = mh;
    g_s5_hash_ok[l] = true;
    S5Col *c = &g_s5.c[l];
    S5iAdd(c, (uint32_t)nr, lrows[l], lthin[l], lquads[l]);
    c->scrollonly += lso[l];
    c->mapsame += lms[l];
    c->mapstatic += lms[l] && cgsame && tdec0;
  }
  uint32_t srows = 0, sthin = 0, squads = 0, smask = 0;
  for (int i = 0; i < g_nbruns; i++) {
    int h = g_bruns[i].r1 - g_bruns[i].r0;
    smask |= 1u << g_bruns[i].l;
    srows += (uint32_t)h;
    if (h <= 2)
      sthin++;
    for (int pr = 0; pr < 2; pr++)
      for (int p = 0; p < 8; p++)
        squads += (uint32_t)g_bcounts[i][pr][p];
  }
  if (g_nbruns)
    S5iAdd(&g_s5.c[kS5Sub], (uint32_t)g_nbruns, srows, sthin, squads);
  g_s5.submask |= smask;
  if (g_nspr || obj_quads)
    S5iAdd(&g_s5.c[kS5Obj], (uint32_t)g_nspr, (uint32_t)g_ovf_rows, 0, (uint32_t)obj_quads);
  uint32_t q = (uint32_t)total + (uint32_t)obj_quads;
  g_s5.frames++;
  g_s5.qsum += q;
  if (q > g_s5.qmax)
    g_s5.qmax = q;
  g_s5.tdec0 += tdec0;
  g_s5.cgsame += cgsame;
  g_s5.blend += blend;
  g_s5.tint += tint;
  g_s5.bands += (uint32_t)g_nbband;
  if ((uint32_t)g_nbband > g_s5.bandmax)
    g_s5.bandmax = (uint32_t)g_nbband;
  if (q >= (uint32_t)kS5Heavy && g_s5.heavy++ < (uint32_t)kS5HeavyMax) {
    PspLog("S5I f=%u q=%d obj=%d tdec=%u cg=%d blend=%d tint=%d bands=%d ovf=%d", (unsigned)g_frame, total, obj_quads,
           (unsigned)g_c_tdec, (int)cgsame, (int)blend, (int)tint, g_nbband, g_ovf_rows);
    for (int l = 0; l < kLayers; l++)
      if (lrows[l])
        PspLog("  BG%d runs=%d rows=%u thin=%u q=%u scrollonly=%d mapsame=%d", l + 1, g_nruns[l], (unsigned)lrows[l],
               (unsigned)lthin[l], (unsigned)lquads[l], (int)lso[l], (int)lms[l]);
    if (g_nbruns)
      PspLog("  SUB runs=%d layers=%x rows=%u thin=%u q=%u", g_nbruns, (unsigned)smask, (unsigned)srows, (unsigned)sthin,
             (unsigned)squads);
  }
}

/* Called with the 300-frame log; averages are per frame in which the layer was drawn (n). */
static void S5iLog(void) {
  if (g_s5.frames == 0)
    return;
#define S5AVG(s, n) ((unsigned)((n) ? (s) / (n) : 0))
  PspLog("S5 n=%u quads avg=%u max=%u tdec0=%u cgsame=%u blend=%u tint=%u bands avg=%u max=%u heavy=%u",
         (unsigned)g_s5.frames, S5AVG(g_s5.qsum, g_s5.frames), (unsigned)g_s5.qmax, (unsigned)g_s5.tdec0,
         (unsigned)g_s5.cgsame, (unsigned)g_s5.blend, (unsigned)g_s5.tint, S5AVG(g_s5.bands, g_s5.frames),
         (unsigned)g_s5.bandmax, (unsigned)g_s5.heavy);
  for (int l = 0; l < kLayers; l++) {
    const S5Col *c = &g_s5.c[l];
    if (c->n)
      PspLog("  BG%d n=%u runs=%u thin=%u rows=%u q=%u/%u scrollonly=%u mapsame=%u mapstatic=%u", l + 1,
             (unsigned)c->n, S5AVG(c->runs, c->n), S5AVG(c->thin, c->n), S5AVG(c->rows, c->n), S5AVG(c->quads, c->n),
             (unsigned)c->qmax, (unsigned)c->scrollonly, (unsigned)c->mapsame, (unsigned)c->mapstatic);
  }
  const S5Col *s = &g_s5.c[kS5Sub], *o = &g_s5.c[kS5Obj];
  if (s->n)
    PspLog("  SUB n=%u layers=%x runs=%u thin=%u rows=%u q=%u/%u", (unsigned)s->n, (unsigned)g_s5.submask, S5AVG(s->runs, s->n),
           S5AVG(s->thin, s->n), S5AVG(s->rows, s->n), S5AVG(s->quads, s->n), (unsigned)s->qmax);
  if (o->n)
    PspLog("  OBJ n=%u spr=%u ovfrows=%u q=%u/%u", (unsigned)o->n, S5AVG(o->runs, o->n), S5AVG(o->rows, o->n),
           S5AVG(o->quads, o->n), (unsigned)o->qmax);
  {  /* capture-relevant PPU register writes in this window, by register (what still forces a full line capture) */
    char buf[200];
    int len = 0;
    buf[0] = 0;
    for (int a = 0; a < 64 && len < (int)sizeof(buf) - 16; a++)
      if (g_ppu_gen_hist[a])
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, " %02x=%u", a, (unsigned)g_ppu_gen_hist[a]);
    if (len)
      PspLog("  regbump:%s", buf);
    memset(g_ppu_gen_hist, 0, sizeof(g_ppu_gen_hist));
  }
#undef S5AVG
  memset(&g_s5, 0, sizeof(g_s5));
}
#define S5I_FRAME(total, oq, blend, tint) S5iFrame(total, oq, blend, tint)
#define S5I_LOG() S5iLog()
#else
#define S5I_FRAME(total, oq, blend, tint) ((void)0)
#define S5I_LOG() ((void)0)
#endif

// Returns false (nothing drawn, GE idle) if the frame must use the software image. noinline: DrawGe() below holds the
// setjmp, which must not change how this large function is compiled.
static __attribute__((noinline)) bool DrawGeBody(void) {
  g_t_draw_start = Now();
  g_last_reason = 0;
  g_ent_top = 0;
  g_rows_top = 0;
  g_m7_nq = 0;
  g_m7w_frame = false;
  g_m7_img = false;
  g_m7_blk = false;
  g_m7_bnseg = g_m7_bnblk = 0;
  g_m7_nrng = 0;
  g_ovf_rows = 0;
  g_glow_obj_n = 0;
  g_verify_now = false;
  g_bright_rows = 0;
  g_win_rows = 0;
  g_frame_multisub = false;
  g_mos_frame = false;
  g_nbband = 0;
  g_prev_rows = 0;
  g_cw_rows = 0;
  g_sf_frame = false;
  g_sf_zero = false;
  g_sfo_frame = false;
  g_sh_frame = false;
  g_shobj_frame = false;
  g_shn_frame = false;
  g_w2_frame = false;
  g_img_frame = false;
  g_imgh_frame = false;
  g_img_nv = 0;
  memset(g_img_on, 0, sizeof(g_img_on));
  g_simg_frame = false;
  memset(g_simg_on, 0, sizeof(g_simg_on));
  if (g_frame_reasons) {
    CountReasons(g_frame_reasons);
    return false;
  }
  for (int r = 0; r < g_line_count; r++)
    g_prev_rows += g_lines[r].prevnoop;

  if (++g_stamp == 0) {
    memset(g_seen2, 0, sizeof(g_seen2));
    memset(g_seen4, 0, sizeof(g_seen4));
    memset(g_seen7, 0, sizeof(g_seen7));
    g_clut7_stamp = 0;
    g_clut2h_stamp = 0;
    memset(g_clut2_stamp, 0, sizeof(g_clut2_stamp));
    memset(g_clut4_stamp, 0, sizeof(g_clut4_stamp));
    memset(g_clutobj_stamp, 0, sizeof(g_clutobj_stamp));
    memset(g_climg_stamp, 0, sizeof(g_climg_stamp));
    memset(g_climgh_stamp, 0, sizeof(g_climgh_stamp));
    memset(g_rc, 0, sizeof(g_rc));
    g_stamp = 1;
  }

  /* Subscreen OBJ (gate 4i): drawn into the multisub target. Needs one contiguous row range,
   * brightness 15, no OBJ window on the subscreen, and multisub not disabled by the audit. */
  bool sobj = SubObjHit();
  int s0 = 0, s1 = 0;
  if (sobj) {
    bool have = false, ok = g_multisub_ok;
    for (int r = 0; r < g_line_count; r++) {
      if (!g_lines[r].subobj)
        continue;
      if (!have) {
        s0 = r;
        have = true;
      } else if (r != s1) {
        ok = false;
      }
      s1 = r + 1;
    }
    if (!ok) {
      CountReasons(kReasonSubObj);
      return false;
    }
  }

  /* Gate 4t-c: a subscreen BG layer added with a non-zero fixed colour (X-ray visor). In ppu.c every math pixel gets
   * the subscreen pixel's colour where its index is non-zero (also black), the fixed colour everywhere else. So the
   * target is cleared to the fixed colour, every non-empty tile matters, the colour goes onto every row of the blend,
   * and layer windows are painted over the target in the fixed colour (only valid with one subscreen layer). */
  bool subfix = false, sfseen = false, subhalf = false;
  uint16_t subfixc = 0;
  int sf0 = -1, sf1 = 0, nsm = 0, nhalf = 0;
  for (int r = 0; r < g_line_count; r++) {
    const LineSnap *ls = &g_lines[r];
    if (!ls->submath)
      continue;
    if (!sfseen) {
      sfseen = true;
      subfixc = ls->subfix;
      sf0 = r;
    } else if (ls->subfix != subfixc) {
      NOTE_SUBMULTI("subfix-varies", r, subfixc, ls->subfix);
      CountReasons(kReasonSubMulti);
      return false;
    }
    nsm++;
    nhalf += ls->subhalf;
    sf1 = r + 1;
  }
  if (nhalf) {  /* gate 4t-d: all subscreen-math rows of the frame must be half rows */
    if (nhalf != nsm) {
      NOTE_SUBMULTI("half-varies", sf0, nhalf, nsm);
      CountReasons(kReasonSubMulti);
      return false;
    }
    for (int r = 0; r < g_line_count; r++)  /* gate 4u: subscreen OBJ rows must be half rows too */
      if (g_lines[r].subobj && !g_lines[r].subhalf) {
        NOTE_SUBMULTI("half-obj-row", r, 0, 0);
        CountReasons(kReasonSubMulti);
        return false;
      }
    subhalf = true;
  }
  if (subfixc != 0 || subhalf) {
    subfix = true;
    if ((sobj && !subhalf && !g_sfo_ok) || g_ge_level != 1 || !(subhalf ? g_sh_ok : g_sf_ok)) {  /* level 2 (hardware bisect): tint frames use the software image */
      CountReasons(kReasonMathTint);
      return false;
    }
    if (sobj && !subhalf && (s0 < sf0 || s1 > sf1)) {  /* gate 5e: sprite rows must lie inside the fixed-colour rows */
      NOTE_SUBMULTI("subfix-obj-rows", s0, sf0, sf1);
      CountReasons(kReasonSubObj);
      return false;
    }
    for (int r = sf0; r < sf1; r++)
      if (!g_lines[r].submath) {
        NOTE_SUBMULTI("subfix-gap", r, sf0, sf1);
        CountReasons(kReasonSubMulti);
        return false;
      }
    if (sobj && !subhalf)  /* gate 5e: a window mask painted over the target would also erase the sprites */
      for (int r = sf0; r < sf1; r++)
        for (int l = 0; l < kLayers; l++)
          if (g_lines[r].bg[l].sm) {
            CountReasons(kReasonSubWin);
            return false;
          }
  }

  /* Subscreen classification (fixed == 0, no half colour; see LineReasons).
   * Subscreen pixels only change the picture where a subscreen BG layer shows a
   * non-black pixel. No such layer: the add is a no-op. Exactly one such layer:
   * real blending below. More than one: the subscreen layers are composited by priority
   * in an offscreen target first (multisub). */
  g_frame_subnoop = false;
  g_nbruns = 0;
  int sub_layer = -1;
  bool multisub = sobj || subfix;
  int sublayers = 0;
  for (int l = 0; l < kLayers; l++) {
    int n = 0;
    for (int r = 0; r < g_line_count; r++) {
      if (!g_lines[r].submath || !(g_lines[r].bg[l].flags & 8))
        continue;
      if (g_lines[r].mosmask & (1 << l)) {  /* mosaic on a subscreen layer: not drawn by the GE path */
        CountReasons(kReasonMosaic);
        return false;
      }
      g_frame_subnoop = true;
      if (n && g_subruns[n - 1].r1 == r &&
          (subfix ? SameSubNoWin(&g_lines[g_subruns[n - 1].first], &g_lines[r], l)
                  : SameSub(&g_lines[g_subruns[n - 1].first], &g_lines[r], l)))
        g_subruns[n - 1].r1 = (uint16_t)(r + 1);
      else
        g_subruns[n++] = (Run){(uint16_t)r, (uint16_t)(r + 1), (uint16_t)r};
    }
    int simg_k = subhalf ? 2 : subfix ? 1 : 0;
    bool simg = SubImgEligible(l, n, simg_k);  /* gate 5p: many scroll runs: draw from the layer image */
    if (simg)
      ImgUpdate(l, &g_lines[g_subruns[0].first].bg[l]);
    for (int i = 0; i < n; i++) {
      TileList kept = {0, 0};
      int rc = simg ? 1 : SubRunContributes(l, &g_lines[g_subruns[i].first], g_subruns[i].r0, g_subruns[i].r1, subfix, &kept);
      if (rc < 0) {
        BUDGET(1, "subpool", g_ent_top, kEntPool);
        return false;
      }
      if (rc == 0)
        continue;
      {
        const BgSnap *sb = &g_lines[g_subruns[i].first].bg[l];
        int wa[2], we[2];
        if (!subfix && WinSpans(sb->sm, sb->sl, sb->sr, wa, we) == 0)
          continue;  /* masked completely on the subscreen: adds nothing (its tile list stays unused in the pool) */
      }
      if (sub_layer >= 0 && sub_layer != l) {
        if (!g_multisub_ok) {
          NOTE_SUBMULTI("two-sub-layers", g_subruns[i].r0, sub_layer, l);
          CountReasons(kReasonSubMulti);
          return false;
        }
        multisub = true;  /* several subscreen layers: composite them (by priority) in an offscreen target first */
      }
      if (g_nbruns >= kMaxLines) {
        BUDGET(2, "nbruns", g_nbruns, kMaxLines);
        return false;
      }
      sub_layer = l;
      sublayers |= 1 << l;
      g_bruns[g_nbruns++] = (BRun){(uint8_t)l, g_subruns[i].first, g_subruns[i].r0, g_subruns[i].r1, kept};
      if (simg) {
        g_simg_on[l] = true;
        g_simg_frame = true;
        g_simg_kind = simg_k;
      }
    }
  }
  for (int l = 1; l < kLayers; l++)  /* gate 5p: strip vertices of the subscreen layer images (display-list guard) */
    if (g_simg_on[l])
      g_img_nv += g_nbruns * 4 * (g_img[l - 1].nhi ? 2 : 1);
  if (subfix && (sublayers & (sublayers - 1))) {  /* several subscreen layers: a window mask would erase the others */
    for (int r = sf0; r < sf1; r++)
      for (int l = 0; l < kLayers; l++)
        if ((sublayers & (1 << l)) && g_lines[r].bg[l].sm) {
          NOTE_SUBMULTI("subfix-win-multi", r, l, sublayers);
          CountReasons(kReasonSubMulti);
          return false;
        }
  }
  bool blend = false;
  int bmath = 0, bneg = 0;
  if (g_nbruns || sobj || subfix) {
    bool first = true;
    for (int r = 0; r < g_line_count; r++) {
      bool on = g_lines[r].submath || (sobj && g_lines[r].subobj);
      if (!on)
        continue;
      if (first) {
        bneg = g_lines[r].subneg;
        first = false;
      } else if (g_lines[r].subneg != bneg) {
        NOTE_SUBMULTI("blend-neg-varies", r, bneg, g_lines[r].subneg);
        CountReasons(kReasonSubMulti);
        return false;
      }
      {  /* band = rows of one colour-math mask; blend-free gap rows are absorbed into the band */
        BlendBand *bb = g_nbband ? &g_bband[g_nbband - 1] : NULL;
        if (bb && bb->mask == g_lines[r].mathmask) {
          bb->r1 = (uint16_t)(r + 1);
        } else {
          if (g_nbband == kMaxBlendBands || (g_nbband >= 1 && (g_scissor_mode == 0 || !g_band_ok))) {
            NOTE_SUBMULTI("blend-bands", r, g_nbband, g_lines[r].mathmask);
            CountReasons(kReasonSubMulti);
            return false;
          }
          g_bband[g_nbband++] = (BlendBand){(uint16_t)r, (uint16_t)(r + 1), g_lines[r].mathmask};
        }
      }
      if (g_lines[r].subwin & (sublayers | ((sobj && g_lines[r].subobj) ? 8 : 0))) {
        CountReasons(kReasonSubWin);
        return false;
      }
    }
    blend = true;
    bmath = g_nbband ? g_bband[0].mask : 0;  /* single-band restore; with several bands each uses its own */
    g_frame_subnoop = false;
  }
  g_frame_multisub = blend && multisub && !subfix;
  g_sf_frame = blend && subfix && !subhalf;
  g_sf_zero = g_sf_frame && (subfixc & 0x7fff) == 0;  /* bit 15 only marks a subfix row */
  g_sfo_frame = g_sf_frame && sobj;
  g_sh_frame = blend && subhalf;
  g_shobj_frame = g_sh_frame && sobj;
  g_shn_frame = g_sh_frame && bneg != 0;

  /* Tint rows: colour math with the fixed colour (no subscreen layer involved). All tint rows must
   * share the layer mask and add/subtract mode; the colour and halving may change per row. */
  bool tint = false;
  g_ntint = 0;
  g_ntr = 0;
  g_stint_frame = false;
  for (int r = 0; r < g_line_count; r++) {
    const LineSnap *ls = &g_lines[r];
    if (!ls->tint)
      continue;
    TintRange *rg = g_ntr ? &g_trange[g_ntr - 1] : NULL;
    bool newrange = false;
    if (rg && rg->r1 == r && rg->mask == ls->mathmask && rg->neg == ls->subneg) {
      rg->r1 = (uint16_t)(r + 1);
    } else {
      if (g_ntr == kMaxTintRanges || (g_ntr >= 1 && g_scissor_mode == 0)) {
        NOTE_SUBMULTI("tint-mask-varies", r, g_trange[0].mask << 1 | g_trange[0].neg, ls->mathmask << 1 | ls->subneg);
        CountReasons(kReasonSubMulti);
        return false;
      }
      g_trange[g_ntr++] = (TintRange){(uint16_t)r, (uint16_t)(r + 1), (uint16_t)g_ntint, 0, ls->mathmask, ls->subneg};
      newrange = true;
    }
    TintRun *tr = g_ntint ? &g_tint[g_ntint - 1] : NULL;
    if (!newrange && tr && tr->r1 == r && tr->fix == ls->tintfix && tr->half == ls->tinthalf &&
        tr->cwm == ls->cwm && tr->cwl == ls->cwl && tr->cwr == ls->cwr)
      tr->r1 = (uint16_t)(r + 1);
    else
      g_tint[g_ntint++] = (TintRun){(uint16_t)r, (uint16_t)(r + 1), ls->tintfix, 0, ls->tinthalf, ls->cwm, ls->cwl,
                                    ls->cwr, 0};
    if (ls->cwm)
      g_cw_rows++;
    g_trange[g_ntr - 1].n = (uint16_t)(g_ntint - g_trange[g_ntr - 1].first);
  }
  if (g_ntr) {
    tint = true;
    bmath = g_trange[0].mask;
    bneg = g_trange[0].neg;
  }
  if (tint && blend) {  /* subscreen-layer blend and fixed-colour tint in one frame: not drawn */
    NOTE_SUBMULTI("tint+blend", 0, g_nbruns, g_ntint);
    CountReasons(kReasonSubMulti);
    return false;
  }
  if (tint && g_ge_level != 1) {  /* level 2 (hardware bisect): tint frames use the software image */
    CountReasons(kReasonMathTint);
    return false;
  }

  PROF(uint32_t t_cls = Now(); g_pc_class = t_cls - g_t_draw_start;)

  /* Pass A, before the list is opened: runs, atlas decode, CLUTs. The GE is
   * idle here (the previous frame was synced in Present), so CPU writes to the
   * atlases cannot race it. */
  int total = 0;
  bool w2rows = false;
  for (int r = 0; r < g_line_count && !w2rows; r++)
    w2rows = g_lines[r].w2main != 0;
  g_w2_frame = w2rows && !g_sh_frame;
  bool winrows = g_sh_frame || w2rows;  /* gates 4t-d/u: runs by geometry only, window state per row at emission */
  for (int l = 0; l < kLayers; l++) {
    int n = 0;
    for (int r = 0; r < g_line_count; r++) {
      if (!(g_lines[r].bg[l].flags & 4))
        continue;
      if (n && g_runs[l][n - 1].r1 == r &&
          (winrows ? SameGeom(&g_lines[g_runs[l][n - 1].first], &g_lines[r], l)
                   : SameBg(&g_lines[g_runs[l][n - 1].first], &g_lines[r], l)))
        g_runs[l][n - 1].r1 = (uint16_t)(r + 1);
      else
        g_runs[l][n++] = (Run){(uint16_t)r, (uint16_t)(r + 1), (uint16_t)r};
    }
    g_nruns[l] = n;
    if (MosPrepare(l) < 0) {
      CountReasons(kReasonMosaic);
      return false;
    }
    if ((winrows || (SM_PSP_MOS_GE && multisub)) && g_mos[l].on) {  /* mosaic with per-row windows (or, GE source images, the subscreen target): not drawn */
      CountReasons(kReasonMosaic);
      return false;
    }
    if (l >= 1 && g_img_ok && !winrows && !g_mos[l].on && n >= kImgMinRuns && ImgEligible(l) &&
        (!g_simg_on[l] || ImgCfgMatches(l, &g_lines[g_runs[l][0].first].bg[l]))) {
      ImgUpdate(l, &g_lines[g_runs[l][0].first].bg[l]);  /* gate 5l: many scroll runs: draw the layer from its cached image, one strip per run */
      g_img_on[l] = true;
      g_img_frame = true;
      if (g_img[l - 1].high)
        g_imgh_frame = true;
      g_img_nv += n * 4 * (g_img[l - 1].nhi ? 2 : 1);
    } else if (l >= 1 && g_img_ok && !winrows && !g_mos[l].on && n >= kImgMinRuns) {
      NOTE_IMGNO(l);  /* log only: why a layer with many runs has no image */
    }
    for (int i = 0; i < n; i++) {
      memset(g_counts[l][i], 0, sizeof(g_counts[l][i]));
      const Run *run = &g_runs[l][i];
      if (g_mos[l].on || g_img_on[l]) {  /* drawn from the mosaic image / layer image, no tiles */
        g_mlist[l][i] = (TileList){0, 0};
        if (g_img_on[l] && g_lines[run->first].bg[l].wm)
          g_win_rows += (uint32_t)(run->r1 - run->r0);
        continue;
      }
      if (!BuildTileList(l, &g_lines[run->first], run->r0, run->r1, &g_mlist[l][i])) {
        BUDGET(3, "mainpool", g_ent_top, l);
        return false;
      }
      if (winrows) {
        BuildRowWin(l, run->r0, run->r1);
        CountListWin(l, &g_mlist[l][i], run->r0, run->r1, g_counts[l][i]);
        for (int r = run->r0; r < run->r1; r++)
          if (g_lines[r].bg[l].wm)
            g_win_rows++;
      } else {
      CountList(&g_mlist[l][i], g_counts[l][i]);
      {
        const BgSnap *wb = &g_lines[run->first].bg[l];
        int wa[2], we[2];
        int ns = WinSpans(wb->wm, wb->wl, wb->wr, wa, we);
        if (wb->wm)
          g_win_rows += (uint32_t)(run->r1 - run->r0);
        if (ns != 1)  /* 0: masked completely; 2: a tile may be emitted once per span */
          for (int pr = 0; pr < 2; pr++)
            for (int p = 0; p < 8; p++)
              g_counts[l][i][pr][p] *= ns;
      }
      }
      for (int pr = 0; pr < 2; pr++)
        for (int p = 0; p < 8; p++)
          total += g_counts[l][i][pr][p];
      if (l < 2)
        BuildClut4(kClutBright);
      else
        BuildClut2(kClutBright);
    }
  }
  for (int i = 0; i < g_nbruns; i++) {
    const BRun *br = &g_bruns[i];
    memset(g_bcounts[i], 0, sizeof(g_bcounts[i]));
    CountList(&br->lst, g_bcounts[i]);
    {
      const BgSnap *sb = &g_lines[br->first].bg[br->l];
      int wa[2], we[2];
      int ns = subfix ? 1 : WinSpans(sb->sm, sb->sl, sb->sr, wa, we);
      if (sb->sm && !subfix)
        g_win_rows += (uint32_t)(br->r1 - br->r0);
      if (ns != 1)  /* a tile may be emitted once per span */
        for (int pr = 0; pr < 2; pr++)
          for (int p = 0; p < 8; p++)
            g_bcounts[i][pr][p] *= ns;
    }
    for (int pr = 0; pr < 2; pr++)
      for (int p = 0; p < 8; p++)
        total += g_bcounts[i][pr][p];
    if (br->l < 2)
      BuildClut4(kClutBright);
    else
      BuildClut2(kClutBright);
  }
  if (subhalf)
    BuildClut2Half();
  for (int l = 1; l < kLayers; l++)  /* gate 5p */
    if (g_simg_on[l] && subhalf && l == 2)
      ImgBuildClutHalf(l - 1, false);
  if (g_m7_rows) {
    if (!M7Prepare()) {
      BUDGET(4, "m7quads", g_m7_nq, kM7MaxQuads);
      return false;
    }
    BuildClut7(kClutBright);
  }
  if ((uint32_t)total > g_peak_tiles)
    g_peak_tiles = (uint32_t)total;
  if (total > kMaxTilesPerFrame) {  /* never draw a frame with holes */
    BUDGET(5, "tiles", total, kMaxTilesPerFrame);
    return false;
  }
  PROF(uint32_t t_bg = Now(); g_pc_bg = t_bg - t_cls;)

  /* Sprites: one contiguous range of rows [o0,o1) with OBJ enabled on the main
   * screen and one brightness (HUD rows have TM=4, so no sprites there). */
  int o0 = 0, o1 = 0;
  const int obj_bright = kClutBright;
  bool obj_ok = true;
  for (int r = 0; r < g_line_count; r++) {
    if (!g_lines[r].objon)
      continue;
    if (o1 == 0) {
      o0 = r;
    } else if (r != o1) {
      obj_ok = false;
    }
    o1 = r + 1;
  }
  g_nspr = 0;
  g_spr_quads = 0;
  g_nobjseg = 0;
  g_objbase = NULL;
  g_nsobjseg = 0;
  g_sobjbase = NULL;
  g_nobjband = 0;
  if (obj_ok) {  /* OBJ window: row bands of one window state */
    for (int r = o0; r < o1; r++) {
      const LineSnap *ls = &g_lines[r];
      ObjBand *ob = g_nobjband ? &g_objband[g_nobjband - 1] : NULL;
      if (ob && ob->wm == ls->owm && ob->wl == ls->owl && ob->wr == ls->owr) {
        ob->r1 = (uint16_t)(r + 1);
      } else if (g_nobjband == kMaxObjBands) {
        CountReasons(kReasonWindow);
        return false;
      } else {
        g_objband[g_nobjband++] = (ObjBand){(uint16_t)r, (uint16_t)(r + 1), ls->owm, ls->owl, ls->owr};
      }
    }
    for (int b = 0; b < g_nobjband; b++)
      if (g_objband[b].wm)
        g_win_rows += (uint32_t)(g_objband[b].r1 - g_objband[b].r0);
  }
  if (!obj_ok) {
    CountReasons(kReasonObjCfg);
    return false;
  }
  
  /* Sprites for the main rows and the subscreen rows share one list: prepare the union of both ranges. */
  int u0 = o0, u1 = o1;
  if (sobj) {
    if (u1 == u0) {
      u0 = s0;
      u1 = s1;
    } else {
      if (s0 < u0) u0 = s0;
      if (s1 > u1) u1 = s1;
    }
  }
  int obj_quads = 0, obj_np = 0;  /* gate 5d: sprite quads BuildObjGeom will emit (main bands + subscreen) */
  if (u1 > u0) {
    if (!PrepareObj(u0, u1))
      return false;
    g_glow_obj_n = g_nspr;
    int np = 0, wa[2], we[2];
    for (int b = 0; b < g_nobjband; b++)
      np += WinSpans(g_objband[b].wm, g_objband[b].wl, g_objband[b].wr, wa, we);
    obj_np = np;
    obj_quads = g_spr_quads * np + (sobj ? g_spr_quads : 0);
    if (np > 1 && g_spr_quads * np > 2 * kMaxSpriteQuads) {  /* window bands/spans multiply the sprite quads */
      BUDGET(6, "sprquads*win", g_spr_quads * np, 2 * kMaxSpriteQuads);
      return false;
    }
    if (g_ovf_rows) {  /* limited rows split tiles: check the quads that will really be emitted */
      ObjBand sbd = {(uint16_t)s0, (uint16_t)s1, 0, 0, 0};
      int nm = ObjCountQuads(g_objband, g_nobjband);
      int nsb = sobj ? ObjCountQuads(&sbd, 1) : 0;
      if (nm > 2 * kMaxSpriteQuads || nsb > kMaxSpriteQuads) {
        BUDGET(7, "sprquads*ovf", nm, nsb);
        return false;
      }
      obj_quads = nm + nsb;
    }
    if (o1 > o0)
      BuildClutObj(obj_bright);
    if (sobj)
      BuildClutObj(15);
  }
  {  /* gates 5d/5f: the list must hold every command (and, without the pool, every vertex) of this frame; the pool every big vertex buffer */
    uint32_t vtx = 0;
    uint32_t need = ListBytesNeeded(total, obj_quads, obj_np, blend, tint, sobj, &vtx);
    if (need > g_peak_list)
      g_peak_list = need;
    if (vtx > g_peak_vtx)
      g_peak_vtx = vtx;
    if (need > (uint32_t)kListBytes) {
      BUDGET(8, "listkb", (int)(need >> 10), (int)(kListBytes >> 10));
      return false;
    }
    if (vtx > (uint32_t)kVtxPoolBytes) {
      BUDGET(9, "vtxkb", (int)(vtx >> 10), (int)(kVtxPoolBytes >> 10));
      return false;
    }
  }

  S5I_FRAME(total, obj_quads, blend, tint);  /* log only */

// Gate 5t: smooth tint. Colour-window rows are included: the strip is indexed by row and the window clips the sprites. The strip is written now, while the GE is idle.
  bool smooth_tint = false;
#if SM_PSP_GE_SMOOTH_TINT
  smooth_tint = tint && g_tint_ok;
  if (smooth_tint) {
    TintStripBuild();
    g_stint_frame = true;
  }
#endif

  sceKernelDcacheWritebackRange(g_clut2[kClutBright], sizeof(g_clut2[0]));
  sceKernelDcacheWritebackRange(g_clut4[kClutBright], sizeof(g_clut4[0]));
  sceKernelDcacheWritebackRange(g_clutobj[kClutBright], sizeof(g_clutobj[0]));
  sceKernelDcacheWritebackRange(g_clut7, sizeof(g_clut7));
  sceKernelDcacheWritebackRange(g_clut2h, sizeof(g_clut2h));

  g_zbg[0][0] = 8 * kZLevel;  g_zbg[0][1] = 12 * kZLevel;  /* BG1 low / high */
  g_zbg[1][0] = 7 * kZLevel;  g_zbg[1][1] = 11 * kZLevel;  /* BG2 */
  g_zbg[2][0] = 1 * kZLevel;  g_zbg[2][1] = 15 * kZLevel;  /* BG3 */

  /* Pass B: the display list. Depth is always on. Sprites are drawn first with the depth test ALWAYS
   * (the last one drawn, the lowest OAM index, wins an overlap) and leave their depth marker; BG
   * passes then draw with GEQUAL against it (z = SNES priority level), so layering against the BGs
   * follows the priority bits. In an overlay frame (subscreen blend or tint) the markers also
   * identify every pass, so the layers that don't take part in colour math can be restored exactly
   * after the overlay with an EQUAL test. */
  g_t_list_start = Now();
  PROF(g_pc_obj = g_t_list_start - t_bg;)
  g_prep_us = g_t_list_start - g_t_draw_start;
  VTX_RESET();  /* the previous frame was synced in Present: the GE no longer reads the pool */
  if (!FrameListStart(g_gu_list, kListBytes, "scene"))
    return false;
  GL_ARM();
  SelectDrawBuffer();
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuClearColor(0xff000000);
  sceGuDepthRange(65535, 0);
  sceGuClearDepth(0);
  sceGuEnable(GU_DEPTH_TEST);
  sceGuDepthMask(GU_FALSE);
  sceGuDepthFunc(GU_ALWAYS);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
  sceGuDepthFunc(GU_GEQUAL);

  /* Backdrop, merged into runs of identical colour, drawn as vertical strips
   * that don't cross 64-pixel framebuffer columns (strip-major order, so
   * consecutive rects stay in one column). */
  int edges[8];
  int nstrip = StripEdges(kLeft, kLeft + kGameWidth, edges);
  static int16_t bd_y0[kMaxLines], bd_y1[kMaxLines];
  static uint32_t bd_col[kMaxLines];
  int nrect = 0;
  for (int r = 0; r < g_line_count;) {
    int e = r + 1;
    while (e < g_line_count && g_lines[e].cg0 == g_lines[r].cg0 && g_lines[e].forced == g_lines[r].forced) e++;
    bd_y0[nrect] = (int16_t)r;
    bd_y1[nrect] = (int16_t)e;
    bd_col[nrect++] = g_lines[r].forced ? 0xff000000u : BakeColor(g_lines[r].cg0, kClutBright, false);
    r = e;
  }
  ColorVert *bd = sceGuGetMemory(nstrip * nrect * 2 * sizeof(ColorVert));
  int nbd = 0;
  for (int st = 0; st < nstrip; st++) {
    for (int k = 0; k < nrect; k++) {
      bd[nbd++] = (ColorVert){bd_col[k], (int16_t)edges[st], (int16_t)(kTop + bd_y0[k]), kZBackdrop};
      bd[nbd++] = (ColorVert){bd_col[k], (int16_t)edges[st + 1], (int16_t)(kTop + bd_y1[k]), kZBackdrop};
    }
  }
  sceGuDisable(GU_TEXTURE_2D);
  GeDraw(kVtColor, nbd, bd);

  /* Geometry for every layer/run. One buffer per layer/priority/palette; the runs fill it one after another. */
  for (int l = 0; l < kLayers; l++) {
    BgVert *w[2][8];
    for (int pr = 0; pr < 2; pr++) {
      for (int p = 0; p < 8; p++) {
        int c = 0;
        for (int i = 0; i < g_nruns[l]; i++)
          c += g_counts[l][i][pr][p];
        w[pr][p] = c ? VtxAlloc(c * 2 * sizeof(BgVert)) : NULL;
       }
    }
    for (int i = 0; i < g_nruns[l]; i++) {
      BgVert *v0[2][8];
      memcpy(v0, w, sizeof(v0));
      const Run *run = &g_runs[l][i];
      if (winrows) {
        EmitListWin(l, &g_mlist[l][i], run->r0, run->r1, w, kLeft, kTop);
      } else {
        const BgSnap *wb = &g_lines[run->first].bg[l];
        int wa[2], we[2];
        int ns = WinSpans(wb->wm, wb->wl, wb->wr, wa, we);
        for (int k = 0; k < ns; k++)
          EmitList(l, &g_mlist[l][i], run->r0, run->r1, w, kLeft, kTop, wa[k], we[k]);
      }
      for (int pr = 0; pr < 2; pr++) {
        for (int p = 0; p < 8; p++) {
          int c = g_counts[l][i][pr][p];
          g_geom[l][i][pr][p].v = c ? v0[pr][p] : NULL;
          g_geom[l][i][pr][p].n = c ? (int)(w[pr][p] - v0[pr][p]) : 0;
        }
      }
    }
  }
  /* Mosaic layers: the stretched image is one quad per run, window span and priority. u/v are placed so that
   * nearest sampling picks column x / size and row (y + 1 - start) / size (see MosBuild); g_uvh as in M7Prepare. */
  for (int l = 0; l < kLayers; l++) {
    MosLayer *m = &g_mos[l];
    if (!m->on)
      continue;
    const float inv = 1.0f / (float)m->size;
    m->v = sceGuGetMemory(g_nruns[l] * 8 * sizeof(TileVert));
    for (int i = 0; i < g_nruns[l]; i++) {
      const Run *run = &g_runs[l][i];
      const BgSnap *wb = &g_lines[run->first].bg[l];
      int wa[2], we[2];
      int ns = WinSpans(wb->wm, wb->wl, wb->wr, wa, we);
      float vt = ((float)(run->r0 + 1 - m->start) + 0.5f - g_uvh[1]) * inv;
      float vb = vt + (float)(run->r1 - run->r0) * inv;
      for (int k = 0; k < ns; k++) {
        float ul = ((float)wa[k] + 0.5f - g_uvh[0]) * inv;
        float ur = ul + (float)(we[k] - wa[k]) * inv;
        for (int pr = 0; pr < 2; pr++) {
          TileVert *q = m->v + ((i * 2 + k) * 2 + pr) * 2;
          q[0] = (TileVert){ul, vt, (int16_t)(kLeft + wa[k]), (int16_t)(kTop + run->r0), (int16_t)g_zbg[l][pr]};
          q[1] = (TileVert){ur, vb, (int16_t)(kLeft + we[k]), (int16_t)(kTop + run->r1), (int16_t)g_zbg[l][pr]};
        }
      }
    }
  }
#if SM_PSP_MOS_GE
  for (int l = 0; l < kLayers; l++)  /* gate 5s: source sprites of the mosaic layers */
    if (g_mos[l].on)
      MosBuildSrc(l);
#endif
  for (int l = 1; l < kLayers; l++)  /* gate 5l: layer image strips */
    if (g_img_on[l])
      ImgBuildStrips(l);
  /* Subscreen overlay geometry. */
  for (int l = 0; l < kLayers; l++) {
    BgVert *w[2][8];
    for (int pr = 0; pr < 2; pr++) {
      for (int p = 0; p < 8; p++) {
        int c = 0;
        for (int i = 0; i < g_nbruns; i++)
          if (g_bruns[i].l == l)
            c += g_bcounts[i][pr][p];
        w[pr][p] = c ? VtxAlloc(c * 2 * sizeof(BgVert)) : NULL;
      }
    }
    for (int i = 0; i < g_nbruns; i++) {
      const BRun *br = &g_bruns[i];
      if (br->l != l)
        continue;
      BgVert *v0[2][8];
      memcpy(v0, w, sizeof(v0));
      {
        const BgSnap *sb = &g_lines[br->first].bg[l];
        int wa[2] = {0, 0}, we[2] = {kGameWidth, kGameWidth};
        int ns = subfix ? 1 : WinSpans(sb->sm, sb->sl, sb->sr, wa, we);
        for (int k = 0; k < ns; k++)
          EmitList(br->l, &br->lst, br->r0, br->r1, w, multisub ? 0 : kLeft, multisub ? 0 : kTop, wa[k], we[k]);
      }
      for (int pr = 0; pr < 2; pr++) {
        for (int p = 0; p < 8; p++) {
          int c = g_bcounts[i][pr][p];
          g_bgeom[i][pr][p].v = c ? v0[pr][p] : NULL;
          g_bgeom[i][pr][p].n = c ? (int)(w[pr][p] - v0[pr][p]) : 0;
        }
      }
    }
  }
  for (int l = 1; l < kLayers; l++)  /* gate 5p: strips of the subscreen layer images */
    if (g_simg_on[l])
      SubImgBuildStrips(l, subfix, multisub ? 0 : kLeft, multisub ? 0 : kTop);

  /* Multi-layer subscreen: black clear rectangle for the offscreen target (also clears its depth) and the
   * quad that adds the finished composite to the main image. */
  ColorVert *subclear = NULL;
  TileVert *subadd = NULL;
  ColorVert *submask = NULL;
  int nsubmask = 0;
  int nsubadd = 0;
  if (multisub) {
    int rlo = g_line_count, rhi = 0;
    for (int i = 0; i < g_nbruns; i++) {
      if (g_bruns[i].r0 < rlo) rlo = g_bruns[i].r0;
      if (g_bruns[i].r1 > rhi) rhi = g_bruns[i].r1;
    }
    if (sobj) {
      if (s0 < rlo) rlo = s0;
      if (s1 > rhi) rhi = s1;
    }
    if (subfix) {  /* the fixed colour goes onto every row of the blend, with or without a subscreen tile */
      rlo = sf0;
      rhi = sf1;
    }
    uint32_t clrcol = subfix ? BakeColor(subfixc, kClutBright, false) : 0xff000000u;
    if (subhalf)
      clrcol &= 0x00ffffffu;  /* alpha 0 = no subscreen pixel; BG3 pixels are written with alpha 0x80 */
    subclear = sceGuGetMemory(2 * sizeof(ColorVert));
    subclear[0] = (ColorVert){clrcol, 0, 0, 0};
    subclear[1] = (ColorVert){clrcol, (int16_t)kGameWidth, (int16_t)g_line_count, 0};
    if (subhalf) {  /* only where the colour window lets math apply */
      nsubadd = SubAddRows(rlo, rhi, NULL);
      subadd = nsubadd ? sceGuGetMemory(nsubadd * sizeof(TileVert)) : NULL;
      if (nsubadd)
        SubAddRows(rlo, rhi, subadd);
    } else {
      nsubadd = 2;
      subadd = sceGuGetMemory(2 * sizeof(TileVert));
      subadd[0] = (TileVert){0.0f, (float)rlo, (int16_t)kLeft, (int16_t)(kTop + rlo), 0};
      subadd[1] = (TileVert){(float)kGameWidth, (float)rhi, (int16_t)(kLeft + kGameWidth), (int16_t)(kTop + rhi), 0};
    }
    if (subfix && sublayers) {  /* layer windows: hidden spans of every row are painted over the target in the fixed colour */
      for (int pass = 0; pass < 2; pass++) {
        int nv = 0;
        for (int i = 0; i < g_nbruns; i++) {
          const BRun *br = &g_bruns[i];
          for (int r = br->r0; r < br->r1;) {
            const BgSnap *s = &g_lines[r].bg[br->l];
            int e = r + 1;
            while (e < br->r1 && g_lines[e].bg[br->l].sm == s->sm && g_lines[e].bg[br->l].sl == s->sl &&
                   g_lines[e].bg[br->l].sr == s->sr)
              e++;
            int ha[3], hb[3];
            int nh = s->sm ? HiddenSpans(s->sm, s->sl, s->sr, ha, hb) : 0;
            for (int k = 0; k < nh; k++, nv += 2) {
              if (pass) {
                submask[nv] = (ColorVert){clrcol, (int16_t)ha[k], (int16_t)r, 0};
                submask[nv + 1] = (ColorVert){clrcol, (int16_t)hb[k], (int16_t)e, 0};
              }
            }
            r = e;
          }
        }
        if (pass)
          nsubmask = nv;
        else if (nv)
          submask = sceGuGetMemory(nv * sizeof(ColorVert));
        else
          break;
      }
    }
    if (subfix) {
      g_sf_r0 = rlo;
      g_sf_r1 = rhi;
      g_sf_col = subfixc;
      g_sf_masks = nsubmask / 2;
    }
  }

  /* Sprite geometry: reverse precedence order, so the sprite that wins an overlap on hardware (lowest
   * OAM index) is drawn last. Every sprite carries its own depth marker; consecutive sprites that
   * share a palette share one draw. */
  g_nobjseg = BuildObjGeom(g_objband, g_nobjband, kLeft, kTop, g_objseg, &g_objbase);
  ObjBand sband = {(uint16_t)s0, (uint16_t)s1, 0, 0, 0};
  g_nsobjseg = sobj ? BuildObjGeom(&sband, 1, 0, 0, g_sobjseg, &g_sobjbase) : 0;

  /* Mode 7 geometry (z = SNES level 5: above OBJ priority 0, below priorities 1-3). */
  g_m7v = NULL;
  if (g_m7_nq > 0) {
    g_m7v = VtxAlloc(g_m7_nq * 2 * sizeof(TileVert));
    for (int i = 0; i < g_m7_nq; i++) {
      const M7Quad *q = &g_m7q[i];
      g_m7v[2 * i] = (TileVert){q->u0, q->v0, (int16_t)(kLeft + q->x0), (int16_t)(kTop + q->y0), (int16_t)(5 * kZLevel)};
      g_m7v[2 * i + 1] = (TileVert){q->u1, q->v1, (int16_t)(kLeft + q->x1), (int16_t)(kTop + q->y1), (int16_t)(5 * kZLevel)};
    }
#if SM_PSP_M7_BLK
  } else if (g_m7_blk) {  /* gate 5w: per segment two triangles (tl bl tr, tr bl br; rows are one pixel high) */
    PROF(uint32_t tv0 = Now();)
    g_m7v = VtxAlloc((uint32_t)g_m7_bnseg * 6 * sizeof(TileVert));
    TileVert *w = g_m7v;
    const int16_t z = (int16_t)(5 * kZLevel);
    for (int i = 0; i < g_m7_bnseg; i++, w += 6) {
      const M7BSeg *sg = &g_m7bseg[i];
      const int16_t xl = (int16_t)(kLeft + sg->x0), xr = (int16_t)(kLeft + sg->x1);
      const int16_t yt = (int16_t)(kTop + sg->y), yb = (int16_t)(yt + 1);
      w[0] = (TileVert){sg->u0, sg->v0, xl, yt, z};
      w[1] = (TileVert){sg->u0, sg->v0, xl, yb, z};
      w[2] = (TileVert){sg->u1, sg->v1, xr, yt, z};
      w[3] = w[2];
      w[4] = w[1];
      w[5] = (TileVert){sg->u1, sg->v1, xr, yb, z};
    }
    PROF(g_m7b_t_vtx += Now() - tv0;)
#endif
  } else if (g_m7_img) {  /* image path: one 1:1 quad per row range (texel (x, row) = pixel (x, row)) */
    g_m7v = VtxAlloc(g_m7_nrng * 2 * sizeof(TileVert));
    const float iu = 0.5f - g_uvh[0];
    for (int i = 0; i < g_m7_nrng; i++) {
      int r0 = g_m7rng[i][0], r1 = g_m7rng[i][1];
      float iv = (float)r0 + 0.5f - g_uvh[1];
      g_m7v[2 * i] = (TileVert){iu, iv, (int16_t)kLeft, (int16_t)(kTop + r0), (int16_t)(5 * kZLevel)};
      g_m7v[2 * i + 1] = (TileVert){iu + (float)kGameWidth, iv + (float)(r1 - r0),
                                    (int16_t)(kLeft + kGameWidth), (int16_t)(kTop + r1), (int16_t)(5 * kZLevel)};
    }
  }

  VTX_FLUSH();  /* every pool vertex is written: make it visible to the GE before any draw that reads it */
  PROF(g_pc_emit = Now() - g_t_list_start;)
  ColorVert *tv = NULL;
  TileVert *gq = NULL;

    if (smooth_tint) {  /* one textured quad per group, rows 1:1 with the strip */
    int nv = 0;
    for (int i = 0; i < g_ntint; i++) {
      int wa[2], we[2];
      int ns = g_tint[i].r0 < kTintTexH ? WinSpans(g_tint[i].cwm, g_tint[i].cwl, g_tint[i].cwr, wa, we) : 0;
      g_tint[i].v0 = (uint16_t)nv;
      g_tint[i].vn = (uint8_t)(ns * 2);
      nv += ns * 2;
    }
    gq = nv ? sceGuGetMemory((unsigned)nv * sizeof(TileVert)) : NULL;
    const float iu = 0.5f - g_uvh[0], iv = 0.5f - g_uvh[1];  /* as the layer-image strips: no-op while the GE samples at the pixel centre */
    for (int i = 0; i < g_ntint; i++) {
      int wa[2], we[2];
      int ya = g_tint[i].r0, yb = g_tint[i].r1;
      if (yb > kTintTexH)
        yb = kTintTexH;
      for (int k = 0; k < (int)g_tint[i].vn / 2; k++) {
        (void)WinSpans(g_tint[i].cwm, g_tint[i].cwl, g_tint[i].cwr, wa, we);
        TileVert *v = &gq[g_tint[i].v0 + 2 * k];
        /* u = game x, so the 8-texel strip pattern (and its dither phase) is the same at any window edge */
        v[0] = (TileVert){(float)wa[k] + iu, (float)ya + iv, (int16_t)(kLeft + wa[k]), (int16_t)(kTop + ya), 0};
        v[1] = (TileVert){(float)we[k] + iu, (float)yb + iv, (int16_t)(kLeft + we[k]), (int16_t)(kTop + yb), 0};
      }
    }
  } else if (tint) {
    int nv = 0;
    for (int i = 0; i < g_ntint; i++) {
      int wa[2], we[2];
      int ns = WinSpans(g_tint[i].cwm, g_tint[i].cwl, g_tint[i].cwr, wa, we);
      g_tint[i].v0 = (uint16_t)nv;
      g_tint[i].vn = (uint8_t)(ns * 2);
      nv += ns * 2;
    }

    tv = nv ? sceGuGetMemory(nv * sizeof(ColorVert)) : NULL;

    for (int i = 0; i < g_ntint; i++) {
      uint32_t col = BakeColor(g_tint[i].fix, 15, false);
      int wa[2], we[2];
      int ns = WinSpans(g_tint[i].cwm, g_tint[i].cwl, g_tint[i].cwr, wa, we);
      for (int k = 0; k < ns; k++) {
        ColorVert *c = &tv[g_tint[i].v0 + 2 * k];
        c[0] = (ColorVert){col, (int16_t)(kLeft + wa[k]), (int16_t)(kTop + g_tint[i].r0), 0};
        c[1] = (ColorVert){col, (int16_t)(kLeft + we[k]), (int16_t)(kTop + g_tint[i].r1), 0};
      }
    }
  }

  /* Gate 4l: brightness is applied last, as dst *= b/15 (colour math is done at 15, as on the SNES).
   * Runs of rows with the same brightness < 15; forced-blank rows are black anyway. */
  static int16_t br_y0[kMaxLines], br_y1[kMaxLines];
  static uint8_t br_b[kMaxLines];
  int nbr = 0;
  for (int r = 0; r < g_line_count;) {
    int b = g_lines[r].brightness;
    if (g_lines[r].forced || b >= 15) {
      r++;
      continue;
    }
    int e = r + 1;
    while (e < g_line_count && !g_lines[e].forced && g_lines[e].brightness == b)
      e++;
    br_y0[nbr] = (int16_t)r;
    br_y1[nbr] = (int16_t)e;
    br_b[nbr++] = (uint8_t)b;
    g_bright_rows += (uint32_t)(e - r);
    r = e;
  }
  ColorVert *brv = nbr ? sceGuGetMemory(nbr * 2 * sizeof(ColorVert)) : NULL;
  for (int i = 0; i < nbr; i++) {
    brv[2 * i] = (ColorVert){0xff000000u, (int16_t)kLeft, (int16_t)(kTop + br_y0[i]), 0};
    brv[2 * i + 1] = (ColorVert){0xff000000u, (int16_t)(kLeft + kGameWidth), (int16_t)(kTop + br_y1[i]), 0};
  }

  sceGuEnable(GU_TEXTURE_2D);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuAlphaFunc(GU_NOTEQUAL, 0, 0xff);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);

#if SM_PSP_MOS_GE
  DrawMosSrc();  /* gate 5s: mosaic source images, rendered by the GE (restores depth state and draw buffer) */
#endif
  /* Sprites: depth ALWAYS, the markers they leave decide the layering against the BGs below. */
  sceGuDepthFunc(GU_ALWAYS);
  DrawObjSegs(g_objseg, g_nobjseg, g_objbase, obj_bright, false, bmath);
  sceGuDepthFunc(GU_GEQUAL);
  DrawM7();
  for (int l = 0; l < kLayers; l++)
    for (int pr = 0; pr < 2; pr++)
      DrawBgPass(l, pr);

  if (blend) {
    if (multisub) {
      /* Composite the subscreen layers into the offscreen target: depth GEQUAL with the same level markers
       * as the main screen decides which subscreen layer wins a pixel; uncovered pixels stay black. */
      sceGuDrawBufferList(GU_PSM_8888, (void *)kVramSub, 256);
      sceGuDepthBuffer((void *)kVramSubZ, 256);
      sceGuEnable(GU_DEPTH_TEST);
      sceGuDepthMask(GU_FALSE);
      sceGuDepthFunc(GU_ALWAYS);
      sceGuDisable(GU_TEXTURE_2D);
      sceGuDisable(GU_ALPHA_TEST);
      if (subhalf) {  /* gate 4t-d: the alpha channel of the target is its stencil value; 0 = no subscreen pixel */
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
        sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
      }
      GeDraw(kVtColor, 2, subclear);
      sceGuEnable(GU_TEXTURE_2D);
      sceGuEnable(GU_ALPHA_TEST);
      if (subhalf) {  /* gate 4u: subscreen sprites write alpha 0x80 where opaque, like BG3 */
        sceGuStencilFunc(GU_ALWAYS, 0x80, 0xff);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
      }
      if (g_nsobjseg)
        DrawObjSegs(g_sobjseg, g_nsobjseg, g_sobjbase, 15, false, 0);
      sceGuDepthFunc(GU_GEQUAL);
      if (subhalf) {  /* BG3 pixels (CLUT alpha != 0 passes the alpha test) write 0x80 */
        sceGuStencilFunc(GU_ALWAYS, 0x80, 0xff);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
      }
      DrawBlendBgs(subhalf);
      if (nsubmask) {  /* window masks of the subscreen layer (gate 4t-c) */
        sceGuDepthFunc(GU_ALWAYS);
        if (subhalf) {  /* hidden BG3 spans: no subscreen pixel again */
          sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
          sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
        }
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);
        GeDraw(kVtColor, nsubmask, submask);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuEnable(GU_ALPHA_TEST);
      }
      /* back to the main target; the composite is then added to (or subtracted from) the main image */
      if (subhalf)
        sceGuDisable(GU_STENCIL_TEST);  /* the 5650 main target has no stencil bits */
      sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)g_draw_off, 512);
      sceGuDepthBuffer((void *)(512 * 272 * 4), 512);
      sceGuDisable(GU_DEPTH_TEST);
      sceGuEnable(GU_BLEND);
      if (subhalf) {  /* alpha 0: main +/- fixed; alpha 0x80: (main +/- sub) / 2 (gate 5c: reverse subtract = dst - src) */
        sceGuBlendFunc(bneg ? GU_REVERSE_SUBTRACT : GU_ADD, GU_ONE_MINUS_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
      } else {
        sceGuBlendFunc(bneg ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, 0x00ffffff, 0x00ffffff);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
      }
      sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
      sceGuTexImage(0, 256, 256, 256, g_vram_u + kVramSub);
      sceGuTexFlush();
      sceGuTexSync();
      sceGuDisable(GU_ALPHA_TEST);
      if (nsubadd)
        GeDraw(kVtTile, nsubadd, subadd);
      sceGuEnable(GU_ALPHA_TEST);
      sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
      PROF(g_c_clut++;)
    } else {
      sceGuDisable(GU_DEPTH_TEST);
      sceGuEnable(GU_BLEND);
      /* Blend the subscreen layer over everything it covers (colour math adds or subtracts the subscreen
       * pixel; fixed colour is 0 so uncovered pixels are unchanged). */
      sceGuBlendFunc(bneg ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, 0x00ffffff, 0x00ffffff);
      DrawBlendBgs(false);
    }
    sceGuDisable(GU_BLEND);
    if (g_nbband > 1) {  /* each band restores the layers its own colour-math mask excludes */
      for (int k = 0; k < g_nbband; k++) {
        ScissorRows(g_bband[k].r0, g_bband[k].r1);
        RestoreLayers(g_bband[k].mask, obj_bright, bd, nbd);
      }
      sceGuScissor(0, 0, kPspWidth, kPspHeight);
    } else {
      RestoreLayers(bmath, obj_bright, bd, nbd);
    }
    g_frames_blend++;
  } else if (tint) {
    /* Tint: dst = dst +/- fixed, or (dst +/- fixed) / 2 with half colour (factors 0.5 / 0.5), per range of
     * rows that share a layer mask and add/subtract mode. */
    for (int k = 0; k < g_ntr; k++) {
      const TintRange *rg = &g_trange[k];
      if (g_ntr > 1)
        sceGuScissor(0, 0, kPspWidth, kPspHeight);
      sceGuDisable(GU_DEPTH_TEST);
      sceGuEnable(GU_BLEND);
      sceGuDisable(GU_ALPHA_TEST);
      if (smooth_tint) {  /* gate 5t: CPU-written strip (no GE dependency, so TexFlush only) */
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexImage(0, kTintTexW, kTintTexH, kTintTexW, g_vram_u + kVramTint);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_REPEAT, GU_CLAMP);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuTexFlush();  /* the GE texture cache may hold the previous frame's strip; the CPU wrote the new one before the list */
      } else {
        sceGuDisable(GU_TEXTURE_2D);
      }
      for (int i = rg->first; i < rg->first + rg->n;) {
        int j = i;
        while (j < rg->first + rg->n && g_tint[j].half == g_tint[i].half)
          j++;
        unsigned f = g_tint[i].half ? 0x00808080u : 0x00ffffffu;
        if (smooth_tint) {
          int nvert = g_tint[j - 1].v0 + g_tint[j - 1].vn - g_tint[i].v0;
          if (nvert > 0) {
            sceGuBlendFunc(rg->neg ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, f, f);
            GeDraw(kVtTile, nvert, gq + g_tint[i].v0);
          }
        } else {
          int nvert = g_tint[j - 1].v0 + g_tint[j - 1].vn - g_tint[i].v0;
          if (nvert > 0) {
            sceGuBlendFunc(rg->neg ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, f, f);
            GeDraw(kVtColor, nvert, tv + g_tint[i].v0);
          }
        }
        i = j;
      }
      if (smooth_tint)  /* back to the scene's texture state for the restore passes */
        SceneTexState();
      sceGuEnable(GU_TEXTURE_2D);
      sceGuEnable(GU_ALPHA_TEST);
      sceGuDisable(GU_BLEND);
      if (g_ntr > 1)
        ScissorRows(rg->r0, rg->r1);
      RestoreLayers(rg->mask, obj_bright, bd, nbd);
    }
    if (g_ntr > 1)
      sceGuScissor(0, 0, kPspWidth, kPspHeight);
    g_frames_tint++;
  }
  if (nbr) {
    /* src factor 0, dst factor b*17/255 per channel; no depth test, so no depth write either (as the tint overlay). */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    for (int i = 0; i < nbr;) {
      int j = i;
      while (j < nbr && br_b[j] == br_b[i])
        j++;
      unsigned f = (unsigned)br_b[i] * 0x00111111u;
      sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00000000u, f);
      GeDraw(kVtColor, (j - i) * 2, brv + 2 * i);
      i = j;
    }
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
  }
  bool m7big = g_m7_nq > kM7BaseQuads && !g_m7w_frame && g_m7_bigv < kM7BigVerify;
  bool brwant = g_bright_rows != 0 && g_br_vn < kBrightVerify;
  bool winwant = g_win_rows != 0 && !g_sh_frame && !g_w2_frame && g_win_vn < kWinVerify;
  bool ovfwant = g_ovf_rows != 0 && g_ovf_vn < kOvfVerify;
  bool moswant = g_mos_frame && ((g_mos_vn < kMosVerify && MosAuditBright() >= kMosMinBright) ||
                                 (g_mos_vnhi < kMosHiVerify && MosAuditBright() >= kMosHiBright));
#if SM_PSP_M7_BLK
  bool m7iwant = g_m7_img && (g_m7_blk ? g_m7b_vn < kM7BVerify : g_m7i_vn < kM7IVerify);
#else
  bool m7iwant = g_m7_img && g_m7i_vn < kM7IVerify;
#endif
  bool bandwant = g_nbband > 1 && g_bnd_vn < kBandVerify;
  bool prevwant = g_prev_rows != 0 && g_prev_vn < kPrevVerify;
  bool cwwant = g_cw_rows != 0 && g_cw_vn < kCwVerify;
  bool m7wwant = g_m7w_frame && g_m7w_vn < kM7WVerify;
  g_verify_now = !g_ppu_skip_draw && ((g_m7_nq > 0 && !g_m7w_frame && g_m7_vn < kM7Verify) || m7wwant || m7iwant || m7big || (g_frame_multisub && g_ms_vn < 6) || brwant || winwant || ovfwant || moswant || bandwant || prevwant || cwwant || g_audit_now);
#if SM_PSP_LOG
  /* diagnostic: which classes asked for this frame to be compared (bits: m7 m7w m7i m7big multisub br win ovf mos band prev cw audit) */
  g_dg_vclass = (uint16_t)(g_ppu_skip_draw ? 0
                           : ((g_m7_nq > 0 && !g_m7w_frame && g_m7_vn < kM7Verify ? 1 : 0) | (m7wwant ? 2 : 0) | (m7iwant ? 4 : 0) |
                              (m7big ? 8 : 0) | ((g_frame_multisub && g_ms_vn < 6) ? 16 : 0) | (brwant ? 32 : 0) | (winwant ? 64 : 0) |
                              (ovfwant ? 128 : 0) | (moswant ? 256 : 0) | (bandwant ? 512 : 0) | (prevwant ? 1024 : 0) |
                              (cwwant ? 2048 : 0) | (g_audit_now ? 4096 : 0)));
#endif
  if (((g_frame_multisub && g_ms_vn < 6) || m7big || m7iwant || m7wwant || brwant || winwant || moswant || bandwant || prevwant || cwwant) && g_ppu_skip_draw)
    g_want_ms_verify = true;  /* no software image this frame: have the next one drawn by both */
  if (g_sf_frame && (g_sfo_frame ? g_sfo_vn : g_sf_zero ? g_sf0_vn : g_sf_vn) < kSfVerify) {  /* gates 4t-c/5b/5e: first frames of each class are checked */
    if (g_ppu_skip_draw)
      g_want_ms_verify = true;
    else
      g_verify_now = true;
  }
  if (g_sh_frame && (g_shn_frame ? g_shn_vn : g_shobj_frame ? g_sho_vn : g_sh_vn) < kShVerify) {  /* gates 4t-d/u/5c: the same for the half-colour paths */
    if (g_ppu_skip_draw)
      g_want_ms_verify = true;
    else
      g_verify_now = true;
  }
  if (g_w2_frame && g_w2_vn < kW2Verify) {  /* gate 4u: window 2 on main BG layers */
    if (g_ppu_skip_draw)
      g_want_ms_verify = true;
    else
      g_verify_now = true;
  }
  if (g_img_frame && (g_imgh_frame ? g_imgh_vn : g_img_vn) < kImgVerify) {  /* gate 5l: the first frames with a layer image are checked */
    if (g_ppu_skip_draw)
      g_want_ms_verify = true;
    else
      g_verify_now = true;
  }
  if (g_simg_frame) {  /* gate 5p: the first frames of each kind of subscreen layer image are checked */
    g_frames_simg++;
    if (g_simg_vn[g_simg_kind] < kSimgVerify) {
      if (g_ppu_skip_draw)
        g_want_ms_verify = true;
      else
        g_verify_now = true;
    }
  }
  if (ovfwant && g_ppu_skip_draw && g_ovf_vn < kOvfArm && g_ovf_arms < kOvfArms) {
    g_ovf_arms++;
    g_ovf_watch = kOvfWatch;  /* no software image for this overflow frame: draw both for a while so later ones get audited */
  }
  if (g_ovf_rows)
    g_frames_ovf++;
  if (g_mos_frame)
    g_frames_mos++;
  if (g_prev_rows)
    g_frames_prev++;
  if (g_cw_rows)
    g_frames_cw++;
  if (g_img_frame)
    g_frames_img++;
  if (g_sf_frame)
    g_frames_sf++;
  if (g_sfo_frame)
    g_frames_sfo++;
  if (g_sh_frame)
    g_frames_sh++;
  if (g_shobj_frame)
    g_frames_sho++;
  if (g_shn_frame)
    g_frames_shn++;
  if (g_w2_frame)
    g_frames_w2++;
  if (g_m7_img)
    g_frames_m7i++;
#if SM_PSP_M7_BLK
  if (g_m7_blk)
    g_frames_m7b++;
#endif
  if (g_m7w_frame)
    g_frames_m7w++;
#if !SM_PSP_VERIFY
  g_verify_now = false;  /* play build: nothing is compared, so the frame can be queued asynchronously */
  g_want_ms_verify = false;
  g_ovf_watch = 0;
#endif
  g_audit_now = false;
  Present(true);
  return true;
}

/* Recovery point of the scene list (and of the code before it). A call the list refuses longjmps here from inside
 * DrawGeBody (never expected: ListBytesNeeded() has admitted the frame). Present() and the compose / mosaic functions it
 * calls arm their own recovery point for their own list, so by the time DrawGeBody returns nothing is armed. */
static bool DrawGe(void) {
  if (setjmp(g_gl_jb)) {
    GlAborted("scene");
    g_verify_now = false;
    g_audit_now = false;
    return false;  /* nothing was drawn or flipped, the GE is idle: EndDraw holds the previous image or draws in software */
  }
  const bool drew = DrawGeBody();
  GL_DISARM();
  return drew;
}

/* ------------------------------------------------------------- lifecycle */

void PspRenderer_ToggleGe(void) {
  if (!g_ge_ok) {
    PspLog("toggle ignored: VRAM budget too small");
    return;
  }
  g_ge_level = (g_ge_level + 1) % 3;
  g_ge_active = g_ge_level != 0;
  g_line_count = 0;
  g_ppu_skip_draw = false;  /* the next frame must be drawn by the software PPU */
  g_streak = 0;
  PspLog("f=%u renderer -> %s", (unsigned)g_frame,
         g_ge_level == 0 ? "software" : g_ge_level == 1 ? "GE (BG1-3 + OBJ + tint)" : "GE without tint");
  PspLog_Flush();
}

bool PspRenderer_GeActive(void) {
  return g_ge_active;
}

/* After a sleep VRAM is not trusted: forget what the atlases hold so every tile used is decoded again, and let
 * the software PPU draw the next frames until the GE streak is rebuilt. Main thread, between frames. */
void PspRenderer_Resume(void) {
  GeWait();  /* complete (and show) a queued frame before the atlases and layer images are dropped */
  DoorTransitionRelease();  /* VRAM (the snapshot) is not trusted after sleep */
#if SM_PSP_DOOR_SNAP
  DoorSnapRelease();
#endif
  memset(g_hash2, 0xff, sizeof(g_hash2));
  memset(g_hash4, 0xff, sizeof(g_hash4));
  memset(g_hash7, 0xff, sizeof(g_hash7));
  memset(g_img, 0, sizeof(g_img));  /* gate 5l: VRAM is not trusted after a sleep: rebuild the layer images */
#if SM_PSP_TILE_GEN
  memset(g_tgen2, 0, sizeof(g_tgen2));  /* every tile is hashed (and decoded) again */
  memset(g_tgen4, 0, sizeof(g_tgen4));
#endif
  g_line_count = 0;
  g_streak = 0;
  g_ppu_skip_draw = false;
  g_vcount_last = sceDisplayGetVcount();
  PspLog("resume: atlases invalidated");
}

/* Measures where the GE samples texture coordinates inside a pixel, because mode 7 quads depend on it:
 * a sprite drawn 3 pixels per texel from u = 0.3 shows texel 1 at x = 2 if the GE samples at the pixel
 * centre, texel 0 if it samples at the pixel corner (same test vertically). Anything else disables
 * GE mode 7. Draws into the draw buffer before the first frame. */
static void UvSelfTest(void) {
  g_uvh[0] = g_uvh[1] = 0.5f;
  uint16_t *tex = g_texture;
  for (int k = 0; k < 8; k++) {
    tex[k] = (uint16_t)k;
    tex[k * kTexWidth] = (uint16_t)k;
  }
  GuStartBoot();
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
  sceGuTexImage(0, kTexWidth, kTexHeight, kTexWidth, g_texture);
  sceGuTexFlush();
  TileVert *tv = sceGuGetMemory(4 * sizeof(TileVert));
  tv[0] = (TileVert){0.3f, 0.5f, 0, 0, 0};
  tv[1] = (TileVert){8.3f, 0.5f, 24, 1, 0};
  tv[2] = (TileVert){0.5f, 0.3f, 40, 0, 0};
  tv[3] = (TileVert){0.5f, 8.3f, 41, 24, 0};
  GeDraw(kVtTile, 4, tv);
  /* scissor semantics: sceGuScissor(0, 4, 480, 8) clips to rows 4..7 if the last two arguments are the
   * right/bottom edge, to rows 4..11 if they are width/height. */
  sceGuDisable(GU_TEXTURE_2D);
  sceGuScissor(0, 4, kPspWidth, 8);
  ColorVert *cv = sceGuGetMemory(2 * sizeof(ColorVert));
  cv[0] = (ColorVert){0xffffffffu, 60, 0, 0};
  cv[1] = (ColorVert){0xffffffffu, 64, 16, 0};
  GeDraw(kVtColor, 2, cv);
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  const uint16_t *px = (const uint16_t *)g_vram_u;  /* draw buffer 0, 5650, stride 512 */
  int okc[2] = {1, 1}, okk[2] = {1, 1};
  for (int axis = 0; axis < 2; axis++)
    for (int i = 2; i < 22; i += 3) {  /* texels 1..7 only; texel 8 is not set */
      int got = (axis == 0 ? px[i] : px[i * 512 + 40]) & 31;
      int centre = (int)(0.3f + ((float)i + 0.5f) / 3.0f), corner = (int)(0.3f + (float)i / 3.0f);
      if (got != centre) okc[axis] = 0;
      if (got != corner) okk[axis] = 0;
    }
  bool ok = true;
  for (int axis = 0; axis < 2; axis++) {
    if (okc[axis])
      g_uvh[axis] = 0.5f;
    else if (okk[axis])
      g_uvh[axis] = 0.0f;
    else
      ok = false;
  }
  int white = 0;
  for (int y = 0; y < 16; y++)
    white += px[y * 512 + 62] == 0xffff;
  g_scissor_mode = white == 4 ? 1 : white == 8 ? 2 : 0;
  PspLog("scissor: %d rows -> mode %d (0 = unknown: tint frames with several colour-math ranges stay in software)", white,
         g_scissor_mode);
  if (!ok)
    g_m7_ok = false;
  PspLog("uvconv: x=%s y=%s (centre = 0.5, corner = 0) -> GE mode 7 %s", okc[0] ? "centre" : okk[0] ? "corner" : "?",
         okc[1] ? "centre" : okk[1] ? "corner" : "?", ok ? "enabled" : "DISABLED");
}

#if SM_PSP_BENCH_ON
/* Gate 5i-b: boot-time micro-benchmark for a layer-image redesign (nothing here is used by the game). A 512x256 layer
 * image drawn as 97 two-row strips (one quad per HDMA band, own horizontal and vertical scroll) is compared with the
 * 3492 eight-by-two tile quads the BG path emits for the same rows today. The strips are checked pixel by pixel against
 * the image (does GE texture wrap work in through mode? the split variant needs none), and the CPU cost of building
 * such an image from tile data is timed. */
enum { kBenchImgW = 512, kBenchImgH = 256, kBenchBands = 97, kBenchBandRows = 2, kBenchTiles = 36, kBenchIters = 40 };
enum { kBenchQuads, kBenchT8, kBenchT8Split, kBenchT4 };
static uint8_t g_bench_img[kBenchImgW * kBenchImgH] __attribute__((aligned(64)));
static uint32_t g_bench_clut[256] __attribute__((aligned(64)));
static uint16_t g_bench_vram[0x8000] __attribute__((aligned(16)));
static uint8_t *g_bench_img4;  /* 512x256 4 bit image in free VRAM, or NULL */
static uint32_t g_bench_rng = 0x2545F491u;

static uint32_t BenchRand(void) {
  g_bench_rng ^= g_bench_rng << 13;
  g_bench_rng ^= g_bench_rng >> 17;
  g_bench_rng ^= g_bench_rng << 5;
  return g_bench_rng;
}

/* Test pattern: a hash of the pixel position, so a wrong texel or a wrong wrap shows up. */
static inline unsigned BenchPix(int x, int y) {
  return (unsigned)((((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u)) >> 7) & 255u;
}

/* CLUT entry i, built so that the 5650 conversion is exact: R = i & 31, G = (i >> 5) & 7, B = (i * 5) & 31. */
static uint32_t BenchColor(unsigned i) {
  uint32_t r = (i & 31) << 3, g = ((i >> 5) & 7) << 2, b = ((i * 5) & 31) << 3;
  return 0xff000000u | b << 16 | g << 8 | r;
}

static unsigned Bench565(unsigned i) {
  return (i & 31) | (((i >> 5) & 7) << 5) | (((i * 5) & 31) << 11);
}

static inline int BenchHs(int i) { return (i * 53 + 17) & (kBenchImgW - 1); }
static inline int BenchVs(int i) { return (i * 29) & (kBenchImgH - 1); }

static void BenchInit(void) {
  for (int i = 0; i < 256; i++)
    g_bench_clut[i] = BenchColor((unsigned)i);
  for (int y = 0; y < kBenchImgH; y++)
    for (int x = 0; x < kBenchImgW; x++)
      g_bench_img[y * kBenchImgW + x] = (uint8_t)BenchPix(x, y);
  sceKernelDcacheWritebackRange(g_bench_img, sizeof(g_bench_img));
  sceKernelDcacheWritebackRange(g_bench_clut, sizeof(g_bench_clut));
  if (g_bench_img4) {
    uint32_t *d = (uint32_t *)g_bench_img4;  /* uncached VRAM, 32 bit stores, pixel x in nibble x & 7 */
    for (int y = 0; y < kBenchImgH; y++)
      for (int x = 0; x < kBenchImgW; x += 8) {
        uint32_t w = 0;
        for (int k = 0; k < 8; k++)
          w |= (uint32_t)(BenchPix(x + k, y) & 15u) << (4 * k);
        *d++ = w;
      }
  }
}

/* CPU cost of building a 64x32-tile 4 bpp layer as an 8 bit image (palette baked in: index = pal * 16 + pixel, 0 =
 * transparent). Same plane decode as EnsureTile. Timing only: its output is not compared with anything. */
static void BenchBuildImage(uint8_t *dst, const uint16_t *vram, int mapadr, int tileadr) {
  for (int ty = 0; ty < 32; ty++) {
    for (int tx = 0; tx < 64; tx++) {
      unsigned t = vram[(mapadr + ((tx & 32) ? 0x400 : 0) + ty * 32 + (tx & 31)) & 0x7fff];
      int base = tileadr + (int)(t & 0x3ff) * 16;
      unsigned pal = (t >> 6) & 0x70;
      bool hf = (t & 0x4000) != 0, vf = (t & 0x8000) != 0;
      uint8_t *d = dst + ty * 8 * kBenchImgW + tx * 8;
      for (int y = 0; y < 8; y++, d += kBenchImgW) {
        int row = vf ? 7 - y : y;
        uint32_t w0 = vram[(base + row) & 0x7fff], w1 = vram[(base + 8 + row) & 0x7fff];
        for (int x = 0; x < 8; x++) {
          int s = 7 - x;
          unsigned px = ((w0 >> s) & 1) | ((w0 >> (s + 7)) & 2) | (((w1 >> s) & 1) << 2) | ((w1 >> (s + 5)) & 8);
          d[hf ? 7 - x : x] = (uint8_t)(px ? pal + px : 0);
        }
      }
    }
  }
}

static void BenchImageBuild(void) {
  for (int i = 0; i < 0x8000; i++)
    g_bench_vram[i] = (uint16_t)(BenchRand() >> 8);
  BenchBuildImage(g_bench_img, g_bench_vram, 0x1000, 0x4000);  /* warm up */
  uint32_t t0 = Now();
  for (int i = 0; i < 4; i++)
    BenchBuildImage(g_bench_img, g_bench_vram, 0x1000, 0x4000);
  uint32_t build = (Now() - t0) / 4;
  t0 = Now();
  sceKernelDcacheWritebackRange(g_bench_img, sizeof(g_bench_img));
  uint32_t wb = Now() - t0;
  PspLog("bench: image build %u us (2048 tiles, %u ns/tile), writeback %u us", (unsigned)build,
         (unsigned)(build * 1000 / 2048), (unsigned)wb);
}

/* Strip i covers screen rows [2i, 2i+2) with the whole 256 px width, sampled from the image at scroll (hs, vs). Without
 * split one quad per strip is emitted and the texture coordinates run past the image edge (GU_REPEAT). With split
 * the strip is cut at the image edge in x and y and every piece stays inside it (GU_CLAMP is enough). Returns the vertex
 * count. */
static int BenchStrips(TileVert *v, bool split) {
  int n = 0;
  const float iu = 0.5f - g_uvh[0], iv = 0.5f - g_uvh[1];
  for (int i = 0; i < kBenchBands; i++) {
    int r0 = i * kBenchBandRows, r1 = r0 + kBenchBandRows;
    int hs = BenchHs(i), vs = BenchVs(i);
    int xa = 256, rk = r1;  /* cut points: screen column and row, or the end (no cut) */
    if (split) {
      if (hs + 256 > kBenchImgW)
        xa = kBenchImgW - hs;
      int v0 = (r0 + vs) & (kBenchImgH - 1);
      if (v0 + kBenchBandRows > kBenchImgH)
        rk = r0 + (kBenchImgH - v0);
    }
    const int xs[3] = {0, xa, 256}, rs[3] = {r0, rk, r1};
    for (int a = 0; a < 2; a++) {
      if (xs[a] == xs[a + 1])
        continue;
      for (int b = 0; b < 2; b++) {
        if (rs[b] == rs[b + 1])
          continue;
        int ub = split ? ((hs + xs[a]) & (kBenchImgW - 1)) : hs + xs[a];
        int vb = split ? ((rs[b] + vs) & (kBenchImgH - 1)) : rs[b] + vs;
        v[n++] = (TileVert){(float)ub + iu, (float)vb + iv, (int16_t)(kLeft + xs[a]), (int16_t)(kTop + rs[b]), 0};
        v[n++] = (TileVert){(float)(ub + xs[a + 1] - xs[a]) + iu, (float)(vb + rs[b + 1] - rs[b]) + iv,
                            (int16_t)(kLeft + xs[a + 1]), (int16_t)(kTop + rs[b + 1]), 0};
      }
    }
  }
  return n;
}

/* One frame: list build, then GE run to completion. build = until sceGuFinish, total = until the GE is idle. */
static void BenchFrame(int kind, uint32_t *build_us, uint32_t *total_us) {
  uint32_t t0 = Now();
  GuStartBoot();
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);
  if (kind == kBenchQuads) {  /* what the BG path emits today: 97 bands x 36 tile quads of 8x2 px from the T4 atlas */
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
    sceGuClutLoad(2, g_bench_clut);
    sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
    sceGuTexImage(0, 256, 512, 256, g_atlas4);
    sceGuTexFlush();
    BgVert *w0 = sceGuGetMemory(kBenchBands * kBenchTiles * 2 * sizeof(BgVert)), *w = w0;
    for (int b = 0; b < kBenchBands; b++)
      for (int t = 0; t < kBenchTiles; t++) {
        int pt = ((b * kBenchTiles + t) * 7) & 2047;
        int u = (pt & 31) * 8, v = (pt >> 5) * 8, y = kTop + b * kBenchBandRows;
        BgQuad(w, u, v, u + 8, v + kBenchBandRows, kLeft + t * 8, y, kLeft + t * 8 + 8, y + kBenchBandRows, 0);
        w += 2;
      }
    sceGuDrawArray(GU_SPRITES, kVtBg, kBenchBands * kBenchTiles * 2, NULL, w0);
  } else {
    bool t4 = kind == kBenchT4, wrap = kind != kBenchT8Split;
    sceGuTexWrap(wrap ? GU_REPEAT : GU_CLAMP, wrap ? GU_REPEAT : GU_CLAMP);
    if (t4) {
      sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
      sceGuClutLoad(2, g_bench_clut);
      sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
      sceGuTexImage(0, kBenchImgW, kBenchImgH, kBenchImgW, g_bench_img4);
    } else {
      sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
      sceGuClutLoad(32, g_bench_clut);
      sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
      sceGuTexImage(0, kBenchImgW, kBenchImgH, kBenchImgW, g_bench_img);
    }
    sceGuTexFlush();
    TileVert *v = sceGuGetMemory(kBenchBands * 8 * sizeof(TileVert));
    int n = BenchStrips(v, kind == kBenchT8Split);
    sceGuDrawArray(GU_SPRITES, kVtTile, n, NULL, v);
  }
  uint32_t t1 = Now();
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  *build_us = t1 - t0;
  *total_us = Now() - t0;
}

static void BenchRun(const char *name, int kind) {
  uint32_t sb = 0, st = 0, mt = 0;
  for (int i = 0; i <= kBenchIters; i++) {
    uint32_t b, t;
    BenchFrame(kind, &b, &t);
    if (i == 0)
      continue;  /* first frame is cold */
    sb += b;
    st += t;
    if (t > mt)
      mt = t;
  }
  PspLog("bench: %s build=%u total=%u max=%u us (mean of %d)", name, (unsigned)(sb / kBenchIters),
         (unsigned)(st / kBenchIters), (unsigned)mt, (int)kBenchIters);
}

/* The strips just drawn against the image: every pixel of rows 0..193 of the game area. Reads the draw buffer (buffer 0,
 * 5650, stride 512) through the uncached VRAM mirror. */
static void BenchVerify(const char *name, bool t4) {
  const uint16_t *ge = (const uint16_t *)g_vram_u;
  uint32_t bad = 0;
  int bx = -1, by = -1;
  unsigned bgot = 0, bwant = 0;
  for (int y = 0; y < kBenchBands * kBenchBandRows; y++) {
    int hs = BenchHs(y / kBenchBandRows), vs = BenchVs(y / kBenchBandRows);
    for (int x = 0; x < 256; x++) {
      unsigned idx = BenchPix((x + hs) & (kBenchImgW - 1), (y + vs) & (kBenchImgH - 1));
      if (t4)
        idx &= 15u;
      unsigned want = Bench565(idx), got = ge[(kTop + y) * 512 + kLeft + x];
      if (got != want) {
        if (!bad) {
          bx = x;
          by = y;
          bgot = got;
          bwant = want;
        }
        bad++;
      }
    }
  }
  PspLog("bench: %s check bad=%u of %d first (%d,%d) got=%04x want=%04x", name, (unsigned)bad, 256 * kBenchBands * kBenchBandRows,
         bx, by, bgot, bwant);
}

static void Bench5ib(void) {
  uint32_t off4 = (uint32_t)(g_triple ? kVramBuf2 + kBufBytes : kVramBuf2);  /* free VRAM behind the frame buffers */
  g_bench_img4 = (uint32_t)sceGeEdramGetSize() >= off4 + kBenchImgW * kBenchImgH / 2 ? g_vram_u + off4 : NULL;
  PspLog("bench: uv x=%.1f y=%.1f, T4 image %s (VRAM offset %u)", (double)g_uvh[0], (double)g_uvh[1],
         g_bench_img4 ? "in VRAM" : "skipped, no room", (unsigned)off4);
  BenchImageBuild();
  BenchInit();
  BenchRun("strips T8 RAM wrap ", kBenchT8);
  BenchVerify("strips T8 RAM wrap ", false);
  if (g_bench_img4) {
    BenchRun("strips T4 VRAM wrap", kBenchT4);
    BenchVerify("strips T4 VRAM wrap", true);
  }
  BenchRun("strips T8 RAM split", kBenchT8Split);
  BenchVerify("strips T8 RAM split", false);
  BenchRun("tile quads 3492    ", kBenchQuads);
  GuStartBoot();  /* leave the draw buffer black, as before */
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}
#endif

#if SM_PSP_BENCH_M7_ON
/* Gate 5r-a (log only, nothing here is used by the game): boot-time audit of GE-native Mode 7 rotation.
 * The 1024x1024 plane is built once from the Mode 7 VRAM layout as an 8 bit image (stride 1024) in RAM. Every screen
 * row is cut into segments of segw pixels; a segment is one triangle strip whose texture window (512x512, stride
 * 1024) starts at the segment's integer plane position (x rounded down to 16 bytes, so every base address is
 * 16-byte aligned like the 1024x1024 reference demo) and whose UVs are relative to that window. This keeps the UV
 * magnitudes small, which is what the GE's interpolation precision needs. Rows use the ppu.c start-position maths
 * and are checked pixel by pixel against the CPU image path (M7RenderRow). Compared: CPU image + one GE quad
 * (today's path) against strips of 256/64/32 pixel segments, with the sample point of the GE at the pixel centre
 * or the pixel corner (the sprite self test says nothing about triangles). Then the whole run is repeated (A/B/A). */
enum { kB7Plane = 1024, kB7PadRows = 64, kB7Win = 512, kB7Iters = 6, kB7MaxSegs = kMaxLines * 8, kB7Scen = 4 };
typedef struct B7Seg { const uint8_t *base; float u0, v0, u1, v1; int16_t x0, x1, y; } B7Seg;
static uint8_t g_b7_plane[kB7Plane * (kB7Plane + kB7PadRows)] __attribute__((aligned(64)));
static uint32_t g_b7_clut[256] __attribute__((aligned(64)));
static B7Seg g_b7_seg[kB7MaxSegs];
static int g_b7_nseg;
/* Gate 5t-b: the plane again in PSP swizzle order (16 x 8 byte blocks, 1024 B pitch), for the frame-window variants. */
static uint8_t g_b7_plane_swz[kB7Plane * kB7Plane] __attribute__((aligned(64)));
static int16_t g_b7_m[8];  /* matrix of the scenario being run: a, b, c, d, centre x, y, scroll h, v */
static uint32_t g_b7_rng = 0x2545F491u;
/* deg, scale %, a = d = cos * scale * 256, b = sin * scale * 256, c = -b (as the Ceres elevator sets M7A-M7D) */
static const int16_t kB7Mat[kB7Scen][4] = {{241, 88, -88, 241}, {181, 181, -181, 181}, {272, 272, -272, 272}, {40, 148, -148, 40}};
static const int kB7Deg[kB7Scen] = {20, 45, 45, 75};
static const int kB7Pct[kB7Scen] = {100, 100, 150, 60};

static uint32_t B7Rand(void) {
  g_b7_rng ^= g_b7_rng << 13;
  g_b7_rng ^= g_b7_rng >> 17;
  g_b7_rng ^= g_b7_rng << 5;
  return g_b7_rng;
}

/* CLUT entry i: its 565 conversion is exact and distinct for all 256 indices (R5 = i & 31, G6 = i >> 5, B5 = i * 5). */
static uint32_t B7Color(unsigned i) {
  return 0xff000000u | (((i * 5) & 31) << 3) << 16 | (((i >> 5) & 7) << 2) << 8 | ((i & 31) << 3);
}

static unsigned B7565(unsigned i) {
  return (i & 31) | (((i >> 5) & 7) << 5) | (((i * 5) & 31) << 11);
}

/* The plane as the mode 7 VRAM describes it: pixel (x, y) = high byte of vram[tile * 64 + (y & 7) * 8 + (x & 7)],
 * tile = low byte of vram[(y >> 3) * 128 + (x >> 3)]. Two 32 bit stores per tile row (4-byte aligned). */
static void B7BuildPlane(void) {
  for (int ty = 0; ty < 128; ty++)
    for (int tx = 0; tx < 128; tx++) {
      const uint16_t *src = &g_vram[(g_vram[ty * 128 + tx] & 0xff) * 64];
      uint32_t *dst = (uint32_t *)(g_b7_plane + ty * 8 * kB7Plane + tx * 8);
      for (int y = 0; y < 8; y++, dst += kB7Plane / 4) {
        uint32_t w0 = 0, w1 = 0;
        for (int x = 0; x < 4; x++) {
          w0 |= (uint32_t)(src[y * 8 + x] >> 8) << (8 * x);
          w1 |= (uint32_t)(src[y * 8 + 4 + x] >> 8) << (8 * x);
        }
        dst[0] = w0;
        dst[1] = w1;
      }
    }
}

/* Start position of screen row y (1-based line) as M7RenderRow computes it (no flips, no mosaic). */
static void B7RowStart(const int16_t *m, int y, int32_t *X0, int32_t *Y0) {
  int xc = ((int16_t)(m[4] << 3)) >> 3, yc = ((int16_t)(m[5] << 3)) >> 3;
  int hs = ((int16_t)(m[6] << 3)) >> 3, vs = ((int16_t)(m[7] << 3)) >> 3;
  int ch = hs - xc, cv = vs - yc;
  ch = (ch & 0x2000) ? (ch | ~1023) : (ch & 1023);
  cv = (cv & 0x2000) ? (cv | ~1023) : (cv & 1023);
  *X0 = ((m[0] * ch) & ~63) + ((m[1] * y) & ~63) + ((m[1] * cv) & ~63) + (xc << 8);
  *Y0 = ((m[2] * ch) & ~63) + ((m[3] * y) & ~63) + ((m[3] * cv) & ~63) + (yc << 8);
}

/* Segments of every row. u/v are the texel position of the pixel (x, row) inside the segment's window plus a shift that
 * keeps floor() on the right texel for every fractional position the segment can have (same centring as M7Prepare:
 * the residues of the start position, in 1/256 texel, span [m0, m1]), minus uvh * step for the GE sample point.
 * Returns false if a segment does not fit one 512x512 window or leaves the plane (wrap is not handled here). */
static bool B7Prepare(const int16_t *m, int segw, float uvh) {
  const int a = m[0], c = m[2];
  const float du = (float)a / 256.0f, dv = (float)c / 256.0f;
  g_b7_nseg = 0;
  for (int r = 0; r < kGameHeight; r++) {
    int32_t X0, Y0;
    B7RowStart(m, r + 1, &X0, &Y0);
    for (int xs = 0; xs < kGameWidth; xs += segw) {
      const int xe = xs + segw;
      int mu0 = 255, mu1 = 0, mv0 = 255, mv1 = 0;
      for (int x = xs; x < xe; x++) {
        int ru = (X0 + a * x) & 255, rv = (Y0 + c * x) & 255;
        if (ru < mu0) mu0 = ru;
        if (ru > mu1) mu1 = ru;
        if (rv < mv0) mv0 = rv;
        if (rv > mv1) mv1 = rv;
      }
      const int32_t Xs = X0 + a * xs, Xe = X0 + a * xe, Ys = Y0 + c * xs, Ye = Y0 + c * xe;
      int minx = IntMin(Xs >> 8, Xe >> 8) - 1, maxx = IntMax(Xs >> 8, Xe >> 8) + 2;
      int miny = IntMin(Ys >> 8, Ye >> 8) - 1, maxy = IntMax(Ys >> 8, Ye >> 8) + 2;
      if (minx < 0 || miny < 0 || maxx >= kB7Plane || maxy >= kB7Plane)
        return false;
      int bx = minx & ~15, by = miny;
      if (maxx - bx >= kB7Win || maxy - by >= kB7Win)
        return false;
      const uint8_t *base = g_b7_plane + by * kB7Plane + bx;
      if ((uintptr_t)base & 15)
        return false;
      const float su = (float)(256 - mu0 - mu1) / 512.0f - uvh * du, sv = (float)(256 - mv0 - mv1) / 512.0f - uvh * dv;
      B7Seg *s = &g_b7_seg[g_b7_nseg++];
      s->base = base;
      s->u0 = (float)(Xs - (bx << 8)) / 256.0f + su;
      s->u1 = (float)(Xe - (bx << 8)) / 256.0f + su;
      s->v0 = (float)(Ys - (by << 8)) / 256.0f + sv;
      s->v1 = (float)(Ye - (by << 8)) / 256.0f + sv;
      s->x0 = (int16_t)xs;
      s->x1 = (int16_t)xe;
      s->y = (int16_t)r;
    }
  }
  return true;
}

/* One frame into draw buffer 0: strips (the segments from B7Prepare) or the image quad (g_m7img, today's path). */
static void B7Frame(bool strips, uint32_t *build_us, uint32_t *total_us) {
  uint32_t t0 = Now();
  GuStartBoot();
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);
  sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
  sceGuClutLoad(32, g_b7_clut);
  sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
  if (!strips) {
    sceGuTexImage(0, 256, 256, 256, g_m7img);
    sceGuTexFlush();
    TileVert *v = sceGuGetMemory(2 * sizeof(TileVert));
    const float iu = 0.5f - g_uvh[0], iv = 0.5f - g_uvh[1];
    v[0] = (TileVert){iu, iv, (int16_t)kLeft, (int16_t)kTop, 0};
    v[1] = (TileVert){iu + (float)kGameWidth, iv + (float)kGameHeight, (int16_t)(kLeft + kGameWidth),
                      (int16_t)(kTop + kGameHeight), 0};
    GeDraw(kVtTile, 2, v);
  } else {
    for (int i = 0; i < g_b7_nseg; i++) {
      const B7Seg *s = &g_b7_seg[i];
      sceGuTexImage(0, kB7Win, kB7Win, kB7Plane, s->base);  /* also flushes the texture cache */
      TileVert *v = sceGuGetMemory(4 * sizeof(TileVert));
      const int16_t xl = (int16_t)(kLeft + s->x0), xr = (int16_t)(kLeft + s->x1);
      const int16_t yt = (int16_t)(kTop + s->y), yb = (int16_t)(kTop + s->y + 1);
      v[0] = (TileVert){s->u0, s->v0, xl, yt, 0};
      v[1] = (TileVert){s->u0, s->v0, xl, yb, 0};
      v[2] = (TileVert){s->u1, s->v1, xr, yt, 0};
      v[3] = (TileVert){s->u1, s->v1, xr, yb, 0};
      sceGuDrawArray(GU_TRIANGLE_STRIP, kVtTile, 4, NULL, v);
    }
  }
  uint32_t t1 = Now();
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  *build_us = t1 - t0;
  *total_us = Now() - t0;
}

/* The GE image (draw buffer 0, 5650, stride 512, read through the uncached mirror) against the CPU image path. */
static uint32_t B7Verify(void) {
  const uint16_t *ge = (const uint16_t *)g_vram_u;
  uint32_t bad = 0;
  for (int y = 0; y < kGameHeight; y++)
    for (int x = 0; x < kGameWidth; x++)
      if (ge[(kTop + y) * 512 + kLeft + x] != (uint16_t)B7565(g_m7img[y * 256 + x]))
        bad++;
  return bad;
}

/* PSPSDK samples swizzle_fast: width in bytes, blocks of 16 x 8 bytes. */
static void B7SwizzlePlane(void) {
  const unsigned wb = kB7Plane / 16, hb = kB7Plane / 8, src_pitch = (kB7Plane - 16) / 4, src_row = kB7Plane * 8;
  const uint8_t *ysrc = g_b7_plane;
  uint32_t *dst = (uint32_t *)g_b7_plane_swz;
  for (unsigned by = 0; by < hb; by++, ysrc += src_row) {
    const uint8_t *xsrc = ysrc;
    for (unsigned bx = 0; bx < wb; bx++, xsrc += 16) {
      const uint32_t *src = (const uint32_t *)xsrc;
      for (unsigned j = 0; j < 8; j++, src += src_pitch) {
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
      }
    }
  }
}

/* Frame window strategy (Gate 5t-b). B7Prepare/B7Frame bind a new 512x512 window for every segment (sceGuTexImage also
 * flushes the GE texture cache) and draw one strip each: 224 to 1800 binds, flushes and draws a frame. Here every segment of
 * the frame shares ONE window (origin bx, a multiple of 16, by, a multiple of 8, so the same origin is valid in the swizzled
 * plane), and all segments are one GU_TRIANGLES draw: a single bind, no mid-frame cache flush, the segment count only costs
 * vertices. u/v are relative to the window; the plane position of every sample is unchanged, so a bad pixel means the GE's
 * texture coordinate interpolation (larger |u| than in the per-segment windows) is the limit. Returns false if the frame
 * does not fit one window or leaves the plane. */
static bool B7PrepareFrame(const int16_t *m, int segw, float uvh, int *wbx, int *wby) {
  const int a = m[0], c = m[2];
  const float du = (float)a / 256.0f, dv = (float)c / 256.0f;
  int gminx = kB7Plane, gminy = kB7Plane, gmaxx = 0, gmaxy = 0;
  g_b7_nseg = 0;
  for (int r = 0; r < kGameHeight; r++) {
    int32_t X0, Y0;
    B7RowStart(m, r + 1, &X0, &Y0);
    for (int xs = 0; xs < kGameWidth; xs += segw) {
      const int xe = xs + segw;
      int mu0 = 255, mu1 = 0, mv0 = 255, mv1 = 0;
      for (int x = xs; x < xe; x++) {
        int ru = (X0 + a * x) & 255, rv = (Y0 + c * x) & 255;
        if (ru < mu0) mu0 = ru;
        if (ru > mu1) mu1 = ru;
        if (rv < mv0) mv0 = rv;
        if (rv > mv1) mv1 = rv;
      }
      const int32_t Xs = X0 + a * xs, Xe = X0 + a * xe, Ys = Y0 + c * xs, Ye = Y0 + c * xe;
      int minx = IntMin(Xs >> 8, Xe >> 8) - 1, maxx = IntMax(Xs >> 8, Xe >> 8) + 2;
      int miny = IntMin(Ys >> 8, Ye >> 8) - 1, maxy = IntMax(Ys >> 8, Ye >> 8) + 2;
      if (minx < 0 || miny < 0 || maxx >= kB7Plane || maxy >= kB7Plane)
        return false;
      if (minx < gminx) gminx = minx;
      if (miny < gminy) gminy = miny;
      if (maxx > gmaxx) gmaxx = maxx;
      if (maxy > gmaxy) gmaxy = maxy;
      const float su = (float)(256 - mu0 - mu1) / 512.0f - uvh * du, sv = (float)(256 - mv0 - mv1) / 512.0f - uvh * dv;
      B7Seg *sg = &g_b7_seg[g_b7_nseg++];
      sg->base = NULL;
      sg->u0 = (float)Xs / 256.0f + su;  /* plane position, exact: multiples of 1/512 below 2^11 */
      sg->u1 = (float)Xe / 256.0f + su;
      sg->v0 = (float)Ys / 256.0f + sv;
      sg->v1 = (float)Ye / 256.0f + sv;
      sg->x0 = (int16_t)xs;
      sg->x1 = (int16_t)xe;
      sg->y = (int16_t)r;
    }
  }
  const int bx = gminx & ~15, by = gminy & ~7;
  if (gmaxx - bx >= kB7Win || gmaxy - by >= kB7Win)
    return false;
  for (int i = 0; i < g_b7_nseg; i++) {  /* window-relative; subtracting integers is exact */
    g_b7_seg[i].u0 -= (float)bx;
    g_b7_seg[i].u1 -= (float)bx;
    g_b7_seg[i].v0 -= (float)by;
    g_b7_seg[i].v1 -= (float)by;
  }
  *wbx = bx;
  *wby = by;
  return true;
}

/* Copies the 512x512 window into the free texture region of VRAM (kVramTexture, 256 KB; nothing else uses it at boot),
 * keeping the layout (linear, or swizzled with 512 B pitch). The part of the window outside the plane is not written: no
 * segment samples it. */
static void B7CopyWindow(int bx, int by, bool swz) {
  uint8_t *dst = g_vram_u + kVramTexture;
  if (!swz) {
    const int nx = IntMin(kB7Win, kB7Plane - bx), ny = IntMin(kB7Win, kB7Plane - by);
    for (int y = 0; y < ny; y++)
      memcpy(dst + y * kB7Win, g_b7_plane + (by + y) * kB7Plane + bx, (size_t)nx);
  } else {
    const int nbx = IntMin(kB7Win / 16, kB7Plane / 16 - (bx >> 4)), nby = IntMin(kB7Win / 8, kB7Plane / 8 - (by >> 3));
    for (int j = 0; j < nby; j++)
      for (int i = 0; i < nbx; i++)
        memcpy(dst + ((j * (kB7Win / 16) + i) << 7),
               g_b7_plane_swz + ((((by >> 3) + j) * (kB7Plane / 16) + (bx >> 4) + i) << 7), 128);
  }
}

/* One frame: a single bind of the window, a single GU_TRIANGLES draw of every segment (two triangles each). */
static void B7FrameWin(const uint8_t *tex, int stride, bool swz, uint32_t *build_us, uint32_t *total_us) {
  uint32_t t0 = Now();
  GuStartBoot();
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);
  sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
  sceGuClutLoad(32, g_b7_clut);
  sceGuTexMode(GU_PSM_T8, 0, 0, SWZ_FLAG(swz));
  sceGuTexImage(0, kB7Win, kB7Win, stride, tex);  /* also flushes the texture cache: once per frame */
  TileVert *v = sceGuGetMemory((unsigned)g_b7_nseg * 6 * sizeof(TileVert));
  TileVert *w = v;
  for (int i = 0; i < g_b7_nseg; i++) {
    const B7Seg *sg = &g_b7_seg[i];
    const int16_t xl = (int16_t)(kLeft + sg->x0), xr = (int16_t)(kLeft + sg->x1);
    const int16_t yt = (int16_t)(kTop + sg->y), yb = (int16_t)(kTop + sg->y + 1);
    const TileVert tl = {sg->u0, sg->v0, xl, yt, 0}, bl = {sg->u0, sg->v0, xl, yb, 0};
    const TileVert tr = {sg->u1, sg->v1, xr, yt, 0}, br = {sg->u1, sg->v1, xr, yb, 0};
    w[0] = tl; w[1] = bl; w[2] = tr;  /* same facing as the strip (tl, bl, tr, br) */
    w[3] = tr; w[4] = bl; w[5] = br;
    w += 6;
  }
  sceGuDrawArray(GU_TRIANGLES, kVtTile, g_b7_nseg * 6, NULL, v);
  uint32_t t1 = Now();
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  *build_us = t1 - t0;
  *total_us = Now() - t0;
}

/* var: bit 0 = swizzled plane, bit 1 = window copied to VRAM (the copy would be paid every frame while rotating: it is
 * reported as copy=, not included in total=). */
static void B7RunWin(const char *tag, int sc, int segw, float uvh, int var) {
  static const char *const kName[4] = {"ram-lin ", "ram-swz ", "vram-lin", "vram-swz"};
  const bool swz = (var & 1) != 0, vram = (var & 2) != 0;
  int bx = 0, by = 0;
  uint32_t t = Now();
  bool ok = B7PrepareFrame(g_b7_m, segw, uvh, &bx, &by);
  uint32_t prep = Now() - t, copy = 0, sb = 0, st = 0, mt = 0;
  if (!ok) {
    PspLog("b7%s: %d deg %d%% win-%s seg=%d unfit (the frame does not fit one 512 window / leaves the plane)", tag, kB7Deg[sc],
           kB7Pct[sc], kName[var], segw);
    return;
  }
  const uint8_t *tex;
  int stride;
  if (vram) {
    t = Now();
    B7CopyWindow(bx, by, swz);
    copy = Now() - t;
    tex = g_vram_u + kVramTexture;
    stride = kB7Win;
  } else if (swz) {
    tex = g_b7_plane_swz + (by >> 3) * (kB7Plane * 8) + (bx >> 4) * 128;
    stride = kB7Plane;
  } else {
    tex = g_b7_plane + by * kB7Plane + bx;
    stride = kB7Plane;
  }
  for (int i = 0; i <= kB7Iters; i++) {
    uint32_t b, tt;
    B7FrameWin(tex, stride, swz, &b, &tt);
    if (i == 0)
      continue;  /* first frame is cold */
    sb += b;
    st += tt;
    if (tt > mt)
      mt = tt;
  }
  PspLog("b7%s: %d deg %d%% win-%s seg=%d uvh=%s segs=%d verts=%d prep=%u copy=%u build=%u total=%u max=%u us bad=%u", tag,
         kB7Deg[sc], kB7Pct[sc], kName[var], segw, uvh > 0.25f ? "centre" : "corner", g_b7_nseg, g_b7_nseg * 6, (unsigned)prep,
         (unsigned)copy, (unsigned)(sb / kB7Iters), (unsigned)(st / kB7Iters), (unsigned)mt, (unsigned)B7Verify());
}

/* Gate 5t-c: block windows. Hardware result of 5t-b: one bind per frame is 7-12x faster on the GE than a bind per segment and
 * a swizzled plane is 40% faster again, but with window-relative |u|,|v| up to 360 the GE's interpolation is not exact at
 * every matrix (45 deg 100%: 706 bad pixels, per-segment windows of <= 64: 0). Here the screen is cut into blocks of
 * bw x bh pixels, every block gets the window of its own samples (extent about (bw + bh) * scale, kept below kB7Blk) and one
 * bind, so |u|,|v| stay small like in the per-segment case while the binds drop from 224-1920 to 56-112. Segments are one
 * row of one block (bw pixels, bw >= 32). The residue range [m0, m1] of a segment, which B7Prepare finds by scanning every
 * pixel (2.4-3.6 ms), only depends on its start residue when the step is the same everywhere: min/max tables for the
 * frame's a and c replace the scan (the scan is run too, once, and compared). */
enum { kB7Blk = 128, kB7MaxBlk = 256 };
typedef struct B7Blk { int wx, wy, first, n; } B7Blk;
static B7Blk g_b7_blk[kB7MaxBlk];
static int g_b7_nblk;
static uint8_t g_b7_rmin[2][256], g_b7_rmax[2][256];

static void B7ResTab(int step, int n, uint8_t *mn, uint8_t *mx) {
  uint8_t off[kGameWidth];
  for (int k = 0; k < n; k++)
    off[k] = (uint8_t)((step * k) & 255);
  for (int r0 = 0; r0 < 256; r0++) {
    int lo = 255, hi = 0;
    for (int k = 0; k < n; k++) {
      int r = (r0 + off[k]) & 255;
      if (r < lo) lo = r;
      if (r > hi) hi = r;
    }
    mn[r0] = (uint8_t)lo;
    mx[r0] = (uint8_t)hi;
  }
}

/* tab: residue ranges from the tables, else by scanning the segment. Returns false if a block does not fit kB7Blk or a
 * sample leaves the plane. */
static bool B7PrepareBlocks(const int16_t *m, int bw, int bh, float uvh, bool tab, int *maxext) {
  const int a = m[0], c = m[2];
  const float du = (float)a / 256.0f, dv = (float)c / 256.0f;
  if (tab) {
    B7ResTab(a & 255, bw, g_b7_rmin[0], g_b7_rmax[0]);
    B7ResTab(c & 255, bw, g_b7_rmin[1], g_b7_rmax[1]);
  }
  g_b7_nseg = 0;
  g_b7_nblk = 0;
  *maxext = 0;
  for (int by0 = 0; by0 < kGameHeight; by0 += bh) {
    for (int bx0 = 0; bx0 < kGameWidth; bx0 += bw) {
      const int first = g_b7_nseg;
      const int rows = IntMin(bh, kGameHeight - by0);
      if (g_b7_nblk >= kB7MaxBlk || g_b7_nseg + rows > kB7MaxSegs)
        return false;
      int gminx = kB7Plane, gminy = kB7Plane, gmaxx = 0, gmaxy = 0;
      for (int r = by0; r < by0 + rows; r++) {
        int32_t X0, Y0;
        B7RowStart(m, r + 1, &X0, &Y0);
        const int32_t Xs = X0 + a * bx0, Xe = X0 + a * (bx0 + bw), Ys = Y0 + c * bx0, Ye = Y0 + c * (bx0 + bw);
        int mu0 = 255, mu1 = 0, mv0 = 255, mv1 = 0;
        if (tab) {
          mu0 = g_b7_rmin[0][Xs & 255];
          mu1 = g_b7_rmax[0][Xs & 255];
          mv0 = g_b7_rmin[1][Ys & 255];
          mv1 = g_b7_rmax[1][Ys & 255];
        } else {
          for (int x = bx0; x < bx0 + bw; x++) {
            int ru = (X0 + a * x) & 255, rv = (Y0 + c * x) & 255;
            if (ru < mu0) mu0 = ru;
            if (ru > mu1) mu1 = ru;
            if (rv < mv0) mv0 = rv;
            if (rv > mv1) mv1 = rv;
          }
        }
        int minx = IntMin(Xs >> 8, Xe >> 8) - 1, maxx = IntMax(Xs >> 8, Xe >> 8) + 2;
        int miny = IntMin(Ys >> 8, Ye >> 8) - 1, maxy = IntMax(Ys >> 8, Ye >> 8) + 2;
        if (minx < 0 || miny < 0 || maxx >= kB7Plane || maxy >= kB7Plane)
          return false;
        if (minx < gminx) gminx = minx;
        if (miny < gminy) gminy = miny;
        if (maxx > gmaxx) gmaxx = maxx;
        if (maxy > gmaxy) gmaxy = maxy;
        const float su = (float)(256 - mu0 - mu1) / 512.0f - uvh * du, sv = (float)(256 - mv0 - mv1) / 512.0f - uvh * dv;
        B7Seg *sg = &g_b7_seg[g_b7_nseg++];
        sg->base = NULL;
        sg->u0 = (float)Xs / 256.0f + su;
        sg->u1 = (float)Xe / 256.0f + su;
        sg->v0 = (float)Ys / 256.0f + sv;
        sg->v1 = (float)Ye / 256.0f + sv;
        sg->x0 = (int16_t)bx0;
        sg->x1 = (int16_t)(bx0 + bw);
        sg->y = (int16_t)r;
      }
      const int wx = gminx & ~15, wy = gminy & ~7;  /* block-aligned for the swizzled plane */
      const int ext = IntMax(gmaxx - wx, gmaxy - wy);
      if (ext >= kB7Blk)
        return false;
      if (ext > *maxext)
        *maxext = ext;
      for (int i = first; i < g_b7_nseg; i++) {
        g_b7_seg[i].u0 -= (float)wx;
        g_b7_seg[i].u1 -= (float)wx;
        g_b7_seg[i].v0 -= (float)wy;
        g_b7_seg[i].v1 -= (float)wy;
      }
      B7Blk *bk = &g_b7_blk[g_b7_nblk++];
      bk->wx = wx;
      bk->wy = wy;
      bk->first = first;
      bk->n = g_b7_nseg - first;
    }
  }
  return true;
}

/* One frame: a bind and a GU_TRIANGLES draw per block. */
static void B7FrameBlk(bool swz, uint32_t *build_us, uint32_t *total_us) {
  uint32_t t0 = Now();
  GuStartBoot();
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);
  sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
  sceGuClutLoad(32, g_b7_clut);
  sceGuTexMode(GU_PSM_T8, 0, 0, SWZ_FLAG(swz));
  TileVert *v = sceGuGetMemory((unsigned)g_b7_nseg * 6 * sizeof(TileVert));
  TileVert *w = v;
  for (int i = 0; i < g_b7_nseg; i++) {
    const B7Seg *sg = &g_b7_seg[i];
    const int16_t xl = (int16_t)(kLeft + sg->x0), xr = (int16_t)(kLeft + sg->x1);
    const int16_t yt = (int16_t)(kTop + sg->y), yb = (int16_t)(kTop + sg->y + 1);
    const TileVert tl = {sg->u0, sg->v0, xl, yt, 0}, bl = {sg->u0, sg->v0, xl, yb, 0};
    const TileVert tr = {sg->u1, sg->v1, xr, yt, 0}, br = {sg->u1, sg->v1, xr, yb, 0};
    w[0] = tl; w[1] = bl; w[2] = tr;
    w[3] = tr; w[4] = bl; w[5] = br;
    w += 6;
  }
  for (int b = 0; b < g_b7_nblk; b++) {
    const B7Blk *bk = &g_b7_blk[b];
    const uint8_t *tex = swz ? g_b7_plane_swz + (bk->wy >> 3) * (kB7Plane * 8) + (bk->wx >> 4) * 128
                             : g_b7_plane + bk->wy * kB7Plane + bk->wx;
    sceGuTexImage(0, kB7Blk, kB7Blk, kB7Plane, tex);  /* also flushes the texture cache: once per block */
    sceGuDrawArray(GU_TRIANGLES, kVtTile, bk->n * 6, NULL, v + bk->first * 6);
  }
  uint32_t t1 = Now();
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  *build_us = t1 - t0;
  *total_us = Now() - t0;
}

static void B7RunBlk(const char *tag, int sc, int bw, int bh, bool swz) {
  static B7Seg ref[kB7MaxSegs];
  int ext = 0;
  uint32_t t = Now();
  bool ok = B7PrepareBlocks(g_b7_m, bw, bh, 0.5f, false, &ext);
  const uint32_t scan = Now() - t;
  if (!ok) {
    PspLog("b7%s: %d deg %d%% blk-%s %dx%d unfit (a block needs a window >= %d or leaves the plane)", tag, kB7Deg[sc],
           kB7Pct[sc], swz ? "swz" : "lin", bw, bh, kB7Blk);
    return;
  }
  memcpy(ref, g_b7_seg, (size_t)g_b7_nseg * sizeof(B7Seg));
  t = Now();
  ok = B7PrepareBlocks(g_b7_m, bw, bh, 0.5f, true, &ext);
  const uint32_t tabus = Now() - t;
  const bool tabok = ok && memcmp(ref, g_b7_seg, (size_t)g_b7_nseg * sizeof(B7Seg)) == 0;
  if (!tabok) {
    PspLog("b7%s: %d deg %d%% blk-%s %dx%d table verification failed; skipping benchmark",
           tag, kB7Deg[sc], kB7Pct[sc], swz ? "swz" : "lin", bw, bh);
    return;
  }  
  uint32_t sb = 0, st = 0, mt = 0;
  for (int i = 0; i <= kB7Iters; i++) {
    uint32_t b, tt;
    B7FrameBlk(swz, &b, &tt);
    if (i == 0)
      continue;
    sb += b;
    st += tt;
    if (tt > mt)
      mt = tt;
  }
  PspLog("b7%s: %d deg %d%% blk-%s %dx%d binds=%d segs=%d maxext=%d prep_scan=%u prep_tab=%u tabok=%d build=%u total=%u max=%u "
         "us bad=%u",
         tag, kB7Deg[sc], kB7Pct[sc], swz ? "swz" : "lin", bw, bh, g_b7_nblk, g_b7_nseg, ext, (unsigned)scan, (unsigned)tabus,
         (int)tabok, (unsigned)(sb / kB7Iters), (unsigned)(st / kB7Iters), (unsigned)mt, (unsigned)B7Verify());
}

static void B7Run(const char *tag, int sc, int segw, float uvh) {
  const bool strips = segw > 0;
  uint32_t prep = 0, sb = 0, st = 0, mt = 0;
  if (strips) {
    uint32_t t = Now();
    bool ok = B7Prepare(g_b7_m, segw, uvh);
    prep = Now() - t;
    if (!ok) {
      PspLog("b7%s: %d deg %d%% seg=%d unfit (a segment does not fit one 512 window / leaves the plane)", tag, kB7Deg[sc],
             kB7Pct[sc], segw);
      return;
    }
  }
  for (int i = 0; i <= kB7Iters; i++) {
    uint32_t b, t;
    B7Frame(strips, &b, &t);
    if (i == 0)
      continue;  /* first frame is cold */
    sb += b;
    st += t;
    if (t > mt)
      mt = t;
  }
  PspLog("b7%s: %d deg %d%% %s seg=%d uvh=%s segs=%d prep=%u build=%u total=%u max=%u us bad=%u", tag, kB7Deg[sc], kB7Pct[sc],
         strips ? "strips" : "image ", segw, uvh > 0.25f ? "centre" : "corner", strips ? g_b7_nseg : 1, (unsigned)prep,
         (unsigned)(sb / kB7Iters), (unsigned)(st / kB7Iters), (unsigned)mt, (unsigned)B7Verify());
}

static void B7Scenario(const char *tag, int sc) {
  static const int kSeg[3] = {256, 64, 32};
  LineSnap ls;
  memset(&ls, 0, sizeof(ls));
  memcpy(g_b7_m, kB7Mat[sc], 4 * sizeof(int16_t));
  g_b7_m[4] = g_b7_m[5] = 512;  /* centre; scroll 384/392 puts the screen centre (128, 120) on plane (512, 512) */
  g_b7_m[6] = 384;
  g_b7_m[7] = 392;
  memcpy(ls.m7m, g_b7_m, sizeof(ls.m7m));
  ls.m7fl = 1;
  ls.mossize = 1;
  memset(g_m7img, 0, sizeof(g_m7img));
  uint32_t t0 = Now();
  for (int r = 0; r < kGameHeight; r++)
    M7RenderRow(&ls, r + 1, g_m7img + r * 256);
  uint32_t cpu = Now() - t0;
  t0 = Now();
  sceKernelDcacheWritebackRange(g_m7img, sizeof(g_m7img));
  PspLog("b7%s: %d deg %d%% CPU image %u us, writeback %u us", tag, kB7Deg[sc], kB7Pct[sc], (unsigned)cpu,
         (unsigned)(Now() - t0));
  B7Run(tag, sc, 0, 0.5f);
  for (int k = 0; k < 3; k++) {
    B7Run(tag, sc, kSeg[k], 0.5f);
    B7Run(tag, sc, kSeg[k], 0.0f);
  }
  for (int k = 0; k < 3; k += 2)  /* frame window: one bind, one draw (segments 256 and 32) */
    for (int var = 0; var < 4; var++) {
      B7RunWin(tag, sc, kSeg[k], 0.5f, var);
      B7RunWin(tag, sc, kSeg[k], 0.0f, var);
    }
  static const int kBlk[3][2] = {{32, 32}, {32, 16}, {64, 16}};  /* block windows (centre only: corner never worked) */
  for (int k = 0; k < 3; k++)
    for (int swz = 0; swz < 2; swz++)
      B7RunBlk(tag, sc, kBlk[k][0], kBlk[k][1], swz != 0);
}

static void Bench5r(void) {
  for (int i = 0; i < 0x8000; i++)
    g_vram[i] = (uint16_t)(B7Rand() >> 8);  /* random tile map and random pixels: any wrong texel shows */
  for (int i = 0; i < 256; i++)
    g_b7_clut[i] = B7Color((unsigned)i);
  sceKernelDcacheWritebackRange(g_b7_clut, sizeof(g_b7_clut));
  uint32_t t0 = Now();
  B7BuildPlane();
  uint32_t build = Now() - t0;
  t0 = Now();
  sceKernelDcacheWritebackRange(g_b7_plane, sizeof(g_b7_plane));
  PspLog("b7: plane build %u us (16384 cells), writeback of %u KB %u us, uv x=%.1f y=%.1f", (unsigned)build,
         (unsigned)(sizeof(g_b7_plane) >> 10), (unsigned)(Now() - t0), (double)g_uvh[0], (double)g_uvh[1]);
  t0 = Now();
  B7SwizzlePlane();
  build = Now() - t0;
  t0 = Now();
  sceKernelDcacheWritebackRange(g_b7_plane_swz, sizeof(g_b7_plane_swz));
  PspLog("b7: plane swizzle %u us (1 MB), writeback %u us", (unsigned)build, (unsigned)(Now() - t0));
  for (int sc = 0; sc < kB7Scen; sc++)
    B7Scenario("", sc);
  B7Scenario("-again", 0);  /* A/B/A: the first scenario once more */
  memset(g_vram, 0, sizeof(g_vram));  /* back to the state before the first frame (the first snapshot copies every page) */
  memset(g_m7img, 0, sizeof(g_m7img));
  memset(g_m7ikey, 0, sizeof(g_m7ikey));  /* gate 5t: the image and the VRAM snapshot were overwritten */
  g_m7i_stale = true;
  GuStartBoot();  /* leave the draw buffer black */
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuClearColor(0xff000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}
#endif

#if SM_PSP_LOG && SM_PSP_BENCH_COMPOSE
/* Boot-time GE timing of the composition pass (PSP_BENCH_COMPOSE=1, log builds). The pass costs about 6.7 ms of GE time per
 * frame in every scene; this times its pieces one by one on the real GE: one list per measurement (start, finish, sync),
 * 24 repeats, the first dropped. Draws into draw buffer 0 before the first frame; the source is g_texture (VRAM). The
 * strip rows draw the same pixels as the single sprite (ComposeQuad keeps the sampling positions), in columns of 64 / 32 /
 * 16 destination pixels. Times include the CPU building the (tiny) list, about 30 us: compare rows, not absolutes. */
enum { kCbReps = 24 };
enum { kCbEmpty, kCbClear, kCbClearFast, kCbCopy, kCbLinear, kCbNearest, kCbStrip64, kCbStrip32, kCbStrip16, kCbFull,
       kCbFullStrip32, kCbKinds };
static const char *const kCbName[kCbKinds] = {"empty list     ", "clear 480x272  ", "clear fast     ", "copy 256x224   ",
                                              "bilinear sprite", "nearest sprite ", "bilinear 64col ", "bilinear 32col ",
                                              "bilinear 16col ", "full compose   ", "full, 32col    "};

static uint32_t CbOnce(int kind) {
  const uint32_t t0 = Now();
  GuStartBoot();
  SelectDrawBuffer();
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_STENCIL_TEST);
  sceGuDisable(GU_DITHER);
  sceGuDisable(GU_CULL_FACE);
  sceGuDisable(GU_TEXTURE_2D);
  const bool full = kind == kCbFull || kind == kCbFullStrip32;
  if (kind == kCbClear || full) {
    sceGuClearColor(0xff000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
  }
  if (kind == kCbClearFast) {
    sceGuClearColor(0xff000000);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
  }
  if (kind == kCbCopy || full) {
    sceGuCopyImage(GU_PSM_5650, kLeft, kTop, kGameWidth, kSrcRows, 512, (void *)(uintptr_t)(0x04000000u | g_draw_off), 0, 0,
                   kGameWidth, (void *)(uintptr_t)(0x04000000u | (uint32_t)kVramTexture));
    sceGuTexSync();
  }
  if (kind >= kCbLinear) {
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
    sceGuTexImage(0, kGameWidth, 256, kGameWidth, g_texture);
    sceGuTexFlush();
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexFilter(kind == kCbNearest ? GU_NEAREST : GU_LINEAR, kind == kCbNearest ? GU_NEAREST : GU_LINEAR);
    const int strip = kind == kCbStrip64 ? 64 : (kind == kCbStrip32 || kind == kCbFullStrip32) ? 32 : kind == kCbStrip16 ? 16 : 0;
    if (!strip) {
      TileVert *v = sceGuGetMemory(2 * (int)sizeof(TileVert));
      ComposeQuad(v, 0.5f, 0.5f, (float)kGameWidth - 0.5f, (float)kSrcRows - 0.5f, kCompX, kCompY, kCompW, kCompH);
      GeDraw(kVtTile, 2, v);
    } else {
      const int n = (kCompW + strip - 1) / strip;
      TileVert *v = sceGuGetMemory(2 * n * (int)sizeof(TileVert));
      const float sx = ((float)kGameWidth - 1.0f) / (float)(kCompW - 1);
      for (int k = 0; k < n; k++) {
        const int i0 = k * strip, w = kCompW - i0 < strip ? kCompW - i0 : strip;
        const float u0 = 0.5f + (float)i0 * sx;
        ComposeQuad(&v[2 * k], u0, 0.5f, u0 + (float)(w - 1) * sx, (float)kSrcRows - 0.5f, kCompX + i0, kCompY, w, kCompH);
      }
      GeDraw(kVtTile, 2 * n, v);
    }
  }
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  return Now() - t0;
}

static void BenchCompose(void) {
  uint16_t *tex = g_texture;
  for (int i = 0; i < kGameWidth * 256; i++)  /* not a flat colour: every texel fetch is a different value */
    tex[i] = (uint16_t)(i * 2654435761u >> 16);
  PspLog("cbench: composition pass on the GE (us per list, %d repeats, first dropped), sprite 360x270 from 256x224", kCbReps);
  for (int kind = 0; kind < kCbKinds; kind++) {
    uint32_t sum = 0, mn = 0xffffffffu, mx = 0;
    CbOnce(kind);
    for (int i = 1; i < kCbReps; i++) {
      const uint32_t t = CbOnce(kind);
      sum += t;
      mn = t < mn ? t : mn;
      mx = t > mx ? t : mx;
    }
    PspLog("cbench: %s avg %u min %u max %u", kCbName[kind], (unsigned)(sum / (kCbReps - 1)), (unsigned)mn, (unsigned)mx);
  }
  memset(g_texture, 0, 256 * 256 * 2);
}
#endif

static bool PspRenderer_Initialize(void) {
  uintptr_t vram = (uintptr_t)sceGeEdramGetAddr() | 0x40000000U;
  g_atlas2 = (uint8_t *)(vram + kVramAtlas2);
  g_atlas4 = (uint8_t *)(vram + kVramAtlas4);
  g_atlas7 = (uint8_t *)(vram + kVramAtlas7);
  g_vram_u = (uint8_t *)vram;
  g_texture = (uint16_t *)(vram + kVramTexture);
  g_ge_ok = (unsigned)sceGeEdramGetSize() >= (unsigned)kVramNeeded;
  PspLog("edram size=%u needed=%u ge_ok=%d", (unsigned)sceGeEdramGetSize(),
         (unsigned)kVramNeeded, (int)g_ge_ok);
  g_glow_ok = SM_PSP_GE_GLOW && g_ge_ok && (unsigned)sceGeEdramGetSize() >= (unsigned)kVramGlowEnd;
  PspLog("glow: build=%d ok=%d (buffers %u..%u of %u) thresh=%d gain=%d%% src=%d haze_light=%d async=%d", (int)SM_PSP_GE_GLOW,
         (int)g_glow_ok, (unsigned)kVramGlow1, (unsigned)kVramGlowEnd, (unsigned)sceGeEdramGetSize(), (int)SM_PSP_GE_GLOW_THRESH,
         (int)SM_PSP_GE_GLOW_GAIN, (int)SM_PSP_GE_GLOW_SRC, (int)SM_PSP_GE_HAZE_LIGHT, (int)SM_PSP_ASYNC);
  g_tint_ok = SM_PSP_GE_SMOOTH_TINT && g_ge_ok && (unsigned)sceGeEdramGetSize() >= (unsigned)kVramTintEnd;
  PspLog("smooth tint: %d (strip %u..%u of %u)", (int)g_tint_ok, (unsigned)kVramTint, (unsigned)kVramTintEnd,
         (unsigned)sceGeEdramGetSize());
  PspLog("vtx16: %d bgvert=%u bytes", (int)SM_PSP_VTX16, (unsigned)sizeof(BgVert));
  g_img_ok = SM_PSP_LAYER_IMG && g_ge_ok && (unsigned)sceGeEdramGetSize() >= (unsigned)kVramImgEnd;
  g_imgh_ok = g_img_ok && SM_PSP_LAYER_IMG >= 2;
  g_img_skip_ok = g_img_ok && SM_PSP_LAYER_IMG >= 3;
  for (int i = 0; i < kImgSlots; i++)
    g_img_tex[i] = (uint8_t *)(vram + kVramImg0 + (uintptr_t)i * kImgBytes);
#if SM_PSP_DOOR_GE
  g_door_ok = g_ge_ok && (unsigned)sceGeEdramGetSize() >= (unsigned)(kVramImg0 + kImgBytes);
#endif
#if SM_PSP_DOOR_SNAP
  g_ds_ok = g_ge_ok && (unsigned)sceGeEdramGetSize() >= (unsigned)kVramImgEnd;  /* both layer image slots hold a snapshot */
  PspLog("door snap: %d (slots %u..%u of %u)", (int)g_ds_ok, (unsigned)kVramImg0, (unsigned)kVramImgEnd,
         (unsigned)sceGeEdramGetSize());
#endif
  PspLog("layer images: %d high=%d skip=%d (needs %u of %u)", (int)g_img_ok, (int)g_imgh_ok, (int)g_img_skip_ok, (unsigned)kVramImgEnd,
         (unsigned)sceGeEdramGetSize());

  sceGuInit();
  GuStartBoot();
  sceGuDrawBuffer(GU_PSM_5650, (void *)0, 512);
  sceGuDispBuffer(kPspWidth, kPspHeight, (void *)(512 * 272 * 2), 512);
  sceGuDepthBuffer((void *)(512 * 272 * 4), 512);
  sceGuOffset(2048 - kPspWidth / 2, 2048 - kPspHeight / 2);
  sceGuViewport(2048, 2048, kPspWidth, kPspHeight);
  sceGuScissor(0, 0, kPspWidth, kPspHeight);
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
  g_vcount_last = sceDisplayGetVcount();

  g_triple = SM_PSP_TRIPLE && (unsigned)sceGeEdramGetSize() >= (unsigned)(kVramBuf2 + kBufBytes);
  if (g_triple) {  /* the buffers the swap rotates through: software frames do not cover the borders */
    memset(g_vram_u + kBufOff[1], 0, kBufBytes);
    memset(g_vram_u + kBufOff[2], 0, kBufBytes);
  }
  PspLog("triple buffer: %d (needs %u of %u)", (int)g_triple, (unsigned)(kVramBuf2 + kBufBytes),
         (unsigned)sceGeEdramGetSize());

  if (g_ge_ok) {
    memset(g_hash2, 0xff, sizeof(g_hash2));
    memset(g_hash4, 0xff, sizeof(g_hash4));
    memset(g_hash7, 0xff, sizeof(g_hash7));
    memset(g_atlas2, 0, kAtlas2Bytes);
    memset(g_atlas4, 0, kAtlas4Bytes);
    memset(g_atlas7, 0, kAtlas7Bytes);
    UvSelfTest();
#if SM_PSP_LOG && SM_PSP_BENCH_COMPOSE
    BenchCompose();
#endif
#if SM_PSP_BENCH_ON
    Bench5ib();
#endif
#if SM_PSP_BENCH_M7_ON
    Bench5r();
#endif
  }
  g_ppu_line_hook = PspRenderer_CaptureLine;
#if SM_PSP_GE_DEFAULT
  // Start in GE + tint mode when the VRAM budget allows it. L+R+SELECT then cycles
  // GE+tint -> GE without tint -> software -> GE+tint.
  if (g_ge_ok) {
    g_ge_level = 1;
    g_ge_active = true;
    PspLog("renderer: GE (BG1-3 + OBJ + tint) by default");
  }
#endif
#if SM_PSP_LOG
  g_ppu_draw_begin = ProfPpuBegin;
  g_ppu_draw_end = ProfPpuEnd;
  {
    uint32_t t0 = Now(), x = 0;
    for (int i = 0; i < 2000; i++)
      x += Now();
    uint32_t dt = Now() - t0;
    PspLog("clock read cost: %u ns/call (x=%u); ppu timing on; strips=%d", (unsigned)(dt * 1000 / 2000),
           (unsigned)(x & 1), (int)SM_PSP_GE_STRIPS);
  }
#endif
  return true;
}

static void PspRenderer_Destroy(void) {
  GeWait();  /* the GE must be idle before the display is switched off */
  DoorTransitionRelease();
#if SM_PSP_DOOR_SNAP
  DoorSnapRelease();
#endif
  g_ppu_line_hook = NULL;
  g_ppu_draw_begin = g_ppu_draw_end = NULL;
  g_ppu_skip_draw = false;
  sceGuDisplay(GU_FALSE);
  sceGuTerm();
  g_atlas2 = g_atlas4 = NULL;
  g_texture = NULL;
}

static void PspRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  if (width != kGameWidth || height != kGameHeight)
    Die("PSP renderer requires a 256x240 framebuffer");
  *pixels = g_framebuffer;
  *pitch = kGameWidth * 4;
}

typedef struct PspVertex { float u, v; float x, y, z; } PspVertex;

#if SM_PSP_LOG
static void ProfLog(void) {
  M7Log();
  if (g_gws_n)
    PspLog("gewait split (us, all classes): scene=%u compose=%u mosaic=%u", (unsigned)(g_gws_scene / g_gws_n),
           (unsigned)(g_gws_compose / g_gws_n), (unsigned)(g_gws_mosaic / g_gws_n));
  g_gws_scene = g_gws_compose = g_gws_mosaic = g_gws_n = 0;
  for (int c = 0; c < 3; c++) {
    const Prof *p = &g_prof[c];
    if (p->period.n == 0)
      continue;
#define AVG(st) (unsigned)((st).n ? (st).sum / (st).n : 0), (unsigned)(st).max
    PspLog("T[%s] n=%u game=%u/%u ppu=%u/%u prep=%u/%u build=%u/%u", kProfName[c], (unsigned)p->period.n,
           AVG(p->game), AVG(p->ppu), AVG(p->prep), AVG(p->build));
    PspLog("  gewait=%u/%u vbwait=%u/%u period=%u/%u late=%u miss=%u hold=%u skip=%d",
           AVG(p->gewait), AVG(p->vbwait), AVG(p->period), (unsigned)p->late, (unsigned)p->miss,
           (unsigned)p->hold, (int)g_ppu_skip_draw);
    if (p->agewait.n)  /* async: the frame queued by the previous EndDraw, completed at the start of this one */
      PspLog("  async n=%u completion gewait=%u/%u vbwait=%u/%u", (unsigned)p->agewait.n, AVG(p->agewait),
             AVG(p->avbwait));
    PspLog("  cap=%u/%u cpy=%u/%u class=%u/%u bg=%u/%u obj=%u/%u emit=%u/%u", AVG(p->cap), AVG(p->cpy), AVG(p->cls),
           AVG(p->bg), AVG(p->obj), AVG(p->emit));
    PspLog("  tiles seen=%u/%u decoded=%u/%u | draws=%u/%u clut=%u/%u verts=%u/%u", AVG(p->tseen),
           AVG(p->tdec), AVG(p->draws), AVG(p->clut), AVG(p->verts));
    if (p->m7i.n)
      PspLog("  m7img cpu=%u/%u us n=%u", AVG(p->m7i), (unsigned)p->m7i.n);
#undef AVG
  }
  PspAudioMe_LogStats();  // no-op unless PSP_LOG, PSP_AUDIO_ME and PSP_ME_LOG are all on
  memset(g_prof, 0, sizeof(g_prof));
}
#endif

#if SM_PSP_LOG
#define DG_WHY(x) (g_dg_why = (uint8_t)(x))
static const char *const kDgCauseName[kDgCause] = {"ver", "swppu", "m7img", "gewait", "logic", "prep", "other"};

static void DgAdd(DgAcc *a, uint32_t period, unsigned cause) {
  static const uint32_t kEdge[kDgBins - 1] = {17000, 25000, 34000, 50000, 67000};
  unsigned b = 0;
  while (b < kDgBins - 1 && period > kEdge[b])
    b++;
  a->n++;
  a->sum += period;
  if (period > a->max)
    a->max = period;
  a->bin[b]++;
  if (period > (uint32_t)kLateUs)
    a->lost[cause] += period - (uint32_t)kVblankUs;
}

static void DgSceneLog(void) {
  const DgScene *c = &g_dg_sc;
  static unsigned lines;
  if (c->n < 20 || lines++ >= 400)  /* very short scenes and a flood of them are not worth a line */
    return;
  unsigned fps100 = (unsigned)((uint64_t)c->n * 100000000ull / c->sum);
  PspLog("SCENE f=%u..%u gs=%04x room=%04x cin=%04x area=%02x frames=%u fps=%u.%02u worst=%ums slow(>25ms)=%u", (unsigned)c->f0,
         (unsigned)(c->f0 + c->n - 1), c->gs, c->room, c->cin, c->area, (unsigned)c->n, fps100 / 100, fps100 % 100,
         (unsigned)(c->worst / 1000), (unsigned)c->slow);
}

/* Once per frame, after the period is known: accumulate, and close the scene summary when the scene changes. */
static void DgFrame(uint32_t period, unsigned cause) {
  DgAdd(&g_dg_win, period, cause);
  DgAdd(&g_dg_all, period, cause);
  const unsigned gs = GameRam16(0x998), room = GameRam16(0x79B), cin = GameRam16(0x1F51), area = g_ram[0x79F];
  DgScene *c = &g_dg_sc;
  if (c->n && (gs != c->gs || room != c->room || cin != c->cin)) {
    DgSceneLog();
    memset(c, 0, sizeof(*c));
  }
  if (!c->n) {
    c->f0 = (uint32_t)g_frame;
    c->gs = gs;
    c->room = room;
    c->cin = cin;
    c->area = area;
  }
  c->n++;
  c->sum += period;
  if (period > c->worst)
    c->worst = period;
  if (period > 25000u)
    c->slow++;
}

/* Every 300 frames: frame rate of the window and of the whole run at the current clock, the frame-time histogram (a frame of
 * 33 ms is a dropped vblank, 50 ms and more is a visible freeze) and which cause the time over one vblank went to. */
static void DgWindowLog(void) {
  const DgAcc *w = &g_dg_win, *a = &g_dg_all;
  if (!w->n || !a->sum)
    return;
  unsigned wf = (unsigned)((uint64_t)w->n * 100000000ull / w->sum), af = (unsigned)((uint64_t)a->n * 100000000ull / a->sum);
  PspLog("FPS f=%u win=%u.%02u mean=%uus max=%ums | all=%u.%02u | <=17ms=%u <=25=%u <=34=%u <=50=%u <=67=%u >67=%u | gs=%04x room=%04x cin=%04x area=%02x",
         (unsigned)g_frame, wf / 100, wf % 100, (unsigned)(w->sum / w->n), (unsigned)(w->max / 1000), af / 100, af % 100,
         (unsigned)w->bin[0], (unsigned)w->bin[1], (unsigned)w->bin[2], (unsigned)w->bin[3], (unsigned)w->bin[4],
         (unsigned)w->bin[5], GameRam16(0x998), GameRam16(0x79B), GameRam16(0x1F51), (unsigned)g_ram[0x79F]);
  char buf[128];
  int len = 0;
  buf[0] = 0;
  for (unsigned i = 0; i < kDgCause; i++)
    len += snprintf(buf + len, sizeof(buf) - (size_t)len, " %s=%u", kDgCauseName[i], (unsigned)(w->lost[i] / 1000));
  PspLog("  lost ms over one vblank:%s", buf);
  memset(&g_dg_win, 0, sizeof(g_dg_win));
}

/* Every frame that leaves the GE path while the GE is active (first 80): which kind, why, and where in the game. After one
 * such frame the software PPU draws kSkipAfter frames, which is the burst of ~35 ms ppu hitches. */
static void NoteFallback(const char *kind) {
  static unsigned n;
  if (n++ >= 80)
    return;
  PspLog("fallback f=%u %s lines=%d frame_reasons=0x%x last_reason=0x%x streak=%u mosaic=%d gs=%04x room=%04x door=%04x area=%02x",
         (unsigned)g_frame, kind, g_line_count, (unsigned)g_frame_reasons, (unsigned)g_last_reason, (unsigned)g_streak,
         g_mosaic_size, GameRam16(0x998), GameRam16(0x79B), GameRam16(0x78D), (unsigned)g_ram[0x79F]);
}
#define NOTE_FALLBACK(kind) NoteFallback(kind)
#else
#define NOTE_FALLBACK(kind) ((void)0)
#define DG_WHY(x) ((void)0)
#endif

/* The software-image frame: the CPU frame (g_texture / g_framebuffer) composed into the draw buffer by the GE, then
 * Present(false). Returns false if the list could not start or refused a call (never expected): the GE is idle and nothing
 * was drawn or flipped, so the caller holds the previous image for a frame. */
static bool SwFrameList(int swpath) {
  if (!FrameListStart(g_gu_list, kListBytes, "software"))
    return false;
  if (setjmp(g_gl_jb)) {
    GlAborted("software");
    return false;
  }
  GL_ARM();
  SelectDrawBuffer();
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
  if (swpath == 0) {
    sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
    sceGuTexImage(0, kTexWidth, kTexHeight, kTexWidth, g_texture);
  } else {
    /* GE copy RAM -> VRAM (physical address 0x04xxxxxx), then sample the VRAM copy. */
    void *vram_phys = (void *)(0x04000000u | ((uintptr_t)g_texture & 0x001fffffu));
    sceGuCopyImage(GU_PSM_8888, 0, 0, kGameWidth, kGameHeight, kGameWidth, g_framebuffer, 0, 0, 256, vram_phys);
    sceGuTexSync();
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, 256, 256, 256, g_texture);
  }
  sceGuTexFlush();
  const ComposeSrc csrc = swpath == 0 ? (ComposeSrc){g_texture, GU_PSM_5650, kTexWidth, kTexHeight, kTexWidth}
                                      : (ComposeSrc){g_texture, GU_PSM_8888, 256, 256, 256};
  ComposeDraw(&csrc);
  Present(false);
  return true;
}

static void PspRenderer_EndDraw(void) {
  PROF(uint32_t t_enter = Now();)
  g_frame++;
#if SM_PSP_LOG
  const unsigned dg_why_this = g_dg_why;  /* the decision the previous frame made for this one's software PPU */
#endif
  g_prep_us = g_build_us = g_gewait_us = g_vbwait_us = 0;
  /* Before anything writes a buffer the GE may still read (atlases, CLUTs, layer images, g_m7img, g_texture, g_gu_list)
   * and before the software path starts a list. Its waits are the previous frame's (see GeWait). */
  const bool waited = GeWait();
  bool drew_ge = false, held = false, handoff = false;

  if (g_ge_active && g_line_count > 0) {
    if (g_mosaic_size > 1 && g_lines[0].forced) {
      held = true;  /* mosaic handoff: the screen is blanked while the next scene loads; keep showing the last (peak mosaic) image */
      handoff = true;  /* expected, not a GE misprediction: the GE streak and the PPU skip stay (the log showed each handoff
                        * cost 8 software-PPU frames of ~35 ms) */
    } else if (DrawGe())
      drew_ge = true;
    else if (g_ppu_skip_draw)
      held = true;  /* the software PPU skipped this frame, so there is no software image to fall back to */
  }

  if (drew_ge) {
    g_frames_ge++;
    g_streak++;
    if (g_frame_subnoop)
      g_frames_subnoop++;
  } else if (held) {
    /* Misprediction: repeat the previous image for one frame (nothing is drawn
     * or swapped, so the panel keeps showing it), keep the pacing, and let the
     * software PPU draw the next frames until the GE has been stable again. */
    NOTE_FALLBACK(handoff ? "handoff" : "held");
    g_frames_sw++;
    PROF(g_holds++;)
    if (!handoff)
      g_streak = 0;  /* a misprediction: draw the next frames in software until the GE has been stable again */
    if (!waited)  /* GeWait() has just waited for a vblank (if ahead) and queued a frame */
      Pace();
    g_vcount_last = sceDisplayGetVcount();
  } else {
    /* Software path (unchanged behaviour). Also used when the GE path hands a
     * frame back, and for the first frame after a toggle. */
    if (g_ge_active) {
      NOTE_FALLBACK(g_line_count > 0 ? "refused" : "nolines");
      g_frames_sw++;
    }
    g_streak = 0;
    g_t_draw_start = Now();
    int swpath = g_swpath >= 0 ? g_swpath : (int)((g_cal_frames / kSwCalFrames) % kSwPaths);
#if SM_PSP_DOOR_SNAP
    if (g_ds_state != kDsIdle) {  /* the snapshots come from GE frames only: the live (destination) frame shows */
      PspLog("door snap: software frame, transition dropped");
      DoorSnapRelease();
    }
#endif
#if SM_PSP_DOOR_GE
    if (g_door_reserved) {  /* the software PPU cannot draw the override palettes: the game's own transition continues */
      PspLog("door: software frame, GE door transition dropped");
      DoorTransitionRelease();
    }
#endif
    if (swpath == 0)
      PspRenderer_Convert();
    sceKernelDcacheWritebackRange(g_framebuffer, sizeof(g_framebuffer));
    g_t_list_start = Now();
    g_prep_us = g_t_list_start - g_t_draw_start;
    if (!SwFrameList(swpath)) {
      /* as a held frame: the panel keeps the previous image, the pacing stays */
      if (!waited)
        Pace();
      g_vcount_last = sceDisplayGetVcount();
    } else if (g_swpath < 0) {  /* calibration: CPU + GE cost of getting the frame on screen, per path */
      uint32_t k = g_cal_frames++;
      if (k % kSwCalFrames >= kSwCalSkip) {
        g_cal_sum[swpath] += g_prep_us + g_gewait_us;
        g_cal_n[swpath]++;
      }
      if (g_cal_frames == kSwPaths * kSwCalFrames) {
        uint32_t a0 = g_cal_n[0] ? g_cal_sum[0] / g_cal_n[0] : ~0u, a1 = g_cal_n[1] ? g_cal_sum[1] / g_cal_n[1] : ~0u;
        g_swpath = a1 < a0 ? 1 : 0;
        PspLog("swpath: convert565=%u us, gecopy8888=%u us -> using %d", (unsigned)a0, (unsigned)a1, g_swpath);
      }
    }
  }

  /* Decide for the NEXT frame whether the software PPU may skip its drawing. */
  g_ppu_skip_draw = g_ge_active && g_streak >= kSkipAfter;
  DG_WHY(g_ppu_skip_draw ? 0 : 1);
  if (g_ovf_watch) {
    g_ovf_watch--;
    g_ppu_skip_draw = false;
    DG_WHY(2);
  }
  if (g_want_ms_verify) {  /* the first frames of each class are checked against the software image */
    g_want_ms_verify = false;
    if ((int32_t)(g_frame - g_verify_next) >= 0) {  /* at most one such frame per SM_PSP_VERIFY_GAP frames: a want that */
      g_verify_next = g_frame + (uint32_t)SM_PSP_VERIFY_GAP;  /* falls inside the gap is dropped, the class asks again */
      g_verify_armed++;                                       /* on the next frame it is still being verified */
      g_ppu_skip_draw = false;
      DG_WHY(3);
      g_audit_now = true;
    } else {
      g_verify_deferred++;
    }
  }
#if SM_PSP_LOG && SM_PSP_VERIFY
  if (g_ge_active && kAuditFrames && (g_frame + 1) % kAuditFrames == 0) {  /* next frame: software PPU draws too, GE image gets checked */
    g_ppu_skip_draw = false;
    DG_WHY(4);
    g_audit_now = true;
  }
#endif

#if SM_PSP_LOG
  /* Per-frame timing. "game" is everything since the previous frame finished:
   * the game logic, input and the software PPU (its share is "ppu"). */
#if SM_PSP_PROBES
  M7FrameEnd();
  WinFrameEnd();
  FlickFrameEnd();
#endif
  uint32_t t_end = Now();
  if (g_t_prev_end != 0) {
    Prof *p = &g_prof[!g_ge_active ? 2 : (drew_ge ? 0 : 1)];
    uint32_t period = t_end - g_t_prev_end;
    StatAdd(&p->game, t_enter - g_t_prev_end);
    StatAdd(&p->ppu, g_ppu_us);
    StatAdd(&p->prep, g_prep_us);
    StatAdd(&p->build, g_build_us);
    StatAdd(&p->gewait, g_gewait_us);
    StatAdd(&p->vbwait, g_vbwait_us);
    StatAdd(&p->period, period);
    StatAdd(&p->cap, g_cap_us);
    StatAdd(&p->cpy, g_cpy_us);
    StatAdd(&p->cls, g_pc_class);
    StatAdd(&p->bg, g_pc_bg);
    StatAdd(&p->obj, g_pc_obj);
    StatAdd(&p->emit, g_pc_emit);
    StatAdd(&p->tseen, g_c_tseen);
    StatAdd(&p->tdec, g_c_tdec);
    StatAdd(&p->draws, g_c_draws);
    StatAdd(&p->clut, g_c_clut);
    StatAdd(&p->verts, g_c_verts);
    if (g_m7i_us)
      StatAdd(&p->m7i, g_m7i_us);
    if (period > kLateUs)
      p->late++;
    if (period > kMissUs)
      p->miss++;
    if (held)
      p->hold++;
    {
      const uint32_t dg_game = t_enter - g_t_prev_end;
      const uint32_t dg_logic = dg_game > g_cap_us + g_ppu_us ? dg_game - g_cap_us - g_ppu_us : 0;
      DgFrame(period, g_hit_verified ? 0u : g_ppu_us ? 1u : g_m7i_us ? 2u : g_gewait_us > 3000 ? 3u : dg_logic > 8000 ? 4u
                      : (g_prep_us + g_build_us) > 6000 ? 5u : 6u);
    }
    if (period > kHitchUs && g_hitch_n++ < (uint32_t)kHitchPerWindow) {
      uint32_t game = t_enter - g_t_prev_end;
      uint32_t logic = game > g_cap_us + g_ppu_us ? game - g_cap_us - g_ppu_us : 0;
      PspLog("HITCH f=%u period=%u logic=%u cap=%u ppu=%u prep=%u build=%u gewait=%u vb=%u | ge=%d held=%d ver=%d tint=%d bruns=%d m7=%d/%d ovf=%d mos=%d sh=%d sf=%d w2=%d | why=%u vc=%x st=%u gs=%04x room=%04x cin=%04x",
             (unsigned)g_frame, (unsigned)period, (unsigned)logic, (unsigned)g_cap_us, (unsigned)g_ppu_us,
             (unsigned)g_prep_us, (unsigned)g_build_us, (unsigned)g_gewait_us, (unsigned)g_vbwait_us,
             drew_ge, held, g_hit_verified, g_ntint, g_nbruns, g_m7_nq, g_m7_img, g_ovf_rows, g_mos_frame,
             g_sh_frame, g_sf_frame, g_w2_frame, dg_why_this, (unsigned)g_dg_vclass, (unsigned)g_streak, GameRam16(0x998),
             GameRam16(0x79B), GameRam16(0x1F51));
    }
  }
  g_ppu_us = g_cap_us = g_cpy_us = g_m7i_us = 0;
  g_hit_verified = false;
  g_dg_vclass = 0;
  g_pc_class = g_pc_bg = g_pc_obj = g_pc_emit = 0;
  g_c_tseen = g_c_tdec = g_c_draws = g_c_clut = g_c_verts = 0;
  g_t_prev_end = t_end;
  if (g_note_pending) {
    g_note_pending = false;
    PspLog("NEW reason bits=0x%x line=%d mode=%u TM=%02x TS=%02x math=%02x add=%u sub=%u half=%u fixed=%04x clip=%u prev=%u tmw=%02x tsw=%02x wsel=%x gs=%04x room=%04x area=%u",
           g_note.bits, g_note.line, g_note.mode, g_note.tm, g_note.ts, g_note.mathmask, g_note.add,
           g_note.sub, g_note.half, g_note.fixed, g_note.clip, g_note.prevent, g_note.tmw, g_note.tsw, g_note.wsel,
           g_note.gs, g_note.room, g_note.area);
  }
  if (g_ge_active && (g_frame % 300) == 0) {
    PspLog("f=%u ge=%u (subnoop=%u blend=%u tint=%u) sw=%u gaps=%u ovf=%u mosf=%u m7i=%u prevf=%u cwf=%u sfx=%u shf=%u sho=%u w2f=%u shn=%u sfo=%u", (unsigned)g_frame, (unsigned)g_frames_ge,
           (unsigned)g_frames_subnoop, (unsigned)g_frames_blend, (unsigned)g_frames_tint, (unsigned)g_frames_sw,
           (unsigned)g_capture_gaps, (unsigned)g_frames_ovf, (unsigned)g_frames_mos, (unsigned)g_frames_m7i,
           (unsigned)g_frames_prev, (unsigned)g_frames_cw, (unsigned)g_frames_sf, (unsigned)g_frames_sh,
           (unsigned)g_frames_sho, (unsigned)g_frames_w2, (unsigned)g_frames_shn, (unsigned)g_frames_sfo);
    PspLog("  why: mode=%u big=%u mos=%u win=%u clip=%u tint=%u b3p=%u budget=%u subobj=%u subbg=%u",
           (unsigned)g_reason_counts[0], (unsigned)g_reason_counts[1], (unsigned)g_reason_counts[2],
           (unsigned)g_reason_counts[3], (unsigned)g_reason_counts[4], (unsigned)g_reason_counts[5],
           (unsigned)g_reason_counts[6], (unsigned)g_reason_counts[7], (unsigned)g_reason_counts[8],
           (unsigned)g_reason_counts[9]);
    PspLog("  why2: objcfg=%u objlimit=%u subwin=%u submulti=%u mathbright=%u m7=%u", (unsigned)g_reason_counts[10],
           (unsigned)g_reason_counts[11], (unsigned)g_reason_counts[12], (unsigned)g_reason_counts[13],
           (unsigned)g_reason_counts[14], (unsigned)g_reason_counts[15]);
    PspLog("  peak: tiles=%u/%d list=%uKB/%dKB vtx=%uKB/%dKB (pool=%d)", (unsigned)g_peak_tiles, (int)kMaxTilesPerFrame,
           (unsigned)(g_peak_list >> 10), (int)(kListBytes >> 10), (unsigned)(g_peak_vtx >> 10),
           (int)(kVtxPoolBytes >> 10), (int)SM_PSP_VTX_POOL);
    g_peak_tiles = g_peak_list = g_peak_vtx = 0;
    PspLog("  gl: finished list peak=%uKB/%dKB refused-calls=%u aborted-frames=%u start-fail=%u finish-err=%u finish-fail=%d",
           (unsigned)(g_gl_peak >> 10), (int)(kListBytes >> 10), (unsigned)g_gl_trips, (unsigned)g_gl_aborts,
           (unsigned)g_gl_start_fail, (unsigned)g_gl_finish_err, (int)g_gl_finish_fail);
    g_gl_peak = 0;
    PspLog("  verify: gap=%d armed=%u deferred=%u", (int)SM_PSP_VERIFY_GAP, (unsigned)g_verify_armed, (unsigned)g_verify_deferred);
    g_verify_armed = g_verify_deferred = 0;
    PspLog("  rowcache: hit=%u miss=%u", (unsigned)g_rc_hit, (unsigned)g_rc_miss);
    g_rc_hit = g_rc_miss = 0;
    PspLog("  capreuse: reused=%u full=%u chk=%u bad=%u on=%d win=%u winon=%d", (unsigned)g_cap_n_reuse,
           (unsigned)g_cap_n_full, (unsigned)g_cap_n_chk, (unsigned)g_cap_n_bad, (int)g_cap_reuse_ok,
           (unsigned)g_cap_n_win, (int)g_cap_win_ok);
    g_cap_n_reuse = g_cap_n_full = g_cap_n_chk = g_cap_n_bad = g_cap_n_win = 0;
    PspLog("  layerimg: on=%d high=%d frames=%u calls=%u skip=%u blk=%u/%u upd=%u/%u us=%u/%u", (int)g_img_ok,
           (int)g_imgh_ok, (unsigned)g_frames_img, (unsigned)g_li_n, (unsigned)g_li_skip, (unsigned)g_li_blkm, (unsigned)g_li_blkt, (unsigned)(g_li_n ? g_li_upd / g_li_n : 0), (unsigned)g_li_updmax,
           (unsigned)(g_li_n ? g_li_us / g_li_n : 0), (unsigned)g_li_usmax);
    {  /* which VRAM pages block the layer-image scans, by page number */
      char pgbuf[160];
      int len = 0;
      pgbuf[0] = 0;
      for (int a = 0; a < kVramPages && len < (int)sizeof(pgbuf) - 16; a++)
        if (g_li_pg[a])
          len += snprintf(pgbuf + len, sizeof(pgbuf) - (size_t)len, " %02x=%u", a, (unsigned)g_li_pg[a]);
      if (len)
        PspLog("  imgblk pages:%s", pgbuf);
      memset(g_li_pg, 0, sizeof(g_li_pg));
    }
    g_li_n = g_li_upd = g_li_updmax = g_li_us = g_li_usmax = g_li_skip = g_li_blkm = g_li_blkt = 0;
    PspLog("  subimg: frames=%u ok=%d%d%d audited=%u,%u,%u", (unsigned)g_frames_simg, (int)g_simg_ok[0], (int)g_simg_ok[1],
           (int)g_simg_ok[2], (unsigned)g_simg_vn[0], (unsigned)g_simg_vn[1], (unsigned)g_simg_vn[2]);
    PspLog("  m7wrap: frames=%u ok=%d audited=%u", (unsigned)g_frames_m7w, (int)g_m7w_ok, (unsigned)g_m7w_vn);
#if SM_PSP_ASYNC
    PspLog("  async: queued=%u sync-nofit=%u sync-startfail=%u viol=%u ok=%d | compose list used<=%u bound<=%u cap=%u, implied cmd B/call<=%u (assumed %u) (window max)",
           (unsigned)g_frames_async, (unsigned)g_async_nofit, (unsigned)g_async_fail, (unsigned)g_async_viol,
           (int)g_async_ok, (unsigned)g_comp_used_hw, (unsigned)g_comp_bound_hw, (unsigned)kComposeCap,
           (unsigned)g_comp_call_hw, (unsigned)kGuCallMax);
    PspLog("  async mode=%d: marker checks=%u late=%u missing=%u | vblanks between flips: 0=%u 1=%u 2=%u 3+=%u (0 = pacing gate let two flips share a vblank)",
           (int)SM_PSP_ASYNC, (unsigned)g_sent_chk, (unsigned)g_sent_late, (unsigned)g_sent_fail,
           (unsigned)g_flip_d[0], (unsigned)g_flip_d[1], (unsigned)g_flip_d[2], (unsigned)g_flip_d[3]);
  #if SM_PSP_LOG
     g_comp_used_hw = g_comp_bound_hw = g_comp_call_hw = 0;
     memset(g_flip_d, 0, sizeof(g_flip_d));
  #endif
#endif
    PspLog("  m7reuse: on=%d rows built=%u kept=%u audited=%u bad=%u", (int)g_m7i_reuse_ok, (unsigned)g_m7i_built,
           (unsigned)g_m7i_kept, (unsigned)g_m7i_audits, (unsigned)g_m7i_bad);
#if SM_PSP_M7_BLK
    PspLog("  m7blk: on=%d frames=%u verified=%u bad=%u | taken=%u (64x16=%u 32x16=%u 32x8=%u) not taken: off=%u same=%u flags=%u window=%u",
           (int)g_m7b_ok, (unsigned)g_frames_m7b, (unsigned)g_m7b_vn, (unsigned)g_m7b_vbad, (unsigned)g_m7b_why[kBlkOk],
           (unsigned)g_m7b_cfg[0], (unsigned)g_m7b_cfg[1], (unsigned)g_m7b_cfg[2], (unsigned)g_m7b_why[kBlkOff],
           (unsigned)g_m7b_why[kBlkSame], (unsigned)g_m7b_why[kBlkFlags], (unsigned)g_m7b_why[kBlkWindow]);
    if (g_m7b_tn)
      PspLog("  m7blk time per frame (us): prep=%u plane=%u vertices=%u draw=%u (%u frames)", (unsigned)(g_m7b_t_prep / g_m7b_tn),
             (unsigned)(g_m7b_t_plane / g_m7b_tn), (unsigned)(g_m7b_t_vtx / g_m7b_tn), (unsigned)(g_m7b_t_draw / g_m7b_tn),
             (unsigned)g_m7b_tn);
    PspLog("  m7blk backoff: %u frames went to the CPU image without trying a block size", (unsigned)g_m7b_fast);
    g_m7b_fast = 0;
    if (g_m7b_rf_n)
      PspLog("  m7blk refused: n=%u max|a|=%d max|b|=%d max|c|=%d max|d|=%d | 32x8 first window px %d..%d (limit %d)",
             (unsigned)g_m7b_rf_n, g_m7b_rf_a, g_m7b_rf_b, g_m7b_rf_c, g_m7b_rf_d, g_m7b_rf_e8min, g_m7b_rf_e8max,
             (int)kM7WinMax);
    g_m7b_rf_n = 0;
    g_m7b_rf_a = g_m7b_rf_b = g_m7b_rf_c = g_m7b_rf_d = g_m7b_rf_e8max = 0;
    g_m7b_rf_e8min = 1 << 30;
    memset(g_m7b_why, 0, sizeof(g_m7b_why));
    memset(g_m7b_cfg, 0, sizeof(g_m7b_cfg));
    g_m7b_t_prep = g_m7b_t_plane = g_m7b_t_vtx = g_m7b_t_draw = g_m7b_tn = 0;
#endif
    PspLog("  m7reuse: image frames with a dropped plane (mode 7 VRAM page written)=%u", (unsigned)g_m7i_stalef);
#if SM_PSP_M7_PROBE
    if (g_m7p_rows)
      PspLog("  m7probe: rows=%u cold=%u.%u us/row warm=%u.%u us/row (miss share %d%%)", (unsigned)g_m7p_rows,
             (unsigned)(g_m7p_cold * 10 / g_m7p_rows / 10), (unsigned)(g_m7p_cold * 10 / g_m7p_rows % 10),
             (unsigned)(g_m7p_warm * 10 / g_m7p_rows / 10), (unsigned)(g_m7p_warm * 10 / g_m7p_rows % 10),
             (int)(g_m7p_cold > g_m7p_warm ? (g_m7p_cold - g_m7p_warm) * 100 / g_m7p_cold : 0));
    g_m7p_rows = g_m7p_cold = g_m7p_warm = 0;
#endif
    g_m7i_stalef = 0;
    g_m7i_built = g_m7i_kept = g_m7i_audits = 0;
#if SM_PSP_TILE_GEN
    PspLog("  tilegen: on=%d skipped=%u audited=%u bad=%u", (int)g_tgen_ok, (unsigned)g_tg_skip, (unsigned)g_tg_audit, (unsigned)g_tg_bad);
    g_tg_skip = g_tg_audit = 0;
#endif    
    S5I_LOG();
  }
  if ((g_frame % 300) == 0) {
    DgWindowLog();
    if (g_hitch_n > (uint32_t)kHitchPerWindow)
      PspLog("hitches: %u in the last 300 frames (first %d shown)", (unsigned)g_hitch_n, (int)kHitchPerWindow);
    g_hitch_n = 0;
#if SM_PSP_PROBES
    FlickLog();
#endif
    ProfLog();
  }
#endif
  /* Write the buffered log when due. The stall is kept out of the statistics. */
  if (PspLog_Tick((unsigned)g_frame)) {
    PROF(g_t_prev_end = Now();)
  }
}

static const struct RendererFuncs kPspRendererFuncs = {
  &PspRenderer_Initialize,
  &PspRenderer_Destroy,
  &PspRenderer_BeginDraw,
  &PspRenderer_EndDraw,
};

uint8_t *PspRenderer_FrameBuffer(void) {
  return g_framebuffer;
}

void PspRenderer_Create(struct RendererFuncs *funcs) {
  *funcs = kPspRendererFuncs;
}
