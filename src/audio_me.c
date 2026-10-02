#include "audio_me.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define ME_ALIGN 64

static void *me_aligned_alloc(size_t size) {
  void *p = malloc(size + ME_ALIGN);
  if (!p) {
    return NULL;
  }

  uintptr_t raw = (uintptr_t)p;
  uintptr_t aligned = (raw + ME_ALIGN - 1) & ~(uintptr_t)(ME_ALIGN - 1);
  void **saved = (void **)(aligned - sizeof(void *));
  *saved = p;
  return (void *)aligned;
}

static void me_aligned_free(void *p) {
  if (!p) {
    return;
  }
  void *original = *(void **)((uintptr_t)p - sizeof(void *));
  free(original);
}

int AudioMeBufferInit(AudioMeBuffer *buf, uint32_t target_samples, uint32_t source_samples) {
  if (!buf || target_samples == 0 || source_samples == 0) {
    return -1;
  }

  memset(buf, 0, sizeof(*buf));

  size_t buffer_bytes = target_samples * 2 * sizeof(int16_t);
  buf->buffer[0] = (int16_t *)me_aligned_alloc(buffer_bytes);
  buf->buffer[1] = (int16_t *)me_aligned_alloc(buffer_bytes);
  if (!buf->buffer[0] || !buf->buffer[1]) {
    if (buf->buffer[0]) {
      me_aligned_free(buf->buffer[0]);
    }
    if (buf->buffer[1]) {
      me_aligned_free(buf->buffer[1]);
    }
    return -2;
  }

  buf->buffer_size = target_samples;
  buf->target_samples = target_samples;
  buf->source_samples = source_samples;

  // Start with buffer 0 as ME write target, buffer 1 as CPU read target.
  buf->me_write_idx = 0;
  buf->cpu_read_idx = 1;
  buf->me_ready = 0;
  buf->me_fault = 0;

  buf->resampler.location = 0;
  buf->resampler.source_count = 0;
  buf->resampler.adder = (uint32_t)(((uint64_t)source_samples << 20) / (uint32_t)target_samples);

  return 0;
}

void AudioMeBufferShutdown(AudioMeBuffer *buf) {
  if (!buf) {
    return;
  }
  if (buf->buffer[0]) {
    me_aligned_free(buf->buffer[0]);
    buf->buffer[0] = NULL;
  }
  if (buf->buffer[1]) {
    me_aligned_free(buf->buffer[1]);
    buf->buffer[1] = NULL;
  }
  memset(buf, 0, sizeof(*buf));
}

int AudioMeEnqueueDspSamples(AudioMeBuffer *buf, const int16_t *dsp_samples, uint32_t dsp_count) {
  if (!buf || !dsp_samples || dsp_count == 0) {
    return -1;
  }

  buf->resampler.source_count = dsp_count;
  AudioMeResampleKernel(buf, dsp_samples);
  return 0;
}

int16_t *AudioMeGetNextFrame(AudioMeBuffer *buf) {
  if (!buf || buf->me_ready == 0) {
    return NULL;
  }

  uint32_t read_idx = buf->cpu_read_idx;
  int16_t *frame = buf->buffer[read_idx];
  buf->cpu_read_idx = 1 - read_idx;
  buf->me_ready = 0;
  return frame;
}

void AudioMeResampleKernel(AudioMeBuffer *buf, const int16_t *src) {
  if (!buf || !src || buf->resampler.source_count == 0) {
    buf->me_fault = 1;
    return;
  }

  uint32_t write_idx = buf->me_write_idx;
  int16_t *out = buf->buffer[write_idx];
  uint32_t target = buf->target_samples;
  uint32_t source_count = buf->resampler.source_count;
  uint32_t adder = buf->resampler.adder;
  uint32_t location = buf->resampler.location;

  // 12.20 fixed-point resampling:
  //   src sample index = location >> 20
  //   location += adder per output sample
  for (uint32_t i = 0; i < target; ++i) {
    uint32_t idx = location >> 20;
    if (idx >= source_count) {
      idx = source_count - 1;
    }
    if (idx >= source_count) {
      out[i * 2] = 0;
      out[i * 2 + 1] = 0;
    } else {
      // stereo frame: left/right interleaved in sampleBuffer
      out[i * 2] = src[idx * 2];
      out[i * 2 + 1] = src[idx * 2 + 1];
    }
    location += adder;
  }

  buf->resampler.location = location;
  buf->me_write_idx = 1 - write_idx;
  buf->me_ready = 1;
  buf->me_fault = 0;
}
