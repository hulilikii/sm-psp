#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../config.h"

#define PSP_MSU_BLOCK_FRAMES 735

#if PSP_AUDIO_ENABLED
bool PspMsu_Init(const char *app_dir);
void PspMsu_Resume(void);
bool PspMsu_Shutdown(void);
void PspMsu_PollDiagnostics(void);
bool PspMsu_HandleMusic(uint8_t requested, uint8_t current, uint8_t bank);
bool PspMsu_PopBlock(int16_t *dst, int frames, bool *stop_spc);
int PspMsu_GetVolume(void);
#else
static inline bool PspMsu_HandleMusic(
    uint8_t requested, uint8_t current, uint8_t bank) {
  (void)requested;
  (void)current;
  (void)bank;
  return false;
}
#endif
