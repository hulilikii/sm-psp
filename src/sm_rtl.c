#include "sm_rtl.h"
#include "sm_cpu_infra.h"
#include "types.h"
//#include "ida_types.h"
#include "variables.h"
#include "funcs.h"
#include "spc_player.h"
#include "util.h"
#include "config.h"
#if PSP_AUDIO_ENABLED
#include "psp/psp_audio_msu.h"
#endif
#if PSP_AUDIO_ME
#include "psp/psp_audio_me.h"
#endif
#include "psp/psp_log.h"
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <psppower.h>
#if SM_PSP_LOG
extern uint32_t g_vram_fill_bytes;  /* snes.c: direct RtlVramFill output bytes */
/* log builds: time of Memory Stick writes and SPC uploads on the game thread (logged when >= 1 ms) */
#define IO_T0() uint32_t io_t0 = sceKernelGetSystemTimeLow()
#define IO_LOG(what) do { uint32_t io_us = sceKernelGetSystemTimeLow() - io_t0; \
  if (io_us >= 1000) PspLog("IO f=%d %s us=%u", snes_frame_counter, what, (unsigned)io_us); } while (0)
#else
#define IO_T0() ((void)0)
#define IO_LOG(what) ((void)0)
#endif

struct StateRecorder;

static void RtlSaveMusicStateToRam_Locked();
static void RtlRestoreMusicAfterLoad_Locked(bool is_reset);

// Result of the last RtlApuLock(): false while ME audio owns (or may own) the SPC/DSP state. Only valid with the lock held.
static bool g_apu_state_ok = true;

uint8 g_ram[0x20000];
uint8 *g_sram;
const uint8 *g_rom;

static uint8 *g_rtl_memory_ptr;
static RunFrameFunc *g_rtl_runframe;
static SyncAllFunc *g_rtl_syncall;

void RtlSetupEmuCallbacks(uint8 *emu_ram, RunFrameFunc *func, SyncAllFunc *sync_all) {
  g_rtl_memory_ptr = emu_ram;
  g_rtl_runframe = func;
  g_rtl_syncall = sync_all;
}

static void RtlSynchronizeWholeState(void) {
  if (g_rtl_syncall)
    g_rtl_syncall();
}

// |ptr| must be a pointer into g_ram, will synchronize the RAM memory with the
// emulator.
static void RtlSyncMemoryRegion(void *ptr, size_t n) {
  uint8 *data = (uint8 *)ptr;
  assert(data >= g_ram && data < g_ram + 0x20000);
  if (g_rtl_memory_ptr)
    memcpy(g_rtl_memory_ptr + (data - g_ram), data, n);
}

void ByteArray_AppendVl(ByteArray *arr, uint32 v) {
  for (; v >= 255; v -= 255)
    ByteArray_AppendByte(arr, 255);
  ByteArray_AppendByte(arr, v);
}

void saveFunc(void *ctx_in, void *data, size_t data_size) {
  ByteArray_AppendData((ByteArray *)ctx_in, (uint8*)data, data_size);
}

typedef struct LoadFuncState {
  uint8 *p, *pend;
} LoadFuncState;

void loadFunc(void *ctx, void *data, size_t data_size) {
  LoadFuncState *st = (LoadFuncState *)ctx;
  assert((size_t)(st->pend - st->p) >= data_size);
  memcpy(data, st->p, data_size);
  st->p += data_size;
}
static void LoadSnesState(SaveLoadFunc *func, void *ctx) {
  // Do the actual loading
  snes_saveload(g_snes, func, ctx);

  g_snes->cpu->e = false;
  uint32 next = (g_snes->ram[g_snes->cpu->sp + 3] | g_snes->ram[g_snes->cpu->sp + 4] << 8 | g_snes->ram[g_snes->cpu->sp + 5] << 16) + 1;
  if (next == 0x82897e) {
    g_snes->ram[g_snes->cpu->sp + 3] = (0xF71B - 1) & 0xff;
    g_snes->ram[g_snes->cpu->sp + 4] = (0xF71B - 1) >> 8;
  }
  RtlSynchronizeWholeState();
}

static void SaveSnesState(SaveLoadFunc *func, void *ctx) {
  snes_saveload(g_snes, func, ctx);
}

typedef struct StateRecorder {
  uint16 last_inputs;
  uint32 frames_since_last;
  uint32 total_frames;

  // For replay
  uint32 replay_pos, replay_pos_last_complete;
  uint32 replay_frame_counter;
  uint32 replay_next_cmd_at;
  uint32 snapshot_flags;
  uint8 replay_cmd;
  bool replay_mode;

  ByteArray log;
  ByteArray base_snapshot;
} StateRecorder;

static StateRecorder state_recorder;

void StateRecorder_Init(StateRecorder *sr) {
  ByteArray_Destroy(&sr->log);
  ByteArray_Destroy(&sr->base_snapshot);
  memset(sr, 0, sizeof(*sr));
}

void StateRecorder_RecordCmd(StateRecorder *sr, uint8 cmd) {
  int frames = sr->frames_since_last;
  sr->frames_since_last = 0;
  int x = (cmd < 0xc0) ? 0xf : 0x1;
  ByteArray_AppendByte(&sr->log, cmd | (frames < x ? frames : x));
  if (frames >= x)
    ByteArray_AppendVl(&sr->log, frames - x);
}

void StateRecorder_Record(StateRecorder *sr, uint16 inputs) {
  uint16 diff = inputs ^ sr->last_inputs;
  if (diff != 0) {
    sr->last_inputs = inputs;
    //    printf("0x%.4x %d: ", diff, sr->frames_since_last);
    //    size_t lb = sr->log.size;
    for (int i = 0; i < 12; i++) {
      if ((diff >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
    //    while (lb < sr->log.size)
    //      printf("%.2x ", sr->log.data[lb++]);
    //    printf("\n");
  }
  sr->frames_since_last++;
  sr->total_frames++;
}

void StateRecorder_RecordPatchByte(StateRecorder *sr, uint32 addr, const uint8 *value, int num) {
  assert(addr < 0x20000);

//  printf("%d: PatchByte(0x%x, 0x%x. %d): ", sr->frames_since_last, addr, *value, num);
  size_t lb = sr->log.size;
  int lq = (num - 1) <= 3 ? (num - 1) : 3;
  StateRecorder_RecordCmd(sr, 0xc0 | (addr & 0x10000 ? 2 : 0) | lq << 2);
  if (lq == 3)
    ByteArray_AppendVl(&sr->log, num - 1 - 3);
  ByteArray_AppendByte(&sr->log, addr >> 8);
  ByteArray_AppendByte(&sr->log, addr);
  for (int i = 0; i < num; i++)
    ByteArray_AppendByte(&sr->log, value[i]);
//    while (lb < sr->log.size)
//      printf("%.2x ", sr->log.data[lb++]);
//    printf("\n");
}

void ReadFromFile(FILE *f, void *data, size_t n) {
  if (fread(data, 1, n, f) != n)
    Die("fread failed\n");
}

void RtlReset(int mode) {
  snes_frame_counter = 0;
  snes_reset(g_snes, true);
  if (!(mode & 1))
    memset(g_sram, 0, 0x2000);
  
  coroutine_state_0 = 1;
    
  g_apu_state_ok = RtlApuLock();
  RtlRestoreMusicAfterLoad_Locked(true);
  RtlApuUnlock();

  RtlSynchronizeWholeState();

  if ((mode & 2) == 0)
    StateRecorder_Init(&state_recorder);
}

int GetFileSize(FILE *f) {
  fseek(f, 0, SEEK_END);
  int r = ftell(f);
  fseek(f, 0, SEEK_SET);
  return r;
}

void StateRecorder_Load(StateRecorder *sr, FILE *f, bool replay_mode) {
  uint32 hdr[16] = { 0 };

  bool is_old = false;
  bool is_reset = false;

  ReadFromFile(f, hdr, 8 * sizeof(uint32));
  if (hdr[0] != 2) {
    hdr[8] = hdr[7];
    hdr[7] = hdr[5] >> 1;
    hdr[5] = (hdr[5] & 1) ? hdr[6] : 0;
  } else if (hdr[0] == 2) {
    ReadFromFile(f, hdr + 8, 8 * sizeof(uint32));

  } else {
    assert(0);
  }

  sr->total_frames = hdr[1];
  ByteArray_Resize(&sr->log, hdr[2]);
  ReadFromFile(f, sr->log.data, sr->log.size);
  sr->last_inputs = hdr[3];
  sr->frames_since_last = hdr[4];

  ByteArray_Resize(&sr->base_snapshot, hdr[5]);
  ReadFromFile(f, sr->base_snapshot.data, sr->base_snapshot.size);

  sr->snapshot_flags = hdr[9];
  sr->replay_next_cmd_at = 0;
  sr->replay_mode = replay_mode;
  if (replay_mode) {
    sr->frames_since_last = 0;
    sr->last_inputs = 0;
    sr->replay_pos = sr->replay_pos_last_complete = 0;
    sr->replay_frame_counter = 0;
    // Load snapshot from |base_snapshot_|, or reset if empty.
    if (sr->base_snapshot.size > 8192 ) {
      LoadFuncState state = { sr->base_snapshot.data, sr->base_snapshot.data + sr->base_snapshot.size };
      LoadSnesState(&loadFunc, &state);
      assert(state.p == state.pend);
    } else {
      RtlReset(2);
      if (sr->base_snapshot.size == 8192)
        memcpy(g_sram, sr->base_snapshot.data, 8192);
      is_reset = true;
    }
  } else {
    // Resume replay from the saved position?
    sr->replay_pos = sr->replay_pos_last_complete = hdr[7];
    sr->replay_frame_counter = hdr[8];
    sr->replay_mode = (sr->replay_frame_counter != 0);

    assert(hdr[6] == 275493);

    ByteArray arr = { 0 };
    ByteArray_Resize(&arr, hdr[6]);
    ReadFromFile(f, arr.data, arr.size);
    LoadFuncState state = { arr.data, arr.data + arr.size };
    LoadSnesState(&loadFunc, &state);
    ByteArray_Destroy(&arr);
    assert(state.p == state.pend);

    if (is_old)
      RtlClearKeyLog();
  }

  if (!is_reset)
    RtlRestoreMusicAfterLoad_Locked(false);

  RtlUpdateSnesPatchForBugfix();

  // Temporarily fix reset state
//  if (g_snes->cpu->k == 0x82 && g_snes->cpu->pc == 0xf716)
//    g_snes->cpu->pc = 0xf71c;
}

void StateRecorder_Save(StateRecorder *sr, FILE *f, bool saving_with_bug) {
  uint32 hdr[16] = { 0 };
  ByteArray arr = { 0 };
  SaveSnesState(&saveFunc, &arr);
  assert(sr->base_snapshot.size == 0 || sr->base_snapshot.size == arr.size || sr->base_snapshot.size == 8192);

  hdr[0] = 2;
  hdr[1] = sr->total_frames;
  hdr[2] = (uint32)sr->log.size;
  hdr[3] = sr->last_inputs;
  hdr[4] = sr->frames_since_last;
  hdr[5] = (uint32)sr->base_snapshot.size;
  hdr[6] = (uint32)arr.size;
  // If saving while in replay mode, also need to persist
  // sr->replay_pos_last_complete and sr->replay_frame_counter
  // so the replaying can be resumed.
  if (sr->replay_mode) {
    hdr[7] = sr->replay_pos_last_complete;
    hdr[8] = sr->replay_frame_counter;
  }
  hdr[9] = saving_with_bug * 1;
  fwrite(hdr, 1, sizeof(hdr), f);
  fwrite(sr->log.data, 1, sr->log.size, f);
  fwrite(sr->base_snapshot.data, 1, sr->base_snapshot.size, f);
  fwrite(arr.data, 1, arr.size, f);

  ByteArray_Destroy(&arr);
}

void StateRecorder_ClearKeyLog(StateRecorder *sr) {
  printf("Clearing key log!\n");
  sr->base_snapshot.size = 0;
  SaveSnesState(&saveFunc, &sr->base_snapshot);
  ByteArray old_log = sr->log;
  int old_frames_since_last = sr->frames_since_last;
  memset(&sr->log, 0, sizeof(sr->log));
  // If there are currently any active inputs, record them initially at timestamp 0.
  sr->frames_since_last = 0;
  if (sr->last_inputs) {
    for (int i = 0; i < 12; i++) {
      if ((sr->last_inputs >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
  }
  if (sr->replay_mode) {
    // When clearing the key log while in replay mode, we want to keep
    // replaying but discarding all key history up until this point.
    if (sr->replay_next_cmd_at != 0xffffffff) {
      sr->replay_next_cmd_at -= old_frames_since_last;
      sr->frames_since_last = sr->replay_next_cmd_at;
      sr->replay_pos_last_complete = (uint32)sr->log.size;
      StateRecorder_RecordCmd(sr, sr->replay_cmd);
      int old_replay_pos = sr->replay_pos;
      sr->replay_pos = (uint32)sr->log.size;
      ByteArray_AppendData(&sr->log, old_log.data + old_replay_pos, old_log.size - old_replay_pos);
    }
    sr->total_frames -= sr->replay_frame_counter;
    sr->replay_frame_counter = 0;
  } else {
    sr->total_frames = 0;
  }
  ByteArray_Destroy(&old_log);
  sr->frames_since_last = 0;
}

uint16 StateRecorder_ReadNextReplayState(StateRecorder *sr) {
  assert(sr->replay_mode);
  while (sr->frames_since_last >= sr->replay_next_cmd_at) {
    int replay_pos = sr->replay_pos;
    if (replay_pos != sr->replay_pos_last_complete) {
      // Apply next command
      sr->frames_since_last = 0;
      if (sr->replay_cmd < 0xc0) {
        sr->last_inputs ^= 1 << (sr->replay_cmd >> 4);
      } else if (sr->replay_cmd < 0xd0) {
        int nb = 1 + ((sr->replay_cmd >> 2) & 3);
        uint8 t;
        if (nb == 4) do {
          nb += t = sr->log.data[replay_pos++];
        } while (t == 255);
        uint32 addr = ((sr->replay_cmd >> 1) & 1) << 16;
        addr |= sr->log.data[replay_pos++] << 8;
        addr |= sr->log.data[replay_pos++];
        do {
          g_ram[addr & 0x1ffff] = sr->log.data[replay_pos++];
          RtlSyncMemoryRegion(&g_ram[addr & 0x1ffff], 1);
        } while (addr++, --nb);
      } else {
        assert(0);
      }
    }
    sr->replay_pos_last_complete = replay_pos;
    if (replay_pos >= sr->log.size) {
      sr->replay_pos = replay_pos;
      sr->replay_next_cmd_at = 0xffffffff;
      break;
    }
    // Read the next one
    uint8 cmd = sr->log.data[replay_pos++], t;
    int mask = (cmd < 0xc0) ? 0xf : 0x1;
    int frames = cmd & mask;
    if (frames == mask) do {
      frames += t = sr->log.data[replay_pos++];
    } while (t == 255);
    sr->replay_next_cmd_at = frames;
    sr->replay_cmd = cmd;
    sr->replay_pos = replay_pos;
  }
  sr->frames_since_last++;
  // Turn off replay mode after we reached the final frame position
  if (++sr->replay_frame_counter >= sr->total_frames) {
    sr->replay_mode = false;
  }
  return sr->last_inputs;
}

void StateRecorder_StopReplay(StateRecorder *sr) {
  if (!sr->replay_mode)
    return;
  sr->replay_mode = false;
  sr->total_frames = sr->replay_frame_counter;
  sr->log.size = sr->replay_pos_last_complete;
}


void RtlClearKeyLog(void) {
  StateRecorder_ClearKeyLog(&state_recorder);
}

void RtlStopReplay(void) {
  StateRecorder_StopReplay(&state_recorder);
}

enum {
  // Version was bumped to 1 after I fixed bug #1
  kCurrentBugFixCounter = 1,
};

bool RtlRunFrame(int inputs) {
  // Avoid up/down and left/right from being pressed at the same time
  if ((inputs & 0x30) == 0x30) inputs ^= 0x30;
  if ((inputs & 0xc0) == 0xc0) inputs ^= 0xc0;

  bool is_replay = state_recorder.replay_mode;

  // Either copy state or apply state
  if (is_replay) {
    inputs = StateRecorder_ReadNextReplayState(&state_recorder);
  } else {
    // Loading a bug snapshot?
    if (state_recorder.snapshot_flags & 1) {
      state_recorder.snapshot_flags &= ~1;
      inputs = state_recorder.last_inputs;
    } else {
      if (bug_fix_counter != kCurrentBugFixCounter) {
        if (g_debug_flag)
          printf("bug_fix_counter %d => %d\n", bug_fix_counter, kCurrentBugFixCounter);
        if (bug_fix_counter < kCurrentBugFixCounter) {
          bug_fix_counter = kCurrentBugFixCounter;
          StateRecorder_RecordPatchByte(&state_recorder, (uint8 *)&bug_fix_counter - g_ram, (uint8 *)&bug_fix_counter, 2);
        }
      }
    }

    StateRecorder_Record(&state_recorder, inputs);
  }

  if (bug_fix_counter != currently_installed_bug_fix_counter)
    RtlUpdateSnesPatchForBugfix();

  g_rtl_runframe(inputs, 0);

  snes_frame_counter++;

  #if PSP_AUDIO_ENABLED
  RtlPushApuState();
  #endif
  return is_replay;
}

void RtlSaveSnapshot(const char *filename, bool saving_with_bug) {
  FILE *f = fopen(filename, "wb");
  if (f == NULL) {
    fprintf(stderr, "Unable to open snapshot for writing: %s\n", filename);
    return;
  }

  g_apu_state_ok = RtlApuLock();
  RtlSaveMusicStateToRam_Locked();
  StateRecorder_Save(&state_recorder, f, saving_with_bug);
  RtlApuUnlock();
  fclose(f);
}

static const char *const kBugSaves[] = {
  "Before Kraid",
  "Before Golden Torizo", "After Crocomire", "Baby Metroid", "Tourian Statue", "Before Ridley", "Enter Mother Brain",
};

void RtlSaveLoad(int cmd, int slot) {
  char name[128];
  if (slot >= 256) {
    int i = slot - 256;
    if (cmd == kSaveLoad_Save || i >= sizeof(kBugSaves) / sizeof(kBugSaves[0]))
      return;
    sprintf(name, "saves/%s.sav", kBugSaves[i]);
  } else {
    sprintf(name, "saves/save%d.sav", slot);
  }
  printf("*** %s slot %d\n",
    cmd == kSaveLoad_Save ? "Saving" : cmd == kSaveLoad_Load ? "Loading" : "Replaying", slot);
  if (cmd != kSaveLoad_Save) {

    FILE *f = fopen(name, "rb");
    if (f == NULL) {
      printf("Failed fopen: %s\n", name);
      return;
    }
    g_apu_state_ok = RtlApuLock();
    StateRecorder_Load(&state_recorder, f, cmd == kSaveLoad_Replay);
    ppu_copy(g_snes->my_ppu, g_snes->ppu);
    RtlApuUnlock();
    RtlSynchronizeWholeState();
    fclose(f);

    if (coroutine_state_0 | coroutine_state_1 | coroutine_state_2 | coroutine_state_3 | coroutine_state_4) {
      printf("Coroutine state: %d, %d, %d, %d, %d\n",
        coroutine_state_0, coroutine_state_1, coroutine_state_2, coroutine_state_3, coroutine_state_4);
    }

    // Earlier versions used coroutine_state_0 differently
    if (coroutine_state_0 == 4)
      coroutine_state_0 = 10 + game_state;

    // bug_fix_counter_BAD didn't actually belong to free ram...
    if (bug_fix_counter == 0)
      bug_fix_counter = bug_fix_counter_BAD;

  } else {
    RtlSaveSnapshot(name, false);
  }
}

void MemCpy(void *dst, const void *src, int size) {
  memcpy(dst, src, size);
}

PairU16 MakePairU16(uint16 k, uint16 j) {
  PairU16 r = { k, j };
  return r;
}

void mov24(struct LongPtr *a, uint32 d) {
  a->addr = d & 0xffff;
  a->bank = d >> 16;
}

uint32 Load24(const LongPtr *src) {
  return src->addr | ((uint32)src->bank << 16);
}

bool Unreachable(void) {
  printf("Unreachable!\n");
  assert(0);
  g_ram[0x1ffff] = 1;
  return false;
}

const uint8 *RomPtr(uint32_t addr) {
  if (!(addr & 0x8000)) {
    printf("RomPtr - Invalid access 0x%x!\n", addr);
    g_fail = true;
  }
  return &g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff];
}

void WriteReg(uint16 reg, uint8 value) {
  snes_write(g_snes, reg, value);
}

uint16 Mult8x8(uint8 a, uint8 b) {
  return a * b;
}

uint16 SnesDivide(uint16 a, uint8 b) {
  return (b == 0) ? 0xffff : a / b;
}

uint16 SnesModulus(uint16 a, uint8 b) {
  return (b == 0) ? a : a % b;
}


uint8 ReadReg(uint16 reg) {
  return snes_read(g_snes, reg);
}

uint16 ReadRegWord(uint16 reg) {
  uint16_t rv = ReadReg(reg);
  rv |= ReadReg(reg + 1) << 8;
  return rv;
}

void WriteRegWord(uint16 reg, uint16 value) {
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

/* Same VRAM semantics as ppu_write 0x18/0x19 (an identical rewrite leaves the page clean). The simple case is the only one
 * in which the VRAM word pointer after every byte of DecompressToVRAM equals dst_pos >> 1. */
bool RtlVramBulkBegin(uint16 **vram, uint8 **dirty, uint16 dst_addr) {
  Ppu *p = g_snes->ppu;
  if (p->vramRemapMode != 0 || !p->vramIncrementOnHigh || p->vramIncrement != 1 ||
      p->vramPointer != (uint16)(dst_addr >> 1))
    return false;
  *vram = p->vram;
  *dirty = p->vramDirty;
  return true;
}

void RtlVramBulkEnd(uint16 ptr, bool copied, uint16 buf, uint8 openbus) {
  Ppu *p = g_snes->ppu;
  p->vramPointer = ptr;
  if (copied) {  /* only the ReadPpuByte / VMADD sequence of a copy command touches these */
    p->vramReadBuffer = buf;
    p->ppu1openBus = openbus;
  }
}

/* count x WriteReg(reg, val) for reg = VMDATAL / VMDATAH. Direct only when the register advances the address and there
 * is no remap (then every write goes to vram[ptr] and ptr += vramIncrement); otherwise the original register loop. */
void RtlVramFill(uint16 reg, int count, uint8 val) {
  Ppu *p = g_snes->ppu;
  bool hi = reg == VMDATAH;
  if (p->vramRemapMode != 0 || (reg != VMDATAL && !hi) || hi != (bool)p->vramIncrementOnHigh) {
    while (count-- > 0)
      WriteReg(reg, val);
    return;
  }
  uint16 *vram = p->vram;
  uint8 *dirty = p->vramDirty;
  uint16 ptr = p->vramPointer, inc = p->vramIncrement;
#if SM_PSP_LOG
  if (count > 0)
    g_vram_fill_bytes += (uint32_t)count;
#endif
  for (; count > 0; count--, ptr += inc) {
    uint16 a = ptr & 0x7fff, old = vram[a];
    uint16 nv = hi ? (uint16)((old & 0x00ff) | (val << 8)) : (uint16)((old & 0xff00) | val);
    if (nv != old) {
      vram[a] = nv;
      dirty[a >> 8] = 1;
    }
  }
  p->vramPointer = ptr;
}

// Maintain a queue cause the snes and audio callback are not in sync.
// If an entry is 255, it means unset.
typedef struct ApuWriteEnt {
  uint8 ports[4];
} ApuWriteEnt;

enum {
  kApuMaxQueueSize = 16,
};

static struct ApuWriteEnt g_apu_write_ents[kApuMaxQueueSize], g_apu_write;
static uint8 g_apu_write_ent_pos, g_apu_queue_size, g_apu_time_since_empty;

void RtlApuWrite(uint32 adr, uint8 val) {
  assert(adr >= APUI00 && adr <= APUI03);

  if (is_uploading_apu) {
    snes_catchupApu(g_snes); // catch up the apu before writing
    g_snes->apu->inPorts[adr & 0x3] = val;
    return;
  }

  if (g_snes->runningWhichVersion != 2) {
    RtlApuQueueLock();
    g_apu_write.ports[adr & 0x3] = val;
    RtlApuQueueUnlock();
  }
}

static bool IsFrameEmpty(ApuWriteEnt *w) {
  return (w->ports[0] == 255) && (w->ports[1] == 255) && (w->ports[2] == 255) && (w->ports[3] == 255);
}

void RtlPushApuState(void) {
  RtlApuQueueLock();
  if (!is_uploading_apu) {
    // Strive for the queue to be empty.
    if (g_apu_queue_size == 0) {
        g_apu_time_since_empty = 0;
    } else {
      if (g_apu_time_since_empty >= 32 && IsFrameEmpty(&g_apu_write)) {
        g_apu_time_since_empty -= 4;
        RtlApuQueueUnlock();
        return;
      }
      g_apu_time_since_empty++;
    }

    // Merge the two oldest to make space
    ApuWriteEnt *w0 = &g_apu_write_ents[g_apu_write_ent_pos++ & (kApuMaxQueueSize - 1)];
    if (g_apu_queue_size == kApuMaxQueueSize) {
      ApuWriteEnt *w1 = &g_apu_write_ents[g_apu_write_ent_pos & (kApuMaxQueueSize - 1)];
      for (int i = 0; i < 4; i++)
        if (w1->ports[i] == 255)
          w1->ports[i] = w0->ports[i];
    } else {
      g_apu_queue_size++;
    }
    *w0 = g_apu_write;
    memset(&g_apu_write, 0xff, sizeof(g_apu_write));
  } else {
    g_apu_queue_size = 0;
  }
  RtlApuQueueUnlock();
}

static void RtlPopApuState_Locked(void) {
  if (is_uploading_apu)
    return;

  uint8 *input_ports = g_use_my_apu_code ? g_spc_player->input_ports : g_snes->apu->inPorts;
  if (g_apu_queue_size != 0) {
    ApuWriteEnt *w = &g_apu_write_ents[(g_apu_write_ent_pos - g_apu_queue_size--) & (kApuMaxQueueSize - 1)];
    for (int i = 0; i != 4; i++) {
      if (w->ports[i] != 255)
        input_ports[i] = w->ports[i];
    }
  }
}

// ME audio producer (APU lock held, state accessible): apply one queued frame of port writes to the SPC state before
// the job is triggered. The writes are then part of the state the ME renders from, so a failed or late job can neither
// lose nor reorder them.
void RtlApuPopPortsToState(void) {
  RtlApuQueueLock();
  RtlPopApuState_Locked();
  RtlApuQueueUnlock();
}

/* APU state lock must already be held.
 * Equivalent to the MSU patch's STZ SPC_COMM_0.
 * Preserve ports 1-3, including pending SFX commands.
 */
void RtlApuStopMusic_Locked(void) {
  uint8 *input_ports =
      g_use_my_apu_code ? g_spc_player->input_ports : g_snes->apu->inPorts;

  RtlApuQueueLock();

  /* Suppress any already-queued port-0 command. */
  for (unsigned i = 0; i < g_apu_queue_size; i++) {
    ApuWriteEnt *w =
        &g_apu_write_ents[(g_apu_write_ent_pos - 1 - i) &
                          (kApuMaxQueueSize - 1)];
    w->ports[0] = 255;
  }
  g_apu_write.ports[0] = 255;

  input_ports[0] = 0;

  RtlApuQueueUnlock();
}

static void RtlResetApuQueue(void) {
  g_apu_write_ent_pos = g_apu_time_since_empty = g_apu_queue_size = 0;
  memset(&g_apu_write, 0xff, sizeof(g_apu_write));
}

void RtlApuUpload(const uint8 *p) {
  IO_T0();
  bool state_ok = RtlApuLock();
  RtlApuQueueLock();
  RtlResetApuQueue();
  RtlApuQueueUnlock();
  if (state_ok)  // ME audio unresolved: the SPC state must not be written (the upload is lost, music stays as it is)
    SpcPlayer_Upload(g_spc_player, p);
  RtlApuUnlock();
  IO_LOG("apu upload");
}

void RtlRestoreMusicAfterLoad_Locked(bool is_reset) {
  if (g_use_my_apu_code && g_apu_state_ok) {
    memcpy(g_spc_player->ram, g_snes->apu->ram, 65536);
    memcpy(g_spc_player->dsp->ram, g_snes->apu->dsp->ram, sizeof(Dsp) - offsetof(Dsp, ram));
    SpcPlayer_CopyVariablesFromRam(g_spc_player);
  }

  if (is_reset && g_apu_state_ok) {
    SpcPlayer_Initialize(g_spc_player);
  }

  RtlApuQueueLock();
  RtlResetApuQueue();
  RtlApuQueueUnlock();
}

void RtlSaveMusicStateToRam_Locked(void) {
  if (g_use_my_apu_code && g_apu_state_ok) {
    // Apply the whole contents of the queue to input ports
    SpcPlayer *spc_player = g_spc_player;
    RtlApuQueueLock();
    for (int i = g_apu_queue_size; i; i--) {
      ApuWriteEnt *we = &g_apu_write_ents[(g_apu_write_ent_pos - i) & (kApuMaxQueueSize - 1)];
      for (int j = 0; j < 4; j++) {
        if (we->ports[j] != 255)
          spc_player->input_ports[j] = we->ports[j];
      }
    }
    RtlApuQueueUnlock();
    SpcPlayer_CopyVariablesToRam(g_spc_player);
    memcpy(g_snes->apu->dsp->ram, g_spc_player->dsp->ram, sizeof(Dsp) - offsetof(Dsp, ram));
    memcpy(g_snes->apu->ram, g_spc_player->ram, 65536);
  }
}

// Resampler history for the CPU path (the ME path keeps its own, inside psp_audio_me.c).
static int16 g_cpu_rs_work[DSP_RS_WORK_SAMPLES];

#if PSP_AUDIO_ENABLED
static int16 g_cpu_msu_block[PSP_MSU_BLOCK_FRAMES * 2]
    __attribute__((aligned(64)));
#endif

// Hand-over of the resampler history between the CPU path and the ME (APU lock held).
void RtlGetCpuRsHistory(int16 *hist) {
  memcpy(hist, g_cpu_rs_work, DSP_RS_HISTORY_SAMPLES * sizeof(int16));
}

void RtlSetCpuRsHistory(const int16 *hist) {
  memcpy(g_cpu_rs_work, hist, DSP_RS_HISTORY_SAMPLES * sizeof(int16));
}

void RtlRenderAudio(int16 *audio_buffer, int samples, int channels) {
  assert(channels == 2);

#if PSP_AUDIO_ME
  // Audio callback: never waits for the ME. While the ME owns the stream it only takes finished blocks from the
  // ring (or plays silence). The CPU renders only when it owns the stream, which is re-checked under the APU lock:
  // PspAudioMe_Init/Suspend change the owner under the same lock.
  for (;;) {
    if (g_use_my_apu_code) {
      PspAudioMePop pop = PspAudioMe_PopBlock(audio_buffer, samples);
      if (pop != PSPME_POP_CPU)
        return;
    }
    RtlApuLock();
    if (!g_use_my_apu_code || !PspAudioMe_IsActive())
      break;
    RtlApuUnlock();
   }
#else
  RtlApuLock();
#endif

  RtlApuQueueLock();
  RtlPopApuState_Locked();
  RtlApuQueueUnlock();

#if PSP_AUDIO_ENABLED
  bool msu_stop_spc = false;
  bool have_msu =
      PspMsu_PopBlock(g_cpu_msu_block, samples, &msu_stop_spc);

  if (have_msu && msu_stop_spc)
    RtlApuStopMusic_Locked();
#endif

  if (!g_use_my_apu_code) {
    if (!is_uploading_apu) {
      while (g_snes->apu->dsp->sampleOffset < 534)
        apu_cycle(g_snes->apu);
      dsp_getSamples(g_snes->apu->dsp, audio_buffer, samples);
    }
  } else {
    SpcPlayer_GenerateSamples(g_spc_player);
    dsp_getSamplesHQ(g_spc_player->dsp, audio_buffer, samples, g_cpu_rs_work);
  }

#if PSP_AUDIO_ENABLED
  if (have_msu) {
    int volume = PspMsu_GetVolume();
    for (int i = 0; i < samples * 2; i++) {
      int32_t mixed = (int32_t)audio_buffer[i] +
          ((int32_t)g_cpu_msu_block[i] * volume) / 100;
      if (mixed > 32767) mixed = 32767;
      if (mixed < -32768) mixed = -32768;
      audio_buffer[i] = (int16)mixed;
    }
  }
#endif

  RtlApuUnlock();
}

void RtlCheat(char c) {
  if (c == 'w') {
    samus_health = samus_max_health;
    StateRecorder_RecordPatchByte(&state_recorder, 0x9C2, (uint8 *)&samus_health, 2);
  } else if (c == 'q') {
    samus_y_pos -= 4;
    samus_y_speed = -8;
    StateRecorder_RecordPatchByte(&state_recorder, 0xafa, (uint8 *)&samus_y_pos, 2);
    StateRecorder_RecordPatchByte(&state_recorder, 0xb2e, (uint8 *)&samus_y_speed, 2);
    menu_index = 0;
  } else if (c == 'q') {
    japanese_text_flag = !japanese_text_flag;
    StateRecorder_RecordPatchByte(&state_recorder, 0x9e2, (uint8 *)&japanese_text_flag, 2);
  }
}

// ---- SRAM persistence (begin) ----
/* The game only reads and writes g_sram (RAM), like the cart's battery RAM ($70:xxxx, plain STA in $81:8000). The
 * Memory Stick files are a persistent mirror. A synchronous save cost 77-459 ms on the game thread (log: IO sram
 * write), so a low-priority thread commits the newest snapshot instead. Files (saves/ in the application directory):
 *   sm.srm      current save (read by earlier builds too)       sm.srm.bak  previous valid save
 *   sm.srm.new  complete, verified newest snapshot, not yet promoted
 *   sm.srm.tmp  snapshot being written; never trusted
 * File format: the 8192 SRAM bytes, then an 8-byte trailer (magic "SMS2", CRC-32 of the SRAM bytes; little endian).
 * Older builds and legacy files are the bare 8192 bytes: still loaded (no CRC to check), and an older build that
 * reads only the first 8192 bytes of a new file loads it unchanged. A CRC catches data that did not reach the card
 * although the size and name did (the read-back below may be served from a cache, so it cannot prove that).
 * Commit: write .tmp, close, best-effort sceIoSync, read back and verify, rename .tmp -> .new, rotate
 * .srm -> .bak (only if .srm validates), rename .new -> .srm. The renames are the commit points and each step
 * leaves a usable file, so an interrupted commit costs at most the newest snapshot. Load order is .new (must carry
 * a valid trailer), .srm, .bak (trailer valid, or a legacy file of exactly 8192 or over 8200 bytes).
 * Only sceIo* is used (sceIoRename cannot overwrite, hence the removes): newlib remove() is a stub in older PSPSDKs,
 * and stdio would resolve "saves/" through newlib's cwd instead of the explicit path used here.
 * Durability: sceIoSync is documented only as "Synchronise the file data on the device"; its result and its effect
 * on a Memory Stick are not documented, so it is best effort and a power cut can still lose the newest snapshot. */
enum { kSramBytes = 8192, kSramFileBytes = kSramBytes + 8, kSramThreadPrio = 0x30, kSramThreadStack = 0x4000 };
enum { kSramMagic = 0x32534D53 };  /* "SMS2" */
enum { kSramBackoffMinUs = 250000, kSramBackoffMaxUs = 2000000, kSramJoinUs = 3000000, kSramShutdownUs = 3000000 };
enum { kPathMain, kPathBak, kPathNew, kPathTmp, kPathCount };
enum { kStepNone, kStepOpenTmp, kStepWriteTmp, kStepCloseTmp, kStepVerify, kStepPublish, kStepRotate, kStepPromote };

static char g_sram_path[kPathCount][320];
static char g_sram_dev[16];  /* "ms0:" / "ef0:" from the application path; empty: no sceIoSync */
static uint8 g_sram_pending[kSramBytes] __attribute__((aligned(64)));  /* newest snapshot; g_sram_lock */
static uint8 g_sram_local[kSramBytes] __attribute__((aligned(64)));    /* committer's copy */
static uint8 g_sram_verify[kSramFileBytes] __attribute__((aligned(64)));  /* read-back buffer */
static SceUID g_sram_lock = -1, g_sram_wake = -1, g_sram_thread = -1;
static uint32_t g_sram_req, g_sram_done;  /* g_sram_lock: newest queued / newest committed; equal = persisted */
static bool g_sram_alive, g_sram_quit;    /* g_sram_lock: worker running / asked to stop once nothing is pending */
static bool g_sram_inited, g_sram_stopped;  /* game thread */
static int g_sram_io_busy;                  /* try-lock: one committer at a time */
static int g_sram_queue_error;              /* __sync_*: a snapshot could not be queued and is lost, see SramQueueSnapshot */
/* diagnostics, g_sram_lock */
static uint32_t g_sram_commits, g_sram_fails, g_sram_us_max, g_sram_sync_errs, g_sram_plock_errs;
static int g_sram_last_step, g_sram_last_err;

static bool SramLock(void) { return g_sram_lock < 0 || sceKernelWaitSema(g_sram_lock, 1, NULL) >= 0; }
static void SramUnlock(void) { if (g_sram_lock >= 0) sceKernelSignalSema(g_sram_lock, 1); }

static void SramSetPaths(const char *dir) {
  static const char *const kNames[kPathCount] = {"saves/sm.srm", "saves/sm.srm.bak", "saves/sm.srm.new", "saves/sm.srm.tmp"};
  size_t n = dir ? strlen(dir) : 0;
  const bool rel = n == 0 || strcmp(dir, ".") == 0 || n + 24 > sizeof(g_sram_path[0]);
  for (int i = 0; i < kPathCount; i++) {
    if (rel)
      snprintf(g_sram_path[i], sizeof(g_sram_path[i]), "%s", kNames[i]);
    else
      snprintf(g_sram_path[i], sizeof(g_sram_path[i]), "%s%s%s", dir, dir[n - 1] == '/' ? "" : "/", kNames[i]);
  }
  g_sram_dev[0] = 0;
  const char *colon = rel ? NULL : strchr(dir, ':');
  if (colon && (size_t)(colon - dir) + 2 <= sizeof(g_sram_dev)) {
    memcpy(g_sram_dev, dir, (size_t)(colon - dir) + 1);
    g_sram_dev[colon - dir + 1] = 0;
  }
}

static uint32_t SramCrc32(const uint8 *p, size_t n) {  /* CRC-32/ISO-HDLC, check("123456789") = 0xCBF43926 */
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}
/* byte-wise: the buffers are not necessarily word aligned at these offsets */
static void SramPut32(uint8 *p, uint32_t v) { p[0] = (uint8)v; p[1] = (uint8)(v >> 8); p[2] = (uint8)(v >> 16); p[3] = (uint8)(v >> 24); }
static uint32_t SramGet32(const uint8 *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Read and validate a save file into buf (kSramFileBytes); the SRAM bytes are buf[0..kSramBytes). A file of exactly
 * kSramFileBytes is a checksummed file and must carry the magic and a matching CRC. A file of exactly kSramBytes or
 * of more than kSramFileBytes is a legacy file (no CRC to check, as the previous loader accepted it) unless need_v2.
 * Sizes in between are a trailer cut short by an interrupted write and are rejected. A cut at exactly kSramBytes
 * (trailer entirely missing) looks like a legacy file and cannot be told apart. */
static bool SramReadFile(const char *path, uint8 *buf, bool need_v2) {
  SceIoStat st;
  if (sceIoGetstat(path, &st) < 0 || FIO_S_ISDIR(st.st_mode) || st.st_size < kSramBytes)
    return false;
  const int want = st.st_size >= kSramFileBytes ? kSramFileBytes : kSramBytes;
  SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
  if (fd < 0)
    return false;
  int got = 0;
  while (got < want) {
    int n = sceIoRead(fd, buf + got, want - got);
    if (n <= 0)
      break;
    got += n;
  }
  sceIoClose(fd);
  if (got != want)
    return false;
  if (st.st_size == kSramFileBytes)
    return SramGet32(buf + kSramBytes) == (uint32_t)kSramMagic && SramGet32(buf + kSramBytes + 4) == SramCrc32(buf, kSramBytes);
  if (st.st_size == kSramBytes || st.st_size > kSramFileBytes)
    return !need_v2;
  return false;  /* 8193..8199 bytes: a checksummed file whose trailer was cut short, never a legacy save */
}

static int SramWriteAll(SceUID fd, const uint8 *p, int n) {  /* 0 or the failing sceIoWrite result */
  for (int done = 0; done < n;) {
    const int w = sceIoWrite(fd, p + done, n - done);
    if (w <= 0)
      return w < 0 ? w : -1;
    done += w;
  }
  return 0;
}

/* Replace the save with `data` (kSramBytes). true only when it is the current saves/sm.srm. On false the previous
 * valid save is still on the card (as .srm, or as .bak/.new while a promote is pending). */
static bool SramCommit(const uint8 *data) {
  const uint32_t t0 = sceKernelGetSystemTimeLow();
  const int plock = scePowerLock(0);  /* a power-switch press during the lock fires after scePowerUnlock */
  int step = kStepNone, err = 0;
  bool sync_err = false;
  do {
    SceUID fd = sceIoOpen(g_sram_path[kPathTmp], PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { step = kStepOpenTmp; err = fd; break; }
    uint8 trailer[8] __attribute__((aligned(16)));
    SramPut32(trailer, (uint32_t)kSramMagic);
    SramPut32(trailer + 4, SramCrc32(data, kSramBytes));
    err = SramWriteAll(fd, data, kSramBytes);
    if (err == 0)
      err = SramWriteAll(fd, trailer, sizeof(trailer));
    const int cr = sceIoClose(fd);
    if (err != 0) { step = kStepWriteTmp; break; }
    if (cr < 0) { step = kStepCloseTmp; err = cr; break; }
    if (g_sram_dev[0] && sceIoSync(g_sram_dev, 0) < 0)
      sync_err = true;  /* best effort only, see above */
    if (!SramReadFile(g_sram_path[kPathTmp], g_sram_verify, true) || memcmp(g_sram_verify, data, kSramBytes) != 0) {
      step = kStepVerify; break;
    }
    sceIoRemove(g_sram_path[kPathNew]);  /* stale, older than the verified .tmp; failure shows up in the rename */
    err = sceIoRename(g_sram_path[kPathTmp], g_sram_path[kPathNew]);
    if (err < 0) { step = kStepPublish; break; }
    SceIoStat pst;
    if (sceIoGetstat(g_sram_path[kPathMain], &pst) >= 0) {
      if (SramReadFile(g_sram_path[kPathMain], g_sram_verify, false)) {
        sceIoRemove(g_sram_path[kPathBak]);  /* old backup; if it stays, the rename below fails and .srm is untouched */
        err = sceIoRename(g_sram_path[kPathMain], g_sram_path[kPathBak]);
      } else {
        err = sceIoRemove(g_sram_path[kPathMain]);  /* unusable primary: never rotated over a good .bak; .new is verified */
      }
      if (err < 0) { step = kStepRotate; break; }
    }
    err = sceIoRename(g_sram_path[kPathNew], g_sram_path[kPathMain]);
    if (err < 0) { step = kStepPromote; break; }
  } while (0);
  if (plock >= 0)
    scePowerUnlock(0);
  const uint32_t us = sceKernelGetSystemTimeLow() - t0;
  if (SramLock()) {
    if (us > g_sram_us_max) g_sram_us_max = us;
    if (sync_err) g_sram_sync_errs++;
    if (plock < 0) g_sram_plock_errs++;
    if (step == kStepNone) {
      g_sram_commits++;
    } else {
      g_sram_fails++;
      g_sram_last_step = step;
      g_sram_last_err = err;
    }
    SramUnlock();
  }
  return step == kStepNone;
}

/* Commit the newest snapshot if it is not persisted yet. true: nothing is pending afterwards. Failure, or another
 * committer being active, leaves it pending; a snapshot queued meanwhile is never dropped. */
static bool SramDrain(void) {
  if (__sync_lock_test_and_set(&g_sram_io_busy, 1))
    return false;
  bool ok = false;
  if (SramLock()) {
    const uint32_t seq = g_sram_req;
    if (seq == g_sram_done) {
      SramUnlock();
      ok = true;
    } else {
      memcpy(g_sram_local, g_sram_pending, kSramBytes);
      SramUnlock();
      if (SramCommit(g_sram_local) && SramLock()) {
        g_sram_done = seq;
        SramUnlock();
        ok = true;
      }
    }
  }
  __sync_lock_release(&g_sram_io_busy);
  return ok;
}

static int SramThread(SceSize args, void *argp) {
  (void)args; (void)argp;
  uint32_t backoff = 0;
  for (bool run = true; run;) {
    SceUInt to = backoff;
    const uint32_t w0 = sceKernelGetSystemTimeLow();
    if (sceKernelWaitSema(g_sram_wake, 1, backoff ? &to : NULL) < 0) {
      if (!backoff)
        break;  /* an untimed wait only fails if the semaphore is gone */
      const uint32_t el = sceKernelGetSystemTimeLow() - w0;
      if (el < backoff)
        sceKernelDelayThread(backoff - el);  /* timeout, or an immediate error: keep the backoff */
    }
    for (;;) {
      if (!SramDrain()) {  /* still pending: retry after a backoff, or at once when woken by a new snapshot */
        backoff = backoff ? (backoff * 2 > kSramBackoffMaxUs ? kSramBackoffMaxUs : backoff * 2) : kSramBackoffMinUs;
        break;
      }
      backoff = 0;
      if (!SramLock()) { run = false; break; }
      const bool more = g_sram_done != g_sram_req, quit = g_sram_quit;
      if (!more && quit)
        g_sram_alive = false;  /* only ever leaves with nothing pending */
      SramUnlock();
      if (more) continue;
      if (quit) run = false;
      break;
    }
  }
  if (SramLock()) {
    g_sram_alive = false;
    SramUnlock();
  }
  return 0;
}

void RtlSramInit(const char *app_dir) {
  if (g_sram_inited)
    return;
  g_sram_inited = true;
  SramSetPaths(app_dir);
  g_sram_lock = sceKernelCreateSema("sm-psp-sram", 0, 1, 1, NULL);
  g_sram_wake = sceKernelCreateSema("sm-psp-sramw", 0, 0, 1, NULL);  /* max 1: wakes coalesce, none is lost */
  if (g_sram_lock >= 0 && g_sram_wake >= 0)
    g_sram_thread = sceKernelCreateThread("sm-sram-writer", SramThread, kSramThreadPrio, kSramThreadStack,
                                          PSP_THREAD_ATTR_USER, NULL);
  if (g_sram_thread >= 0) {
    g_sram_alive = true;  /* before the start: the worker may clear it at once */
    if (sceKernelStartThread(g_sram_thread, 0, NULL) < 0) {
      g_sram_alive = false;
      sceKernelDeleteThread(g_sram_thread);
      g_sram_thread = -1;
    }
  }
  if (g_sram_thread < 0) {
    if (g_sram_lock >= 0) sceKernelDeleteSema(g_sram_lock);
    if (g_sram_wake >= 0) sceKernelDeleteSema(g_sram_wake);
    g_sram_lock = g_sram_wake = -1;
    PspLog("sram: writer thread unavailable, saves stay synchronous");
  }
}

/* 1: queued for the worker; 0: queued, no worker, the caller drains synchronously; -1: not queued. A snapshot that
 * could not be queued is lost (it was never copied) and cannot be recovered by writing g_sram directly: that write
 * would race the worker and an older pending snapshot could later overwrite it. It is recorded in
 * g_sram_queue_error so Flush/Shutdown cannot report success; the next snapshot that does get queued holds the
 * newest g_sram and clears it. */
static int SramQueueSnapshot(void) {
  if (!SramLock()) {
    __sync_lock_test_and_set(&g_sram_queue_error, 1);
    return -1;
  }
  memcpy(g_sram_pending, g_sram, kSramBytes);
  ++g_sram_req;
  __sync_lock_release(&g_sram_queue_error);
  const bool worker = g_sram_alive && !g_sram_quit;
  SramUnlock();
  return worker ? 1 : 0;
}

void RtlSramKick(void) {
  if (g_sram_wake >= 0)
    sceKernelSignalSema(g_sram_wake, 1);  /* an overflow error just means a wake is already pending */
}

/* Wait until the newest queued snapshot is on the card. false: timed out or failed; the snapshot stays pending
 * and keeps being retried. Without a worker the caller commits it here. */
bool RtlSramFlush(uint32_t timeout_us) {
  if (!g_sram_inited)
    return true;
  const uint32_t t0 = sceKernelGetSystemTimeLow();
  bool kicked = false;
  for (;;) {
    if (!SramLock())
      return false;
    const bool persisted = g_sram_done == g_sram_req, alive = g_sram_alive;
    SramUnlock();
    if (persisted)  /* checked after the persisted test: a lost snapshot is not in req/done */
      return __sync_fetch_and_add(&g_sram_queue_error, 0) == 0;
    if (!alive) {
      if (SramDrain())
        continue;
    } else if (!kicked) {
      RtlSramKick();  /* skip the retry backoff once */
      kicked = true;
    }
    if (sceKernelGetSystemTimeLow() - t0 >= timeout_us)
      return false;
    sceKernelDelayThread(2000);
  }
}

/* Exit path, game thread. true: everything persisted and the worker is gone. false: a snapshot is still pending, or
 * the worker did not stop; the worker is then left running (never asked to stop with work pending) and nothing is
 * freed, so the caller can only report it and exit. Safe to call repeatedly. */
bool RtlSramShutdown(void) {
  if (!g_sram_inited || g_sram_stopped)
    return true;
  bool ok = RtlSramFlush(kSramShutdownUs);
  if (SramLock()) {
    PspLog("sram: %u commits, %u failures (last step %d err %08x), max %u us, sync errs %u, power-lock errs %u, %s",
           (unsigned)g_sram_commits, (unsigned)g_sram_fails, g_sram_last_step, (unsigned)g_sram_last_err,
           (unsigned)g_sram_us_max, (unsigned)g_sram_sync_errs, (unsigned)g_sram_plock_errs,
           ok ? "all persisted" : __sync_fetch_and_add(&g_sram_queue_error, 0) ? "SNAPSHOT LOST (not queued)" : "PENDING");
    SramUnlock();
  }
  if (!ok)
    return false;
  if (g_sram_thread >= 0) {
    if (!SramLock())
      return false;
    ok = g_sram_done == g_sram_req && __sync_fetch_and_add(&g_sram_queue_error, 0) == 0;  /* nothing queued or lost since the flush */
    if (ok)
      g_sram_quit = true;
    SramUnlock();
    if (!ok)
      return false;
    RtlSramKick();
    SceUInt to = kSramJoinUs;
    if (sceKernelWaitThreadEnd(g_sram_thread, &to) < 0) {
      PspLog("sram: writer did not stop, left running");
      return false;
    }
    sceKernelDeleteThread(g_sram_thread);
    sceKernelDeleteSema(g_sram_wake);
    SceUID lock = g_sram_lock;
    g_sram_thread = g_sram_wake = g_sram_lock = -1;  /* later saves are synchronous */
    sceKernelDeleteSema(lock);
  }
  g_sram_stopped = true;
  return true;
}

/* Copy the best saved file into dst. Returns the kPath* it came from, or -1. */
static int SramLoadBest(uint8 *dst) {  /* dst: kSramFileBytes */
  static const int kOrder[3] = {kPathNew, kPathMain, kPathBak};
  for (int i = 0; i < 3; i++)
    if (SramReadFile(g_sram_path[kOrder[i]], dst, kOrder[i] == kPathNew))  /* .new only exists as a checksummed file */
      return kOrder[i];
  return -1;
}
// ---- SRAM persistence (end) ----

void RtlReadSram(void) {
  static uint8 data[kSramFileBytes] __attribute__((aligned(64)));
  if (!g_sram_inited)
    RtlSramInit(NULL);
  const int src = SramLoadBest(data);
  if (src >= 0) {
    PspLog("sram: loaded %s", src == kPathMain ? "sm.srm" : src == kPathNew ? "sm.srm.new (interrupted save)" : "sm.srm.bak (recovery)");
    memcpy(g_sram, data, kSramBytes);
    RtlSynchronizeWholeState();
    ByteArray_Resize(&state_recorder.base_snapshot, 8192);
    memcpy(state_recorder.base_snapshot.data, g_sram, 8192);
    if (src != kPathMain && SramQueueSnapshot() > 0)  /* put the recovered state back into sm.srm */
      RtlSramKick();
    return;
  }
  /* Nothing under the explicit paths: the previous behaviour (newlib's cwd), for an older save location. */
  FILE *f = fopen("saves/sm.srm", "rb");
  if (f) {
    size_t bytes_read = fread(data, 1, kSramBytes, f);
    int close_result = fclose(f);
    if (bytes_read != kSramBytes || close_result != 0) {
      fprintf(stderr, "Incomplete or failed read of saves/sm.srm\n");
      return;
    }
    memcpy(g_sram, data, kSramBytes);
    RtlSynchronizeWholeState();
    ByteArray_Resize(&state_recorder.base_snapshot, 8192);
    memcpy(state_recorder.base_snapshot.data, g_sram, 8192);
  }
}

void RtlWriteSram(void) {
  IO_T0();
  if (!g_sram_inited)
    RtlSramInit(NULL);
  const int q = SramQueueSnapshot();
  if (q > 0) {
    RtlSramKick();
    IO_LOG("sram queue");
    return;
  }
  if (q == 0) {  /* no worker (not creatable, or stopped): synchronous, as before */
    SramDrain();
    IO_LOG("sram write");
    return;
  }
  PspLog("sram: unable to queue snapshot");  /* no uncoordinated write; Flush/Shutdown report it */
  IO_LOG("sram queue failed");
}
