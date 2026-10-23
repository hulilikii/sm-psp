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
#include "psp/psp_renderer.h"
#include "psp/psp_input.h"
#include "psp/psp_log.h"
#if PSP_AUDIO_ME
#include "psp/psp_audio_me.h"
#endif

PSP_MODULE_INFO("スーパーメトロイド", 0, 1, 0);
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
static volatile bool g_running = true;
static bool g_turbo;
static uint8 g_replay_turbo = true;
static int g_ppu_render_flags;
static int g_snes_width = 256;
static int g_snes_height = 240;
static uint32_t g_frame_ctr;


/* HOME is owned by the firmware in user mode: without an exit callback the
 * button shows the exit dialog and then hangs the app */
static int ExitCallback(int arg1, int arg2, void *common) {
  (void)arg1; (void)arg2; (void)common;
  g_running = false;
  return 0;
}

static int CallbackThread(SceSize args, void *argp) {
  (void)args; (void)argp;
  int cb = sceKernelCreateCallback("exit_cb", ExitCallback, NULL);
  sceKernelRegisterExitCallback(cb);
  sceKernelSleepThreadCB();
  return 0;
}

static void SetupExitCallback(void) {
  int thid = sceKernelCreateThread("exit_thread", CallbackThread, 0x11, 0xFA0, 0, NULL);
  if (thid >= 0)
    sceKernelStartThread(thid, 0, NULL);
}

void NORETURN Die(const char *error) {
  fprintf(stderr, "Error: %s\n", error);
  PspLog("DIE: %s", error);
  PspLog_Flush();
  sceKernelExitGame();
  for (;;) sceKernelDelayThread(1000000);
}

void Warning(const char *error) {
  fprintf(stderr, "Warning: %s\n", error);
}

#if PSP_AUDIO_ENABLED

// g_audio_sema guards the SPC/DSP state (held across a whole ME job);
// g_apu_queue_sema guards only the port-write queue (short holds).
static SceUID g_audio_sema = -1;
static SceUID g_apu_queue_sema = -1;

void RtlApuLockRaw(void) {
  if (g_audio_sema >= 0)
    sceKernelWaitSema(g_audio_sema, 1, NULL);
}

void RtlApuUnlockRaw(void) {
  if (g_audio_sema >= 0)
    sceKernelSignalSema(g_audio_sema, 1);
}

void RtlApuLock(void) {
  RtlApuLockRaw();
#if PSP_AUDIO_ME
  PspAudioMe_LockState();
#endif
}

void RtlApuUnlock(void) {
#if PSP_AUDIO_ME
  PspAudioMe_UnlockState();
#endif
  RtlApuUnlockRaw();
}

void RtlApuQueueLock(void) {
  if (g_apu_queue_sema >= 0)
    sceKernelWaitSema(g_apu_queue_sema, 1, NULL);
}

void RtlApuQueueUnlock(void) {
  if (g_apu_queue_sema >= 0)
    sceKernelSignalSema(g_apu_queue_sema, 1);
}

// One SPC frame is 534 samples @ ~32040 Hz = 735 frames @ 44100 Hz. The PSP audio
// callback asks for a different block size (1024), so render whole 735-frame
// blocks and hand them out in pieces.
#define SM_AUDIO_BLOCK_FRAMES 735
static int16_t g_audio_block[SM_AUDIO_BLOCK_FRAMES * 2] __attribute__((aligned(64)));
static unsigned g_audio_block_pos = SM_AUDIO_BLOCK_FRAMES;

static void AudioCallback(void *buf, unsigned int reqn, void *pdata) {
  (void)pdata;
  int16_t *out = (int16_t *)buf;
  while (reqn != 0) {
    if (g_audio_block_pos >= SM_AUDIO_BLOCK_FRAMES) {
      RtlRenderAudio(g_audio_block, SM_AUDIO_BLOCK_FRAMES, 2);
      g_audio_block_pos = 0;
    }
    unsigned n = SM_AUDIO_BLOCK_FRAMES - g_audio_block_pos;
    if (n > reqn)
      n = reqn;
    memcpy(out, &g_audio_block[g_audio_block_pos * 2], n * 2 * sizeof(int16_t));
    out += n * 2;
    g_audio_block_pos += n;
    reqn -= n;
  }
}

static void Audio_Init(void) {
  g_audio_sema = sceKernelCreateSema("sm-psp-audio", 0, 1, 1, NULL);
  g_apu_queue_sema = sceKernelCreateSema("sm-psp-apuq", 0, 1, 1, NULL);
  if (g_audio_sema < 0 || g_apu_queue_sema < 0)
    Die("sceKernelCreateSema failed");

#if PSP_AUDIO_ME
  // Before pspAudioInit so no audio callback can race the ME start-up.
  if (PspAudioMe_Init(g_spc_player))
    PspLog("audio: SPC/DSP running on the Media Engine");
  else
    PspLog("audio: Media Engine unavailable, using CPU audio");
#endif

  if (pspAudioInit() < 0)
    Die("pspAudioInit failed");

  pspAudioSetChannelCallback(0, AudioCallback, NULL);
  pspAudioSetVolume(0, PSP_VOLUME_MAX, PSP_VOLUME_MAX);
}

static void Audio_Shutdown(void) {
  // Order: stop the audio callback first, then the ME, then the semaphores.
  pspAudioSetChannelCallback(0, NULL, NULL);
  pspAudioEnd();
#if PSP_AUDIO_ME
  PspAudioMe_Shutdown();
#endif
  if (g_audio_sema >= 0) {
    sceKernelDeleteSema(g_audio_sema);
    g_audio_sema = -1;
  }
  if (g_apu_queue_sema >= 0) {
    sceKernelDeleteSema(g_apu_queue_sema);
    g_apu_queue_sema = -1;
  }
}

#else

void RtlApuLock(void) {
}

void RtlApuUnlock(void) {
}

void RtlApuLockRaw(void) {
}

void RtlApuUnlockRaw(void) {
}

void RtlApuQueueLock(void) {
}

void RtlApuQueueUnlock(void) {
}

#endif

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

static void SetupFilesystem(const char *argv0) {
  char app_dir[256];
  const char *last_slash = argv0 ? strrchr(argv0, '/') : NULL;

  if (last_slash != NULL && last_slash != argv0) {
    size_t app_dir_len = (size_t)(last_slash - argv0);
    if (app_dir_len >= sizeof(app_dir))
      Die("Application path is too long");
    memcpy(app_dir, argv0, app_dir_len);
    app_dir[app_dir_len] = '\0';
  } else {
    /* Fallback for launchers that do not provide a full argv[0] path. */
    strcpy(app_dir, "ms0:/PSP/GAME/SMPSP");
  }

  int chdir_result = sceIoChdir(app_dir);
  if (chdir_result < 0) {
    fprintf(stderr, "sceIoChdir(%s) failed: 0x%08X\n", app_dir, chdir_result);
    Die("Unable to access the application directory");
  }

  scePowerSetClockFrequency(333, 333, 166);
  mkdir("saves", 0777);
}

int main(int argc, char **argv) {
  (void)argc;
  
  SetupExitCallback();
  SetupFilesystem(argv[0]);
  PspLog_Init();
  PspLog("clock cpu=%d bus=%d", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
  PspInput_Init();
  ParseConfigFile(NULL);

  /* The PSP renderer is fixed at the native 256x240 software-PPU size. */
  g_snes_width = 256;
  g_snes_height = 240;
  g_ppu_render_flags = kPpuRenderFlags_NewRenderer | kPpuRenderFlags_Height240;

  PspLog("boot: before renderer init");
  PspRenderer_Create(&g_renderer_funcs);
  if (!g_renderer_funcs.Initialize())
    return 1;
  PspLog("boot: renderer ready, loading ROM");

  g_snes = SnesInit("sm.smc");
  if (g_snes == NULL)
    Die("Unable to load sm.smc");

  PpuBeginDrawing(g_snes->snes_ppu, g_pixels, 256 * 4, 0);
  PpuBeginDrawing(g_snes->my_ppu, g_my_pixels, 256 * 4, 0);

  PspLog("boot: ROM loaded, entering main loop");
  PspLog_Flush();
  g_spc_player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_spc_player);
  #if PSP_AUDIO_ENABLED
  Audio_Init();
  #endif

  RtlReadSram();
  if (g_config.autosave)
    HandleCommand(kKeys_Load, true);

  while (g_running) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    /* Renderer toggle (software <-> GE): NOTE or SCREEN button, or L+R+SELECT.
     * Edge-triggered. NOTE/SCREEN are not forwarded to the game. */
    {
      static unsigned prev_buttons;
      const unsigned chord = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT;
      const unsigned single = PSP_CTRL_NOTE | PSP_CTRL_SCREEN;
      unsigned b = pad.Buttons;
      bool now = (b & single) != 0 || (b & chord) == chord;
      bool before = (prev_buttons & single) != 0 || (prev_buttons & chord) == chord;
      if (now && !before)
        PspRenderer_ToggleGe();
      prev_buttons = b;
    }

    uint16 inputs = PspInput_Read();
    uint8 is_replay = RtlRunFrame(inputs);
    ++g_frame_ctr;

    g_snes->disableRender = (g_turbo ^ (is_replay & g_replay_turbo)) &&
                             (g_frame_ctr & (g_turbo ? 0xf : 0x7f)) != 0;
    if (!g_snes->disableRender)
      DrawPpuFrame();
  }

  if (g_config.autosave)
    HandleCommand(kKeys_Save, true);

  #if PSP_AUDIO_ENABLED
  Audio_Shutdown();
  #endif
  PspLog("clean exit after %u frames", (unsigned)g_frame_ctr);
  PspLog_Flush();
  g_renderer_funcs.Destroy();
  sceKernelExitGame();
  return 0;
}
