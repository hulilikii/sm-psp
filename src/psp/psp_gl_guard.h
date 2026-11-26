/* psp_gl_guard.h - every libgu call that writes into a display list is checked against the list's capacity BEFORE it writes.
 *
 * Why: PSPSDK libgu has no capacity check (guInternal.h sendCommandi() is `*(gu_list->current++) = ...`, sceGuGetMemory()
 * advances `current` by round4(size) + 8, sceGuStart() takes no length). The only hard bound is to refuse a call before it
 * is made. This header shadows every emitting sceGu function psp_renderer.c uses with a wrapper that
 *   1. adds the worst-case size of the call (table below) to g_gl.est, an upper bound of the bytes written so far,
 *   2. proceeds only if that stays <= cap - kGlFinish (the fast path: one add and one compare, no libgu call),
 *   3. otherwise re-reads the exact position, sceGuCheckList() (= current - start), and decides on that.
 * Invariant: after every successful wrapper, bytes written <= cap - kGlFinish, so sceGuFinish() (FINISH + END = 2 words,
 * PSPSDK sceGuFinishId.c, GU_DIRECT) always fits. The costs are checked against the PSPSDK sources by tools/test_gl_bounds.c.
 *
 * A refused call is a "trip": nothing is written and g_gl.lim drops to 0 (every later call is refused too). If the caller
 * armed a recovery point (GL_ARM) the trip longjmp()s to it and the caller abandons the frame (GlAbortList(): finish +
 * wait for the GE, then its fallback). Unarmed, a refused command is skipped and a refused allocation stops the program
 * (Die), because it has no valid pointer to return - still before any write. Boot-time lists are GlStartFatal: any refusal
 * is a Die.
 *
 * A FAILED FINISH (sceGuFinish < 0: sceGeListUpdateStallAddr failed after FINISH + END were written) is not recoverable
 * here and is never treated as a completed list. GlFinish polls (bounded, no blocking wait, no sceGeBreak / ListDeQueue)
 * whether the GE reports the list done. If it does, the failure is recorded (g_gl_finish_fail, async stays off for the
 * rest of the run) and rendering continues synchronously. If it does not, nothing the list references may be reused: the
 * program stops (Die saves SRAM and the log first).
 *
 * Every other emitting sceGu function is made a compile error below: a new unchecked call cannot slip in. */
#ifndef PSP_GL_GUARD_H
#define PSP_GL_GUARD_H

#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

/* Worst-case bytes written by one libgu call (4 bytes per command word), from the PSPSDK sources (src/gu/sceGu*.c):
 *   Enable / Disable   4 words (GU_SCISSOR_TEST: SCISSOR1, SCISSOR2, REGION1, REGION2; every other state 1)
 *   Scissor            4 (the same four, when scissor is enabled)    TexImage   4 (TEX_ADDR, TEX_BUF_WIDTH, TEX_SIZE, TEX_FLUSH)
 *   TexMode            3 (TEX_MODE, TEX_FORMAT, TEX_FLUSH)           TexFunc/TexFlush/TexSync/TexWrap/TexFilter 1
 *   TexScale/TexOffset 2                                             ClutLoad 3 (PTR, WIDTH, LOAD)       ClutMode 1
 *   DepthFunc/DepthMask/AlphaFunc/StencilFunc/StencilOp/ShadeModel 1 BlendFunc 3                           DepthRange 4
 *   Color              4 (sceGuMaterial with all three: AMBIENT_COLOR, AMBIENT_ALPHA, DIFFUSE, SPECULAR)
 *   Viewport 4, Offset 2, SetDither 4, DepthBuffer 2, DrawBufferList 3, DrawBuffer 5
 *   CopyImage          8 (TRANSFER_SRC, _SRC_W, _SRC_OFFSET, _DST, _DST_W, _DST_OFFSET, _SIZE, _START)
 *   DrawArray          6 (VERTEX_TYPE, BASE, IADDR, BASE, VADDR, PRIM)
 *   SetMatrix          17 (GU_PROJECTION: MATRIX_NUMBER + 16 data; VIEW / MODEL / TEXTURE 13)
 *   ColorMaterial 1, LightMode 1, Ambient 2 (colour, alpha), Material 4 (ambient colour + alpha, diffuse, specular)
 *   Light 4 (X, Y, Z, TYPE), LightColor 2 (ambient and diffuse, or diffuse and specular), LightAtt 3
 *   Clear (not FAST)   14 = CLEAR_MODE + GetMemory(2 * 12 B vertices = 24, +8 header = 8 words) + DrawArray(VERTEX_TYPE, BASE,
 *                      VADDR, PRIM) + CLEAR_MODE;  FAST: 30 vertices at most (480 px, 32 px blocks) = 360 -> 368 B + 28 B
 *   GetMemory(n)       round4(n) + 8 (sceGuGetMemory.c; negative n and n > cap are refused first)
 *   Start (GU_DIRECT)  11 words at most: FRAME_BUF_PTR, FRAME_BUF_WIDTH, and on the first start SetDither 4 + PatchDivide 1 +
 *                      ColorMaterial 1 + Specular 1 + TexScale 2. (GlStart reads the exact size after it: not a cost here.)
 *   Finish             2 words. */
enum {
  kGlFinish = 2 * 4,
  kGlStartMax = 11 * 4,
  kGlStateCmd = 4 * 4,
  kGlTexImage = 4 * 4,
  kGlTexMode = 3 * 4,
  kGlCopyImage = 8 * 4,
  kGlDrawArray = 6 * 4,
  kGlSetMatrix = 17 * 4,
  kGlMaterial = 4 * 4,
  kGlLight = 4 * 4,
  kGlLightAtt = 3 * 4,
  kGlClear = 14 * 4,
  kGlClearFast = 400,
  kGlGetMemHeader = 8,
  kGlMinList = 1024,         /* smallest list GlStart accepts: Start + Finish with room to spare */
  kGlConfirmUs = 100 * 1000  /* how long GlFinish polls for the GE after a failed sceGuFinish (see the header comment) */
};

/* The includer provides Die(const char *) (noreturn), PspLog(), and the libgu / GE prototypes (pspgu.h, pspge.h, pspkernel.h). */

#define GL_FN static __attribute__((noinline, unused))
#define GL_LIKELY(x) __builtin_expect(!!(x), 1)

/* The two words the fast path reads, adjacent: one address computation per wrapper. lim is 0 whenever nothing may be written
 * (no open list, or the list tripped): every cost is >= 4, so `est + cost <= lim` is then false and the slow path decides. */
static struct {
  uint32_t est;  /* upper bound of the bytes written to the open list (exact right after GlStart / a resync) */
  uint32_t lim;  /* cap - kGlFinish while the list is open and has not tripped, else 0 */
} g_gl;
static uint32_t g_gl_cap;       /* bytes the open list may hold */
static uint32_t g_gl_peak;      /* diagnostics only: the largest finished list */
static uint32_t g_gl_trips;     /* diagnostics only: refused calls */
static bool g_gl_open;          /* between a successful GlStart and GlFinish: the only time anything may be written */
static bool g_gl_full;          /* this list refused a call: it is incomplete, the frame is to be abandoned */
static volatile bool g_gl_armed;
static bool g_gl_fatal;         /* boot-time list (GlStartFatal): a refused call stops the program instead of being skipped */
static uint32_t g_gl_finish_err;   /* diagnostics only: sceGuFinish failures */
static bool g_gl_finish_fail;      /* sticky: a sceGuFinish failed (the GE was seen done afterwards); async must stay off */
static jmp_buf g_gl_jb;

/* Recovery point for the function that builds a list (setjmp only in the forms C allows: the whole controlling expression):
 *     if (setjmp(g_gl_jb)) { GlAbortList(); ...fallback, using only globals (locals are indeterminate)...; return; }
 *     GL_ARM();
 * ORDER MATTERS: GlStart() disarms (a stale recovery point must not survive into a new list), so GL_ARM() comes AFTER the
 * GlStart of the list it protects. Armed only until the list ends (GlFinish / next GlStart) or GL_DISARM(): the jmp_buf's
 * frame must still be live. One jmp_buf: a nested setjmp (a function called while armed that arms again) replaces the
 * outer one, which is only sound because the inner region always ends (GlFinish or GL_DISARM) before it returns.
 * tools/test_gl_bounds.sh rejects a GL_ARM() that is not in the function that called GlStart. */
#define GL_ARM() (g_gl_armed = true)
#define GL_DISARM() (g_gl_armed = false)

static void GlTrip(void) {
  g_gl_full = true;
  g_gl.lim = 0;
  g_gl_trips++;
  if (g_gl_armed) {
    g_gl_armed = false;
    longjmp(g_gl_jb, 1);
  }
  if (g_gl_fatal)
    Die("display list full (boot-time list)");  /* nothing has been written */
}

/* Slow path: the estimate says the call might not fit. Decide on the exact position, and keep the exact one as the new
 * estimate (est >= actual always holds, so the fast path can never admit a call that does not fit). */
GL_FN bool GlRoomSlow(uint32_t bytes) {
  if (g_gl_open && !g_gl_full) {
    const int u = sceGuCheckList();
    const uint32_t room = g_gl_cap - (uint32_t)kGlFinish;  /* g_gl_cap >= kGlMinList */
    if (u >= 0 && (uint32_t)u <= room && bytes <= room - (uint32_t)u) {
      g_gl.est = (uint32_t)u + bytes;
      return true;
    }
  }
  GlTrip();
  return false;
}

static inline bool GlRoom(uint32_t bytes) {
  const uint32_t e = g_gl.est + bytes;
  if (GL_LIKELY(e <= g_gl.lim)) {
    g_gl.est = e;
    return true;
  }
  return GlRoomSlow(bytes);
}

/* Starts a GU_DIRECT list in `list` (16 byte aligned, `cap` bytes). Fails closed: after any failure nothing can be written
 * (g_gl_open is false, lim 0). sceGuStart (src/gu/sceGuStart.c) sets gu_curr_context / gu_list to the new list BEFORE
 * sceGeListEnQueue() can fail and returns without emitting or consuming anything; every GlStart re-initialises both, and
 * no emitting call reaches libgu while g_gl_open is false, so a failed list is never written, finished or synced. */
GL_FN int GlStart(void *list, uint32_t cap) {
  g_gl_open = false;
  g_gl_full = false;
  g_gl_armed = false;
  g_gl_fatal = false;
  g_gl.est = 0;
  g_gl.lim = 0;
  if (!list || ((uintptr_t)list & 15u) || cap < (uint32_t)kGlMinList)
    return -1;
  const int r = sceGuStart(GU_DIRECT, list);
  if (r < 0)
    return r;
  g_gl_cap = cap;
  g_gl_open = true;
  g_gl.est = (uint32_t)sceGuCheckList();  /* exact: what sceGuStart wrote (<= kGlStartMax) */
  g_gl.lim = cap - (uint32_t)kGlFinish;
  return 0;
}

/* Boot-time list: as GlStart, but a call the list cannot take stops the program (before it writes) instead of being skipped. */
GL_FN int GlStartFatal(void *list, uint32_t cap) {
  const int r = GlStart(list, cap);
  g_gl_fatal = (r >= 0);
  return r;
}

/* sceGuFinish failed: FINISH + END were written but the GE was not told (sceGeListUpdateStallAddr < 0), and libgu returned
 * before restoring its context. Nothing here tries to repair the queue (sceGeBreak / sceGeListDeQueue need a queue id libgu
 * keeps private and their recovery semantics are not established). Poll, bounded, whether the GE reports done: only then
 * is the list's memory known to be free. Otherwise stop: the list memory, vertex pool, atlases and textures it references
 * must never be written again, and nothing else is safe to run. */
GL_FN void GlFinishFailed(int err) {
  (void)err;  /* only logged (PspLog is empty in a PSP_LOG=0 build) */
  g_gl_finish_err++;
  g_gl_finish_fail = true;
  const uint32_t t0 = sceKernelGetSystemTimeLow();
  do {
    if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) {  /* 1: peek, never blocks */
      PspLog("ge: sceGuFinish failed (0x%08x) but the GE reports done; async disabled for the run", (unsigned)err);
      return;
    }
  } while (sceKernelGetSystemTimeLow() - t0 < (uint32_t)kGlConfirmUs);
  PspLog("ge: sceGuFinish failed (0x%08x) and the GE did not report done in %u us", (unsigned)err, (unsigned)kGlConfirmUs);
  Die("GE list finish failed, completion unconfirmed");
}

/* Returns what sceGuFinish returned (the list size, or < 0 after GlFinishFailed confirmed the GE done). The list is closed
 * to writes whatever it returns. */
GL_FN int GlFinish(void) {
  g_gl_armed = false;
  g_gl_fatal = false;
  if (!g_gl_open)
    return 0;
  g_gl_open = false;
  g_gl.lim = 0;
  const int r = sceGuFinish();
  if (r >= 0) {
    if ((uint32_t)r > g_gl_peak)
      g_gl_peak = (uint32_t)r;
    return r;
  }
  GlFinishFailed(r);
  return r;
}

/* The list is abandoned (a trip, or the caller gives up): terminate it (2 words, reserved) and wait until the GE is done
 * with it. The frame's pixels are incomplete; the caller redraws or holds. A failed finish never reaches the wait: GlFinish
 * has either confirmed the GE done or stopped the program. */
GL_FN void GlAbortList(void) {
  GlFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

GL_FN void *GlGetMemory(int size) {
  if (size >= 0 && (uint32_t)size <= g_gl_cap) {
    const uint32_t n = ((uint32_t)size + 3u) & ~3u;
    if (GlRoom(n + (uint32_t)kGlGetMemHeader))
      return sceGuGetMemory(size);
  } else {
    GlTrip();
  }
  Die("display list full (no recovery point armed)");  /* nothing has been written */
  return NULL;
}

#define GL_CMD0(name, cost, rtype, args, call) \
  GL_FN rtype Gl_##name args { if (GlRoom(cost)) call; }
GL_CMD0(Enable, kGlStateCmd, void, (int s), sceGuEnable(s))
GL_CMD0(Disable, kGlStateCmd, void, (int s), sceGuDisable(s))
GL_CMD0(Scissor, kGlStateCmd, void, (int x, int y, int w, int h), sceGuScissor(x, y, w, h))
GL_CMD0(TexImage, kGlTexImage, void, (int m, int w, int h, int b, const void *p), sceGuTexImage(m, w, h, b, p))
GL_CMD0(TexMode, kGlTexMode, void, (int t, int m, int c, int s), sceGuTexMode(t, m, c, s))
GL_CMD0(TexFunc, 4, void, (int a, int b), sceGuTexFunc(a, b))
GL_CMD0(TexFlush, 4, void, (void), sceGuTexFlush())
GL_CMD0(TexSync, 4, void, (void), sceGuTexSync())
GL_CMD0(TexWrap, 4, void, (int u, int v), sceGuTexWrap(u, v))
GL_CMD0(TexFilter, 4, void, (int a, int b), sceGuTexFilter(a, b))
GL_CMD0(TexScale, 8, void, (float u, float v), sceGuTexScale(u, v))
GL_CMD0(TexOffset, 8, void, (float u, float v), sceGuTexOffset(u, v))
GL_CMD0(ClutLoad, 12, void, (int n, const void *p), sceGuClutLoad(n, p))
GL_CMD0(ClutMode, 4, void, (unsigned a, unsigned b, unsigned c, unsigned d), sceGuClutMode(a, b, c, d))
GL_CMD0(DepthFunc, 4, void, (int f), sceGuDepthFunc(f))
GL_CMD0(DepthMask, 4, void, (int m), sceGuDepthMask(m))
GL_CMD0(DepthRange, kGlStateCmd, void, (int n, int f), sceGuDepthRange(n, f))
GL_CMD0(DepthBuffer, 8, void, (void *p, int w), sceGuDepthBuffer(p, w))
GL_CMD0(AlphaFunc, 4, void, (int f, int v, int m), sceGuAlphaFunc(f, v, m))
GL_CMD0(StencilFunc, 4, void, (int f, int r, int m), sceGuStencilFunc(f, r, m))
GL_CMD0(StencilOp, 4, void, (int a, int b, int c), sceGuStencilOp(a, b, c))
GL_CMD0(BlendFunc, 12, void, (int o, int s, int d, unsigned sf, unsigned df), sceGuBlendFunc(o, s, d, sf, df))
GL_CMD0(ShadeModel, 4, void, (int m), sceGuShadeModel(m))
GL_CMD0(Color, kGlStateCmd, void, (unsigned c), sceGuColor(c))
GL_CMD0(Viewport, kGlStateCmd, void, (int cx, int cy, int w, int h), sceGuViewport(cx, cy, w, h))
GL_CMD0(Offset, 8, void, (unsigned x, unsigned y), sceGuOffset(x, y))
GL_CMD0(SetDither, kGlStateCmd, void, (const ScePspIMatrix4 *m), sceGuSetDither(m))
GL_CMD0(DrawBufferList, 12, void, (int p, void *f, int w), sceGuDrawBufferList(p, f, w))
GL_CMD0(DrawBuffer, 20, void, (int p, void *f, int w), sceGuDrawBuffer(p, f, w))
GL_CMD0(CopyImage, kGlCopyImage, void,
        (int p, int sx, int sy, int w, int h, int sw, void *s, int dx, int dy, int dw, void *d),
        sceGuCopyImage(p, sx, sy, w, h, sw, s, dx, dy, dw, d))
GL_CMD0(DrawArray, kGlDrawArray, void, (int pr, int vt, int n, const void *i, const void *v), sceGuDrawArray(pr, vt, n, i, v))

/* Fixed-function matrix and lighting (HazeDrawMesh). */
GL_CMD0(SetMatrix, kGlSetMatrix, void, (int t, const ScePspFMatrix4 *m), sceGuSetMatrix(t, m))
GL_CMD0(ColorMaterial, 4, void, (int c), sceGuColorMaterial(c))
GL_CMD0(Material, kGlMaterial, void, (int m, int c), sceGuMaterial(m, c))
GL_CMD0(Ambient, 8, void, (unsigned c), sceGuAmbient(c))
GL_CMD0(LightMode, 4, void, (int m), sceGuLightMode(m))
GL_CMD0(Light, kGlLight, void, (int l, int t, int c, const ScePspFVector3 *p), sceGuLight(l, t, c, p))
GL_CMD0(LightColor, 8, void, (int l, int c, unsigned v), sceGuLightColor(l, c, v))
GL_CMD0(LightAtt, kGlLightAtt, void, (int l, float a, float b, float c), sceGuLightAtt(l, a, b, c))

GL_FN void Gl_Clear(int flags) {
  if (GlRoom((flags & GU_FAST_CLEAR_BIT) ? (uint32_t)kGlClearFast : (uint32_t)kGlClear))
    sceGuClear(flags);
}

/* From here on psp_renderer.c can only reach libgu through the wrappers (function-like macros: `(sceGuX)(...)` would bypass
 * them and is not used). */
#define sceGuEnable(...) Gl_Enable(__VA_ARGS__)
#define sceGuDisable(...) Gl_Disable(__VA_ARGS__)
#define sceGuScissor(...) Gl_Scissor(__VA_ARGS__)
#define sceGuTexImage(...) Gl_TexImage(__VA_ARGS__)
#define sceGuTexMode(...) Gl_TexMode(__VA_ARGS__)
#define sceGuTexFunc(...) Gl_TexFunc(__VA_ARGS__)
#define sceGuTexFlush(...) Gl_TexFlush(__VA_ARGS__)
#define sceGuTexSync(...) Gl_TexSync(__VA_ARGS__)
#define sceGuTexWrap(...) Gl_TexWrap(__VA_ARGS__)
#define sceGuTexFilter(...) Gl_TexFilter(__VA_ARGS__)
#define sceGuTexScale(...) Gl_TexScale(__VA_ARGS__)
#define sceGuTexOffset(...) Gl_TexOffset(__VA_ARGS__)
#define sceGuClutLoad(...) Gl_ClutLoad(__VA_ARGS__)
#define sceGuClutMode(...) Gl_ClutMode(__VA_ARGS__)
#define sceGuDepthFunc(...) Gl_DepthFunc(__VA_ARGS__)
#define sceGuDepthMask(...) Gl_DepthMask(__VA_ARGS__)
#define sceGuDepthRange(...) Gl_DepthRange(__VA_ARGS__)
#define sceGuDepthBuffer(...) Gl_DepthBuffer(__VA_ARGS__)
#define sceGuAlphaFunc(...) Gl_AlphaFunc(__VA_ARGS__)
#define sceGuStencilFunc(...) Gl_StencilFunc(__VA_ARGS__)
#define sceGuStencilOp(...) Gl_StencilOp(__VA_ARGS__)
#define sceGuBlendFunc(...) Gl_BlendFunc(__VA_ARGS__)
#define sceGuShadeModel(...) Gl_ShadeModel(__VA_ARGS__)
#define sceGuColor(...) Gl_Color(__VA_ARGS__)
#define sceGuViewport(...) Gl_Viewport(__VA_ARGS__)
#define sceGuOffset(...) Gl_Offset(__VA_ARGS__)
#define sceGuSetDither(...) Gl_SetDither(__VA_ARGS__)
#define sceGuDrawBufferList(...) Gl_DrawBufferList(__VA_ARGS__)
#define sceGuDrawBuffer(...) Gl_DrawBuffer(__VA_ARGS__)
#define sceGuCopyImage(...) Gl_CopyImage(__VA_ARGS__)
#define sceGuDrawArray(...) Gl_DrawArray(__VA_ARGS__)
#define sceGuSetMatrix(...) Gl_SetMatrix(__VA_ARGS__)
#define sceGuColorMaterial(...) Gl_ColorMaterial(__VA_ARGS__)
#define sceGuMaterial(...) Gl_Material(__VA_ARGS__)
#define sceGuAmbient(...) Gl_Ambient(__VA_ARGS__)
#define sceGuLightMode(...) Gl_LightMode(__VA_ARGS__)
#define sceGuLight(...) Gl_Light(__VA_ARGS__)
#define sceGuLightColor(...) Gl_LightColor(__VA_ARGS__)
#define sceGuLightAtt(...) Gl_LightAtt(__VA_ARGS__)
#define sceGuClear(...) Gl_Clear(__VA_ARGS__)
#define sceGuGetMemory(...) GlGetMemory(__VA_ARGS__)
#define sceGuFinish(...) GlFinish(__VA_ARGS__)

/* Everything else that writes a list (or starts / ends one) must not be called: use GlStart / GlFinish, or add a wrapper
 * with its cost to the table above first. The expansion is a syntax error on purpose (an undeclared function would only be a
 * warning on older compilers). */
#define GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(...) (GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper_instead 1 2)
#define sceGuStart(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuFinishId(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuSendCommandi(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuSendCommandf(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuSendList(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuCallList(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuSignal(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuBeginObject(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuEndObject(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuDrawArrayN(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuDrawBezier(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuDrawSpline(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuSetStatus(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuSetAllStatus(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuBoneMatrix(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuMorphWeight(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuLightSpot(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuAmbientColor(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuModelColor(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuColorFunc(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuPixelMask(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuLogicalOp(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuFrontFace(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuFog(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuSpecular(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuDepthOffset(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuTexMapMode(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuTexProjMapMode(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuTexLevelMode(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuTexSlope(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuTexEnvColor(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuPatchDivide(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuPatchFrontFace(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#define sceGuPatchPrim(...) GL_UNCHECKED_LIBGU_CALL_use_the_Gl_wrapper(__VA_ARGS__)
#endif
