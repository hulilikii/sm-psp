#include "psp_input.h"

#include <pspctrl.h>

#include "src/sm_rtl.h"

void PspInput_Init(void) {
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
}

uint16_t PspInput_Read(void) {
  SceCtrlData pad;

  sceCtrlPeekBufferPositive(&pad, 1);

  uint16_t buttons = 0;

  if (pad.Buttons & PSP_CTRL_UP)
  buttons |= 1 << 4;
if (pad.Buttons & PSP_CTRL_DOWN)
  buttons |= 1 << 5;
if (pad.Buttons & PSP_CTRL_LEFT)
  buttons |= 1 << 6;
if (pad.Buttons & PSP_CTRL_RIGHT)
  buttons |= 1 << 7;
if (pad.Buttons & PSP_CTRL_SELECT)
  buttons |= 1 << 2;
if (pad.Buttons & PSP_CTRL_START)
  buttons |= 1 << 3;
if (pad.Buttons & PSP_CTRL_CROSS)
  buttons |= 1 << 0;  // B
if (pad.Buttons & PSP_CTRL_CIRCLE)
  buttons |= 1 << 8;  // A
if (pad.Buttons & PSP_CTRL_SQUARE)
  buttons |= 1 << 1;  // Y
if (pad.Buttons & PSP_CTRL_TRIANGLE)
  buttons |= 1 << 9;  // X
if (pad.Buttons & PSP_CTRL_LTRIGGER)
  buttons |= 1 << 10;
if (pad.Buttons & PSP_CTRL_RTRIGGER)
  buttons |= 1 << 11;

  return buttons;
}
