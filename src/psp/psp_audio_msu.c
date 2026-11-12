#include "psp_audio_msu.h"

#include "../config.h"
#include "psp_log.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define MSU_SAMPLES       (PSP_MSU_BLOCK_FRAMES * 2)
#define MSU_BLOCK_BYTES   (MSU_SAMPLES * sizeof(int16_t))
#define MSU_RING_BLOCKS   16
#define MSU_READY_BLOCKS  3
#define MSU_READER_STACK  0x4000
#define MSU_READER_PRIO   0x1f

typedef struct MsuBlock {
  uint32_t generation;
  int16_t samples[MSU_SAMPLES];
} MsuBlock;

static MsuBlock g_ring[MSU_RING_BLOCKS]
    __attribute__((aligned(64)));

/* SPSC ownership:
 * reader thread owns g_ring_w;
 * audio/ME consumer owns g_ring_r.
 */
static volatile uint32_t g_ring_w;
static volatile uint32_t g_ring_r;

static volatile uint32_t g_generation;
static volatile uint32_t g_ready_generation;
static volatile uint32_t g_stopped_generation;
static volatile uint32_t g_failed_generation;

static volatile int g_stop;
static bool g_enabled;
static SceUID g_thread = -1;

static uint8_t g_request_track;
static uint8_t g_request_game_track;
static bool g_request_loop;
static int g_volume = 100;

typedef struct MsuDiagEvent {
  volatile uint32_t sequence;
  uint32_t generation;
  uint8_t game_track;
  uint8_t track;
  uint8_t bank;
  uint8_t status;
  char path[64];
} MsuDiagEvent;

static MsuDiagEvent g_diag_request;
static MsuDiagEvent g_diag_open;
static MsuDiagEvent g_diag_ready;
static MsuDiagEvent g_diag_consumed;

static uint32_t AtomicLoad32(volatile uint32_t *p);

static void PublishDiag(MsuDiagEvent *event, uint32_t generation,
                        uint8_t game_track, uint8_t track,
                        uint8_t bank, uint8_t status,
                        const char *path) {
  event->generation = generation;
  event->game_track = game_track;
  event->track = track;
  event->bank = bank;
  event->status = status;
  if (path)
    snprintf(event->path, sizeof(event->path), "%s", path);
  else
    event->path[0] = 0;
  __sync_synchronize();
  __sync_add_and_fetch(&event->sequence, 1);
}

static bool ReadDiag(MsuDiagEvent *event, uint32_t *seen,
                     MsuDiagEvent *snapshot) {
  uint32_t before = AtomicLoad32(&event->sequence);
  if (before == *seen)
    return false;

  __sync_synchronize();
  snapshot->generation = event->generation;
  snapshot->game_track = event->game_track;
  snapshot->track = event->track;
  snapshot->bank = event->bank;
  snapshot->status = event->status;
  memcpy(snapshot->path, event->path, sizeof(snapshot->path));
  __sync_synchronize();

  uint32_t after = AtomicLoad32(&event->sequence);
  if (before != after)
    return false;

  *seen = after;
  return true;
}

static uint32_t AtomicLoad32(volatile uint32_t *p) {
  return __sync_fetch_and_add(p, 0);
}

static uint32_t NextGeneration(void) {
  uint32_t v = __sync_add_and_fetch(&g_generation, 1);
  if (v == 0)
    v = __sync_add_and_fetch(&g_generation, 1);
  return v;
}

static uint32_t ReadLe32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static bool ReadExact(SceUID fd, void *dst, unsigned bytes) {
  uint8_t *p = (uint8_t *)dst;
  unsigned done = 0;

  while (done < bytes) {
    int n = sceIoRead(fd, p + done, bytes - done);
    if (n <= 0)
      return false;
    done += (unsigned)n;
  }

  return true;
}

/* MSU-1 PCM header:
 * "MSU1", uint32 little-endian loop point in stereo frames.
 * Followed by signed 16-bit interleaved stereo PCM at 44100 Hz.
 */
static bool OpenTrack(uint8_t track, bool repeat,
                      SceUID *out_fd, int *loop_offset,
                      char *out_path, size_t out_path_size) {
  static const char *const patterns[] = {
    "audio/supermetroid_msu1-%u.pcm",
    "audio/%u.pcm",
  };

  char path[64];

  for (unsigned i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
    snprintf(path, sizeof(path), patterns[i], (unsigned)track);

    SceUID fd = sceIoOpen(path, O_RDONLY, 0);
    if (fd < 0)
      continue;

    uint8_t header[8];
    if (!ReadExact(fd, header, sizeof(header)) ||
        memcmp(header, "MSU1", 4) != 0) {
      sceIoClose(fd);
      continue;
    }

    int file_size = sceIoLseek32(fd, 0, SEEK_END);
    if (file_size < 8 || ((file_size - 8) & 3) != 0) {
      sceIoClose(fd);
      continue;
    }

    uint32_t total_frames = (uint32_t)(file_size - 8) / 4;
    uint32_t loop_frame = ReadLe32(header + 4);
    uint64_t offset = 8ULL + (uint64_t)loop_frame * 4ULL;

    if (total_frames == 0 ||
        (repeat && loop_frame >= total_frames) ||
        (repeat && (offset > INT_MAX || offset >= (uint32_t)file_size)) ||
        sceIoLseek32(fd, 8, SEEK_SET) < 0) {
      sceIoClose(fd);
      continue;
    }

    *loop_offset = repeat ? (int)offset : 8;
    *out_fd = fd;
    if (out_path && out_path_size)
      snprintf(out_path, out_path_size, "%s", path);
    return true;
  }

  return false;
}

/* Returns 1 for a block, 0 for clean EOF, -1 for I/O failure.
 * A short final non-looping block is zero-padded.
 */
static int ReadPcmBlock(SceUID fd, int16_t *dst, bool repeat,
                        int loop_offset, bool *eof_after) {
  unsigned done = 0;
  *eof_after = false;

  while (done < MSU_BLOCK_BYTES) {
    int n = sceIoRead(fd, (uint8_t *)dst + done,
                      MSU_BLOCK_BYTES - done);

    if (n < 0)
      return -1;

    if (n == 0) {
      if (repeat) {
        if (sceIoLseek32(fd, loop_offset, SEEK_SET) < 0)
          return -1;
        continue;
      }

      memset((uint8_t *)dst + done, 0, MSU_BLOCK_BYTES - done);
      *eof_after = true;
      return done ? 1 : 0;
    }

    done += (unsigned)n;
  }

  return 1;
}

static bool ReaderStopping(void) {
  return __sync_fetch_and_add(&g_stop, 0) != 0;
}

static int ReaderThread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  SceUID fd = -1;
  uint32_t seen_generation = 0;
  bool repeat = false;
  bool eof = false;
  int loop_offset = 8;

  int16_t block[MSU_SAMPLES];

  while (!ReaderStopping()) {
    uint32_t generation = AtomicLoad32(&g_generation);

    if (generation != seen_generation) {
      if (fd >= 0)
        sceIoClose(fd);

      fd = -1;
      eof = false;
      seen_generation = generation;

      __sync_synchronize();
      uint8_t track = g_request_track;
      repeat = g_request_loop;

      if (track) {
        char path[64] = { 0 };
        bool opened = OpenTrack(track, repeat, &fd, &loop_offset,
                                path, sizeof(path));

        if (generation != AtomicLoad32(&g_generation)) {
          if (fd >= 0)
            sceIoClose(fd);
          fd = -1;
          continue;
        }

        PublishDiag(&g_diag_open, generation, g_request_game_track,
                    track, 0, opened ? 1 : 0, path);

        if (!opened)
          __sync_lock_test_and_set(&g_failed_generation, generation);
      }

      continue;
    }

    if (fd < 0 || eof) {
      sceKernelDelayThread(1000);
      continue;
    }

    uint32_t w = AtomicLoad32(&g_ring_w);
    uint32_t r = AtomicLoad32(&g_ring_r);

    if ((uint32_t)(w - r) >= MSU_RING_BLOCKS) {
      sceKernelDelayThread(1000);
      continue;
    }

    bool eof_after = false;
    int result = ReadPcmBlock(fd, block, repeat, loop_offset, &eof_after);

    if (generation != AtomicLoad32(&g_generation))
      continue;

    if (result < 0) {
      /* A read error before prebuffer completion must not start MSU
       * playback with a partial buffer. SPC remains the fallback. */
      if (AtomicLoad32(&g_ready_generation) != generation)
        __sync_lock_test_and_set(&g_failed_generation, generation);
      result = 0;
      eof_after = true;
    }

    if (result > 0) {
      MsuBlock *slot = &g_ring[w % MSU_RING_BLOCKS];

      memcpy(slot->samples, block, MSU_BLOCK_BYTES);
      slot->generation = generation;

      /* Publish the completed slot only after its contents. */
      __sync_synchronize();
      __sync_lock_test_and_set(&g_ring_w, w + 1);
    }

    /* Count only this generation. The consumer may still be draining
     * old blocks, but it cannot consume current-generation blocks until
     * readiness has been published. */
    if (AtomicLoad32(&g_ready_generation) != generation &&
        AtomicLoad32(&g_failed_generation) != generation) {
      uint32_t queued = 0;
      uint32_t r = AtomicLoad32(&g_ring_r);
      uint32_t w = AtomicLoad32(&g_ring_w);

      while (r != w) {
        MsuBlock *slot = &g_ring[r % MSU_RING_BLOCKS];
        __sync_synchronize();
        if (slot->generation == generation)
          ++queued;
        ++r;
      }

      if (queued >= MSU_READY_BLOCKS || (eof_after && queued != 0)) {
        __sync_synchronize();
        __sync_lock_test_and_set(&g_ready_generation, generation);
        PublishDiag(&g_diag_ready, generation, g_request_game_track,
                    track, 0, 1, NULL);
      }
    }

    if (eof_after || result <= 0) {
      eof = true;
      sceIoClose(fd);
      fd = -1;
    }
  }

  if (fd >= 0)
    sceIoClose(fd);

  return 0;
}

bool PspMsu_Init(void) {
  if (g_enabled)
    return true;

  SceUID dir = sceIoDopen("audio");
  if (dir < 0)
    return false;
  sceIoDclose(dir);

  g_ring_w = 0;
  g_ring_r = 0;
  g_generation = 1;
  g_ready_generation = 0;
  g_stopped_generation = 0;
  g_failed_generation = 0;
  g_request_track = 0;
  g_request_game_track = 0;
  g_request_loop = false;
  g_stop = 0;

  g_volume = g_config.msuvolume;
  if (g_volume < 0)
    g_volume = 0;
  if (g_volume > 100)
    g_volume = 100;

  g_thread = sceKernelCreateThread(
      "sm-msu-reader", ReaderThread,
      MSU_READER_PRIO, MSU_READER_STACK,
      PSP_THREAD_ATTR_USER, NULL);

  if (g_thread < 0)
    return false;

  g_enabled = true;

  if (sceKernelStartThread(g_thread, 0, NULL) < 0) {
    g_enabled = false;
    sceKernelDeleteThread(g_thread);
    g_thread = -1;
    return false;
  }

  return true;
}

bool PspMsu_Shutdown(void) {
  if (!g_enabled)
    return true;

  __sync_lock_test_and_set(&g_stop, 1);

  SceUInt timeout = 2000000;
  if (sceKernelWaitThreadEnd(g_thread, &timeout) < 0)
    return false;

  sceKernelDeleteThread(g_thread);
  g_thread = -1;
  g_enabled = false;
  return true;
}

static void RequestTrack(uint8_t track, bool repeat) {
  if (g_request_track == track)
    return;

  g_request_track = track;
  g_request_loop = repeat;

  __sync_synchronize();
  NextGeneration();
}

static void StopTrack(void) {
  g_request_track = 0;
  g_request_game_track = 0;
  g_request_loop = false;

  __sync_synchronize();
  NextGeneration();
}

/* Exact data and lengths from supermetroid_msu1.asm.
 * Rows correspond to MusicBank values $00, $03, ... $48.
 * Unlike the original proposal, short rows are not padded.
 */
static const uint8_t kMusicMap[] = {
  4, 5,
  4, 5,
  6, 0, 7,
  8, 9,
  10,
  11,
  12,
  13,
  14,
  15, 16,
  17, 0,
  18,
  19, 21, 20,
  22, 23,
  24,
  0, 25, 0,
  26, 27,
  0, 0, 0,
  28,
  29,
  30,
  0,
  0,
  22, 23,
  10,
};

static const uint8_t kMusicMapOffset[25] = {
   0,  2,  4,  7,  9,
  10, 11, 12, 13, 14,
  16, 18, 19, 22, 24,
  25, 28, 30, 33, 34,
  35, 36, 37, 38, 40,
};

static const uint8_t kMusicMapLength[25] = {
  2, 2, 3, 2, 1,
  1, 1, 1, 1, 2,
  2, 1, 3, 2, 1,
  3, 2, 3, 1, 1,
  1, 1, 1, 2, 1,
};

static bool TrackRepeats(uint8_t track) {
  return track != 1 && track != 2 && track != 29 && track != 30;
}

bool PspMsu_HandleMusic(uint8_t requested, uint8_t current, uint8_t bank) {
  if (!g_enabled)
    return false;

  if (requested == 0) {
    StopTrack();
    PublishDiag(&g_diag_request, AtomicLoad32(&g_generation),
                requested, 0, bank, 0, NULL);
    return false;
  }

  /* Original patch leaves ambience on the native SPC path and does not
   * stop an already-playing MSU track for request 4.
   */
  if (requested == 4)
    return false;

  if (requested == current)
    return true;

  uint8_t track;

  if (requested < 5) {
    track = requested;
  } else {
    unsigned index = requested - 5;
    unsigned row = bank / 3;

    if (row >= 25 || index >= kMusicMapLength[row])
      return false;

    track = kMusicMap[kMusicMapOffset[row] + index];
  }

  if (track == 0)
    return false;

  g_request_game_track = requested;
  RequestTrack(track, TrackRepeats(track));

  PublishDiag(&g_diag_request, AtomicLoad32(&g_generation),
              requested, track, bank, 1, NULL);

  return AtomicLoad32(&g_ready_generation) ==
         AtomicLoad32(&g_generation);
}

/* Called by exactly one audio producer: either the ME producer or CPU renderer.
 * It never waits and never takes a kernel semaphore.
 */
bool PspMsu_PopBlock(int16_t *dst, int frames, bool *stop_spc) {
  if (stop_spc)
    *stop_spc = false;

  if (!g_enabled || !dst || frames != PSP_MSU_BLOCK_FRAMES)
    return false;


  for (;;) {
    uint32_t generation = AtomicLoad32(&g_generation);
    uint32_t r = AtomicLoad32(&g_ring_r);
    uint32_t w = AtomicLoad32(&g_ring_w);

    if (r == w)
      return false;

    MsuBlock *slot = &g_ring[r % MSU_RING_BLOCKS];

    __sync_synchronize();
    uint32_t slot_generation = slot->generation;

    /* Discard buffered PCM belonging to an earlier request. */
    if (slot_generation != generation) {
      __sync_lock_test_and_set(&g_ring_r, r + 1);
      continue;
    }

    /* A failed open or prebuffer read leaves SPC running. Discard any
     * partial blocks rather than publishing an incomplete MSU track. */
    if (AtomicLoad32(&g_failed_generation) == generation &&
        AtomicLoad32(&g_ready_generation) != generation) {
      __sync_lock_test_and_set(&g_ring_r, r + 1);
      continue;
    }

    /* Do this after stale-block disposal: otherwise an old full ring
     * prevents the new generation from ever reaching its prebuffer. */
    if (AtomicLoad32(&g_ready_generation) != generation)
      return false;

    memcpy(dst, slot->samples, MSU_BLOCK_BYTES);
    __sync_synchronize();
    __sync_lock_test_and_set(&g_ring_r, r + 1);

    if (stop_spc &&
        AtomicLoad32(&g_stopped_generation) != generation) {
      *stop_spc = true;
      __sync_lock_test_and_set(&g_stopped_generation, generation);
      PublishDiag(&g_diag_consumed, generation, g_request_game_track,
                  g_request_track, 0, 1, NULL);
    }

    return true;
  }
}

int PspMsu_GetVolume(void) {
  return g_volume;
}

/* Called from the main thread only. Never perform file logging from the
 * reader or audio/ME producer threads. */
void PspMsu_PollDiagnostics(void) {
  static uint32_t seen_request, seen_open, seen_ready, seen_consumed;
  MsuDiagEvent event;

  if (ReadDiag(&g_diag_request, &seen_request, &event))
    PspLog("msu: request game=%u bank=%u track=%u generation=%u",
           event.game_track, event.bank, event.track, event.generation);

  if (ReadDiag(&g_diag_open, &seen_open, &event))
    PspLog("msu: open %s track=%u generation=%u path=%s",
           event.status ? "OK" : "FAILED", event.track,
           event.generation, event.path[0] ? event.path : "(no valid file)");

  if (ReadDiag(&g_diag_ready, &seen_ready, &event))
    PspLog("msu: prebuffer ready track=%u generation=%u",
           event.track, event.generation);

  if (ReadDiag(&g_diag_consumed, &seen_consumed, &event))
    PspLog("msu: first PCM block consumed track=%u generation=%u",
           event.track, event.generation);
}
