#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <psputils.h>

#include "src/types.h"
#include "src/util.h"

#define PSP_WIDTH 480
#define PSP_HEIGHT 272
#define GAME_WIDTH 256
#define GAME_HEIGHT 240
#define TEX_WIDTH 512
#define TEX_HEIGHT 256
#define GU_LIST_WORDS (64 * 1024)

static uint32_t g_gu_list[GU_LIST_WORDS] __attribute__((aligned(16)));
static uint16_t g_texture[TEX_WIDTH * TEX_HEIGHT] __attribute__((aligned(16)));
static uint8_t g_framebuffer[GAME_WIDTH * GAME_HEIGHT * 4] __attribute__((aligned(16)));

static inline uint16_t Rgb565(const uint8_t *p) {
  const uint8_t b = p[0];
  const uint8_t g = p[1];
  const uint8_t r = p[2];
  return (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

static void PspRenderer_Convert(void) {
  for (int y = 0; y < GAME_HEIGHT; ++y) {
    const uint8_t *src = g_framebuffer + y * GAME_WIDTH * 4;
    uint16_t *dst = g_texture + y * TEX_WIDTH;
    for (int x = 0; x < GAME_WIDTH; ++x)
      dst[x] = Rgb565(src + x * 4);
  }
}

static bool PspRenderer_Initialize(void) {
  sceGuInit();
  sceGuStart(GU_DIRECT, g_gu_list);

  sceGuDrawBuffer(GU_PSM_5650, (void *)0, 512);
  sceGuDispBuffer(PSP_WIDTH, PSP_HEIGHT, (void *)(512 * 272 * 2), 512);
  sceGuDepthBuffer((void *)(512 * 272 * 4), 512);

  sceGuOffset(2048 - PSP_WIDTH / 2, 2048 - PSP_HEIGHT / 2);
  sceGuViewport(2048, 2048, PSP_WIDTH, PSP_HEIGHT);
  sceGuScissor(0, 0, PSP_WIDTH, PSP_HEIGHT);
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
  return true;
}

static void PspRenderer_Destroy(void) {
  sceGuDisplay(GU_FALSE);
  sceGuTerm();
}

static void PspRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  if (width != GAME_WIDTH || height != GAME_HEIGHT)
    Die("PSP renderer requires a 256x240 framebuffer");
  *pixels = g_framebuffer;
  *pitch = GAME_WIDTH * 4;
}

typedef struct PspVertex {
  float u, v;
  float x, y, z;
} PspVertex;

static void PspRenderer_EndDraw(void) {
  PspRenderer_Convert();

  sceKernelDcacheWritebackAll();
  sceGuStart(GU_DIRECT, g_gu_list);
  sceGuTexImage(0, TEX_WIDTH, TEX_HEIGHT, TEX_WIDTH, g_texture);

  const int left = (PSP_WIDTH - GAME_WIDTH) / 2;
  const int top = (PSP_HEIGHT - GAME_HEIGHT) / 2;

  PspVertex *v = (PspVertex *)sceGuGetMemory(2 * sizeof(PspVertex));
  v[0].u = 0;          v[0].v = 0;           v[0].x = left;          v[0].y = top;           v[0].z = 0;
  v[1].u = GAME_WIDTH; v[1].v = GAME_HEIGHT; v[1].x = left + GAME_WIDTH; v[1].y = top + GAME_HEIGHT; v[1].z = 0;

  sceGuDrawArray(GU_SPRITES,
                 GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                 2, NULL, v);
  sceGuFinish();
  sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
  sceDisplayWaitVblankStart();
  sceGuSwapBuffers();
}

static const struct RendererFuncs kPspRendererFuncs = {
  &PspRenderer_Initialize,
  &PspRenderer_Destroy,
  &PspRenderer_BeginDraw,
  &PspRenderer_EndDraw,
};

void PspRenderer_Create(struct RendererFuncs *funcs) {
  *funcs = kPspRendererFuncs;
}
