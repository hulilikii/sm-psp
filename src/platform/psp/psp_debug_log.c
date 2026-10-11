#include "psp_debug_log.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include <pspiofilemgr.h>

#define PSP_DEBUG_LOG_PATH "ms0:/PSP/GAME/SMPSP/psp_scene_trace.txt"
#define PSP_DEBUG_LOG_PREV "ms0:/PSP/GAME/SMPSP/psp_scene_trace.prev.txt"
#define PSP_DEBUG_LOG_MAX_BYTES (32 * 1024)

static bool g_psp_debug_log_enabled;

static void PspDebugLog_RollCurrent(void) {
  SceIoStat st;

  if (sceIoGetstat(PSP_DEBUG_LOG_PATH, &st) >= 0) {
    sceIoRemove(PSP_DEBUG_LOG_PREV);
    sceIoRename(PSP_DEBUG_LOG_PATH, PSP_DEBUG_LOG_PREV);
  }
}

static void PspDebugLog_WriteLine(uint32_t marker) {
  SceIoStat st;
  char line[32];
  int len;
  SceUID fd;

  if (!g_psp_debug_log_enabled)
    return;

  if (sceIoGetstat(PSP_DEBUG_LOG_PATH, &st) >= 0 &&
      st.st_size >= PSP_DEBUG_LOG_MAX_BYTES) {
    PspDebugLog_RollCurrent();
  }

  len = snprintf(line, sizeof(line), "%u\n", (unsigned int)marker);
  if (len <= 0 || len >= (int)sizeof(line))
    return;

  fd = sceIoOpen(PSP_DEBUG_LOG_PATH,
                 PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

  if (fd >= 0) {
    sceIoWrite(fd, line, len);
    sceIoClose(fd);
  }
}

void PspDebugLog_Init(bool enabled) {
  g_psp_debug_log_enabled = false;
  if (enabled)
    PspDebugLog_SetEnabled(true);
}

void PspDebugLog_SetEnabled(bool enabled) {
  if (enabled == g_psp_debug_log_enabled)
    return;

  if (enabled) {
    /* Preserve the previous run and start a fresh current log. */
    PspDebugLog_RollCurrent();
    g_psp_debug_log_enabled = true;
    PspDebugLog_WriteLine(9001); /* CIRCLE_START */
    PspDebugLog_WriteLine(700001u); /* LOGGING_STARTED_AT_CIRCLE */
  } else {
    PspDebugLog_WriteLine(9000); /* CIRCLE_STOP */
    g_psp_debug_log_enabled = false;
  }
}

bool PspDebugLog_IsEnabled(void) {
  return g_psp_debug_log_enabled;
}

void PspDebugMarker(uint32_t marker) {
  PspDebugLog_WriteLine(marker);
}
