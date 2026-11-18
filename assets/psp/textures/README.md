# PSP texture source PNGs

These are the editable RGBA8 source images used to build the native PSP UI textures.
The Makefile runs `tools/gen_psp_textures.py` and writes generated C headers and message
RGBA payloads under `build/generated/`; generated files are not edited by hand.

## Required files and dimensions

- `buttons/`: circle, cross, square, triangle (16x16); l, r, select (32x16);
  dpad_ud, dpad_lr, dpad_lrud (16x16).
- `hud/`: reserve_on, reserve_off (32x16); speed_selected, speed_unselected (16x16).
- `message/`: font (256x256); grapple, powerbomb, supermissile, xray (16x16);
  missile (32x16; visible art uses the first 24 pixels).

Use non-interlaced 8-bit RGB or RGBA PNGs. 
the generator preserves all four channels. 
