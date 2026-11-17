#pragma once
#include <stdint.h>

// ROM message ids 1-29, index = id - 1.
// Tokens: {item:...}, {button:shoot|run}. The renderer wraps lines; "" is a blank line. */
// TITLE: line 0 is a title, the rest is body; SAVE: YES/NO prompt 
enum { PSP_MSG_TITLE = 1, PSP_MSG_SAVE = 2 };  
typedef struct PspMessageText { const char *line[3]; uint8_t n, flags; } PspMessageText;

static const PspMessageText g_psp_message_text[29] = {
  [1 - 1]  = {{"ENERGY TANK"}, 1, PSP_MSG_TITLE},
  [2 - 1]  = {{"MISSILE", "select {item:missile} & press the {button:shoot} button."}, 2, PSP_MSG_TITLE},
  [3 - 1]  = {{"SUPER MISSILE", "select {item:super_missile} & press the {button:shoot} button."}, 2, PSP_MSG_TITLE},
  [4 - 1]  = {{"POWER BOMB", "select {item:power_bomb} & set it with the {button:shoot} button."}, 2, PSP_MSG_TITLE},
  [5 - 1]  = {{"GRAPPLING BEAM", "select {item:grapple_beam} & hold the {button:shoot} button."}, 2, PSP_MSG_TITLE},
  [6 - 1]  = {{"X-RAY SCOPE", "press the {button:run} button twice to activate {item:xray_scope}."}, 2, PSP_MSG_TITLE},
  [7 - 1]  = {{"VARIA SUIT"}, 1, PSP_MSG_TITLE},
  [8 - 1]  = {{"SPRING BALL"}, 1, PSP_MSG_TITLE},
  [9 - 1]  = {{"MORPHING BALL"}, 1, PSP_MSG_TITLE},
  [10 - 1] = {{"SCREW ATTACK"}, 1, PSP_MSG_TITLE},
  [11 - 1] = {{"HI-JUMP BOOTS"}, 1, PSP_MSG_TITLE},
  [12 - 1] = {{"SPACE JUMP"}, 1, PSP_MSG_TITLE},
  [13 - 1] = {{"SPEED BOOSTER", "press the {button:run} button to activate {item:speedbooster}."}, 2, PSP_MSG_TITLE},
  [14 - 1] = {{"CHARGE BEAM"}, 1, PSP_MSG_TITLE},
  [15 - 1] = {{"ICE BEAM"}, 1, PSP_MSG_TITLE},
  [16 - 1] = {{"WAVE BEAM"}, 1, PSP_MSG_TITLE},
  [17 - 1] = {{"SPAZER"}, 1, PSP_MSG_TITLE},
  [18 - 1] = {{"PLASMA BEAM"}, 1, PSP_MSG_TITLE},
  [19 - 1] = {{"BOMB", "when in morphball, set it with the {button:shoot} button."}, 2, PSP_MSG_TITLE},
  [20 - 1] = {{"MAP DATA ACCESS", "", "COMPLETED."}, 3, 0},
  [21 - 1] = {{"ENERGY RECHARGE", "", "COMPLETED."}, 3, 0},
  [22 - 1] = {{"MISSILE RELOAD", "", "COMPLETED."}, 3, 0},
  [23 - 1] = {{"WOULD YOU LIKE", "TO SAVE?"}, 2, PSP_MSG_SAVE},
  [24 - 1] = {{"SAVE COMPLETED."}, 1, 0},
  [25 - 1] = {{"RESERVE TANK"}, 1, PSP_MSG_TITLE},
  [26 - 1] = {{"GRAVITY SUIT"}, 1, PSP_MSG_TITLE},
  [27 - 1] = {{0}, 0, 0},
  [28 - 1] = {{"WOULD YOU LIKE", "TO SAVE?"}, 2, PSP_MSG_SAVE},
  [29 - 1] = {{0}, 0, 0},
};
