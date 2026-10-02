#include "audio_me.h"
#include <string.h>
#include <stdio.h>

static AudioMeSharedBuffer *g_audio_me_shared = NULL;
static bool g_audio_me_initialized = false;

int AudioMeInit(AudioMeSharedBuffer *shared_buf, uint32_t samples_per_frame) {
  if (!shared_buf) {
    fprintf(stderr, "AudioMeInit: shared_buf is NULL\n");
    return -1;
  }

  if (samples_per_frame == 0 || samples_per_frame > 1024) {
    fprintf(stderr, "AudioMeInit: invalid samples_per_frame %u\n", samples_per_frame);
    return -2;
  }

  memset(shared_buf, 0, sizeof(AudioMeSharedBuffer));
  shared_buf->samples_per_frame = samples_per_frame;
  shared_buf->source_samples = 534;
  shared_buf->me_write_index = 0;
  shared_buf->cpu_read_index = 1;
  shared_buf->me_ready = 0;

  g_audio_me_shared = shared_buf;
  g_audio_me_initialized = true;
  return 0;
}

void AudioMeShutdown(void) {
  if (g_audio_me_shared) {
    g_audio_me_shared->me_ready = 0;
    memset(g_audio_me_shared, 0, sizeof(AudioMeSharedBuffer));
  }
  g_audio_me_shared = NULL;
  g_audio_me_initialized = false;
}

int16_t* AudioMeGetFrame(void) {
  if (!g_audio_me_initialized || !g_audio_me_shared)
    return NULL;

  int timeout = 100000;
  while (!g_audio_me_shared->me_ready && timeout-- > 0) {
    // spin
  }

  if (!g_audio_me_shared->me_ready)
    return NULL;

  uint32_t read_index = g_audio_me_shared->cpu_read_index;
  int16_t *frame = g_audio_me_shared->buffer[read_index];
  g_audio_me_shared->cpu_read_index = 1 - read_index;
  g_audio_me_shared->me_ready = 0;
  return frame;
}

void AudioMeStart(void) {
  if (g_audio_me_initialized && g_audio_me_shared) {
    // no-op hook for future ME start logic
  }
}

void AudioMeStop(void) {
  if (g_audio_me_initialized && g_audio_me_shared)
    g_audio_me_shared->me_ready = 0;
}

AudioMeSharedBuffer* AudioMeGetSharedBuffer(void) {
  return g_audio_me_shared;
}

void AudioMeWriteDspSamples(const int16_t *dsp_samples, uint32_t dsp_count) {
  if (!g_audio_me_initialized || !g_audio_me_shared)
    return;

  uint32_t target_samples = g_audio_me_shared->samples_per_frame;
  if (target_samples == 0 || dsp_count == 0)
    return;

  uint32_t write_index = g_audio_me_shared->me_write_index;
  int16_t *write_buffer = g_audio_me_shared->buffer[write_index];

  uint32_t adder = (uint32_t)(((uint64_t)dsp_count << 20) / (uint32_t)target_samples);
  uint32_t location = 0;

  for (uint32_t i = 0; i < target_samples; i++) {
    uint32_t idx = location >> 20;
    if (idx >= dsp_count - 1)
      idx = dsp_count - 1;

    write_buffer[i * 2] = dsp_samples[idx * 2];
    write_buffer[i * 2 + 1] = dsp_samples[idx * 2 + 1];
    location += adder;
  }

  g_audio_me_shared->me_write_index = 1 - write_index;
  g_audio_me_shared->me_ready = 1;
}
