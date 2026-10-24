#pragma once

//Makefile flags:
// make PSP_LOG=0   -> no log file, no timing hooks, no diagnostics (release)
//make PSP_LOG=1   -> buffered log

#ifndef SM_PSP_LOG
#define SM_PSP_LOG 1
#endif
#ifndef SM_PSP_LOG_FLUSH_FRAMES
#define SM_PSP_LOG_FLUSH_FRAMES 1800
#endif

#ifndef SM_PSP_GE_STRIPS
#define SM_PSP_GE_STRIPS 0
#endif
#define PSP_STR2(x) #x
#define PSP_STR(x) PSP_STR2(x)
#define PSP_LOG_FILE "smpsp_g4d_strips" PSP_STR(SM_PSP_GE_STRIPS) ".log"

#include <stdbool.h>

#if SM_PSP_LOG
void PspLog_Init(void);
void PspLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void PspLog_Flush(void);

bool PspLog_Tick(unsigned frame);
#else
#define PspLog_Init() ((void)0)
#define PspLog(...) ((void)0)
#define PspLog_Flush() ((void)0)
#define PspLog_Tick(frame) ((void)(frame), false)
#endif
