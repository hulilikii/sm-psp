#ifndef AUDIO_ME_H
#define AUDIO_ME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Shared audio buffer structure for CPU-ME synchronization
typedef struct {
  // Double buffer: two buffers that ME writes to while CPU reads from
  int16_t buffer[2][534 * 2];  // 534 samples per frame, stereo

  // Synchronization state
  volatile uint32_t me_write_index;  // Index of buffer ME is writing to (0 or 1)
  volatile uint32_t cpu_read_index;  // Index of buffer CPU should read from (0 or 1)
  volatile uint32_t me_ready;        // Set to 1 when ME has sample data ready

  // Configuration
  uint32_t samples_per_frame;        // Target sample count
  uint32_t source_samples;           // Source DSP sample count (534 for SNES)

} AudioMeSharedBuffer;

// Initialize ME audio rendering
// Returns 0 on success, negative on error
int AudioMeInit(AudioMeSharedBuffer *shared_buf, uint32_t samples_per_frame);

// Shutdown ME audio rendering
void AudioMeShutdown(void);

// Get the next audio frame from ME
// Returns pointer to audio buffer with samples_per_frame stereo samples
// Call this from your audio callback
int16_t* AudioMeGetFrame(void);

// Start ME processing
void AudioMeStart(void);

// Stop ME processing
void AudioMeStop(void);

#ifdef __cplusplus
}
#endif
#endif // AUDIO_ME_H
