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

#if SM_PSP_LOG
#define PROF(...) __VA_ARGS__
#else
#define PROF(...)
#endif

#ifndef SM_PSP_SW_PATH
#define SM_PSP_SW_PATH -1
#endif

#ifndef SM_PSP_GE_STRIPS
#define SM_PSP_GE_STRIPS 0
#endif

/* Gate 4b: the PPU writes 8888 in the GE's byte order (R first, see ppu.c). Sampling that frame straight
 * from main RAM cost 17.5 ms of GE time on hardware (log g4b), so software frames now pick, by
 * measuring at run time (SM_PSP_SW_PATH = -1), between the old CPU conversion to a 565 VRAM texture
 * and a GE copy of the 8888 frame into VRAM followed by sampling it there.
 * Capture stops once a frame has a fallback reason (no VRAM copy / per-line work for lost frames).
 * Gate 4a: sprites are drawn first, in reverse precedence order with the depth test ALWAYS, each
 * sprite writing its own depth marker ((prio*4+2)*256 + 127-k); sprite-vs-sprite is decided by OAM
 * order alone and the priority bits only matter against the BGs. BG passes follow with GEQUAL, z =
 * SNES level * 256 (BG3 high is always level 15, as in ppu.c). Depth is always on. New: fixed-colour
 * and half-colour tint (colour math without a subscreen layer) as a blended overlay of CPU rectangles
 * plus the depth-marker restore pass; colour math below full brightness falls back to software.
 * Gate 3.2: each layer run is walked once into a list of non-empty tiles that the
 * subscreen check, counting and emission share; cheaper per-line capture; skip
 * hysteresis 8 frames; strips off by default (no measurable gain on hardware).
 * Gate 3.1: logging/profiling compile out with PSP_LOG=0 (see psp_log.h); finer
 * profiling; large 2D rectangles drawn as 64-pixel strips (PSP_GE_STRIPS).
 * Gate 3 scope: as 2c plus frame-time profiling (per path, logged every 300 frames)
 * and skipping the software PPU's per-line drawing once the GE path has
 * handled a run of consecutive frames (see PspRenderer_EndDraw).
 * Gate 2c scope: as 2b plus real subscreen blending for one subscreen BG layer
 * (add or subtract, no half colour, fixed colour 0).
 * Gate 2b scope: as 2a.1 plus sprites (OBJ) with SNES priority ordering.
 * Gate 2a.1 scope: as 2a, plus exact fallback for the game's default color-math
 * setup (empty subscreen add is a no-op). Earlier text follows.
 * Gate 2a scope: software blit (unchanged) + GE path for Mode 1 BG1/BG2 (4bpp),
 * BG3 (2bpp) and the per-line backdrop, with SNES tile-priority ordering.
 * NOT drawn by the GE yet: sprites (Gate 2b). Frames that use windows, color
 * math, mosaic, big tiles, Mode != 1 or exceed the tile budget fall back to the
 * software image for that frame; the reason counters in the log say how often. */

#define PSP_VBLANK_GATE 1  /* wait for vblank only when running ahead of the panel */

enum {
  kPspWidth = 480,
  kPspHeight = 272,
  kGameWidth = 256,
  kGameHeight = 240,
  kTexWidth = 512,
  kTexHeight = 256,
  kMaxLines = 240,
  kLayers = 3,
  kListWords = 64 * 1024,
  kMaxTilesPerFrame = 3500,  /* 3500 * 2 * 16 B = 112 KB of the 256 KB list */
  kMaxSpriteQuads = 2000,    /* 2000 * 2 * 16 B = 64 KB */
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
  kVramNeeded = kVramAtlas7 + kAtlas7Bytes,
};

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
} BgSnap;

typedef struct LineSnap {
  BgSnap bg[kLayers];
  uint16_t cg0;       /* CGRAM entry 0 (backdrop); colour is baked per run in DrawGe */
  uint8_t forced;     /* forced blank: black */
  uint16_t reasons;
  uint8_t brightness, bg3prio, mathmask, submath;  /* submath: subscreen add may matter */
  uint8_t objon;                                   /* OBJ enabled on the main screen */
  uint8_t subneg, subwin;                          /* subtract mode; subscreen layers (bit l) windowed */
  uint8_t tint, tinthalf;                          /* fixed-colour math on this row; result halved */
  uint16_t tintfix;                                /* fixed colour, 5-5-5 (R | G << 5 | B << 10) */
  uint8_t subobj;                                  /* OBJ on the subscreen and added by colour math */
} LineSnap;

typedef struct TileVert { float u, v; int16_t x, y, z; } TileVert;
typedef struct ColorVert { uint32_t color; int16_t x, y, z; } ColorVert;
typedef struct Run { uint16_t r0, r1, first; } Run;
typedef struct Geom { TileVert *v; int n; } Geom;

static uint32_t g_gu_list[kListWords] __attribute__((aligned(16)));
static uint8_t *g_atlas2, *g_atlas4;        /* uncached VRAM tile atlases */
static uint16_t *g_texture;                 /* uncached VRAM: 565 texture, or the 8888 copy target (256x256x4 = same bytes) */
/* Software-PPU frame, 256x256 8888 (rows 240-255 unused). */
static uint8_t g_framebuffer[kGameWidth * 256 * 4] __attribute__((aligned(64)));

static LineSnap g_lines[kMaxLines];
static int g_line_count;
static uint16_t g_frame_reasons;
#if SM_PSP_LOG
static uint16_t g_logged_reasons;
#endif
static bool g_frame_dead;  /* a fallback reason is already known: stop capturing this frame */
static bool g_frame_subnoop;
static uint32_t g_frames_subnoop;
static Run g_subruns[kMaxLines];
#if SM_PSP_LOG
typedef struct Note { int line; unsigned bits, mode, tm, ts, mathmask, add, sub, half, fixed, clip, prevent, tmw, wsel; } Note;
static Note g_note;
static bool g_note_pending;
#endif
static uint32_t g_capture_gaps;
static uint16_t g_vram[0x8000] __attribute__((aligned(16)));
static uint16_t g_cgram[256] __attribute__((aligned(16)));

static uint16_t g_oam[256];
static uint8_t g_hoam[32];
static uint8_t g_obj_size, g_obj_start;
static uint16_t g_obj_adr1, g_obj_adr2;
static bool g_obj_rot;
typedef struct Sprite { int16_t x; uint8_t y, size; uint16_t attr; } Sprite;
typedef struct ObjSeg { uint8_t pal; uint16_t first, n; } ObjSeg;  /* first/n in vertices */
static Sprite g_spr[128];                 /* in precedence order (index k wins over k+1) */
static int g_nspr, g_spr_quads;
static ObjSeg g_objseg[128];              /* runs of consecutive sprites with one palette */
static int g_nobjseg;
static TileVert *g_objbase;
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
/* Mode 7 (GE path): one constant matrix per frame, b = c = 0, a, d > 0, large field, no flips/charfill/extbg. */
typedef struct M7Cfg { int16_t m[8]; uint8_t fl, bright, bg1; } M7Cfg;
static M7Cfg g_m7c;
static bool g_m7_set;
static int g_frame_mode = -1;        /* BG mode of this frame (first line that is not forced blank) */
static bool g_m7_ok = true;          /* cleared if the texture-coordinate self test or the verification fails */
static float g_uvh[2] = {0.5f, 0.5f}; /* texture coordinate offset in pixels at the sample point (x, y) */
static bool g_verify_now;
static uint32_t g_m7_vn, g_m7_vbad, g_m7_vnear;
enum { kM7Verify = 20, kM7MaxQuads = 4096 };
typedef struct M7Quad { int16_t x0, x1, y0, y1; float u0, u1, v0, v1; } M7Quad;
static M7Quad g_m7q[kM7MaxQuads];
static int g_m7_nq;
static TileVert *g_m7v;
static uint16_t g_seen2[4096], g_seen4[2048];
static uint8_t g_empty2[4096], g_empty4[2048];
static uint16_t g_mask2[4096], g_mask4[2048];  /* bit i set: tile uses pixel value i */
static uint16_t g_stamp;
static uint32_t g_clut2[16][8][8] __attribute__((aligned(64)));
static uint32_t g_clut4[16][8][16] __attribute__((aligned(64)));
static uint16_t g_clut2_stamp[16], g_clut4_stamp[16];

static Run g_runs[kLayers][kMaxLines];
static int g_nruns[kLayers];
static int g_counts[kLayers][kMaxLines][2][8];
static Geom g_geom[kLayers][kMaxLines][2][8];

// One non-empty tile of a layer run: destination before clipping, atlas tile, palette and flags. Built once per run and shared
typedef struct TileEnt { int16_t x, y; uint16_t pt; uint8_t pal, fl; } TileEnt;  /* fl: 1 prio, 2 hflip, 4 vflip */
typedef struct TileList { uint16_t first, n; } TileList;
enum { kEntPool = 4608 };
static TileEnt g_ents[kEntPool];
static int g_ent_top;
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
enum { kZLevel = 256, kZBackdrop = 64 };
/* Rows [r0,r1) whose main-screen pixels get the fixed colour added/subtracted (optionally halved). */
typedef struct TintRun { uint16_t r0, r1, fix; uint8_t half; } TintRun;
static TintRun g_tint[kMaxLines];
static int g_ntint;

static bool g_ge_ok;
static bool g_ge_active;
static int g_ge_level;  /* 0 software, 1 GE, 2 GE without tint (tint frames go back to software) */
static unsigned g_vcount_last;
static uint32_t g_frame;
static int g_swpath = SM_PSP_SW_PATH;  /* -1 calibrating, 0 CPU 565 convert, 1 GE copy to VRAM */
enum { kSwPaths = 2, kSwCalFrames = 30, kSwCalSkip = 3 };
static uint32_t g_cal_frames, g_cal_sum[kSwPaths], g_cal_n[kSwPaths];
static uint32_t g_frames_ge, g_frames_sw, g_reason_counts[kReasonCount];

//profiling

enum { kSkipAfter = 8 };  /* consecutive GE frames before the software PPU is skipped */
static uint32_t g_streak;
static uint32_t g_t_draw_start, g_t_list_start;
static uint32_t g_prep_us, g_build_us, g_gewait_us, g_vbwait_us;

#if SM_PSP_LOG
enum { kLateUs = 17000, kMissUs = 33800 };

typedef struct Stat { uint32_t n, sum, max; } Stat;
typedef struct Prof {
  Stat game, ppu, prep, build, gewait, vbwait, period;
  Stat cap, cpy, cls, bg, obj;          /* capture hook (and its VRAM copy); prep split: classify, BG, OBJ */
  Stat tseen, tdec, draws, clut, verts; /* per-frame counters */
  uint32_t late, miss, hold;
} Prof;
static Prof g_prof[3];  /* 0: GE frame, 1: software frame while GE is on, 2: software mode */
static const char *const kProfName[3] = {"ge", "swfb", "off"};
static uint32_t g_ppu_us, g_ppu_t0, g_cap_us, g_cpy_us;  /* software PPU draw / capture / VRAM-copy time this frame */
static uint32_t g_pc_class, g_pc_bg, g_pc_obj;            /* prep phases this frame */
static uint32_t g_c_tseen, g_c_tdec, g_c_draws, g_c_clut, g_c_verts;
static uint32_t g_t_prev_end, g_holds;

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

static uint16_t LineReasons(const Ppu *p, uint8_t mathmask, bool *submath, bool *tint, uint16_t *tintfix,
                           bool *tinthalf) {
  *submath = false;
  *tint = false;
  *tintfix = 0;
  *tinthalf = false;
  if (p->forcedBlank)
    return 0;
  uint16_t r = 0;
  int nl = p->mode == 7 ? 1 : kLayers;  /* mode 7 has BG1 only */
  for (int l = 0; l < nl; l++) {
    if (p->bgLayer[l].bigTiles)
      r |= kReasonBigTile;
    if (p->bgLayer[l].mosaicEnabled && p->mosaicSize > 1)
      r |= kReasonMosaic;
    if ((p->screenWindowed[0] & (1 << l)) && ((p->windowsel >> (l * 4)) & 0xa))
      r |= kReasonWindow;
  }
  if (p->clipMode != 0)
    r |= kReasonMathClip;
  if (mathmask != 0 && p->preventMathMode != 3) {
    if (p->preventMathMode != 0)
      r |= kReasonMathClip;  /* prevent inside/outside the colour window: depends on the window */
    if (p->mode == 7 && p->addSubscreen && p->screenEnabled[1] != 0)
      r |= kReasonM7;  /* subscreen layers in mode 7 are not drawn */
    uint16_t fix = (uint16_t)(p->fixedColorR | p->fixedColorG << 5 | p->fixedColorB << 10);
    if (p->addSubscreen && p->screenEnabled[1] != 0) {
      if (fix || p->halfColor) {
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
   }
  if ((*submath || *tint) && p->brightness < 15)
    r |= kReasonMathBright;
  return r;
}

static uint16_t M7LineReasons(const Ppu *p) {
  const int16_t *m = p->m7matrix;
  uint8_t fl = (uint8_t)((p->m7largeField ? 1 : 0) | (p->m7charFill ? 2 : 0) | (p->m7xFlip ? 4 : 0) |
                         (p->m7yFlip ? 8 : 0) | (p->m7extBg ? 16 : 0));
  uint16_t r = 0;
  if (m[1] || m[2] || m[0] <= 0 || m[3] <= 0 || fl != 1 || !g_m7_ok)
    r |= kReasonM7;
  M7Cfg c;
  memset(&c, 0, sizeof(c));
  memcpy(c.m, m, sizeof(c.m));
  c.fl = fl;
  c.bright = (uint8_t)(p->brightness & 15);
  c.bg1 = (p->screenEnabled[0] & 1) ? 1 : 0;
  if (!g_m7_set) {
    g_m7c = c;
    g_m7_set = true;
  } else if (memcmp(&c, &g_m7c, sizeof(c))) {
    r |= kReasonM7;  /* matrix, scroll, centre or brightness changes inside the frame (HDMA) */
  }
  return r;
}

static void CaptureImpl(const Ppu *p, int line) {
  if (!g_ge_active || line < 1 || line > kMaxLines)
    return;
  if (line == 1) {
    g_frame_dead = false;
    g_frame_mode = -1;
    g_m7_set = false;
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
  bool forced = p->forcedBlank;
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
  }
  s->brightness = p->brightness & 15;
  s->bg3prio = p->bg3priority ? 1 : 0;
  s->cg0 = p->cgram[0];
  s->forced = forced ? 1 : 0;
  uint8_t mm = 0;
  for (int i = 0; i < 6; i++)
    mm |= (uint8_t)((p->mathEnabled[i] ? 1 : 0) << i);
  s->mathmask = mm;
  bool submath, tint, tinthalf;
  uint16_t tintfix;
  s->reasons = LineReasons(p, mm, &submath, &tint, &tintfix, &tinthalf);
  s->submath = submath;
  s->tint = tint;
  s->tinthalf = tinthalf;
  s->tintfix = tintfix;
  if (!forced) {
    if (g_frame_mode < 0)
      g_frame_mode = p->mode;
    if (p->mode != g_frame_mode || (p->mode != 1 && p->mode != 7))
      s->reasons |= kReasonMode;
    if (p->mode == 7)
      s->reasons |= M7LineReasons(p);
  }
  /* only matters where the subscreen is added to the main screen with the fixed colour at 0 */
  s->subobj = (!forced && mm != 0 && p->preventMathMode != 3 && p->addSubscreen && (p->screenEnabled[1] & 16)) ? 1 : 0;
  s->subneg = p->subtractColor ? 1 : 0;
  {
    uint8_t sw = 0;
    for (int l = 0; l < kLayers; l++)
      if ((p->screenWindowed[1] & (1 << l)) && ((p->windowsel >> (l * 4)) & 0xa))
        sw |= (uint8_t)(1 << l);
    s->subwin = sw;
  }
  bool objon = !forced && (p->mode == 1 || p->mode == 7) && (p->screenEnabled[0] & 16);
  s->objon = objon;
  if (objon) {
    uint16_t r2 = 0;
    if (p->objInterlace || p->objSize != g_obj_size || p->objTileAdr1 != g_obj_adr1 ||
        p->objTileAdr2 != g_obj_adr2 || (bool)p->objPriority != g_obj_rot ||
        (g_obj_rot && (uint8_t)((p->oamAdr & 0xfe) >> 1) != g_obj_start))
      r2 |= kReasonObjCfg;
    if ((p->screenWindowed[0] & 16) && ((p->windowsel >> 16) & 0xa))
      r2 |= kReasonWindow;
    s->reasons |= r2;
  }
  g_frame_reasons |= s->reasons;
  if (line == 1 && !g_frame_reasons) {
    /* VRAM/CGRAM as the display saw them for this frame. One 64 KB copy per frame, never per line,
     * and only if the frame can still be drawn by the GE. */
    PROF(uint32_t t_cpy = Now();)
    memcpy(g_vram, p->vram, sizeof(g_vram));
    memcpy(g_cgram, p->cgram, sizeof(g_cgram));
    memcpy(g_oam, p->oam, sizeof(g_oam));
    memcpy(g_hoam, p->highOam, sizeof(g_hoam));
    PROF(g_cpy_us = Now() - t_cpy;)
  }
  if (g_frame_reasons)
    g_frame_dead = true;
#if SM_PSP_LOG
  if (s->reasons & ~g_logged_reasons) {  /* first time each reason is seen: remember the register state */
    g_logged_reasons |= s->reasons;
    g_note = (Note){line, s->reasons, p->mode, p->screenEnabled[0], p->screenEnabled[1], mm,
                    p->addSubscreen, p->subtractColor, p->halfColor,
                    p->fixedColorR | p->fixedColorG << 5 | p->fixedColorB << 10,
                    p->clipMode, p->preventMathMode, p->screenWindowed[0], p->windowsel};
    g_note_pending = true;
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
#endif

void PspRenderer_CaptureLine(const Ppu *p, int line) {
#if SM_PSP_LOG
  uint32_t t = Now();
  M7Probe(p, line);
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
static bool EnsureTile(bool bpp4, int pt) {
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
  PROF(g_c_tdec++;)
  uint32_t *dst;
  int stride_words;
  if (bpp4) {
    dst = (uint32_t *)(g_atlas4 + (pt >> 5) * 8 * 128 + (pt & 31) * 4);
    stride_words = 128 / 4;
  } else {
    dst = (uint32_t *)(g_atlas2 + (pt >> 6) * 8 * 256 + (pt & 63) * 4);
    stride_words = 256 / 4;
  }
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
  uint32_t *dst = (uint32_t *)(g_atlas7 + (t >> 4) * 8 * 128 + (t & 15) * 8);
  uint32_t any = 0;
  for (int y = 0; y < 8; y++, dst += 128 / 4) {
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

//BG layers

static bool SameBg(const LineSnap *a, const LineSnap *b, int l) {
  const BgSnap *x = &a->bg[l], *y = &b->bg[l];
  return x->hscroll == y->hscroll && x->vscroll == y->vscroll && x->mapadr == y->mapadr &&
         x->tileadr == y->tileadr && (x->flags & 7) == (y->flags & 7) && a->brightness == b->brightness;
}

static bool SameSub(const LineSnap *a, const LineSnap *b, int l) {
  return SameBg(a, b, l) && a->bg[l].flags == b->bg[l].flags && a->mathmask == b->mathmask &&
         a->bg3prio == b->bg3prio;
}

/* Walks the tilemap of layer l once for rows [r0,r1) and appends every non-empty
 * tile to the pool (decoding it into the atlas on the way). Consecutive equal
 * tilemap words reuse the previous lookup. Returns false if the pool is full. */
static bool BuildTileList(int l, const LineSnap *snap, int r0, int r1, TileList *out) {
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

static void CountList(const TileList *lst, int counts[2][8]) {
  for (int i = 0; i < lst->n; i++) {
    const TileEnt *e = &g_ents[lst->first + i];
    counts[e->fl & 1][e->pal]++;
  }
}

/* Emits CPU-clipped quads for rows [r0,r1) into out[prio][pal] (no negative or
 * off-screen vertex ever reaches the GE). */
static void EmitList(int l, const TileList *lst, int r0, int r1, TileVert *out[2][8]) {
  int cols = l < 2 ? 32 : 64;
  for (int i = 0; i < lst->n; i++) {
    const TileEnt *e = &g_ents[lst->first + i];
    int x0 = e->x, y0 = e->y, prio = e->fl & 1, pal = e->pal, pt = e->pt;
    int cl = x0 < 0 ? -x0 : 0, cr = x0 + 8 > kGameWidth ? x0 + 8 - kGameWidth : 0;
    int ct = y0 < r0 ? r0 - y0 : 0, cb = y0 + 8 > r1 ? y0 + 8 - r1 : 0;
    if (cl + cr >= 8 || ct + cb >= 8)
      continue;
    float u = (float)((pt % cols) * 8), v = (float)((pt / cols) * 8);
    float ul, ur, vt, vb;
    if (e->fl & 2) { ul = u + 8 - cl; ur = u + cr; } else { ul = u + cl; ur = u + 8 - cr; }
    if (e->fl & 4) { vt = v + 8 - ct; vb = v + cb; } else { vt = v + ct; vb = v + 8 - cb; }
    TileVert *w = out[prio][pal];
    w[0] = (TileVert){ul, vt, (int16_t)(kLeft + x0 + cl), (int16_t)(kTop + y0 + ct), (int16_t)g_zbg[l][prio]};
    w[1] = (TileVert){ur, vb, (int16_t)(kLeft + x0 + 8 - cr), (int16_t)(kTop + y0 + 8 - cb), (int16_t)g_zbg[l][prio]};
    out[prio][pal] += 2;
  }
}

//GE Frame

static void CountReasons(int mask);

//sprites

static const uint8_t kSpriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static void EmitQuad8(TileVert **w, int x0, int y0, int r0, int r1, int pt, bool hf, bool vf, int z) {
  int cl = x0 < 0 ? -x0 : 0, cr = x0 + 8 > kGameWidth ? x0 + 8 - kGameWidth : 0;
  int ct = y0 < r0 ? r0 - y0 : 0, cb = y0 + 8 > r1 ? y0 + 8 - r1 : 0;
  if (cl + cr >= 8 || ct + cb >= 8)
    return;
  float u = (float)((pt % 32) * 8), v = (float)((pt / 32) * 8);
  float ul, ur, vt, vb;
  if (hf) { ul = u + 8 - cl; ur = u + cr; } else { ul = u + cl; ur = u + 8 - cr; }
  if (vf) { vt = v + 8 - ct; vb = v + cb; } else { vt = v + ct; vb = v + 8 - cb; }
  TileVert *o = *w;
  o[0] = (TileVert){ul, vt, (int16_t)(kLeft + x0 + cl), (int16_t)(kTop + y0 + ct), (int16_t)z};
  o[1] = (TileVert){ur, vb, (int16_t)(kLeft + x0 + 8 - cr), (int16_t)(kTop + y0 + 8 - cb), (int16_t)z};
  *w += 2;
}

/* w == NULL: decode every tile the sprite uses (atlas fill, before the list is
 * open). Otherwise emit CPU-clipped quads for rows [o0,o1). Tile selection
 * mirrors ppu.c: 16x16-page wrap for the low nibble, row offset added to the
 * high nibble, hflip mirrors columns, vflip mirrors rows. */
static void WalkSprite(const Sprite *sp, int o0, int o1, TileVert **w, int z) {
  int n = sp->size >> 3;
  bool hf = (sp->attr & 0x4000) != 0, vf = (sp->attr & 0x8000) != 0;
  int adr = (sp->attr & 0x100) ? g_obj_adr2 : g_obj_adr1;
  int ycopies = (sp->y + sp->size > 256) ? 2 : 1;  /* rows wrap modulo 256 (uint8 row in ppu.c) */
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
      for (int c = 0; c < ycopies; c++)
        EmitQuad8(w, x0, (c ? sp->y - 256 : sp->y) + tr * 8, o0, o1, pt, hf, vf, z);
    }
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
  for (int r = o0; r < o1; r++)
    if (cs[r] > 32 || ct[r] > 34) {
      CountReasons(kReasonObjLimit);
      return false;
    }
  if (g_spr_quads > kMaxSpriteQuads) {
    CountReasons(kReasonBudget);
    return false;
  }
  for (int i = 0; i < g_nspr; i++)
    WalkSprite(&g_spr[i], o0, o1, NULL, 0);
  return true;
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
  if (g_frame_mode != 7 || !g_m7c.bg1)
    return true;
  const int16_t *m = g_m7c.m;
  int a = m[0], d = m[3];
  int xCenter = ((int16_t)(m[4] << 3)) >> 3, yCenter = ((int16_t)(m[5] << 3)) >> 3;
  int hScroll = ((int16_t)(m[6] << 3)) >> 3, vScroll = ((int16_t)(m[7] << 3)) >> 3;
  int clippedH = hScroll - xCenter, clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  uint32_t X0 = (uint32_t)((a * clippedH) & ~63) + ((uint32_t)xCenter << 8);
  uint32_t Yc = (uint32_t)((d * clippedV) & ~63) + ((uint32_t)yCenter << 8);
  const float du = (float)a / 256.0f, dv = (float)d / 256.0f, eps = 1.0f / 512.0f;

  struct { int xa, xb, tx; float u; } cs[160];
  int ncs = 0;
  for (int x = 0; x < kGameWidth; x++) {
    uint32_t X = X0 + (uint32_t)a * (uint32_t)x;
    if (X > 0x3ffff)
      continue;
    int tx = (int)(X >> 11);
    if (ncs && cs[ncs - 1].xb == x && cs[ncs - 1].tx == tx) {
      cs[ncs - 1].xb = x + 1;
    } else {
      if (ncs == 160)
        return false;
      cs[ncs].xa = x;
      cs[ncs].xb = x + 1;
      cs[ncs].tx = tx;
      cs[ncs].u = (float)(X - ((uint32_t)tx << 11)) / 256.0f + eps;
      ncs++;
    }
  }

  bool band = (d % 64) == 0;
  int r = 0;
  while (r < g_line_count) {
    uint32_t Y = (((uint32_t)d * (uint32_t)(r + 1)) & ~63u) + Yc;
    if (g_lines[r].forced || Y > 0x3ffff) {
      r++;
      continue;
    }
    uint32_t key = band ? Y >> 11 : Y >> 8;
    int r1 = r + 1;
    while (r1 < g_line_count && !g_lines[r1].forced) {
      uint32_t Y1 = (((uint32_t)d * (uint32_t)(r1 + 1)) & ~63u) + Yc;
      if (Y1 > 0x3ffff || (band ? Y1 >> 11 : Y1 >> 8) != key)
        break;
      r1++;
    }
    int ty = (int)(Y >> 11);
    float v0, v1;
    if (band) {
      float tv = (float)(Y - ((uint32_t)ty << 11)) / 256.0f + eps;
      v0 = tv - g_uvh[1] * dv;
      v1 = v0 + (float)(r1 - r) * dv;
    } else {
      v0 = v1 = (float)((Y >> 8) & 7) + 0.5f;
    }
    for (int k = 0; k < ncs; k++) {
      int t = g_vram[ty * 128 + cs[k].tx] & 0xff;
      if (EnsureTile7(t))
        continue;
      if (g_m7_nq >= kM7MaxQuads)
        return false;
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
  return true;
}

// Wait for the panel only if ahead
static void Pace(void) {
  uint32_t t = Now();
#if PSP_VBLANK_GATE
  if (sceDisplayGetVcount() == g_vcount_last)
    sceDisplayWaitVblankStart();
#else
  sceDisplayWaitVblankStart();
#endif
  g_vbwait_us = Now() - t;
}

/* Compares the GE image of a mode 7 frame with the software image (the software PPU drew this frame too).
 * Only the first kM7Verify mode 7 frames; reads the draw buffer through the uncached VRAM mirror. */
static void M7Verify(void) {
  const uint16_t *ge = (const uint16_t *)(g_vram_u + g_draw_off);
  uint32_t exact = 0, near = 0, bad = 0;
  for (int y = 0; y < g_line_count; y++)
    for (int x = 0; x < kGameWidth; x++) {
      uint16_t sw = Rgb565(g_framebuffer + (y * kGameWidth + x) * 4), g = ge[(kTop + y) * 512 + kLeft + x];
      if (sw == g) {
        exact++;
      } else {
        int dr = (sw & 31) - (g & 31), dg = ((sw >> 5) & 63) - ((g >> 5) & 63), db = (sw >> 11) - (g >> 11);
        if (dr >= -1 && dr <= 1 && dg >= -2 && dg <= 2 && db >= -1 && db <= 1)
          near++;
        else
          bad++;
      }
    }
  g_m7_vn++;
  g_m7_vbad += bad;
  g_m7_vnear += near;
  PspLog("m7verify f=%u exact=%u near=%u bad=%u (quads=%d)", (unsigned)g_frame, (unsigned)exact, (unsigned)near, (unsigned)bad,
         g_m7_nq);
  if (g_m7_vn >= 6 && g_m7_vbad / g_m7_vn > 600) {
    g_m7_ok = false;
    PspLog("m7verify: too many differences, GE mode 7 disabled");
  }
}

static void Present(void) {
  uint32_t t = Now();
  g_build_us = t - g_t_list_start;
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  g_gewait_us = Now() - t;
  if (g_verify_now) {
    g_verify_now = false;
    M7Verify();
  }
  Pace();
  sceGuSwapBuffers();
  g_draw_off ^= 0x44000;
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
static uint32_t g_subbg_diag;
#endif

/* Classifies layer l, shown on the subscreen over rows [r0,r1). A subscreen
 * pixel adds cgram[index] (ppu.c), so a tile only changes the picture if it uses
 * a pixel value whose colour is not black. BG3 HUD tiles (high priority, also on
 * the main screen with math off) are covered by main-screen BG3 pixels and never
 * matter. Returns -1 if the tile pool is full, 1 if the layer contributes (its
 * tile list is kept in *keep for the blend pass), 0 if not (list discarded). */
static int SubRunContributes(int l, const LineSnap *ls, int r0, int r1, TileList *keep) {
#if SM_PSP_LOG
  const BgSnap *s = &ls->bg[l];
#endif
  bool bpp4 = l < 2;
  int mark = g_ent_top;
  TileList lst;
  if (!BuildTileList(l, ls, r0, r1, &lst))
    return -1;
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
  if (contrib && (g_subbg_diag < 3 || (g_frame % 3000) == 0)) {
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
  for (int i = 0; i < kReasonCount; i++)
    if (mask & (1 << i))
      g_reason_counts[i]++;
}

/* Depth levels (ppu.c z values, level * kZLevel): BG3 low 1, OBJ prio0 2, OBJ1 6, BG2 low 7, BG1 low 8,
 * OBJ2 10, BG2 high 11, BG1 high 12, OBJ3 14, BG3 high 15. ppu.c always uses 15 for BG3 high (it never
 * looks at the BG3-priority bit). Sprites: (prio*4+2)*kZLevel + 127-k, k = precedence index. */

enum { kVtTile = GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
       kVtColor = GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
       kVtTexF = GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D };

static inline void GeDraw(int vtype, int n, const void *v) {
  PROF(g_c_draws++; g_c_verts += (uint32_t)n;)
  sceGuDrawArray(GU_SPRITES, vtype, n, NULL, v);
}

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
    sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
    sceGuTexImage(0, 256, 512, 256, g_atlas4);
  } else {
    sceGuClutLoad(1, g_clut2[b][p]);
    sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
    sceGuTexImage(0, 512, 512, 512, g_atlas2);
  }
}

static void DrawBgPass(int l, int pr) {
  for (int i = 0; i < g_nruns[l]; i++) {
    int b = g_lines[g_runs[l][i].first].brightness;
    for (int p = 0; p < 8; p++) {
      const Geom *g = &g_geom[l][i][pr][p];
      if (g->n == 0)
        continue;
      BindBg(l, b, p);
      GeDraw(kVtTile, g->n, g->v);
    }
  }
}

/* only_excluded: draw just the sprites that never take part in colour math
 * (OBJ palettes 0-3 always; 4-7 when the OBJ math bit is off). */
static void DrawObjSegs(int bright, bool only_excluded, int mathmask) {
  for (int i = 0; i < g_nobjseg; i++) {
    const ObjSeg *sg = &g_objseg[i];
    if (only_excluded && !(sg->pal < 4 || !(mathmask & 0x10)))
      continue;
    PROF(g_c_clut++;)
    sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
    sceGuClutLoad(2, g_clutobj[bright][sg->pal]);
    sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
    sceGuTexImage(0, 256, 512, 256, g_atlas4);
    GeDraw(kVtTile, sg->n, g_objbase + sg->first);
  }
}

static void DrawM7(void) {
  if (g_m7_nq == 0)
    return;
  PROF(g_c_clut++;)
  sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
  sceGuClutLoad(32, g_clut7);
  sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
  sceGuTexImage(0, 128, 128, 128, g_atlas7);
  GeDraw(kVtTile, g_m7_nq * 2, g_m7v);
}

// Returns false (nothing drawn, GE idle) if the frame must use the software image.
static bool DrawGe(void) {
  g_t_draw_start = Now();
  g_ent_top = 0;
  g_m7_nq = 0;
  g_verify_now = false;
  if (g_frame_reasons) {
    CountReasons(g_frame_reasons);
    return false;
  }

  if (++g_stamp == 0) {
    memset(g_seen2, 0, sizeof(g_seen2));
    memset(g_seen4, 0, sizeof(g_seen4));
    memset(g_seen7, 0, sizeof(g_seen7));
    g_clut7_stamp = 0;
    memset(g_clut2_stamp, 0, sizeof(g_clut2_stamp));
    memset(g_clut4_stamp, 0, sizeof(g_clut4_stamp));
    memset(g_clutobj_stamp, 0, sizeof(g_clutobj_stamp));
    g_stamp = 1;
  }

  /* Subscreen classification (fixed == 0, no half colour; see LineReasons).
   * Subscreen pixels only change the picture where a subscreen BG layer shows a
   * non-black pixel. No such layer: the add is a no-op. Exactly one such layer:
   * real blending below. More than one would need priority compositing of the
   * subscreen first, so those frames go back to software. */
  g_frame_subnoop = false;
  g_nbruns = 0;
  int sub_layer = -1;
  for (int l = 0; l < kLayers; l++) {
    int n = 0;
    for (int r = 0; r < g_line_count; r++) {
      if (!g_lines[r].submath || !(g_lines[r].bg[l].flags & 8))
        continue;
      g_frame_subnoop = true;
      if (n && g_subruns[n - 1].r1 == r && SameSub(&g_lines[g_subruns[n - 1].first], &g_lines[r], l))
        g_subruns[n - 1].r1 = (uint16_t)(r + 1);
      else
        g_subruns[n++] = (Run){(uint16_t)r, (uint16_t)(r + 1), (uint16_t)r};
    }
    for (int i = 0; i < n; i++) {
      TileList kept;
      int rc = SubRunContributes(l, &g_lines[g_subruns[i].first], g_subruns[i].r0, g_subruns[i].r1, &kept);
      if (rc < 0) {
        CountReasons(kReasonBudget);
        return false;
      }
      if (rc == 0)
        continue;
      if (sub_layer >= 0 && sub_layer != l) {
        CountReasons(kReasonSubMulti);
        return false;
      }
      sub_layer = l;
      g_bruns[g_nbruns++] = (BRun){(uint8_t)l, g_subruns[i].first, g_subruns[i].r0, g_subruns[i].r1, kept};
    }
  }
  bool blend = false;
  int bmath = 0, bneg = 0;
  if (g_nbruns) {
    bool first = true;
    for (int r = 0; r < g_line_count; r++) {
      if (!g_lines[r].submath)
        continue;
      if (first) {
        bmath = g_lines[r].mathmask;
        bneg = g_lines[r].subneg;
        first = false;
      } else if (g_lines[r].mathmask != bmath || g_lines[r].subneg != bneg) {
        CountReasons(kReasonSubMulti);
        return false;
      }
      if (g_lines[r].subwin & (1 << sub_layer)) {
        CountReasons(kReasonSubWin);
        return false;
      }
    }
    blend = true;
    g_frame_subnoop = false;
  }

  /* Tint rows: colour math with the fixed colour (no subscreen layer involved). All tint rows must
   * share the layer mask and add/subtract mode; the colour and halving may change per row. */
  bool tint = false;
  g_ntint = 0;
  for (int r = 0; r < g_line_count; r++) {
    const LineSnap *ls = &g_lines[r];
    if (!ls->tint)
      continue;
    if (!tint) {
      tint = true;
      bmath = ls->mathmask;
      bneg = ls->subneg;
    } else if (ls->mathmask != bmath || ls->subneg != bneg) {
      CountReasons(kReasonSubMulti);
      return false;
    }
    TintRun *tr = g_ntint ? &g_tint[g_ntint - 1] : NULL;
    if (tr && tr->r1 == r && tr->fix == ls->tintfix && tr->half == ls->tinthalf)
      tr->r1 = (uint16_t)(r + 1);
    else
      g_tint[g_ntint++] = (TintRun){(uint16_t)r, (uint16_t)(r + 1), ls->tintfix, ls->tinthalf};
  }
  if (tint && blend) {  /* subscreen-layer blend and fixed-colour tint in one frame: not drawn */
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
  for (int l = 0; l < kLayers; l++) {
    int n = 0;
    for (int r = 0; r < g_line_count; r++) {
      if (!(g_lines[r].bg[l].flags & 4))
        continue;
      if (n && g_runs[l][n - 1].r1 == r && SameBg(&g_lines[g_runs[l][n - 1].first], &g_lines[r], l))
        g_runs[l][n - 1].r1 = (uint16_t)(r + 1);
      else
        g_runs[l][n++] = (Run){(uint16_t)r, (uint16_t)(r + 1), (uint16_t)r};
    }
    g_nruns[l] = n;
    for (int i = 0; i < n; i++) {
      memset(g_counts[l][i], 0, sizeof(g_counts[l][i]));
      const Run *run = &g_runs[l][i];
      if (!BuildTileList(l, &g_lines[run->first], run->r0, run->r1, &g_mlist[l][i])) {
        CountReasons(kReasonBudget);
        return false;
      }
      CountList(&g_mlist[l][i], g_counts[l][i]);
      for (int pr = 0; pr < 2; pr++)
        for (int p = 0; p < 8; p++)
          total += g_counts[l][i][pr][p];
      if (l < 2)
        BuildClut4(g_lines[run->first].brightness);
      else
        BuildClut2(g_lines[run->first].brightness);
    }
  }
  for (int i = 0; i < g_nbruns; i++) {
    const BRun *br = &g_bruns[i];
    memset(g_bcounts[i], 0, sizeof(g_bcounts[i]));
    CountList(&br->lst, g_bcounts[i]);
    for (int pr = 0; pr < 2; pr++)
      for (int p = 0; p < 8; p++)
        total += g_bcounts[i][pr][p];
    if (br->l < 2)
      BuildClut4(g_lines[br->first].brightness);
    else
      BuildClut2(g_lines[br->first].brightness);
  }
  if (g_frame_mode == 7) {
    if (!M7Prepare()) {
      CountReasons(kReasonBudget);
      return false;
    }
    BuildClut7(g_m7c.bright);
  }
  if (total > kMaxTilesPerFrame) {  /* never draw a frame with holes */
    CountReasons(kReasonBudget);
    return false;
  }
  PROF(uint32_t t_bg = Now(); g_pc_bg = t_bg - t_cls;)

  /* Sprites: one contiguous range of rows [o0,o1) with OBJ enabled on the main
   * screen and one brightness (HUD rows have TM=4, so no sprites there). */
  int o0 = 0, o1 = 0, obj_bright = 0;
  bool obj_ok = true;
  for (int r = 0; r < g_line_count; r++) {
    if (!g_lines[r].objon)
      continue;
    if (o1 == 0) {
      o0 = r;
      obj_bright = g_lines[r].brightness;
    } else if (r != o1 || g_lines[r].brightness != obj_bright) {
      obj_ok = false;
    }
    o1 = r + 1;
  }
  g_nspr = 0;
  g_spr_quads = 0;
  g_nobjseg = 0;
  g_objbase = NULL;
  if (!obj_ok) {
    CountReasons(kReasonObjCfg);
    return false;
  }
  
  if (o1 > o0) {
    if (!PrepareObj(o0, o1))
      return false;
    BuildClutObj(obj_bright);
  }
  
  /* Sprites on the subscreen (added by colour math) are not drawn: fall back if any sprite touches a
   * row where that matters. Scans the OAM itself, so it also works when OBJ is off on the main screen. */
  if (SubObjHit()) {
    CountReasons(kReasonSubObj);
    return false;
  }

  sceKernelDcacheWritebackRange(g_clut2, sizeof(g_clut2));
  sceKernelDcacheWritebackRange(g_clut4, sizeof(g_clut4));
  sceKernelDcacheWritebackRange(g_clutobj, sizeof(g_clutobj));
  sceKernelDcacheWritebackRange(g_clut7, sizeof(g_clut7));

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
  sceGuStart(GU_DIRECT, g_gu_list);
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
    while (e < g_line_count && g_lines[e].cg0 == g_lines[r].cg0 && g_lines[e].brightness == g_lines[r].brightness &&
           g_lines[e].forced == g_lines[r].forced) e++;
    bd_y0[nrect] = (int16_t)r;
    bd_y1[nrect] = (int16_t)e;
    bd_col[nrect++] = g_lines[r].forced ? 0xff000000u : BakeColor(g_lines[r].cg0, g_lines[r].brightness, false);
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

  /* Geometry for every layer/run. */
  for (int l = 0; l < kLayers; l++) {
    for (int i = 0; i < g_nruns[l]; i++) {
      TileVert *w[2][8];
      for (int pr = 0; pr < 2; pr++) {
        for (int p = 0; p < 8; p++) {
          int c = g_counts[l][i][pr][p];
          g_geom[l][i][pr][p].v = w[pr][p] = c ? sceGuGetMemory(c * 2 * sizeof(TileVert)) : NULL;
        }
      }
      const Run *run = &g_runs[l][i];
      EmitList(l, &g_mlist[l][i], run->r0, run->r1, w);
      for (int pr = 0; pr < 2; pr++)
        for (int p = 0; p < 8; p++)
          g_geom[l][i][pr][p].n = g_counts[l][i][pr][p] ? (int)(w[pr][p] - g_geom[l][i][pr][p].v) : 0;
    }
  }
  /* Subscreen overlay geometry. */
  for (int i = 0; i < g_nbruns; i++) {
    const BRun *br = &g_bruns[i];
    TileVert *w[2][8];
    for (int pr = 0; pr < 2; pr++) {
      for (int p = 0; p < 8; p++) {
        int c = g_bcounts[i][pr][p];
        g_bgeom[i][pr][p].v = w[pr][p] = c ? sceGuGetMemory(c * 2 * sizeof(TileVert)) : NULL;
      }
    }
    EmitList(br->l, &br->lst, br->r0, br->r1, w);
    for (int pr = 0; pr < 2; pr++)
      for (int p = 0; p < 8; p++)
        g_bgeom[i][pr][p].n = g_bcounts[i][pr][p] ? (int)(w[pr][p] - g_bgeom[i][pr][p].v) : 0;
  }

  /* Sprite geometry: reverse precedence order, so the sprite that wins an overlap on hardware (lowest
   * OAM index) is drawn last. Every sprite carries its own depth marker; consecutive sprites that
   * share a palette share one draw. */
  if (g_spr_quads > 0) {
    TileVert *base = sceGuGetMemory(g_spr_quads * 2 * sizeof(TileVert));
    TileVert *w = base;
    g_objbase = base;
    for (int k = g_nspr - 1; k >= 0; k--) {
      const Sprite *sp = &g_spr[k];
      int pal = (sp->attr >> 9) & 7, prio = (sp->attr >> 12) & 3;
      TileVert *start = w;
      WalkSprite(sp, o0, o1, &w, (prio * 4 + 2) * kZLevel + 127 - k);
      if (w == start)
        continue;
      if (g_nobjseg == 0 || g_objseg[g_nobjseg - 1].pal != pal)
        g_objseg[g_nobjseg++] = (ObjSeg){(uint8_t)pal, (uint16_t)(start - base), 0};
      g_objseg[g_nobjseg - 1].n = (uint16_t)(g_objseg[g_nobjseg - 1].n + (w - start));
    }
  }

  /* Mode 7 geometry (z = SNES level 5: above OBJ priority 0, below priorities 1-3). */
  g_m7v = NULL;
  if (g_m7_nq > 0) {
    g_m7v = sceGuGetMemory(g_m7_nq * 2 * sizeof(TileVert));
    for (int i = 0; i < g_m7_nq; i++) {
      const M7Quad *q = &g_m7q[i];
      g_m7v[2 * i] = (TileVert){q->u0, q->v0, (int16_t)(kLeft + q->x0), (int16_t)(kTop + q->y0), (int16_t)(5 * kZLevel)};
      g_m7v[2 * i + 1] = (TileVert){q->u1, q->v1, (int16_t)(kLeft + q->x1), (int16_t)(kTop + q->y1), (int16_t)(5 * kZLevel)};
    }
  }

  /* Tint overlay: one colour rectangle per run of rows with the same fixed colour. */
  ColorVert *tv = NULL;
  if (tint) {
    tv = sceGuGetMemory(g_ntint * 2 * sizeof(ColorVert));
    for (int i = 0; i < g_ntint; i++) {
      uint32_t col = BakeColor(g_tint[i].fix, 15, false);
      tv[2 * i] = (ColorVert){col, (int16_t)kLeft, (int16_t)(kTop + g_tint[i].r0), 0};
      tv[2 * i + 1] = (ColorVert){col, (int16_t)(kLeft + kGameWidth), (int16_t)(kTop + g_tint[i].r1), 0};
    }
  }

  sceGuEnable(GU_TEXTURE_2D);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuAlphaFunc(GU_NOTEQUAL, 0, 0xff);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexScale(1.0f, 1.0f);
  sceGuTexOffset(0.0f, 0.0f);

  /* Sprites: depth ALWAYS, the markers they leave decide the layering against the BGs below. */
  sceGuDepthFunc(GU_ALWAYS);
  DrawObjSegs(obj_bright, false, bmath);
  sceGuDepthFunc(GU_GEQUAL);
  DrawM7();
  for (int l = 0; l < kLayers; l++)
    for (int pr = 0; pr < 2; pr++)
      DrawBgPass(l, pr);

  if (blend || tint) {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    if (blend) {
      /* Blend the subscreen layer over everything it covers (colour math adds or
       * subtracts the subscreen pixel; fixed colour is 0 so uncovered pixels are
       * unchanged). */
      sceGuBlendFunc(bneg ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, 0x00ffffff, 0x00ffffff);
      for (int i = 0; i < g_nbruns; i++) {
        int b = g_lines[g_bruns[i].first].brightness;
        for (int pr = 0; pr < 2; pr++) {
          for (int p = 0; p < 8; p++) {
            const Geom *g = &g_bgeom[i][pr][p];
            if (g->n == 0)
              continue;
            BindBg(g_bruns[i].l, b, p);
            GeDraw(kVtTile, g->n, g->v);
          }
        }
      }
    } else {
      /* Tint: dst = dst +/- fixed, or (dst +/- fixed) / 2 with half colour (factors 0.5 / 0.5). */
      sceGuDisable(GU_TEXTURE_2D);
      sceGuDisable(GU_ALPHA_TEST);
      for (int i = 0; i < g_ntint;) {
        int j = i;
        while (j < g_ntint && g_tint[j].half == g_tint[i].half)
          j++;
        unsigned f = g_tint[i].half ? 0x00808080u : 0x00ffffffu;
        sceGuBlendFunc(bneg ? GU_REVERSE_SUBTRACT : GU_ADD, GU_FIX, GU_FIX, f, f);
        GeDraw(kVtColor, (j - i) * 2, tv + 2 * i);
        i = j;
      }
      sceGuEnable(GU_TEXTURE_2D);
      sceGuEnable(GU_ALPHA_TEST);
    }
    sceGuDisable(GU_BLEND);

    /* Restore the layers whose colour-math bit is off: redraw them where their depth marker is
     * still in the buffer (the overlay neither tests nor writes depth). */

    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_EQUAL);
    sceGuDepthMask(GU_TRUE);  /* GU_TRUE masks depth writes */
    if (!(bmath & 0x20)) {
      sceGuDisable(GU_TEXTURE_2D);
      sceGuDisable(GU_ALPHA_TEST);
      GeDraw(kVtColor, nbd, bd);
      sceGuEnable(GU_TEXTURE_2D);
      sceGuEnable(GU_ALPHA_TEST);
    }
    if (!(bmath & 1))
      DrawM7();
    for (int l = 0; l < kLayers; l++)
      if (!(bmath & (1 << l)))
        for (int pr = 0; pr < 2; pr++)
          DrawBgPass(l, pr);
    DrawObjSegs(obj_bright, true, bmath);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_DEPTH_TEST);
    if (blend)
      g_frames_blend++;
    else
      g_frames_tint++;
  }
  g_verify_now = g_m7_nq > 0 && !g_ppu_skip_draw && g_m7_vn < kM7Verify;
  Present();
  return true;
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
  sceGuStart(GU_DIRECT, g_gu_list);
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
  if (!ok)
    g_m7_ok = false;
  PspLog("uvconv: x=%s y=%s (centre = 0.5, corner = 0) -> GE mode 7 %s", okc[0] ? "centre" : okk[0] ? "corner" : "?",
         okc[1] ? "centre" : okk[1] ? "corner" : "?", ok ? "enabled" : "DISABLED");
}

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

  sceGuInit();
  sceGuStart(GU_DIRECT, g_gu_list);
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

  if (g_ge_ok) {
    memset(g_hash2, 0xff, sizeof(g_hash2));
    memset(g_hash4, 0xff, sizeof(g_hash4));
    memset(g_hash7, 0xff, sizeof(g_hash7));
    memset(g_atlas2, 0, kAtlas2Bytes);
    memset(g_atlas4, 0, kAtlas4Bytes);
    memset(g_atlas7, 0, kAtlas7Bytes);
    UvSelfTest();
  }
  g_ppu_line_hook = PspRenderer_CaptureLine;
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
    PspLog("  cap=%u/%u cpy=%u/%u class=%u/%u bg=%u/%u obj=%u/%u", AVG(p->cap), AVG(p->cpy), AVG(p->cls),
           AVG(p->bg), AVG(p->obj));
    PspLog("  tiles seen=%u/%u decoded=%u/%u | draws=%u/%u clut=%u/%u verts=%u/%u", AVG(p->tseen),
           AVG(p->tdec), AVG(p->draws), AVG(p->clut), AVG(p->verts));
#undef AVG
  }
  PspAudioMe_LogStats();  // no-op unless PSP_LOG, PSP_AUDIO_ME and PSP_ME_LOG are all on
  memset(g_prof, 0, sizeof(g_prof));
}
#endif

static void PspRenderer_EndDraw(void) {
  PROF(uint32_t t_enter = Now();)
  g_frame++;
  g_prep_us = g_build_us = g_gewait_us = g_vbwait_us = 0;
  bool drew_ge = false, held = false;

  if (g_ge_active && g_line_count > 0) {
    if (DrawGe())
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
    g_frames_sw++;
    PROF(g_holds++;)
    g_streak = 0;
    g_ppu_skip_draw = false;
    Pace();
    g_vcount_last = sceDisplayGetVcount();
  } else {
    /* Software path (unchanged behaviour). Also used when the GE path hands a
     * frame back, and for the first frame after a toggle. */
    if (g_ge_active)
      g_frames_sw++;
    g_streak = 0;
    g_t_draw_start = Now();
    int swpath = g_swpath >= 0 ? g_swpath : (int)((g_cal_frames / kSwCalFrames) % kSwPaths);
    if (swpath == 0)
      PspRenderer_Convert();
    sceKernelDcacheWritebackRange(g_framebuffer, sizeof(g_framebuffer));
    g_t_list_start = Now();
    g_prep_us = g_t_list_start - g_t_draw_start;
    sceGuStart(GU_DIRECT, g_gu_list);
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
    int edges[8];
    int nstrip = StripEdges(kLeft, kLeft + kGameWidth, edges);
    PspVertex *v = (PspVertex *)sceGuGetMemory(nstrip * 2 * sizeof(PspVertex));
    for (int st = 0; st < nstrip; st++) {
      v[2 * st] = (PspVertex){(float)(edges[st] - kLeft), 0, (float)edges[st], kTop, 0};
      v[2 * st + 1] = (PspVertex){(float)(edges[st + 1] - kLeft), kGameHeight, (float)edges[st + 1],
                                  kTop + kGameHeight, 0};
    }
    GeDraw(kVtTexF, nstrip * 2, v);
    Present();
    if (g_swpath < 0) {  /* calibration: CPU + GE cost of getting the frame on screen, per path */
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

#if SM_PSP_LOG
  /* Per-frame timing. "game" is everything since the previous frame finished:
   * the game logic, input and the software PPU (its share is "ppu"). */
  M7FrameEnd();
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
    StatAdd(&p->tseen, g_c_tseen);
    StatAdd(&p->tdec, g_c_tdec);
    StatAdd(&p->draws, g_c_draws);
    StatAdd(&p->clut, g_c_clut);
    StatAdd(&p->verts, g_c_verts);
    if (period > kLateUs)
      p->late++;
    if (period > kMissUs)
      p->miss++;
    if (held)
      p->hold++;
  }
  g_ppu_us = g_cap_us = g_cpy_us = 0;
  g_pc_class = g_pc_bg = g_pc_obj = 0;
  g_c_tseen = g_c_tdec = g_c_draws = g_c_clut = g_c_verts = 0;
  g_t_prev_end = t_end;
  if (g_note_pending) {
    g_note_pending = false;
    PspLog("NEW reason bits=0x%x line=%d mode=%u TM=%02x TS=%02x math=%02x add=%u sub=%u half=%u fixed=%04x clip=%u prev=%u tmw=%02x wsel=%x",
           g_note.bits, g_note.line, g_note.mode, g_note.tm, g_note.ts, g_note.mathmask, g_note.add,
           g_note.sub, g_note.half, g_note.fixed, g_note.clip, g_note.prevent, g_note.tmw, g_note.wsel);
  }
  if (g_ge_active && (g_frame % 300) == 0) {
    PspLog("f=%u ge=%u (subnoop=%u blend=%u tint=%u) sw=%u gaps=%u", (unsigned)g_frame, (unsigned)g_frames_ge,
           (unsigned)g_frames_subnoop, (unsigned)g_frames_blend, (unsigned)g_frames_tint, (unsigned)g_frames_sw,
           (unsigned)g_capture_gaps);
    PspLog("  why: mode=%u big=%u mos=%u win=%u clip=%u tint=%u b3p=%u budget=%u subobj=%u subbg=%u",
           (unsigned)g_reason_counts[0], (unsigned)g_reason_counts[1], (unsigned)g_reason_counts[2],
           (unsigned)g_reason_counts[3], (unsigned)g_reason_counts[4], (unsigned)g_reason_counts[5],
           (unsigned)g_reason_counts[6], (unsigned)g_reason_counts[7], (unsigned)g_reason_counts[8],
           (unsigned)g_reason_counts[9]);
    PspLog("  why2: objcfg=%u objlimit=%u subwin=%u submulti=%u mathbright=%u m7=%u", (unsigned)g_reason_counts[10],
           (unsigned)g_reason_counts[11], (unsigned)g_reason_counts[12], (unsigned)g_reason_counts[13],
           (unsigned)g_reason_counts[14], (unsigned)g_reason_counts[15]);
  }
  if ((g_frame % 300) == 0)
    ProfLog();
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
