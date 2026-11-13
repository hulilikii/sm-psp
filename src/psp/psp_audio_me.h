#pragma once
// SPC700 player + S-DSP running on the PSP Media Engine (ME), one MIST task per audio block.
// Uses the UNMODIFIED public API of psp-media-engine-mist (meMistInit/InjectSyscall/Trigger/Wait/
// Finish) and psp-media-engine-custom-core (me-core-mapper), both MIT.
//
// What the public MIST API can and cannot tell us (checked against the upstream source):
//  * meMistTrigger() always returns 0; it cannot report that the task started.
//  * meMistWait() is void. It polls the ME status word for ~500 x sceKernelDelayThread(1000)
//    and returns, with no way to tell "finished" from "gave up". It cannot be interrupted.
//  * There is no API to read the ME, to stop a task, or to prove the ME stopped.
// So the only completion evidence is a marker the ME task itself writes to game memory (with a
// sequence number) after its last access to game data, followed by meMistWait() as a barrier.
// A task without such a marker is "ambiguous": the ME may still own the shared memory forever.
#include <stdbool.h>
#include <stdint.h>

#include "../config.h"
#include "psp_log.h"

// ME audio statistics in the log: only when logging AND ME audio are both on
// (Makefile: PSP_LOG=1 PSP_AUDIO_ME=1 PSP_ME_LOG=1).
#ifndef SM_PSP_ME_LOG
#define SM_PSP_ME_LOG 0
#endif
#if SM_PSP_ME_LOG && SM_PSP_LOG && PSP_AUDIO_ME
#define PSP_ME_STATS 1
#else
#define PSP_ME_STATS 0
#endif

struct SpcPlayer;

// One audio block: 534 DSP samples resampled to 735 stereo frames at 44100 Hz (16.67 ms).
#define PSP_ME_BLOCK_FRAMES 735

typedef enum {
  PSPME_IDLE,               // no task in flight; the CPU may touch the SPC/DSP state
  PSPME_SUBMITTED,          // trigger issued, task not seen yet
  PSPME_RUNNING,            // the task reported that it started
  PSPME_COMPLETED,          // marker (sequence, status) seen and meMistWait() returned; PCM not yet queued
  PSPME_FAILED,             // the ME finished but refused the job (state untouched) or broke its stack canary
  PSPME_RECOVERY_REQUIRED,  // no marker in time: the ME may still own the state. Nothing is reused.
} PspAudioMeState;

typedef enum {
  PSPME_INIT_OK = 0,
  PSPME_INIT_BAD_ARGS,      // player/dsp missing or not 64-byte aligned
  PSPME_INIT_DISABLED,      // an earlier ping or recovery left ME-owned memory unresolved: ME audio is off for the session
  PSPME_INIT_NO_RESOURCES,  // producer thread could not be created
  PSPME_INIT_MIST,          // meMistInit() < 0: no CFW kernel loader, unsupported ME core (BL/SD/unknown), ...
  PSPME_INIT_UNSUPPORTED,   // mapper table lacks an entry the task needs
  PSPME_INIT_PING,          // the injected task did not report completion
  PSPME_INIT_UNRESOLVED,    // ownership not established (state still ME-owned / a task unresolved); ME audio stays off
  PSPME_INIT_PRODUCER_BUSY, // the previous producer thread has not ended yet; none is started (never two). CPU audio stays
} PspAudioMeInit;

typedef enum {
  PSPME_SUSPEND_NOT_ENGAGED, // the ME never owned the stream (or a refusal streak already gave it back)
  PSPME_SUSPEND_OK,          // producer parked, no task in flight, CPU owns state and resampler history
  PSPME_SUSPEND_UNRESOLVED,  // a task could not be confirmed finished: the ME keeps the stream (silent)
  PSPME_SUSPEND_ALREADY,     // a previous call returned OK and no Init has re-engaged since (duplicate power callback)
} PspAudioMeSuspend;

typedef enum {
  PSPME_SHUTDOWN_NOT_STARTED,
  // Producer joined and the last task was confirmed finished by its marker. NOT proven: that the ME stopped
  // executing this PRX (meMistFinish() text), that the injected syscall entry was restored (it is left in
  // place unless SM_PSP_ME_RESTORE_SYSCALL=1, and even then the original is unverified).
  PSPME_SHUTDOWN_QUIESCENT,
  // An ME task may still run, or the producer did not stop: the module's memory must stay mapped and the
  // caller must not exit (see SM_PSP_ME_PARK_ON_UNSAFE in main.c).
  PSPME_SHUTDOWN_UNSAFE,
} PspAudioMeShutdown;

typedef enum {
  PSPME_POP_BLOCK,           // out holds the next rendered block
  PSPME_POP_SILENCE,         // the ME owns the stream but no block is ready (or it is dead): out is silence
  PSPME_POP_CPU,             // ring empty and the CPU owns the stream: the caller renders the block
} PspAudioMePop;

// Boot and every resume: a full re-init (ME restart, reinjection, ping, fresh shared state, one new producer). Requires
// that the previous producer is gone and the CPU owns the state. Not with the APU lock held. On any failure the CPU keeps the audio path.
PspAudioMeInit PspAudioMe_Init(struct SpcPlayer *player);
// Power callback, before sleep. Returns the stream to the CPU and stops + joins the producer if (and only if) no task is unresolved. Blocks up to ~2 s.
PspAudioMeSuspend PspAudioMe_Suspend(void);
// After a sleep (main thread, not with the APU lock held). Re-runs Suspend first if the stream is still ME-owned (duplicate/missed/unresolved suspend), and only calls Init when the state is back with the CPU. A sleep reset of the ME is not accepted as proof that a task stopped.
PspAudioMeInit PspAudioMe_Resume(struct SpcPlayer *player);
// Exit paths (also Die). Idempotent. Blocks up to ~3 s. Never reports a "clean" ME: see PspAudioMeShutdown.
PspAudioMeShutdown PspAudioMe_Shutdown(void);

// True while the stream belongs to the ME (also when it is dead: the CPU must then stay away).
bool PspAudioMe_IsActive(void);
PspAudioMeState PspAudioMe_GetState(void);

// Audio callback side. Never blocks, never waits for the ME. frames must be PSP_ME_BLOCK_FRAMES.
PspAudioMePop PspAudioMe_PopBlock(int16_t *out, int frames);

// Called with the APU state lock held. LockState returns false when the SPC/DSP state must NOT be touched (ME ownership unresolved or ME state untrusted); the caller must skip its state access.
// Otherwise it drops stale CPU lines. UnlockState writes CPU changes back for the ME.
bool PspAudioMe_LockState(void);
void PspAudioMe_UnlockState(void);

// Writes one stats line to the log and resets the counters.
#if PSP_ME_STATS
void PspAudioMe_LogStats(void);
#else
#define PspAudioMe_LogStats() ((void)0)
#endif

// ---- provided by the game side (main.c / sm_rtl.c) ----
void RtlApuLockRaw(void);
bool RtlApuLockTimed(uint32_t timeout_us);     // false on timeout
void RtlApuUnlockRaw(void);
void RtlApuPopPortsToState(void);              // applies one queued frame of port writes to the SPC state; APU lock held
void RtlGetCpuRsHistory(int16_t *hist);        // DSP_RS_HISTORY_SAMPLES values
void RtlSetCpuRsHistory(const int16_t *hist);  // APU lock held
