#pragma once
/* Mosaic-driven screen transitions (PSP). Include after variables.h and psp_renderer.h.
 * Size = 16 - brightness of the original fade: each helper moves the size on the frame the original fade routine
 * would have changed the brightness and finishes on the same frame, so durations and state changes are unchanged.
 * reg_INIDISP is not faded: 15 while a transition runs, 0x80 when a fade-out completes (peak mosaic; the renderer
 * holds the last image while the next screen loads), 15 when a fade-in completes. */

/* HandleFadeOut/HandleFadeIn rule ($80:8924, $80:894D): screen_fade_counter counts down and is reloaded from
 * screen_fade_delay, so one step every screen_fade_delay + 1 frames. Neither variable is cleared on completion. */
static inline bool PspMenuFadeDue(void) {
  if ((int16)(screen_fade_counter - 1) < 0) {
    screen_fade_counter = screen_fade_delay;
    return true;
  }
  --screen_fade_counter;
  return false;
}

/* Replaces HandleFadeOut plus the caller's "brightness == 0" test: true on the frame the screen would have reached black. */
static inline bool PspMosaicFadeOut(void) {
  const bool due = PspMenuFadeDue();  /* same counter side effects as HandleFadeOut, also when already blanked */
  if (reg_INIDISP & 0x80)
    return true;
  if (!due)
    return false;
  const unsigned s = PspRenderer_GetMosaicTransition();
  if (s < 15) {
    PspRenderer_SetMosaicTransition(s + 1);
    return false;
  }
  PspRenderer_SetMosaicTransition(16);
  reg_INIDISP = 0x80;
  return true;
}

/* Replaces HandleFadeIn plus the "brightness == 15" test; starts from the peak (16). */
static inline bool PspMosaicFadeIn(void) {
  const bool due = PspMenuFadeDue();
  if (!due)
    return false;
  reg_INIDISP = 15;
  const unsigned s = PspRenderer_GetMosaicTransition();
  if (s > 2) {
    PspRenderer_SetMosaicTransition(s - 1);
    return false;
  }
  PspRenderer_SetMosaicTransition(1);
  return true;
}

/* AdvanceSlowScreenFadeOut ($8B:90D5): the cinematic rule is the opposite: screen_fade_delay counts down, is reloaded
 * from screen_fade_counter, and both are cleared on completion. */
static inline bool PspMosaicCineFadeOut(void) {
  const bool neg = (int8)(screen_fade_delay - 1) < 0;
  LOBYTE(screen_fade_delay) = screen_fade_delay - 1;
  if ((uint8)screen_fade_delay && !neg)
    return false;
  if (reg_INIDISP & 0xF) {
    const unsigned s = PspRenderer_GetMosaicTransition();
    if (s < 15) {
      PspRenderer_SetMosaicTransition(s + 1);
      LOBYTE(screen_fade_delay) = screen_fade_counter;
      return false;
    }
    PspRenderer_SetMosaicTransition(16);
    reg_INIDISP = 0x80;
    LOBYTE(screen_fade_delay) = 0;
    LOBYTE(screen_fade_counter) = 0;
  }
  return true;
}

/* AdvanceFastScreenFadeOut ($8B:90B8): brightness falls by screen_fade_counter every frame. */
static inline bool PspMosaicFastOut(void) {
  if (reg_INIDISP & 0x80)
    return true;
  unsigned s = PspRenderer_GetMosaicTransition() + (uint8)screen_fade_counter;
  if (s < 16) {
    PspRenderer_SetMosaicTransition(s);
    return false;
  }
  PspRenderer_SetMosaicTransition(16);
  reg_INIDISP = 0x80;
  return true;
}

/* AdvanceFastScreenFadeIn ($8B:9100). */
static inline bool PspMosaicFastIn(void) {
  const unsigned c = (uint8)screen_fade_counter, s = PspRenderer_GetMosaicTransition();
  reg_INIDISP = 15;
  if (s > c + 1) {
    PspRenderer_SetMosaicTransition(s - c);
    return false;
  }
  PspRenderer_SetMosaicTransition(1);
  return true;
}
