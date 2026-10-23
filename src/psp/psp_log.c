#include "psp_log.h"

#if SM_PSP_LOG

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <pspiofilemgr.h>
#include <pspkernel.h>

enum { kLogBuf = 32 * 1024, kLineMax = 256 };

static char g_buf[kLogBuf];
static unsigned g_len;
static unsigned g_last_flush_frame;
static bool g_started;

static bool Append(int flags, const char *s, unsigned n) {
  SceUID fd = sceIoOpen(PSP_LOG_FILE, flags, 0777);
  if (fd < 0)
    return false;
  sceIoWrite(fd, s, n);
  sceIoClose(fd);  /* close commits the file size; there is no fsync */
  return true;
}

static unsigned g_note_bytes, g_note_us;  /* last flush, reported with the next real line */
static bool g_note_pending;

void PspLog_Flush(void) {
  if (g_len == 0)
    return;
  unsigned t0 = sceKernelGetSystemTimeLow();
  unsigned n = g_len;
  g_len = 0;
  if (Append(PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, g_buf, n)) {
    g_note_bytes = n;
    g_note_us = sceKernelGetSystemTimeLow() - t0;
    g_note_pending = true;
  }
}

void PspLog_Init(void) {
  char hdr[128];
  int n = snprintf(hdr, sizeof(hdr), "smpsp me_audio build %s %s (log flush every %d frames; 0 = every line)\n",
                   __DATE__, __TIME__, (int)SM_PSP_LOG_FLUSH_FRAMES);
  if (n > 0)
    Append(PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, hdr, (unsigned)n);
  g_len = 0;
  g_started = true;
}

void PspLog(const char *fmt, ...) {
  char line[kLineMax];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  if (n > (int)sizeof(line) - 2)
    n = (int)sizeof(line) - 2;
  line[n++] = '\n';
  if (g_note_pending) {  /* report the previous flush without refilling an empty buffer by itself */
    g_note_pending = false;
    char note[64];
    int m = snprintf(note, sizeof(note), "(previous log flush: %u bytes, %u us)\n", g_note_bytes, g_note_us);
    if (m > 0 && g_len + (unsigned)m < kLogBuf) {
      memcpy(g_buf + g_len, note, (unsigned)m);
      g_len += (unsigned)m;
    }
  }
  if (g_len + (unsigned)n > kLogBuf)
    PspLog_Flush();
  if (g_len + (unsigned)n > kLogBuf)
    return;
  memcpy(g_buf + g_len, line, (unsigned)n);
  g_len += (unsigned)n;
  if (SM_PSP_LOG_FLUSH_FRAMES == 0 || !g_started)
    PspLog_Flush();
}

bool PspLog_Tick(unsigned frame) {
  if (SM_PSP_LOG_FLUSH_FRAMES == 0 || g_len == 0)
    return false;
  if (frame - g_last_flush_frame < (unsigned)SM_PSP_LOG_FLUSH_FRAMES &&
      g_len < kLogBuf * 3 / 4)
    return false;
  g_last_flush_frame = frame;
  PspLog_Flush();
  return true;
}

#endif /* SM_PSP_LOG */
