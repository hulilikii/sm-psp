#include "psp_audio_me.h"

#include "../spc_player.h"
#include "../sm_rtl.h"
#include "psp_audio_msu.h"

#include <psptypes.h>
#include <me-core-mapper/me-core-mapper.h>  // meCoreDcache* (ME core functions), meCoreSystemTable
#include <me-mist/me-mist.h>
#include <pspkernel.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Design (see psp_audio_me.h for the limits of the MIST API this relies on)
//  * One MIST task per block: SpcPlayer_GenerateSamples() then dsp_getSamplesHQ(), as on the CPU.
//  * A producer thread owns every ME interaction after Init. It holds the APU lock (g_audio_sema) from
//    "ports applied" until the job is resolved, so the game thread never touches SPC/DSP state while the
//    ME may.
//  * A new task is triggered only from IDLE. A task is finished only when its result marker carries the
//    current sequence number AND meMistWait() has returned. No marker by the deadline: RECOVERY_REQUIRED.
//    The producer then drops the APU lock (the game thread must not stall) but LockState() refuses state
//    access, nothing is freed or reused, and no new task is triggered. If the marker shows up later the
//    job is finished normally (its PCM is published, no state is lost). Otherwise the ME audio stays
//    silent for the session: the CPU never renders from state the ME may still be writing.
//  * There is NO rollback and NO checkpoint: the state is only ever either "after N whole jobs" or
//    untouchable. Port writes are applied to the SPC state by the CPU before the trigger, so they are
//    part of the state and cannot be lost or reordered by a failed job.
//  * The audio callback pops finished blocks from a single-producer/single-consumer ring.
// ---------------------------------------------------------------------------

#define ME_CACHE_LINE 64
#define ME_FRAMES PSP_ME_BLOCK_FRAMES
#define ME_CHANNELS 2
#define ME_MIST_SYSCALL 13  // the index upstream's samples use; unverified against firmware tables (see notes)
#define ME_RING_BLOCKS 3
#define ME_HIST_SAMPLES DSP_RS_HISTORY_SAMPLES
#ifndef ME_POLL_US
#define ME_POLL_US 200
#endif
#ifndef ME_JOB_DEADLINE_US
#define ME_JOB_DEADLINE_US 100000      // ~6 blocks; a render normally takes a few ms
#endif
#ifndef ME_PING_DEADLINE_US
#define ME_PING_DEADLINE_US 200000
#endif
#ifndef ME_RECOVER_POLL_US
#define ME_RECOVER_POLL_US 5000
#endif
#ifndef ME_SUSPEND_WAIT_US
#define ME_SUSPEND_WAIT_US 1500000
#endif
#ifndef ME_SHUTDOWN_WAIT_US
#define ME_SHUTDOWN_WAIT_US 3000000
#endif
#ifndef ME_FIRST_BLOCK_WAIT_US
#define ME_FIRST_BLOCK_WAIT_US 100000
#endif
#define ME_PARK_POLL_US 5000
#define ME_PRODUCER_PRIO 0x14         // below the audio thread (0x12), above the main thread (0x20)
#define ME_PRODUCER_STACK 0x4000
#define ME_MAX_REFUSALS 3
#ifndef SM_PSP_ME_PRIVATE_STACK
#define SM_PSP_ME_PRIVATE_STACK 1     // run the SPC/DSP code on our own stack: the firmware's size is unknown
#endif
#ifndef SM_PSP_ME_RESTORE_SYSCALL
#define SM_PSP_ME_RESTORE_SYSCALL 1   // write the mapper's original address back into the injected entry at exit
#endif
#define ME_STACK_BYTES 8192
#define ME_STACK_CANARY_BYTES 256
#define ME_STACK_FILL 0xA5A5A5A5u
// Entries of meCoreSystemTable the ME task calls through (me-core-mapper: sysCall(65..67)).
#define ME_TBL_DCACHE_WBINV 65
#define ME_TBL_DCACHE_INV 66
#define ME_TBL_DCACHE_WB 67
#ifdef __mips__
#define ME_SYNC() __asm__ volatile("sync" ::: "memory")
#else
#define ME_SYNC() __sync_synchronize()  // host tests only
#endif
#define ME_BARRIER() __asm__ volatile("" ::: "memory")  // compiler barrier: both ring threads run on the main CPU
#define ME_PAD(x) (((size_t)(x) + ME_CACHE_LINE - 1) & ~(size_t)(ME_CACHE_LINE - 1))
#define ME_PLAYER_BYTES ME_PAD(sizeof(SpcPlayer))  // players are allocated padded to whole lines (spc_player.c)
#define ME_DSP_BYTES ME_PAD(sizeof(Dsp))           // so are DSPs (dsp.c)
#define ME_OUT_BYTES (ME_FRAMES * ME_CHANNELS * sizeof(int16_t))

enum { kJobPing = 1, kJobRender = 2 };
enum { kStatusOk = 0x600d, kStatusPing = 0x70696e67, kStatusStack = 0x57ac };

#if PSP_ME_STATS
// Diagnostics only. Unsynchronised on purpose: an occasional lost update in a counter is harmless.
typedef struct { uint32_t n, sum, max; } MeStat;
static struct {
  MeStat job;   // trigger -> marker seen (us)
  MeStat fin;   // marker seen -> meMistWait returned (us)
  uint32_t blocks, underruns, ambiguous, late_ok, refused, stack_bad, pcm_dropped;
  uint32_t stack_max;
} g_st;
static inline void StatAdd(MeStat *s, uint32_t v) {
  s->n++;
  s->sum += v;
  if (v > s->max)
    s->max = v;
}
#define ME_STAT_T(var) uint32_t var = sceKernelGetSystemTimeLow()
#define ME_STAT(code) code
#else
#define ME_STAT_T(var) ((void)0)
#define ME_STAT(code) ((void)0)
#endif

// ---- shared structures (CPU <-> ME). Every one owns whole cache lines. ----
// Job: written by the CPU while the ME is idle, written back before each trigger, invalidated by the ME on entry.
typedef struct MeJob {
  uint32_t cmd, seq;
  SpcPlayer *player;
  Dsp *dsp;
  const int16_t *pcm;
  uint32_t pcm_volume;
} MeJob;
// Result: written by the ME (marker last), written back by the ME; the CPU invalidates before every read and
// never writes it while a task may run.
typedef struct MeResult {
  volatile uint32_t started_seq, done_seq, status, stack_used;
} MeResult;
typedef union { MeJob v; uint8_t raw[ME_CACHE_LINE]; } MeJobLine;
typedef union { MeResult v; uint8_t raw[ME_CACHE_LINE]; } MeResultLine;
static MeJobLine g_job __attribute__((aligned(ME_CACHE_LINE)));
static MeResultLine g_result __attribute__((aligned(ME_CACHE_LINE)));
// PCM of the ME: written by the ME only, invalidated by the CPU before it reads. Padded to whole lines.
#define ME_OUT_PAD_SAMPLES ((int)(ME_PAD(ME_OUT_BYTES) / sizeof(int16_t)))
static int16_t g_me_output[ME_OUT_PAD_SAMPLES] __attribute__((aligned(ME_CACHE_LINE)));

static int16_t g_me_pcm[ME_OUT_PAD_SAMPLES]    __attribute__((aligned(ME_CACHE_LINE)));
// Resampler workspace: the first DSP_RS_HISTORY_SAMPLES are the history carried between blocks. The ME
// invalidates the whole buffer first and writes it back last, so RAM always holds the history.
#define ME_RS_PAD ((DSP_RS_WORK_SAMPLES + 31) & ~31)  // int16 count, multiple of 32 = 64 bytes
static int16_t g_me_rs_work[ME_RS_PAD] __attribute__((aligned(ME_CACHE_LINE)));
// Private ME stack (see MeCallOnStack). Painted by the CPU while the ME is idle, scanned by the ME.
static uint32_t g_me_stack[ME_STACK_BYTES / 4] __attribute__((aligned(ME_CACHE_LINE)));

// ---- CPU-only state ----
static SpcPlayer *g_me_player;
static volatile PspAudioMeState g_state = PSPME_IDLE;
static volatile bool g_engaged;    // the stream belongs to the ME (set under the APU lock)
static volatile bool g_accepting;  // the producer may start new jobs
static volatile bool g_stop;       // the producer must exit
static volatile bool g_unknown;    // RECOVERY_REQUIRED: ME ownership of the state is unresolved (changed under the APU lock)
static volatile bool g_fatal;      // the ME finished a job but its output/state cannot be trusted (stack canary)
static volatile bool g_pending;    // a rendered block waits in g_me_output for ring space
static bool g_disabled;            // ME audio is off for the session (unresolved ping)
static bool g_injected;
static SceUID g_thread = -1;
static uint32_t g_seq, g_outstanding;
static uint32_t g_refusals;
static PspAudioMeShutdown g_shutdown_result = PSPME_SHUTDOWN_NOT_STARTED;
static int16_t g_ring[ME_RING_BLOCKS][ME_FRAMES * ME_CHANNELS] __attribute__((aligned(ME_CACHE_LINE)));
static volatile uint32_t g_ring_w, g_ring_r;  // free-running: the producer owns w, the audio callback owns r

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
// Everything from here to the CPU-side marker runs on the Media Engine inside the firmware's syscall
// handler: no libc, no syscalls, no printf/assert. Reachable code (audited): SpcPlayer_GenerateSamples,
// dsp_getSamplesHQ and what they call, all free of libc calls (dsp.c/spc_player.c were changed for this).
// It may only touch the job, the result, the SPC/DSP state and the buffers above, and must leave through
// meMistFinish().

#if SM_PSP_ME_PRIVATE_STACK && defined(__mips__)
// Runs fn(arg) with $sp on g_me_stack, then restores the firmware's $sp. The old $sp lives in $s0
// (callee-saved by the callee; declared clobbered so the compiler preserves its own value).
// O32: top is 8-byte aligned and keeps the 16-byte argument home area above $sp inside the buffer.
// UNVERIFIED on the PSP toolchain: see the disassembly check in the patch notes.
static void MeCallOnStack(void (*fn)(MeJob *), MeJob *arg) {
  void *top = (void *)((uintptr_t)g_me_stack + ME_STACK_BYTES - 16);
  __asm__ volatile(
      ".set push\n\t"
      ".set noreorder\n\t"
      "move $s0, $sp\n\t"
      "move $sp, %2\n\t"
      "jalr %0\n\t"
      "move $a0, %1\n\t"
      "move $sp, $s0\n\t"
      ".set pop\n\t"
      :
      : "r"(fn), "r"(arg), "r"(top)
      : "$16" /* s0 */, "$31" /* ra */, "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12", "$13",
        "$14", "$15", "$24", "$25", /* v0-v1, a0-a3, t0-t9: caller-saved, the callee may use them */
        "hi", "lo", "memory");
}
#else
static void MeCallOnStack(void (*fn)(MeJob *), MeJob *arg) {  // host tests / SM_PSP_ME_PRIVATE_STACK=0: firmware stack
  fn(arg);
}
#endif

// Bytes of g_me_stack touched so far (high-water mark) and whether the lowest ME_STACK_CANARY_BYTES are intact.
static uint32_t MeStackUsed(bool *canary_ok) {
  uint32_t i = 0;
  while (i < ME_STACK_BYTES / 4 && g_me_stack[i] == ME_STACK_FILL)
    i++;
  *canary_ok = i * 4 >= ME_STACK_CANARY_BYTES;
  return ME_STACK_BYTES - i * 4;
}

static void MePingWork(MeJob *job) {
  volatile uint32_t probe[32];  // touches the private stack so Init can prove the switch worked
  for (uint32_t i = 0; i < 32; i++)
    probe[i] = i ^ job->seq;
  (void)probe[31];
}

static void MeRenderWork(MeJob *job) {
  SpcPlayer *player = job->player;
  Dsp *dsp = job->dsp;

  // Drop whatever the ME cached last job; the CPU changed the state (ports) and may have loaded a save.
  MeInvalidate(player, ME_PLAYER_BYTES);
  MeInvalidate(dsp, ME_DSP_BYTES);
  MeInvalidate(g_me_rs_work, sizeof(g_me_rs_work));

  SpcPlayer_GenerateSamples(player);
  dsp_getSamplesHQ(dsp, g_me_output, ME_FRAMES, g_me_rs_work);
  
  if (job->pcm && job->pcm_volume) {
    MeInvalidate(job->pcm, sizeof(g_me_pcm));

    for (int i = 0; i < ME_FRAMES * ME_CHANNELS; i++) {
      int32_t mixed = (int32_t)g_me_output[i] +
          ((int32_t)job->pcm[i] * (int32_t)job->pcm_volume) / 100;

      if (mixed > 32767) mixed = 32767;
      if (mixed < -32768) mixed = -32768;
      g_me_output[i] = (int16_t)mixed;
    }
  }

  // Publish everything the job wrote (whole padded allocations).
  MeWriteback(g_me_output, sizeof(g_me_output));
  MeWriteback(g_me_rs_work, sizeof(g_me_rs_work));
  MeWriteback(dsp, ME_DSP_BYTES);
  MeWriteback(player, ME_PLAYER_BYTES);
}

static int MeAudioTask(int index, void *param) {
  (void)index;
  if (param != (void *)&g_job)  // not our trigger: touch nothing, write no marker
    return meMistFinish();
  MeInvalidate(&g_job, sizeof(g_job));
  const uint32_t seq = g_job.v.seq, cmd = g_job.v.cmd;
  // A job whose sequence this task already completed is never run twice (spurious second dispatch).
  if (seq != 0 && seq != g_result.v.done_seq && (cmd == kJobPing || cmd == kJobRender)) {
    uint32_t status = 0, stack_used = 0;
    g_result.v.started_seq = seq;  // lets the CPU tell SUBMITTED from RUNNING
    MeWriteback(&g_result, sizeof(g_result));
    ME_SYNC();
    if (cmd == kJobPing) {
      MeCallOnStack(MePingWork, &g_job.v);
      status = kStatusPing;
    } else if (g_job.v.player && g_job.v.dsp) {
      MeCallOnStack(MeRenderWork, &g_job.v);
      status = kStatusOk;
    }
    bool canary_ok = true;
    stack_used = MeStackUsed(&canary_ok);
    if (!canary_ok)
      status = kStatusStack;
    // The marker goes last: a matching done_seq means everything above is in RAM.
    ME_SYNC();
    g_result.v.status = status;
    g_result.v.stack_used = stack_used;
    ME_SYNC();
    g_result.v.done_seq = seq;
    MeWriteback(&g_result, sizeof(g_result));
    ME_SYNC();
  }
  return meMistFinish();
}

#ifdef SM_PSP_ME_HOST_TEST
int (*const PspAudioMe_TestTask)(int, void *) = MeAudioTask;
uint32_t *PspAudioMe_TestStack(void) { return g_me_stack; }
void *PspAudioMe_TestJob(void) { return &g_job; }
#endif

// --------------------------------------------------------------- CPU side
typedef enum { kOutOk, kOutRefused, kOutStack, kOutAmbiguous } JobOutcome;

static uint32_t NextSeq(void) {
  if (++g_seq == 0)
    g_seq = 1;
  return g_seq;
}

static inline volatile MeResult *Res(void) { return &g_result.v; }

// Marker for seq is in g_result: use meMistWait() as the barrier for the ME leaving the task, then validate
// sequence and status. The marker proves the task made its last access to game DATA; the barrier is
// best-effort for the few instructions of meMistFinish() still to come (they live in this PRX's text).
static JobOutcome FinishJob(uint32_t cmd, uint32_t seq) {
  ME_STAT_T(t1);
  meMistWait();  // bounded by the library (~0.5 s), returns void: result is judged from the marker only
  ME_STAT(StatAdd(&g_st.fin, sceKernelGetSystemTimeLow() - t1));
  CpuInvalidate(&g_result, sizeof(g_result));
  if (Res()->done_seq != seq) {  // cannot change once seen, but never trust a single read
    g_state = PSPME_RECOVERY_REQUIRED;
    return kOutAmbiguous;
  }
  const uint32_t status = Res()->status;
  ME_STAT(if (Res()->stack_used > g_st.stack_max) g_st.stack_max = Res()->stack_used);
  if (status == kStatusStack) {
    g_state = PSPME_FAILED;
    ME_STAT(g_st.stack_bad++);
    return kOutStack;
  }
  if (status != (cmd == kJobPing ? (uint32_t)kStatusPing : (uint32_t)kStatusOk)) {
    g_state = PSPME_FAILED;  // the ME ran to the end without doing the job: state untouched
    ME_STAT(g_st.refused++);
    return kOutRefused;
  }
  g_state = PSPME_COMPLETED;
  return kOutOk;
}

// g_state is IDLE on entry; the caller holds the APU lock when a render job is submitted.
static JobOutcome RunJob(uint32_t cmd, uint32_t deadline_us) {
  const uint32_t seq = NextSeq();
  g_outstanding = seq;
  g_job.v.cmd = cmd;
  g_job.v.seq = seq;
  CpuWritebackInvalidate(&g_job, sizeof(g_job));
  g_state = PSPME_SUBMITTED;

  MistTrigger trigger = { ME_MIST_SYSCALL, &g_job };
  if (meMistTrigger(&trigger) != 0) {  // the library never returns non-zero; if it ever does, assume nothing
    g_state = PSPME_RECOVERY_REQUIRED;
    ME_STAT(g_st.ambiguous++);
    return kOutAmbiguous;
  }
  ME_STAT_T(t0);
  const uint32_t t_start = sceKernelGetSystemTimeLow();
  for (;;) {
    CpuInvalidate(&g_result, sizeof(g_result));
    if (Res()->started_seq == seq && g_state == PSPME_SUBMITTED)
      g_state = PSPME_RUNNING;
    if (Res()->done_seq == seq)
      break;
    if (sceKernelGetSystemTimeLow() - t_start >= deadline_us) {
      g_state = PSPME_RECOVERY_REQUIRED;
      ME_STAT(g_st.ambiguous++);
      return kOutAmbiguous;
    }
    sceKernelDelayThread(ME_POLL_US);
  }
  ME_STAT(StatAdd(&g_st.job, sceKernelGetSystemTimeLow() - t0));
  return FinishJob(cmd, seq);
}

static void PaintSharedBuffers(void) {
  // The ME is freshly restarted and idle here: CPU writes cannot race it.
  memset(&g_job, 0, sizeof(g_job));
  memset(&g_result, 0, sizeof(g_result));
  for (size_t i = 0; i < ME_STACK_BYTES / 4; i++)
    g_me_stack[i] = ME_STACK_FILL;
  g_job.v.player = g_me_player;
  g_job.v.dsp = g_me_player->dsp;
  CpuWritebackInvalidate(&g_job, sizeof(g_job));
  CpuWritebackInvalidate(&g_result, sizeof(g_result));
  CpuWritebackInvalidate(g_me_stack, sizeof(g_me_stack));
  g_seq = g_outstanding = 0;
  g_state = PSPME_IDLE;
}

// ---- ring (single producer: the producer thread; single consumer: the audio callback) ----
static uint32_t RingFree(void) { return ME_RING_BLOCKS - (g_ring_w - g_ring_r); }

// g_pending: the CPU owns g_me_output (the ME is not running). Queue it if there is room. Takes the APU lock
// so that Suspend (which also changes g_pending) never races it.
static bool PublishPending(void) {
  if (!g_pending)
    return true;
  if (RingFree() == 0)
    return false;
  bool ok = false;
  RtlApuLockRaw();
  if (g_pending && RingFree() != 0) {
    const uint32_t w = g_ring_w;
    memcpy(g_ring[w % ME_RING_BLOCKS], g_me_output, ME_OUT_BYTES);
    ME_BARRIER();
    g_ring_w = w + 1;
    g_pending = false;
    g_state = PSPME_IDLE;
    ME_STAT(g_st.blocks++);
    ok = true;
  }
  RtlApuUnlockRaw();
  return ok;
}

// A render job completed (marker + barrier). APU lock held. Takes the ME's output and state view.
static void AcceptRender(void) {
  CpuInvalidate(g_me_player, ME_PLAYER_BYTES);
  CpuInvalidate(g_me_player->dsp, ME_DSP_BYTES);
  CpuInvalidate(g_me_output, sizeof(g_me_output));
  g_pending = true;
  g_refusals = 0;
}

// APU lock held, state IDLE, nothing unresolved: hand the stream (and its resampler history) to the CPU.
static void Disengage_Locked(void) {
  CpuInvalidate(g_me_player, ME_PLAYER_BYTES);
  CpuInvalidate(g_me_player->dsp, ME_DSP_BYTES);
  CpuInvalidate(g_me_rs_work, sizeof(g_me_rs_work));
  RtlSetCpuRsHistory(g_me_rs_work);
  g_engaged = false;
  g_accepting = false;
}

static void ProduceOne(void) {
  RtlApuLockRaw();
  if (g_stop || !g_accepting || !g_engaged || g_unknown || g_fatal) {
    RtlApuUnlockRaw();
    return;
  }
  RtlApuPopPortsToState();  // CPU writes the SPC state: the ports are part of it from here on

  bool stop_spc = false;
  g_job.v.pcm = NULL;
  g_job.v.pcm_volume = 0;

  if (PspMsu_PopBlock(g_me_pcm, ME_FRAMES, &stop_spc)) {
    if (stop_spc)
      RtlApuStopMusic_Locked();

    CpuWritebackInvalidate(g_me_pcm, sizeof(g_me_pcm));
    g_job.v.pcm = g_me_pcm;
    g_job.v.pcm_volume = (uint32_t)PspMsu_GetVolume();
  }

  CpuWritebackInvalidate(g_me_player, ME_PLAYER_BYTES);
  CpuWritebackInvalidate(g_me_player->dsp, ME_DSP_BYTES);
  const JobOutcome o = RunJob(kJobRender, ME_JOB_DEADLINE_US);
  switch (o) {
  case kOutOk:
    AcceptRender();
    break;
  case kOutRefused:
    // The ME finished without touching anything (marker seen): the state is intact, CPU ownership is sound.
    g_state = PSPME_IDLE;
    if (++g_refusals >= ME_MAX_REFUSALS)
      Disengage_Locked();
    break;
  case kOutStack:
    g_fatal = true;  // memory below the stack may have been overwritten: no PCM, no CPU fallback, no more jobs
    break;
  case kOutAmbiguous:
    g_unknown = true;  // keep g_state = RECOVERY_REQUIRED; drop the lock so the game thread is not stalled
    break;
  }
  RtlApuUnlockRaw();
}

// RECOVERY_REQUIRED, no APU lock held. Looks for the late marker of g_outstanding; if it appears the job
// is finished exactly like a normal one. Never triggers anything.
static void RecoveryStep(void) {
  CpuInvalidate(&g_result, sizeof(g_result));
  if (Res()->done_seq != g_outstanding)
    return;
  RtlApuLockRaw();
  const JobOutcome o = FinishJob(g_job.v.cmd, g_outstanding);
  if (o == kOutOk) {
    AcceptRender();
    g_unknown = false;
    ME_STAT(g_st.late_ok++);
  } else if (o == kOutRefused) {
    g_state = PSPME_IDLE;
    g_unknown = false;
  } else if (o == kOutStack) {
    g_fatal = true;
    g_unknown = false;
  }
  RtlApuUnlockRaw();
}

static int ProducerThread(SceSize args, void *argp) {
  (void)args; (void)argp;
  while (!g_stop) {
    if (g_unknown) {
      RecoveryStep();
      sceKernelDelayThread(ME_RECOVER_POLL_US);
      continue;
    }
    if (!PublishPending()) {
      sceKernelDelayThread(1000);
      continue;
    }
    if (!g_accepting || g_fatal || !g_engaged) {
      sceKernelDelayThread(ME_PARK_POLL_US);
      continue;
    }
    if (RingFree() == 0) {
      sceKernelDelayThread(1000);
      continue;
    }
    ProduceOne();
  }
  return 0;
}

// ---- syscall entry lifetime ----
// MIST has no API to remove an injected entry or to read the original one. The only restore possible with
// the public API is to inject the address the mapper lists for this index (me-core-mapper's table mirrors
// the firmware's syscall table: assumption, NOT verified in code).
static bool RestoreSyscallEntry(void) {
#if SM_PSP_ME_RESTORE_SYSCALL
  if (!g_injected)
    return true;
  const u32 off = meCoreSystemTable ? meCoreSystemTable[ME_MIST_SYSCALL] : 0;
  if (off == 0)
    return false;
  MistInjector injector = { ME_MIST_SYSCALL, CACHED_KERNEL_MASK | (ME_CORE_BASE_ADDR + off) };
  meMistInjectSyscall(&injector);
  g_injected = false;
  return true;
#else
  return false;
#endif
}

PspAudioMeInit PspAudioMe_Init(SpcPlayer *player) {
  if (g_disabled || g_unknown || g_fatal)
    return PSPME_INIT_DISABLED;
  if (g_engaged)
    return PSPME_INIT_OK;
  if (!player || !player->dsp || ((uintptr_t)player & (ME_CACHE_LINE - 1)) ||
      ((uintptr_t)player->dsp & (ME_CACHE_LINE - 1)))
    return PSPME_INIT_BAD_ARGS;  // would share cache lines with unrelated heap data
  g_me_player = player;

  if (g_thread < 0) {
    g_stop = false;
    g_accepting = false;
    g_thread = sceKernelCreateThread("sm-me-audio", ProducerThread, ME_PRODUCER_PRIO, ME_PRODUCER_STACK,
                                     PSP_THREAD_ATTR_USER, NULL);
    if (g_thread < 0) {
      g_thread = -1;
      return PSPME_INIT_NO_RESOURCES;
    }
    if (sceKernelStartThread(g_thread, 0, NULL) < 0) {
      sceKernelDeleteThread(g_thread);
      g_thread = -1;
      return PSPME_INIT_NO_RESOURCES;
    }
  }

  // No ME task can be in flight here (boot, or resume after a suspend that confirmed idle): meMistInit()
  // restarts the ME core, so it must never run while a task may be live.
  if (meMistInit() < 0)
    return PSPME_INIT_MIST;  // BL/SD/unknown core, kcall.prx could not be written/loaded, ...
  // The ME reads these through the cache: make the CPU's selected table pointer visible in RAM. The
  // table itself is const data that the CPU never dirties.
  if (!meCoreSystemTable || !meCoreSystemTable[ME_TBL_DCACHE_WBINV] || !meCoreSystemTable[ME_TBL_DCACHE_INV] ||
      !meCoreSystemTable[ME_TBL_DCACHE_WB] || !meCoreSystemTable[ME_MIST_SYSCALL])
    return PSPME_INIT_UNSUPPORTED;  // fat-model table has holes; ME unused so far
  CpuWritebackInvalidate((const void *)&meCoreSystemTable, sizeof(meCoreSystemTable));

  PaintSharedBuffers();
  MistInjector injector = { ME_MIST_SYSCALL, CACHED_KERNEL_MASK | (u32)MeAudioTask };
  meMistInjectSyscall(&injector);  // void: success is only provable by running the task
  g_injected = true;

  // Ping: proves the injection took, the private stack works and the marker protocol runs. It touches no
  // SPC/DSP state, so the CPU path stays sound whatever happens to it.
  if (RunJob(kJobPing, ME_PING_DEADLINE_US) != kOutOk) {
    if (g_state == PSPME_RECOVERY_REQUIRED) {
      g_disabled = true;  // job/result/stack stay reserved: never reused while the ME might still write them
      RestoreSyscallEntry();
    } else {
      g_state = PSPME_IDLE;
    }
    return PSPME_INIT_PING;
  }
  g_state = PSPME_IDLE;

  // Hand the stream to the ME. The CPU path's resampler history moves into the ME's workspace.
  RtlApuLockRaw();
  int16_t hist[ME_HIST_SAMPLES];
  RtlGetCpuRsHistory(hist);
  memset(g_me_rs_work, 0, sizeof(g_me_rs_work));
  memcpy(g_me_rs_work, hist, sizeof(hist));
  CpuWritebackInvalidate(g_me_rs_work, sizeof(g_me_rs_work));
  CpuWritebackInvalidate(g_me_player, ME_PLAYER_BYTES);
  CpuWritebackInvalidate(g_me_player->dsp, ME_DSP_BYTES);
  g_refusals = 0;
  g_engaged = true;
  g_accepting = true;
  RtlApuUnlockRaw();

  const uint32_t t0 = sceKernelGetSystemTimeLow();  // best effort: give the callback a block to start with
  while (g_ring_w == g_ring_r && !g_unknown && !g_fatal && sceKernelGetSystemTimeLow() - t0 < ME_FIRST_BLOCK_WAIT_US)
    sceKernelDelayThread(500);
  return PSPME_INIT_OK;
}

PspAudioMeSuspend PspAudioMe_Suspend(void) {
  if (!g_engaged)
    return PSPME_SUSPEND_NOT_ENGAGED;
  g_accepting = false;  // no job starts after the one in flight (the producer re-checks under the lock)
  const uint32_t t0 = sceKernelGetSystemTimeLow();
  // A block that is rendered but not yet queued still belongs to the stream: wait for the callback to make room.
  while (g_pending && !g_unknown && sceKernelGetSystemTimeLow() - t0 < ME_SUSPEND_WAIT_US / 4)
    sceKernelDelayThread(2000);
  const uint32_t used = sceKernelGetSystemTimeLow() - t0;
  const uint32_t left = used < ME_SUSPEND_WAIT_US ? ME_SUSPEND_WAIT_US - used : 0;
  if (!RtlApuLockTimed(left))
    return PSPME_SUSPEND_UNRESOLVED;
  if (g_unknown || g_fatal || (g_state != PSPME_IDLE && !(g_state == PSPME_COMPLETED && g_pending))) {
    RtlApuUnlockRaw();
    return PSPME_SUSPEND_UNRESOLVED;  // sleep resets the ME, but that is not evidence this code can use
  }
  if (g_pending) {
    ME_STAT(g_st.pcm_dropped++);  // ring still full after the wait: the block is lost, the state stays consistent
    g_pending = false;
    g_state = PSPME_IDLE;
  }
  Disengage_Locked();
  RtlApuUnlockRaw();
  return PSPME_SUSPEND_OK;
}

PspAudioMeShutdown PspAudioMe_Shutdown(void) {
  if (g_thread < 0 && !g_injected)
    return g_shutdown_result;
  g_accepting = false;
  g_stop = true;
  bool joined = true;
  if (g_thread >= 0) {
    SceUInt to = ME_SHUTDOWN_WAIT_US;
    if (sceKernelWaitThreadEnd(g_thread, &to) < 0) {
      joined = false;  // the producer is stuck (inside a library wait): keep every resource it may use
    } else {
      sceKernelDeleteThread(g_thread);
      g_thread = -1;
    }
  }
  // Clean = the producer is gone and no task is unresolved. A FAILED/fatal task has also left the ME.
  const bool clean = joined && !g_unknown && !g_disabled;
  if (joined)
    RestoreSyscallEntry();  // best effort, unverifiable; also done when unresolved, it cannot make that worse
  if (clean) {
    g_engaged = false;
    g_shutdown_result = PSPME_SHUTDOWN_CLEAN;
  } else {
    g_shutdown_result = PSPME_SHUTDOWN_UNSAFE;
  }
  return g_shutdown_result;
}

bool PspAudioMe_IsActive(void) { return g_engaged; }

PspAudioMeState PspAudioMe_GetState(void) { return g_state; }

PspAudioMePop PspAudioMe_PopBlock(int16_t *out, int frames) {
  const bool engaged = g_engaged;  // read first: a CPU hand-over publishes every block before it clears this
  ME_BARRIER();
  const uint32_t r = g_ring_r, w = g_ring_w;
  if (w != r && frames == ME_FRAMES) {
    memcpy(out, g_ring[r % ME_RING_BLOCKS], ME_OUT_BYTES);
    ME_BARRIER();
    g_ring_r = r + 1;
    return PSPME_POP_BLOCK;
  }
  if (engaged) {
    memset(out, 0, (size_t)frames * ME_CHANNELS * sizeof(int16_t));
    ME_STAT(g_st.underruns++);
    return PSPME_POP_SILENCE;
  }
  return PSPME_POP_CPU;
}

bool PspAudioMe_LockState(void) {
  if (!g_engaged)
    return true;
  if (g_unknown || g_fatal || g_disabled)
    return false;
  CpuInvalidate(g_me_player, ME_PLAYER_BYTES);  // the ME wrote them back
  CpuInvalidate(g_me_player->dsp, ME_DSP_BYTES);
  return true;
}

void PspAudioMe_UnlockState(void) {
  if (!g_engaged || g_unknown || g_fatal || g_disabled)
    return;
  CpuWritebackInvalidate(g_me_player->dsp, ME_DSP_BYTES);
  CpuWritebackInvalidate(g_me_player, ME_PLAYER_BYTES);
  ME_SYNC();
}

#if PSP_ME_STATS
void PspAudioMe_LogStats(void) {
  if (!g_engaged && g_st.job.n == 0)
    return;
  typeof(g_st) s = g_st;  // snapshot, then reset
  memset(&g_st, 0, sizeof(g_st));
#define AVG(st) (unsigned)((st).n ? (st).sum / (st).n : 0), (unsigned)(st).max
  PspLog("ME state=%d unk=%d fatal=%d blocks=%u job=%u/%u fin=%u/%u | underruns=%u ambiguous=%u late_ok=%u refused=%u stackbad=%u pcmdrop=%u stack=%u/%d",
         (int)g_state, (int)g_unknown, (int)g_fatal, (unsigned)s.blocks, AVG(s.job), AVG(s.fin),
         (unsigned)s.underruns, (unsigned)s.ambiguous, (unsigned)s.late_ok, (unsigned)s.refused,
         (unsigned)s.stack_bad, (unsigned)s.pcm_dropped, (unsigned)s.stack_max, (int)ME_STACK_BYTES);
#undef AVG
}
#endif
