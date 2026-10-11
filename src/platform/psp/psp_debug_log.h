#ifndef PSP_DEBUG_LOG_H
#define PSP_DEBUG_LOG_H

#include <stdbool.h>
#include <stdint.h>

void PspDebugLog_Init(bool enabled);
void PspDebugLog_SetEnabled(bool enabled);
bool PspDebugLog_IsEnabled(void);
void PspDebugMarker(uint32_t marker);

#endif
