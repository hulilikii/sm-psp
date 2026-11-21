#pragma once

#include "types.h"

// Overridable from the Makefile.
#ifndef PSP_AUDIO_ENABLED
#define PSP_AUDIO_ENABLED 0
#endif

// Door/elevator room-change scroll: quadratic ease in/out (0 = original constant 4 px per step).
#ifndef SM_DOOR_EASE
#define SM_DOOR_EASE 1
#endif

// Fold the pre-scroll camera alignment into the eased scroll so both axes move together (diagonal).
// 0 = original: align the old room first (1 px per frame), then scroll. Needs SM_DOOR_EASE.
#ifndef SM_DOOR_DIAG
#define SM_DOOR_DIAG 1
#endif

// Start the new room's music track while the door scroll is still running (0 = after it, as the original).
#ifndef SM_DOOR_MUSIC_SYNC
#define SM_DOOR_MUSIC_SYNC 1
#endif

// Run SPC + DSP on the Media Engine.
#ifndef PSP_AUDIO_ME
#define PSP_AUDIO_ME 0
#endif

#if PSP_AUDIO_ME && !PSP_AUDIO_ENABLED
#error "PSP_AUDIO_ME requires PSP_AUDIO_ENABLED"
#endif
