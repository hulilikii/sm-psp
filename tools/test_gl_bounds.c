/* Host test of src/psp/psp_gl_guard.h against the REAL PSPSDK libgu sources (not copies): tools/test_gl_bounds.sh compiles
 * $PSPSDK_SRC/gu/ *.c (with the 0x40000000 uncached alias removed, a PSP address-space detail that does not exist on the
 * host) next to this file. The list buffers are mapped below 4 GB, as libgu stores addresses in 32 bits.
 *
 * Checks, in this order:
 *   1. cost table: for every wrapper and every argument class, the bytes the real libgu call wrote <= the bytes the guard
 *      charged, and the charged estimate never falls below the exact position (est >= actual);
 *   2. a randomised sequence of wrapper calls on lists of many sizes: nothing is ever written past `cap - kGlFinish`, the
 *      canary behind the list is intact, a tripped list never changes again, GlFinish fits;
 *   3. recovery: longjmp to an armed recovery point, unarmed skip, unarmed GetMemory / boot list = Die;
 *   4. failure policy: sceGeListEnQueue failure, sceGuFinish failure with the GE seen done / never done. */
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <pspge.h>
#include <pspgu.h>

#include "guInternal.h"

/* ---- stand-ins for the PSP side: the guard header needs these before it is included ---- */
static jmp_buf g_die_jb;
static bool g_die_armed;
static int g_die_n;
static char g_die_msg[160];
void Die(const char *e) __attribute__((noreturn));
void Die(const char *e) {
  g_die_n++;
  snprintf(g_die_msg, sizeof g_die_msg, "%s", e);
  if (g_die_armed)
    longjmp(g_die_jb, 1);
  fprintf(stderr, "FAIL: unexpected Die(%s)\n", e);
  exit(1);
}
#define PspLog(...) ((void)0)

static uint32_t g_clock;
static int g_ge_done_after;   /* polls before it reports done (-1: never) */
static int g_polls;
static int g_enqueue_result;  /* sceGeListEnQueue */
static int g_stall_result;    /* sceGeListUpdateStallAddr */
static int g_sync_waits;
unsigned int sceKernelGetSystemTimeLow(void) { return g_clock += 1000; }
int sceKernelCpuSuspendIntr(void) { return 0; }
void sceKernelCpuResumeIntr(int f) { (void)f; }
int sceGeListEnQueue(const void *l, void *s, int cb, PspGeListArgs *a) { (void)l; (void)s; (void)cb; (void)a; return g_enqueue_result; }
int sceGeListUpdateStallAddr(int q, void *s) { (void)q; (void)s; return g_stall_result; }
int sceGeDrawSync(int t) {
  if (t == 0) {
    g_sync_waits++;
    return 0;
  }
  g_polls++;
  return (g_ge_done_after >= 0 && g_polls > g_ge_done_after) ? PSP_GE_LIST_DONE : PSP_GE_LIST_STALL_REACHED;
}
int sceGeListSync(int q, int t) { (void)q; (void)t; return 0; }
int sceGeBreak(int m, PspGeBreakParam *a) { (void)m; (void)a; return 0; }
int sceGeContinue(void) { return 0; }
int sceGeListEnQueueHead(const void *l, void *s, int cb, PspGeListArgs *a) { (void)l; (void)s; (void)cb; (void)a; return 1; }

#include "src/psp/psp_gl_guard.h"

/* The real libgu functions, bypassing the guard's macros: `(name)(...)` is not a macro invocation. */
#define RAW(name) (name)

#define CHECK(c)                                                                      \
  do {                                                                                \
    if (!(c)) {                                                                       \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                    \
      exit(1);                                                                        \
    }                                                                                 \
  } while (0)

enum { kBuf = 64 * 1024, kCanary = 64 };
static uint32_t *g_list;   /* kBuf bytes + canary, below 4 GB */
static int g_tests;

static void MapList(void) {
  void *m = mmap(NULL, 2 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  CHECK(m != MAP_FAILED);
  CHECK((uintptr_t)m + 2 * 1024 * 1024 < 0xffffffffull);
  g_list = (uint32_t *)(((uintptr_t)m + 255) & ~(uintptr_t)255);
}

static void Canary(uint32_t cap) { memset((uint8_t *)g_list + cap, 0xA5, kCanary); }
static void CanaryOk(uint32_t cap) {
  const uint8_t *p = (const uint8_t *)g_list + cap;
  for (int i = 0; i < kCanary; i++)
    CHECK(p[i] == 0xA5);
}

static void ResetGu(void) {
  g_enqueue_result = 1;
  g_stall_result = 0;
  g_die_n = 0;
  g_ge_done_after = 0;
  g_polls = 0;
  g_gl_finish_fail = false;
  gu_init = 1;
  gu_first_start = 0;  /* the first-start preamble is covered separately */
  gu_curr_context = 0;
  gu_draw_buffer.width = 480;
  gu_draw_buffer.height = 272;
  gu_draw_buffer.pixel_size = GU_PSM_5650;
  gu_draw_buffer.frame_width = 512;
  gu_draw_buffer.depth_buffer = 0;
}

static uint32_t Used(void) { return (uint32_t)RAW(sceGuCheckList)(); }

/* ---------------------------------------------------------------- 1. cost table */
/* Calls `call` through the guard, and returns (charged, written). `call` is a statement using the wrapper macros; the
 * same arguments are then replayed on the real function in a second list to measure what libgu wrote. */
static uint32_t g_charged, g_written;

#define MEASURE(wrapped, unused_)                                                                              \
  do {                                                                                                     \
    CHECK(GlStart(g_list, kBuf) >= 0);                                                                     \
    const uint32_t e0 = g_gl.est, u0 = Used();                                                             \
    wrapped;                                                                                               \
    g_charged = g_gl.est - e0;                                                                             \
    g_written = Used() - u0;                                                                               \
    CHECK(g_gl.est >= Used());                                                                             \
    CHECK(g_written <= g_charged);                                                                         \
    GlFinish();                                                                                            \
    g_tests++;                                                                                             \
  } while (0)

static void TestCostTable(void) {
  ResetGu();
  Canary(kBuf);
  int maxs = 0;
  /* Enable / Disable: every state, scissor test on and off (the scissor state is the 4-word one) */
  for (int st = 0; st < 24; st++) {
    MEASURE(sceGuEnable(st), 0);
    MEASURE(sceGuDisable(st), 0);
    maxs = st;
  }
  CHECK(maxs == 23);
  for (int w = 1; w <= 512; w += 31) {
    MEASURE(sceGuScissor(0, 0, w, 272), 0);
    MEASURE(sceGuViewport(2048, 2048, w, -272), 0);
  }
  /* scissor while GU_SCISSOR_TEST is enabled / disabled */
  CHECK(GlStart(g_list, kBuf) >= 0);
  sceGuEnable(GU_SCISSOR_TEST);
  { const uint32_t e0 = g_gl.est, u0 = Used(); sceGuScissor(1, 2, 3, 4); CHECK(Used() - u0 <= g_gl.est - e0); }
  sceGuDisable(GU_SCISSOR_TEST);
  { const uint32_t e0 = g_gl.est, u0 = Used(); sceGuScissor(1, 2, 3, 4); CHECK(Used() - u0 <= g_gl.est - e0); }
  GlFinish();

  static const float fv[3] = {0.0f, 1.0f, 0.5f};
  for (int i = 0; i < 3; i++) {
    MEASURE(sceGuTexScale(fv[i], fv[i]), 0);
    MEASURE(sceGuTexOffset(fv[i], fv[i]), 0);
  }
  for (int psm = 0; psm <= 10; psm++) {
    MEASURE(sceGuTexMode(psm, 0, 0, GU_FALSE), 0);
    MEASURE(sceGuTexMode(psm, 3, 0, GU_TRUE), 0);
    MEASURE(sceGuTexImage(0, 512, 512, 512, g_list), 0);
    MEASURE(sceGuTexImage(0, 8, 8, 8, NULL), 0);
    MEASURE(sceGuClutMode(psm, 0, 0xff, 0), 0);
    MEASURE(sceGuClutLoad(32, g_list), 0);
    MEASURE(sceGuDrawBufferList(psm, g_list, 512), 0);
    MEASURE(sceGuDrawBuffer(psm, g_list, 512), 0);
  }
  for (int f = 0; f < 8; f++) {
    MEASURE(sceGuDepthFunc(f), 0);
    MEASURE(sceGuDepthMask(f & 1), 0);
    MEASURE(sceGuAlphaFunc(f, 0x80, 0xff), 0);
    MEASURE(sceGuStencilFunc(f, 1, 0xff), 0);
    MEASURE(sceGuStencilOp(f, f, f), 0);
    MEASURE(sceGuShadeModel(f & 1), 0);
    MEASURE(sceGuTexFunc(f, f & 1), 0);
    MEASURE(sceGuTexWrap(f & 1, f & 1), 0);
    MEASURE(sceGuTexFilter(f, f), 0);
    MEASURE(sceGuBlendFunc(f, f, f, 0x12345678u, 0x9abcdef0u), 0);
    MEASURE(sceGuBlendFunc(f, GU_FIX, GU_FIX, 0x12345678u, 0x9abcdef0u), 0);
  }
  MEASURE(sceGuTexFlush(), 0);
  MEASURE(sceGuTexSync(), 0);
  MEASURE(sceGuDepthRange(65535, 0), 0);
  MEASURE(sceGuDepthRange(0, 65535), 0);
  MEASURE(sceGuDepthBuffer(g_list, 512), 0);
  MEASURE(sceGuOffset(2048 - 240, 2048 - 136), 0);
  MEASURE(sceGuColor(0xff00ff00u), 0);
  static const ScePspIMatrix4 dm = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  MEASURE(sceGuSetDither(&dm), 0);
  MEASURE(sceGuCopyImage(GU_PSM_5650, 1, 2, 256, 224, 512, g_list, 3, 4, 256, g_list), 0);
  MEASURE(sceGuCopyImage(GU_PSM_8888, 0, 0, 256, 240, 256, g_list, 0, 0, 256, g_list), 0);
  for (int vt = 0; vt < 2; vt++)
    for (int idx = 0; idx < 2; idx++)
      MEASURE(sceGuDrawArray(GU_SPRITES, vt ? 0x1234 : 0, 2, idx ? g_list : NULL, g_list), 0);

  /* Fixed-function matrix and lighting (HazeDrawMesh): every variant, and the worst one must be charged exactly (a loose
   * charge would waste list room, a short one is already caught by MEASURE) */
  static const float mtx[16] __attribute__((aligned(16))) = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  static const ScePspFVector3 lpos = {1.0f, 2.0f, 3.0f};
  uint32_t wmax_mtx = 0, wmax_mat = 0, wmax_light = 0, wmax_lcol = 0;
  for (int t = GU_PROJECTION; t <= GU_TEXTURE; t++) {
    MEASURE(sceGuSetMatrix(t, (const ScePspFMatrix4 *)mtx), 0);
    wmax_mtx = g_written > wmax_mtx ? g_written : wmax_mtx;
  }
  CHECK(wmax_mtx == (uint32_t)kGlSetMatrix);
  for (int c = 0; c < 8; c++) {  /* GU_AMBIENT | GU_DIFFUSE | GU_SPECULAR combinations */
    MEASURE(sceGuColorMaterial(c), 0);
    MEASURE(sceGuMaterial(c, 0xff102030u), 0);
    wmax_mat = g_written > wmax_mat ? g_written : wmax_mat;
  }
  CHECK(wmax_mat == (uint32_t)kGlMaterial);
  MEASURE(sceGuAmbient(0xff000000u), 0);
  for (int m = GU_SINGLE_COLOR; m <= GU_SEPARATE_SPECULAR_COLOR; m++)
    MEASURE(sceGuLightMode(m), 0);
  for (int l = 0; l < 4; l++) {
    for (int type = GU_DIRECTIONAL; type <= GU_SPOTLIGHT; type++)
      for (int comp = GU_AMBIENT; comp <= GU_DIFFUSE_AND_SPECULAR; comp++) {
        MEASURE(sceGuLight(l, type, comp, &lpos), 0);
        wmax_light = g_written > wmax_light ? g_written : wmax_light;
      }
    for (int comp = GU_AMBIENT; comp <= GU_DIFFUSE_AND_SPECULAR; comp++) {
      MEASURE(sceGuLightColor(l, comp, 0xff203040u), 0);
      wmax_lcol = g_written > wmax_lcol ? g_written : wmax_lcol;
    }
    MEASURE(sceGuLightAtt(l, 1.0f, 0.0f, 1.0f / 10000.0f), 0);
    CHECK(g_written == (uint32_t)kGlLightAtt);
  }
  CHECK(wmax_light == (uint32_t)kGlLight);
  CHECK(wmax_lcol == 8u);

  /* Clear: every pixel format, width, flag set; fast and plain */
  const int fmts[4] = {GU_PSM_5650, GU_PSM_5551, GU_PSM_4444, GU_PSM_8888};
  for (int f = 0; f < 4; f++)
    for (int w = 1; w <= 480; w += (w < 80 ? 1 : 17)) {
      gu_draw_buffer.pixel_size = fmts[f];
      gu_draw_buffer.width = w;
      for (int fl = 0; fl < 16; fl++) {
        MEASURE(sceGuClear(fl), 0);
        MEASURE(sceGuClear(fl | GU_FAST_CLEAR_BIT), 0);
      }
    }
  ResetGu();

  /* GetMemory: every size, including the unaligned ones */
  for (int n = 0; n <= 70; n++)
    MEASURE((void)sceGuGetMemory(n), 0);
  for (int n = 4000; n <= 4010; n++)
    MEASURE((void)sceGuGetMemory(n), 0);

  CanaryOk(kBuf);
  printf("1. cost table: %d measured calls, charged >= written and est >= exact for all\n", g_tests);
}

/* ---------------------------------------------------------------- 2. randomised sequences */
static uint32_t g_rng = 12345;
static uint32_t Rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void RandomCall(void) {
  switch (Rnd() % 24) {
    case 0: sceGuEnable((int)(Rnd() % 24)); break;
    case 1: sceGuDisable((int)(Rnd() % 24)); break;
    case 2: sceGuScissor(0, 0, 480, 272); break;
    case 3: sceGuTexImage(0, 512, 512, 512, g_list); break;
    case 4: sceGuTexMode(GU_PSM_T4, 0, 0, 1); break;
    case 5: sceGuTexFlush(); break;
    case 6: sceGuTexSync(); break;
    case 7: sceGuClutLoad(2, g_list); break;
    case 8: sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xffffffu, 0x808080u); break;
    case 9: sceGuDrawBufferList(GU_PSM_5650, g_list, 512); break;
    case 10: sceGuDrawBuffer(GU_PSM_5650, g_list, 512); break;
    case 11: sceGuCopyImage(GU_PSM_5650, 0, 0, 256, 224, 512, g_list, 0, 0, 256, g_list); break;
    case 12: sceGuDrawArray(GU_SPRITES, 0x1234, 2, NULL, g_list); break;
    case 13: sceGuClear(GU_COLOR_BUFFER_BIT | (Rnd() & 1 ? GU_FAST_CLEAR_BIT : 0)); break;
    case 14: sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT | GU_FAST_CLEAR_BIT); break;
    case 15: (void)sceGuGetMemory((int)(Rnd() % 700)); break;
    case 16: (void)sceGuGetMemory((int)(Rnd() % 40)); break;
    case 17: sceGuColor(Rnd()); break;
    case 18: sceGuDepthRange(1, 2); break;
    case 19: sceGuViewport(1, 2, 3, 4); break;
    case 20: sceGuTexScale(1.0f, 2.0f); break;
    case 21: sceGuTexOffset(1.0f, 2.0f); break;
    case 22: (void)sceGuGetMemory((int)(Rnd() % 3000)); break;
    default: sceGuStencilOp(1, 2, 3); break;
  }
}

static void TestRandom(void) {
  static const uint32_t caps[] = {1024, 1100, 1536, 2048, 4000, 4096, 9000, 16384, 65536};
  volatile uint32_t full = 0, ok = 0;
  for (volatile int rep = 0; rep < 400; rep++) {
    for (volatile unsigned c = 0; c < sizeof caps / sizeof caps[0]; c++) {
      const uint32_t cap = caps[c];
      ResetGu();
      Canary(cap);
      memset(g_list, 0, cap);
      CHECK(GlStart(g_list, cap) >= 0);
      CHECK(g_gl.est == Used());
      volatile bool tripped = false;
      volatile uint32_t frozen = 0;
      const int ncalls = 1 + (int)(Rnd() % 4000);
      for (volatile int i = 0; i < ncalls; i++) {
        g_die_armed = true;  /* an unarmed GetMemory that does not fit is a Die: it must only happen on a full list */
        g_die_n = 0;
        if (!setjmp(g_die_jb))
          RandomCall();
        g_die_armed = false;
        CHECK(g_die_n == 0 || g_gl_full);
        CHECK(g_gl.est >= Used());                 /* the estimate is an upper bound */
        CHECK(Used() <= cap - kGlFinish);          /* room for FINISH + END always */
        if (g_gl_full) {
          if (!tripped) {
            tripped = true;
            frozen = Used();
          }
          CHECK(Used() == frozen);                 /* a tripped list never changes again */
        }
      }
      if (tripped) full++; else ok++;
      const int r = GlFinish();
      CHECK(r >= 0 && (uint32_t)r <= cap);
      CanaryOk(cap);
    }
  }
  printf("2. random sequences: %u lists tripped, %u fitted; never wrote past cap - %d, canary intact\n", full, ok, (int)kGlFinish);
  CHECK(full > 0);
}

/* ---------------------------------------------------------------- 3. recovery */
static void TestRecovery(void) {
  ResetGu();
  Canary(2048);
  /* armed: the refused call longjmps, nothing was written by it, the abort finishes and waits */
  CHECK(GlStart(g_list, 2048) >= 0);
  int landed = 0;
  if (setjmp(g_gl_jb)) {
    landed = 1;
  } else {
    GL_ARM();
    for (int i = 0; i < 100000; i++)
      sceGuDrawArray(GU_SPRITES, 0x1234, 2, NULL, g_list);
    CHECK(0);  /* must have tripped */
  }
  CHECK(landed && g_gl_full && !g_gl_armed);
  const uint32_t at_trip = Used();
  const int waits0 = g_sync_waits;
  GlAbortList();
  CHECK(g_sync_waits == waits0 + 1 && !g_gl_open);
  CHECK(Used() >= at_trip && Used() <= 2048);
  CanaryOk(2048);

  /* unarmed: commands are skipped, still no write */
  Canary(1024);
  CHECK(GlStart(g_list, 1024) >= 0);
  for (int i = 0; i < 1000; i++)
    sceGuTexFlush();
  CHECK(g_gl_full && Used() <= 1024 - kGlFinish);
  const uint32_t u = Used();
  sceGuTexFlush();
  sceGuDrawArray(GU_SPRITES, 0, 2, NULL, g_list);
  CHECK(Used() == u);
  /* unarmed allocation: Die, before any write */
  g_die_armed = true;
  if (!setjmp(g_die_jb))
    (void)sceGuGetMemory(16);
  g_die_armed = false;
  CHECK(g_die_n == 1 && Used() == u);
  GlFinish();
  CanaryOk(1024);

  /* sizes that must be refused outright */
  const int bad[] = {-1, -4, INT_MIN, 1 << 30, 100000};
  for (volatile unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CHECK(GlStart(g_list, 4096) >= 0);
    const uint32_t u0 = Used();
    g_die_armed = true;
    g_die_n = 0;
    if (!setjmp(g_die_jb))
      (void)sceGuGetMemory(bad[i]);
    g_die_armed = false;
    CHECK(g_die_n == 1 && Used() == u0 && g_gl_full);
    GlFinish();
  }

  /* boot-time list: a call that does not fit is a Die, before it writes */
  CHECK(GlStartFatal(g_list, 1024) >= 0);
  g_die_armed = true;
  g_die_n = 0;
  if (!setjmp(g_die_jb))
    for (int i = 0; i < 1000; i++)
      sceGuTexFlush();
  g_die_armed = false;
  CHECK(g_die_n == 1 && Used() <= 1024 - kGlFinish);
  GlFinish();
  CHECK(!g_gl_fatal);

  /* a new list clears a stale recovery point */
  CHECK(GlStart(g_list, 4096) >= 0);
  GL_ARM();
  CHECK(GlStart(g_list, 4096) >= 0 && !g_gl_armed);
  GlFinish();
  printf("3. recovery: armed longjmp, unarmed skip / Die, refused sizes, boot list, stale arm: ok\n");
}

/* ---------------------------------------------------------------- 4. failure policy */
static void TestFailures(void) {
  ResetGu();
  Canary(4096);
  /* sceGeListEnQueue fails: nothing may be written, finished or waited for */
  g_enqueue_result = -1;
  CHECK(GlStart(g_list, 4096) < 0 && !g_gl_open);
  const uint32_t u = gu_list->current - gu_list->start;
  sceGuTexFlush();
  sceGuDrawArray(GU_SPRITES, 0, 2, NULL, g_list);
  CHECK((uint32_t)(gu_list->current - gu_list->start) == u && g_gl_trips > 0);
  CHECK(GlFinish() == 0);
  g_enqueue_result = 1;
  /* bad arguments */
  CHECK(GlStart(NULL, 4096) < 0 && GlStart((uint8_t *)g_list + 4, 4096) < 0 && GlStart(g_list, 16) < 0 && !g_gl_open);

  /* sceGuFinish fails (stall update) but the GE reports done within the poll window: recorded, async off, no Die */
  CHECK(GlStart(g_list, 4096) >= 0);
  sceGuTexFlush();
  g_stall_result = -1;
  g_ge_done_after = 3;
  g_polls = 0;
  const uint32_t errs = g_gl_finish_err;
  CHECK(GlFinish() < 0);
  CHECK(g_gl_finish_err == errs + 1 && g_gl_finish_fail && !g_gl_open && g_gl.lim == 0 && g_die_n == 0);
  CHECK(g_polls >= 3);
  g_stall_result = 0;
  gu_curr_context = 0;

  /* ... and never done: the run stops (Die), after a bounded poll, without a blocking wait */
  ResetGu();
  CHECK(GlStart(g_list, 4096) >= 0);
  g_stall_result = -1;
  g_ge_done_after = -1;
  g_polls = 0;
  const int waits0 = g_sync_waits;
  const uint32_t c0 = g_clock;
  g_die_armed = true;
  if (!setjmp(g_die_jb))
    GlFinish();
  g_die_armed = false;
  CHECK(g_die_n == 1 && strstr(g_die_msg, "unconfirmed"));
  CHECK(g_sync_waits == waits0);                              /* never the blocking sceGeDrawSync(0) */
  CHECK(g_clock - c0 <= (uint32_t)kGlConfirmUs + 5000);       /* bounded */
  CHECK(g_gl_finish_fail && !g_gl_open);

  /* an abort whose finish fails must not reach the blocking wait either */
  ResetGu();
  CHECK(GlStart(g_list, 4096) >= 0);
  g_stall_result = -1;
  g_ge_done_after = -1;
  const int w1 = g_sync_waits;
  g_die_armed = true;
  g_die_n = 0;
  if (!setjmp(g_die_jb))
    GlAbortList();
  g_die_armed = false;
  CHECK(g_die_n == 1 && g_sync_waits == w1);
  g_stall_result = 0;
  ResetGu();
  printf("4. failure policy: failed start writes nothing; failed finish = confirmed-done (async off) or Die, never an unbounded wait\n");
}

int main(void) {
  MapList();
  TestCostTable();
  TestRandom();
  TestRecovery();
  TestFailures();
  printf("test_gl_bounds: all passed\n");
  return 0;
}
