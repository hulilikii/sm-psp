#include "psp_debug_log.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pspiofilemgr.h>

#ifndef PSP_DEBUG_BUILD_ID
#define PSP_DEBUG_BUILD_ID "unknown"
#endif

#define PSP_DEBUG_LOG_PATH \
  "ms0:/PSP/GAME/SMPSP/psp_scene_trace_" PSP_DEBUG_BUILD_ID ".txt"
#define PSP_DEBUG_LOG_PREV \
  "ms0:/PSP/GAME/SMPSP/psp_scene_trace_" PSP_DEBUG_BUILD_ID ".prev.txt"
#define PSP_DEBUG_LOG_MAX_BYTES (32 * 1024)

static bool g_psp_debug_log_enabled;
static bool g_psp_debug_log_started;

static void PspDebugLog_RollCurrent(void) {
  SceIoStat st;

  if (sceIoGetstat(PSP_DEBUG_LOG_PATH, &st) >= 0) {
    sceIoRemove(PSP_DEBUG_LOG_PREV);
    sceIoRename(PSP_DEBUG_LOG_PATH, PSP_DEBUG_LOG_PREV);
  }
}

static void PspDebugLog_WriteText(const char *text) {
  SceIoStat st;
  int len;
  int written;
  SceUID fd;

  if (!g_psp_debug_log_enabled || text == NULL)
    return;

  if (sceIoGetstat(PSP_DEBUG_LOG_PATH, &st) >= 0 &&
      st.st_size >= PSP_DEBUG_LOG_MAX_BYTES) {
    PspDebugLog_RollCurrent();
  }

  len = (int)strlen(text);
  if (len <= 0)
    return;

  fd = sceIoOpen(PSP_DEBUG_LOG_PATH,
                 PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

  if (fd < 0)
    return;

  written = 0;
  while (written < len) {
    int n = sceIoWrite(fd, text + written, len - written);
    if (n <= 0)
      break;
    written += n;
  }
  sceIoClose(fd);
}

static void PspDebugLog_WriteLine(uint32_t marker) {
  char line[32];
  int len = snprintf(line, sizeof(line), "%u\n", (unsigned int)marker);
  if (len <= 0 || len >= (int)sizeof(line))
    return;
  PspDebugLog_WriteText(line);
}

static void PspDebugLog_WriteBuildInfo(void) {
  char line[192];
  int len = snprintf(line, sizeof(line),
                     "BUILD_ID=%s\nBUILD_DATE=%s %s\n",
                     PSP_DEBUG_BUILD_ID, __DATE__, __TIME__);
  if (len > 0 && len < (int)sizeof(line))
    PspDebugLog_WriteText(line);
}

void PspDebugLog_Init(bool enabled) {
  g_psp_debug_log_enabled = false;
  g_psp_debug_log_started = false;
  if (enabled)
    PspDebugLog_SetEnabled(true);
}

void PspDebugLog_SetEnabled(bool enabled) {
  if (enabled == g_psp_debug_log_enabled)
    return;

  if (enabled) {
     /*
     * Roll only when starting this process's log. Re-enabling logging
     * must not move the startup trace away from the current file.
     */
    if (!g_psp_debug_log_started) {
      PspDebugLog_RollCurrent();
      g_psp_debug_log_started = true;
    }
    g_psp_debug_log_enabled = true;
     PspDebugLog_WriteBuildInfo();
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
 
void PspDebugValue(uint32_t tag, uint32_t value) {
  char line[48];
  int len = snprintf(line, sizeof(line), "%u=%u\n",
                     (unsigned int)tag, (unsigned int)value);
  if (len <= 0 || len >= (int)sizeof(line))
    return;
  PspDebugLog_WriteText(line);
}
