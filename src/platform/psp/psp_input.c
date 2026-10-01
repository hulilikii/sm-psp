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
    buttons |= kJoypadH_Up;

  if (pad.Buttons & PSP_CTRL_DOWN)
    buttons |= kJoypadH_Down;

  if (pad.Buttons & PSP_CTRL_LEFT)
    buttons |= kJoypadH_Left;

  if (pad.Buttons & PSP_CTRL_RIGHT)
    buttons |= kJoypadH_Right;

  if (pad.Buttons & PSP_CTRL_SELECT)
    buttons |= kJoypadH_Select;

  if (pad.Buttons & PSP_CTRL_START)
    buttons |= kJoypadH_Start;

  /*
   * PSP face-button convention:
   *
   * Cross    = SNES B
   * Circle   = SNES A
   * Square   = SNES Y
   * Triangle = SNES X
   */

  if (pad.Buttons & PSP_CTRL_CROSS)
    buttons |= kJoypadH_B;

  if (pad.Buttons & PSP_CTRL_CIRCLE)
    buttons |= kJoypadL_A;

  if (pad.Buttons & PSP_CTRL_SQUARE)
    buttons |= kJoypadH_Y;

  if (pad.Buttons & PSP_CTRL_TRIANGLE)
    buttons |= kJoypadL_X;

  if (pad.Buttons & PSP_CTRL_LTRIGGER)
    buttons |= kJoypadL_L;

  if (pad.Buttons & PSP_CTRL_RTRIGGER)
    buttons |= kJoypadL_R;

  return buttons;
}
