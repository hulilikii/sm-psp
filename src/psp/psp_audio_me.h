#pragma once
// SPC700 player + S-DSP running on the PSP Media Engine (ME).
// Pattern adapted from the Zelda3 PSP port; uses psp-media-engine-custom-core (MIT).
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

// player and player->dsp must be 64-byte aligned and padded to 64 bytes.
// Returns false (and leaves the CPU path usable) on any failure.
bool PspAudioMe_Init(struct SpcPlayer *player);
void PspAudioMe_Shutdown(void);
bool PspAudioMe_IsActive(void);

// Render one SPC frame (534 DSP samples) resampled to `samples` stereo frames.
// ports[i] == 255 means "no new write". Caller must hold the APU state lock.
// samples <= 1024. On failure writes silence and returns false.
bool PspAudioMe_Render(int16_t *output, int samples, int channels, const uint8_t ports[4]);

// Called with the APU state lock held. Make CPU access to the SPC/DSP state
// coherent with the ME: wait for the ME to be idle and drop stale CPU cache
// lines (Lock); write CPU changes back so the ME sees them (Unlock).
void PspAudioMe_LockState(void);
void PspAudioMe_UnlockState(void);

// Writes one stats line to the log and resets the counters.
#if PSP_ME_STATS
void PspAudioMe_LogStats(void);
#else
#define PspAudioMe_LogStats() ((void)0)
#endif
