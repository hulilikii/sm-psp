#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <pspaudiolib.h>
#include <pspthreadman.h>

#include "snes/ppu.h"
#include "types.h"
#include "sm_rtl.h"
#include "sm_cpu_infra.h"
#include "config.h"
#include "util.h"
#include "spc_player.h"
#include "platform/psp/psp_renderer.h"
#include "platform/psp/psp_input.h"

PSP_MODULE_INFO("SuperMetroidPSP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_HEAP_SIZE_KB(-1024);

bool g_debug_flag;
bool g_is_turbo;
bool g_want_dump_memmap_flags;
bool g_new_ppu = true;
bool g_other_image;
struct SpcPlayer *g_spc_player;
Snes *g_snes;
int g_got_mismatch_count;

static uint8_t g_pixels[256 * 4 * 240] __attribute__((aligned(16)));
static uint8_t g_my_pixels[256 * 4 * 240] __attribute__((aligned(16)));
static struct RendererFuncs g_renderer_funcs;
static SceUID g_audio_sema = -1;
static bool g_running = true;
static bool g_turbo;
static uint8 g_replay_turbo = true;
static int g_ppu_render_flags;
static int g_snes_width = 256;
static int g_snes_height = 240;
static uint32_t g_frame_ctr;
static uint64_t g_profile_runframe_us;
static uint64_t g_profile_audio_wait_us;
static uint32_t g_profile_audio_lock_count;
extern uint64_t g_profile_audio_render_us;
extern uint64_t g_profile_audio_generate_us;
extern uint64_t g_profile_audio_dsp_us;
extern uint32_t g_profile_audio_callback_count;
extern uint64_t g_profile_audio_samples;

void NORETURN Die(const char *error) {
  fprintf(stderr, "Error: %s\n", error);
  sceKernelExitGame();
  for (;;) sceKernelDelayThread(1000000);
}

void Warning(const char *error) {
  fprintf(stderr, "Warning: %s\n", error);
}

void RtlApuLock(void) {
  if (g_audio_sema >= 0) {
    const uint64_t start = sceKernelGetSystemTimeWide();
    sceKernelWaitSema(g_audio_sema, 1, NULL);
    g_profile_audio_wait_us += sceKernelGetSystemTimeWide() - start;
    ++g_profile_audio_lock_count;
  }
}

void RtlApuUnlock(void) {
  if (g_audio_sema >= 0)
    sceKernelSignalSema(g_audio_sema, 1);
}

static void AudioCallback(void *buf, unsigned int reqn, void *pdata) {
  (void)pdata;
  ++g_profile_audio_callback_count;
  g_profile_audio_samples += reqn;
  RtlRenderAudio((int16 *)buf, (int)reqn, 2);
}

static void Audio_Init(void) {
  g_audio_sema = sceKernelCreateSema("sm-psp-audio", 0, 1, 1, NULL);
  if (g_audio_sema < 0)
    Die("sceKernelCreateSema failed");

  if (pspAudioInit() < 0)
    Die("pspAudioInit failed");

  pspAudioSetChannelCallback(0, AudioCallback, NULL);
  pspAudioSetVolume(0, PSP_VOLUME_MAX, PSP_VOLUME_MAX);
}

static void Audio_Shutdown(void) {
  pspAudioSetChannelCallback(0, NULL, NULL);
  pspAudioEnd();
  if (g_audio_sema >= 0) {
    sceKernelDeleteSema(g_audio_sema);
    g_audio_sema = -1;
  }
}

void RtlDrawPpuFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
  (void)render_flags;
  const uint8 *src = g_other_image ? g_my_pixels : g_pixels;
  for (int y = 0; y < 240; ++y)
    memcpy(pixel_buffer + y * pitch, src + y * 256 * 4, 256 * 4);
}

static void DrawPpuFrame(void) {
  uint8 *pixel_buffer;
  int pitch;
  g_renderer_funcs.BeginDraw(g_snes_width, g_snes_height, &pixel_buffer, &pitch);
  RtlDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
  g_renderer_funcs.EndDraw();
}

static void HandleCommand(uint32 j, bool pressed) {
  if (j <= kKeys_Controls_Last) {
    static const uint8 kRemap[] = { 0, 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };
    if (pressed)
      return;
    (void)kRemap;
    return;
  }
  if (!pressed)
    return;
  if (j <= kKeys_Load_Last)
    RtlSaveLoad(kSaveLoad_Load, j - kKeys_Load);
  else if (j <= kKeys_Save_Last)
    RtlSaveLoad(kSaveLoad_Save, j - kKeys_Save);
  else if (j <= kKeys_Replay_Last)
    RtlSaveLoad(kSaveLoad_Replay, j - kKeys_Replay);
  else if (j <= kKeys_LoadRef_Last)
    RtlSaveLoad(kSaveLoad_Load, 256 + j - kKeys_LoadRef);
  else if (j <= kKeys_ReplayRef_Last)
    RtlSaveLoad(kSaveLoad_Replay, 256 + j - kKeys_ReplayRef);
}

static void SetupFilesystem(void) {
  sceIoChdir("ms0:/PSP/GAME/SMPSP");
  scePowerSetClockFrequency(333, 333, 166);
  mkdir("saves", 0777);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  SetupFilesystem();
  PspInput_Init();
  ParseConfigFile(NULL);

  /* The PSP renderer is fixed at the native 256x240 software-PPU size. */
  g_snes_width = 256;
  g_snes_height = 240;
  g_ppu_render_flags = kPpuRenderFlags_NewRenderer | kPpuRenderFlags_Height240;

  PspRenderer_Create(&g_renderer_funcs);
  if (!g_renderer_funcs.Initialize())
    return 1;

  g_snes = SnesInit("sm.smc");
  if (g_snes == NULL)
    Die("Unable to load sm.smc");

  PpuBeginDrawing(g_snes->snes_ppu, g_pixels, 256 * 4, 0);
  PpuBeginDrawing(g_snes->my_ppu, g_my_pixels, 256 * 4, 0);

  g_spc_player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_spc_player);
  Audio_Init();

  RtlReadSram();
  if (g_config.autosave)
    HandleCommand(kKeys_Load, true);

  while (g_running) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    if (pad.Buttons & PSP_CTRL_HOME) {
      g_running = false;
      break;
    }

    uint16 inputs = PspInput_Read();
    const uint64_t runframe_start = sceKernelGetSystemTimeWide();
    uint8 is_replay = RtlRunFrame(inputs);
    g_profile_runframe_us += sceKernelGetSystemTimeWide() - runframe_start;
    ++g_frame_ctr;

    if ((g_frame_ctr % 60) == 0) {
      fprintf(stderr,
              "PSP profile/main: frames=60 RtlRunFrame=%.2fms "
              "audio_lock_wait=%.2fms audio_locks=%u\n",
              (double)g_profile_runframe_us / 60.0 / 1000.0,
              (double)g_profile_audio_wait_us / 1000.0,
              (unsigned)g_profile_audio_lock_count);
      fprintf(stderr,
              "PSP profile/audio: callbacks=%u samples=%llu render=%.2fms "
              "generate=%.2fms dsp=%.2fms\n",
              (unsigned)g_profile_audio_callback_count,
              (unsigned long long)g_profile_audio_samples,
              (double)g_profile_audio_render_us / 1000.0,
              (double)g_profile_audio_generate_us / 1000.0,
              (double)g_profile_audio_dsp_us / 1000.0);
      g_profile_runframe_us = 0;
      g_profile_audio_wait_us = 0;
      g_profile_audio_lock_count = 0;
      g_profile_audio_render_us = 0;
      g_profile_audio_generate_us = 0;
      g_profile_audio_dsp_us = 0;
      g_profile_audio_callback_count = 0;
      g_profile_audio_samples = 0;
    }

    g_snes->disableRender = (g_turbo ^ (is_replay & g_replay_turbo)) &&
                             (g_frame_ctr & (g_turbo ? 0xf : 0x7f)) != 0;
    if (!g_snes->disableRender)
      DrawPpuFrame();
  }

  if (g_config.autosave)
    HandleCommand(kKeys_Save, true);

  Audio_Shutdown();
  g_renderer_funcs.Destroy();
  sceKernelExitGame();
  return 0;
}
