// Message boxes
#include "ida_types.h"
#include "variables.h"
#include "funcs.h"
#include "src/psp/psp_renderer.h"

uint16 message_box_das0l_value;


#define kMessageBoxDefs ((MsgBoxConfig*)RomFixedPtr(0x85869b))

// Native PSP message presentation. The original BG3 message tilemap, HDMA, palette and VRAM work is replaced by the renderer's final-composition overlay. Waits, music/SFX servicing, input and save-selection logic are unchanged.
static void InitializePpuForMessageBoxes(void) {  // 0x858143
  PspRenderer_SetMessageDimHold(false);
  save_confirmation_selection = 0;
}

static void ClearMessageBoxBg3Tilemap(void) {  // 0x8581F3
PspRenderer_ClearMessageBox();
}

static void SetupPpuForActiveMessageBox(uint16 r52) {  // 0x85831E
  (void)r52;
}

static void DrawSpecialButtonAndSetupPpuForLargeMessageBox(uint16 a) {
  uint16 v1 = 0;
  if ((a & kButton_A) == 0) {
    v1 = 2;
    if ((a & 0x8000) == 0) {
      v1 = 4;
      if ((a & kButton_X) == 0) {
        v1 = 6;
        if ((a & kButton_Y) == 0) {
          v1 = 8;
          if ((a & kButton_Select) == 0) {
            v1 = 10;
            if ((a & kButton_L) == 0) {
              v1 = 12;
              if ((a & kButton_R) == 0)
                v1 = 14;
            }
          }
        }
      }
    }
  }
  
  int icon = -1;
  const char *label = "";

  switch (v1) {
  case 0: icon = PSP_BUTTON_ICON_CIRCLE; break;    /* SNES A */
  case 2: icon = PSP_BUTTON_ICON_CROSS; break;     /* SNES B */
  case 4: icon = PSP_BUTTON_ICON_TRIANGLE; break;  /* SNES X */
  case 6: icon = PSP_BUTTON_ICON_SQUARE; break;    /* SNES Y */
  case 8: icon = PSP_BUTTON_ICON_SELECT; break;
  case 10: icon = PSP_BUTTON_ICON_L; break;
  case 12: icon = PSP_BUTTON_ICON_R; break;
  default: break;
  }

  PspRenderer_SetMessageButton(icon, label);
  SetupPpuForActiveMessageBox(416);
}

static void DrawShootButtonAndSetupPpuForLargeMessageBox(void) {  // 0x8583C5
  DrawSpecialButtonAndSetupPpuForLargeMessageBox(button_config_shoot_x);
}

static void DrawRunButtonAndSetupPpuForLargeMessageBox(void) {  // 0x8583CC
  DrawSpecialButtonAndSetupPpuForLargeMessageBox(button_config_run_b);
}

static void SetupPpuForSmallMessageBox(void) {  // 0x858436
  SetupPpuForActiveMessageBox(448);
}

static void SetupPpuForLargeMessageBox(void) {  // 0x858441
  SetupPpuForActiveMessageBox(416);
}

static void CallMsgBoxModify(uint32 ea) {
  switch (ea) {
  case fnDrawShootButtonAndSetupPpuForLargeMessageBox: DrawShootButtonAndSetupPpuForLargeMessageBox(); return;
  case fnDrawRunButtonAndSetupPpuForLargeMessageBox: DrawRunButtonAndSetupPpuForLargeMessageBox(); return;
  case fnSetupPpuForSmallMessageBox: SetupPpuForSmallMessageBox(); return;
  case fnSetupPpuForLargeMessageBox: SetupPpuForLargeMessageBox(); return;
  default: Unreachable();
  }
}

static void InitializeMessageBox(void) {  // 0x858241
  int v0 = message_box_index - 1;
  PspRenderer_SetMessageBox(message_box_index, save_confirmation_selection);
  CallMsgBoxModify(kMessageBoxDefs[v0].modify_box_func | 0x850000);
}

static void ToggleSaveConfirmationSelection(void) {
  save_confirmation_selection ^= 2;
  PspRenderer_SetMessageBoxSelection(save_confirmation_selection);
}

static void RestorePpuForMessageBox(void) {  // 0x85861A
// The overlay never touched PPU, VRAM, HDMA, BG3SC or the layer shadows except the three below, which $85:8665-866D restores.
  gameplay_TM = reg_TM;
  gameplay_CGWSEL = next_gameplay_CGWSEL;
  gameplay_CGADSUB = next_gameplay_CGADSUB;
}

// Refill stations change resource values before opening the message box. The normal gameplay loop stops calling HandleHudTilemap while the box is open, so keep the original HUD counters advancing during these no-update waits.
static void UpdateRefillStationHud(void) {
  if (message_box_index == 0x15 || message_box_index == 0x16)
    HandleHudTilemap();
}

static CoroutineRet OpenMessageBox_Async(void) {  // 0x85844C
  COROUTINE_BEGIN(coroutine_state_4, 0);
  for (message_box_animation_y_radius = 0;; message_box_animation_y_radius += 512) {
    COROUTINE_AWAIT(1, WaitForNMI_NoUpdate_Async());
    UpdateRefillStationHud();
    HandleMusicQueue();
    HandleSoundEffects();
    PspRenderer_SetMessageFade(message_box_animation_y_radius * 255u / 0x1800u);
    if (message_box_animation_y_radius == 0x1800)
      break;
  }
  COROUTINE_END(0);
}

static CoroutineRet HandleMessageBoxInteraction_Async(void) {  // 0x85846D
  COROUTINE_BEGIN(coroutine_state_4, 0);

  if (message_box_index == 23 || message_box_index == 28) {
    save_confirmation_selection = 0;
    PspRenderer_SetMessageBoxSelection(save_confirmation_selection);
    while (1) {
      do {
        COROUTINE_AWAIT(1, WaitForNMI_NoUpdate_Async());
        HandleMusicQueue();
        HandleSoundEffects();
        ReadJoypadInputs();
      } while (!joypad1_newkeys);
      if ((joypad1_newkeys & kButton_A) != 0)
        break;
      if ((joypad1_newkeys & kButton_B) != 0) {
        save_confirmation_selection = 2;
        PspRenderer_SetMessageBoxSelection(save_confirmation_selection);
        goto GETOUT;
      }
      if ((joypad1_newkeys & (kButton_Select | kButton_Left | kButton_Right)) != 0) {
        ToggleSaveConfirmationSelection();
        QueueSfx1_Max6(0x37);
      }
    }
  } else {
    my_counter = 10;
    if (message_box_index != 20 && message_box_index != 21 && message_box_index != 22 && message_box_index != 24)
      my_counter = 360;
    do {
      COROUTINE_AWAIT(2, WaitForNMI_NoUpdate_Async());
      UpdateRefillStationHud();
      HandleMusicQueue();
      HandleSoundEffects();
    } while (--my_counter);
    do {
      COROUTINE_AWAIT(3, WaitForNMI_NoUpdate_Async());
      UpdateRefillStationHud();
      ReadJoypadInputs();
    } while ((bug_fix_counter < 1 ? joypad1_newkeys : joypad1_lastkeys) == 0);
  }
GETOUT:;
  COROUTINE_END(0);
}

static CoroutineRet CloseMessageBox_Async(void) {  // 0x858589
  COROUTINE_BEGIN(coroutine_state_4, 0);
  PspRenderer_ClearButtonIcon(0);
  do {
    COROUTINE_AWAIT(1, WaitForNMI_NoUpdate_Async());
    UpdateRefillStationHud();
    HandleMusicQueue();
    HandleSoundEffects();
    PspRenderer_SetMessageFade(message_box_animation_y_radius * 255u / 0x1800u);
    message_box_animation_y_radius -= 512;
  } while ((message_box_animation_y_radius & 0x8000) == 0);
  COROUTINE_END(0);
}

CoroutineRet DisplayMessageBox_Async(uint16 a) {  // 0x858080
  COROUTINE_BEGIN(coroutine_state_3, 0)
  message_box_index = a;
  CancelSoundEffects();
  InitializePpuForMessageBoxes();
  ClearMessageBoxBg3Tilemap();
  InitializeMessageBox();
  COROUTINE_AWAIT(2, OpenMessageBox_Async());
  COROUTINE_AWAIT(3, HandleMessageBoxInteraction_Async());
  if (message_box_index == 28 && save_confirmation_selection != 2)
    PspRenderer_SetMessageDimHold(true);
  COROUTINE_AWAIT(4, CloseMessageBox_Async());

  if (message_box_index == 28 && save_confirmation_selection != 2) {
    message_box_index = 24;
    ClearMessageBoxBg3Tilemap();
    QueueSfx1_Max6(0x2E);

    my_counter = 160;
    do {
      HandleMusicQueue();
      HandleSoundEffects();
      COROUTINE_AWAIT(8, WaitForNMI_NoUpdate_Async());
    } while (--my_counter);

    InitializeMessageBox();
    COROUTINE_AWAIT(5, OpenMessageBox_Async());
    COROUTINE_AWAIT(6, HandleMessageBoxInteraction_Async());
    COROUTINE_AWAIT(7, CloseMessageBox_Async());
    message_box_index = 28;
  }
  PspRenderer_SetMessageDimHold(false);
  ClearMessageBoxBg3Tilemap();
  RestorePpuForMessageBox();
  QueueSamusMovementSfx();

  if (message_box_index == 20) {
    game_state = kGameState_12_Pausing;
  }
  COROUTINE_END(0);
  //  else if (message_box_index == 28 || message_box_index == 23) {
  //    return save_confirmation_selection;
  //  }
  //  return 0;
}

int DisplayMessageBox_Poll(uint16 a) {
  if (a == message_box_index) {
    message_box_index = 0;
    return save_confirmation_selection;
  }
  queued_message_box_index = a;
  return -1;
}

void DisplayMessageBox(uint16 a) {  // 0x858080
  queued_message_box_index = a;
}
