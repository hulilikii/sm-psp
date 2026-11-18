#pragma once

#include "types.h"

// Overridable from the Makefile.
#ifndef PSP_AUDIO_ENABLED
#define PSP_AUDIO_ENABLED 0
#endif

// Run SPC + DSP on the Media Engine.
#ifndef PSP_AUDIO_ME
#define PSP_AUDIO_ME 0
#endif

#if PSP_AUDIO_ME && !PSP_AUDIO_ENABLED
#error "PSP_AUDIO_ME requires PSP_AUDIO_ENABLED"
#endif
