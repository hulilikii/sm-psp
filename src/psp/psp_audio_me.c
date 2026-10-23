#include "psp_audio_me.h"

#include "../spc_player.h"

#include <me-core-mapper/me-core.h>  // defines the ME entry code: include in THIS file only
#include <pspkernel.h>
#include <string.h>

#define ME_CACHE_LINE 64
#define ME_MAX_SAMPLES 1024
#define ME_CHANNELS 2
#define ME_BOOT_TIMEOUT_US 250000
#define ME_JOB_TIMEOUT_US 250000
#define ME_POLL_US 50

enum { kBooting, kIdle, kRun, kStop, kHalted, kFault };

enum {
  kShState, kShProgress, kShPlayer, kShDsp, kShSamples, kShPortsIn,
  kShCount = 16,  // 64 bytes: whole cache line
};

// Mailbox: only ever touched through the uncached alias, from both CPUs.
static volatile uint32_t g_me_shared_storage[kShCount]
    __attribute__((aligned(ME_CACHE_LINE), section(".uncached")));
#define ME_SHARED ((volatile uint32_t *)(UNCACHED_USER_MASK | (uintptr_t)g_me_shared_storage))

static int16_t g_me_output[ME_MAX_SAMPLES * ME_CHANNELS] __attribute__((aligned(ME_CACHE_LINE)));
static SpcPlayer *g_me_player;
static bool g_me_active;

static void AlignRange(const void *a, uint32_t size, void **start, uint32_t *out) {
  uintptr_t s = (uintptr_t)a & ~(uintptr_t)(ME_CACHE_LINE - 1);
  uintptr_t e = ((uintptr_t)a + size + ME_CACHE_LINE - 1) & ~(uintptr_t)(ME_CACHE_LINE - 1);
  *start = (void *)s;
  *out = (uint32_t)(e - s);
}
static void CpuWritebackInvalidate(const void *a, uint32_t n) {
  void *s; uint32_t z; AlignRange(a, n, &s, &z); sceKernelDcacheWritebackInvalidateRange(s, z);
}
static void CpuInvalidate(const void *a, uint32_t n) {
  void *s; uint32_t z; AlignRange(a, n, &s, &z); sceKernelDcacheInvalidateRange(s, z);
}
static void MeInvalidate(const void *a, uint32_t n) {
  void *s; uint32_t z; AlignRange(a, n, &s, &z); meCoreDcacheInvalidateRange(s, z);
}
static void MeWriteback(const void *a, uint32_t n) {
  void *s; uint32_t z; AlignRange(a, n, &s, &z); meCoreDcacheWritebackRange(s, z);
}

// ---------------------------------------------------------------- ME side
// Runs on the Media Engine: no libc init, no syscalls, no printf/assert.
__attribute__((noinline, aligned(4))) void meLibOnException(void) {
  ME_SHARED[kShState] = kFault;
  meLibSync();
  meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnExternalInterrupt(void) {
  ME_SHARED[kShState] = kFault;
  meLibSync();
  meLibHalt();
}

__attribute__((noinline, aligned(4))) void meLibOnProcess(void) {
  ME_SHARED[kShProgress] = 1;
  meLibSync();
  while (ME_SHARED[kShState] == kBooting)
    meLibDelayPipeline();
  ME_SHARED[kShProgress] = 2;
  meLibSync();

  while (ME_SHARED[kShState] != kStop) {
    if (ME_SHARED[kShState] != kRun) {
      meLibDelayPipeline();
      continue;
    }
    SpcPlayer *player = (SpcPlayer *)(uintptr_t)ME_SHARED[kShPlayer];
    Dsp *dsp = (Dsp *)(uintptr_t)ME_SHARED[kShDsp];
    uint32_t pk = ME_SHARED[kShPortsIn];
    uint32_t samples = ME_SHARED[kShSamples];
    if (samples == 0 || samples > ME_MAX_SAMPLES) {
      ME_SHARED[kShState] = kFault;
      meLibSync();
      meLibHalt();
    }

    // Drop whatever the ME cached last job; the CPU may have changed the state.
    MeInvalidate(player, sizeof(*player));
    MeInvalidate(dsp, sizeof(*dsp));

    for (int i = 0; i < 4; i++) {
      uint8_t v = (uint8_t)(pk >> (i * 8));
      if (v != 255)
        player->input_ports[i] = v;
    }
    SpcPlayer_GenerateSamples(player);
    dsp_getSamples(dsp, g_me_output, (int)samples);

    // Publish everything the job wrote, then signal completion.
    MeWriteback(g_me_output, samples * ME_CHANNELS * sizeof(int16_t));
    MeWriteback(dsp, sizeof(*dsp));
    MeWriteback(player, sizeof(*player));
    meLibSync();
    ME_SHARED[kShState] = kIdle;
    meLibSync();
  }
  ME_SHARED[kShState] = kHalted;
  meLibSync();
  meLibHalt();
}

// --------------------------------------------------------------- CPU side
static bool WaitForValue(int idx, uint32_t value, uint32_t timeout_us) {
  uint32_t start = sceKernelGetSystemTimeLow();
  while (ME_SHARED[idx] != value) {
    if (ME_SHARED[kShState] == kFault || sceKernelGetSystemTimeLow() - start >= timeout_us)
      return false;
    sceKernelDelayThread(ME_POLL_US);
  }
  return true;
}

bool PspAudioMe_Init(SpcPlayer *player) {
  if (g_me_active)
    return true;
  if (!player || !player->dsp || ((uintptr_t)player & (ME_CACHE_LINE - 1)) ||
      ((uintptr_t)player->dsp & (ME_CACHE_LINE - 1)))
    return false;  // would share cache lines with unrelated heap data

  // Clear the mailbox through the UNCACHED alias only. A cached memset would leave
  // dirty lines that could later overwrite the ME's uncached stores.
  for (int i = 0; i < kShCount; i++)
    ME_SHARED[i] = 0;
  ME_SHARED[kShState] = kBooting;
  meLibSync();

  if (meLibDefaultInit() < 0)
    return false;
  if (!WaitForValue(kShProgress, 1, ME_BOOT_TIMEOUT_US))
    goto fail;

  g_me_player = player;
  CpuWritebackInvalidate(player, sizeof(*player));
  CpuWritebackInvalidate(player->dsp, sizeof(*player->dsp));
  ME_SHARED[kShPlayer] = (uint32_t)(uintptr_t)player;
  ME_SHARED[kShDsp] = (uint32_t)(uintptr_t)player->dsp;
  meLibSync();
  ME_SHARED[kShState] = kIdle;
  meLibSync();
  if (!WaitForValue(kShProgress, 2, ME_BOOT_TIMEOUT_US))
    goto fail;

  g_me_active = true;
  return true;
fail:
  ME_SHARED[kShState] = kStop;  // let the ME park itself if it is alive
  meLibSync();
  return false;
}

bool PspAudioMe_IsActive(void) { return g_me_active; }

bool PspAudioMe_Render(int16_t *output, int samples, int channels, const uint8_t ports[4]) {
  if (samples <= 0 || samples > ME_MAX_SAMPLES || channels != ME_CHANNELS || !g_me_active) {
    if (samples > 0 && channels > 0)
      memset(output, 0, (size_t)samples * channels * sizeof(*output));
    return false;
  }
  size_t bytes = (size_t)samples * ME_CHANNELS * sizeof(*output);

  if (!WaitForValue(kShState, kIdle, ME_JOB_TIMEOUT_US))
    goto fail;

  ME_SHARED[kShSamples] = (uint32_t)samples;
  ME_SHARED[kShPortsIn] = ports[0] | ports[1] << 8 | ports[2] << 16 | (uint32_t)ports[3] << 24;
  meLibSync();
  ME_SHARED[kShState] = kRun;
  meLibSync();

  if (!WaitForValue(kShState, kIdle, ME_JOB_TIMEOUT_US))
    goto fail;

  CpuInvalidate(g_me_output, bytes);  // output is only ever written by the ME
  memcpy(output, g_me_output, bytes);
  return true;

fail:
  if (ME_SHARED[kShState] == kFault) {
    // ME is dead: fall back to the CPU path. Drop stale CPU lines first.
    g_me_active = false;
    CpuInvalidate(g_me_player, sizeof(*g_me_player));
    CpuInvalidate(g_me_player->dsp, sizeof(*g_me_player->dsp));
  }
  memset(output, 0, bytes);
  return false;
}

void PspAudioMe_LockState(void) {
  if (!g_me_active)
    return;
  // Bounded: an ME that hangs without faulting must not freeze the game thread.
  uint32_t start = sceKernelGetSystemTimeLow();
  while (ME_SHARED[kShState] == kRun) {
    if (sceKernelGetSystemTimeLow() - start >= ME_JOB_TIMEOUT_US) {
      // Treat the ME as dead; the CPU audio path takes over. Its state is as of
      // the ME's last write-back.
      g_me_active = false;
      break;
    }
    sceKernelDelayThread(ME_POLL_US);
  }
  CpuInvalidate(g_me_player, sizeof(*g_me_player));
  CpuInvalidate(g_me_player->dsp, sizeof(*g_me_player->dsp));
}

void PspAudioMe_UnlockState(void) {
  if (!g_me_active)
    return;
  CpuWritebackInvalidate(g_me_player->dsp, sizeof(*g_me_player->dsp));
  CpuWritebackInvalidate(g_me_player, sizeof(*g_me_player));
  meLibSync();
}

void PspAudioMe_Shutdown(void) {
  if (!g_me_active)
    return;
  PspAudioMe_LockState();
  ME_SHARED[kShState] = kStop;
  meLibSync();
  WaitForValue(kShState, kHalted, ME_BOOT_TIMEOUT_US);
  g_me_active = false;
}
