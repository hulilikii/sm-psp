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

// Switch room music at the middle of the door scroll (0 = original: after the scroll finishes).
// Needs SM_DOOR_EASE (the scroll length is what the midpoint is taken from).
#ifndef SM_DOOR_MUSIC_SYNC
#define SM_DOOR_MUSIC_SYNC 1
#endif

// Never read outside the room when drawing scrolled-in blocks (solid black tile instead of wrapped/garbage data), and
// make the BG2 parallax position handle negative camera positions. Both only matter once the transition is visible.
#ifndef SM_DOOR_FIXOOB
#define SM_DOOR_FIXOOB 1
#endif

// Run SPC + DSP on the Media Engine.
#ifndef PSP_AUDIO_ME
#define PSP_AUDIO_ME 0
#endif

#if PSP_AUDIO_ME && !PSP_AUDIO_ENABLED
#error "PSP_AUDIO_ME requires PSP_AUDIO_ENABLED"
#endif
