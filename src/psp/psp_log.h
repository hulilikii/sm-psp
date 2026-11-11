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
#ifndef SM_PSP_VTX_POOL
#define SM_PSP_VTX_POOL 0
#endif
#ifndef SM_PSP_TRIPLE
#define SM_PSP_TRIPLE 1
#endif
#ifndef SM_PSP_PROBES
#define SM_PSP_PROBES 0
#endif
#ifndef SM_PSP_VRAM_DIRTY
#define SM_PSP_VRAM_DIRTY 1
#endif
#ifndef SM_PSP_CAP_REUSE
#define SM_PSP_CAP_REUSE 1
#endif
#ifndef SM_PSP_VTX16
#define SM_PSP_VTX16 1  /* Gate 5h-c: 10-byte BG/sprite vertices with 16-bit texture coordinates */
#endif
#ifndef SM_PSP_STATS
#define SM_PSP_STATS 1  /* Gate 5i-a: per-frame quad/run statistics in the log (log builds only) */
#endif
#if SM_PSP_LOG && SM_PSP_STATS
#define SM_PSP_S5I 1
#else
#define SM_PSP_S5I 0
#endif
#ifndef SM_PSP_CAP_WIN
#define SM_PSP_CAP_WIN 1  /* Gate 5k: window edge writes do not force a full capture of the line */
#endif
#ifndef SM_PSP_BENCH
#define SM_PSP_BENCH 0  /* Gate 5i-b: boot-time layer-image strip benchmark (log builds only), done */
#endif
#ifndef SM_PSP_LAYER_IMG
#define SM_PSP_LAYER_IMG 3  /* Gate 5l/5m: BG2/BG3 layers with many scroll runs are drawn from a cached image (2: + 32x64 maps, 3: + scan skip) */
#endif
#if SM_PSP_LOG && SM_PSP_BENCH
#define SM_PSP_BENCH_ON 1
#else
#define SM_PSP_BENCH_ON 0
#endif
#ifndef SM_PSP_BENCH_M7
#define SM_PSP_BENCH_M7 0  /* Gate 5r-a: boot-time GE-native mode 7 rotation benchmark (log builds only) */
#endif
#if SM_PSP_LOG && SM_PSP_BENCH_M7
#define SM_PSP_BENCH_M7_ON 1
#else
#define SM_PSP_BENCH_M7_ON 0
#endif
#define PSP_STR2(x) #x
#define PSP_STR(x) PSP_STR2(x)
#define PSP_LOG_FILE "smpsp_g5s2_vd" PSP_STR(SM_PSP_VRAM_DIRTY) "_cr" PSP_STR(SM_PSP_CAP_REUSE) "_cw" PSP_STR(SM_PSP_CAP_WIN) "_pr" PSP_STR(SM_PSP_PROBES) "_tb" PSP_STR(SM_PSP_TRIPLE) "_v16" PSP_STR(SM_PSP_VTX16) "_st" PSP_STR(SM_PSP_STATS) "_bn" PSP_STR(SM_PSP_BENCH) "_li" PSP_STR(SM_PSP_LAYER_IMG) ".log"

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
