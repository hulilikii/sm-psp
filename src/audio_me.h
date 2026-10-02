#ifndef AUDIO_ME_H
#define AUDIO_ME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int16_t buffer[2][534 * 2];
  volatile uint32_t me_write_index;
  volatile uint32_t cpu_read_index;
  volatile uint32_t me_ready;
  uint32_t samples_per_frame;
  uint32_t source_samples;
} AudioMeSharedBuffer;

int AudioMeInit(AudioMeSharedBuffer *shared_buf, uint32_t samples_per_frame);
void AudioMeShutdown(void);
int16_t* AudioMeGetFrame(void);
void AudioMeStart(void);
void AudioMeStop(void);
AudioMeSharedBuffer* AudioMeGetSharedBuffer(void);
void AudioMeWriteDspSamples(const int16_t *dsp_samples, uint32_t dsp_count);

#ifdef __cplusplus
}
#endif
#endif // AUDIO_ME_H
