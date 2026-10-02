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

  // Initialize shared buffer state
  memset(shared_buf, 0, sizeof(AudioMeSharedBuffer));

  shared_buf->samples_per_frame = samples_per_frame;
  shared_buf->source_samples = 534;  // SNES DSP produces 534 samples per frame
  shared_buf->me_write_index = 0;
  shared_buf->cpu_read_index = 1;  // Start reading from buffer 1
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
  if (!g_audio_me_initialized || !g_audio_me_shared) {
    return NULL;
  }

  // Wait for ME to have data ready
  // Timeout after ~100ms to avoid deadlock
  int timeout = 100000;
  while (!g_audio_me_shared->me_ready && timeout-- > 0) {
    // Busy wait with occasional yields would go here
    // For now, simple spin
  }

  if (!g_audio_me_shared->me_ready) {
    fprintf(stderr, "AudioMeGetFrame: timeout waiting for ME data\n");
    return NULL;
  }

  // Get the buffer that ME wrote to
  uint32_t read_index = g_audio_me_shared->cpu_read_index;
  int16_t *frame = g_audio_me_shared->buffer[read_index];

  // Toggle which buffer CPU will read from next
  g_audio_me_shared->cpu_read_index = 1 - read_index;

  // Clear the ready flag for next frame
  g_audio_me_shared->me_ready = 0;

  return frame;
}

void AudioMeStart(void) {
  if (g_audio_me_initialized && g_audio_me_shared) {
    // Signal ME to start processing
    // This would trigger ME kernel callback
    // Actual implementation depends on ME integration
  }
}

void AudioMeStop(void) {
  if (g_audio_me_initialized && g_audio_me_shared) {
    // Signal ME to stop processing
    g_audio_me_shared->me_ready = 0;
  }
}

AudioMeSharedBuffer* AudioMeGetSharedBuffer(void) {
  return g_audio_me_shared;
}
