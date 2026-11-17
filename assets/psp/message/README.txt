PSP message UI texture payloads

font.rgba: 256x256 RGBA8888 BMFont atlas; RGB white, alpha glyph coverage.
Item payloads: RGBA8888, opaque alpha, power-of-two row stride.
  grapple.rgba      16x16
  missile.rgba      32x16 (source art 24x16, padded to 32)
  powerbomb.rgba    16x16
  supermissile.rgba 16x16
  xray.rgba         16x16

The corresponding .S file embeds these raw files in the PRX. Do not change
the binary files without regenerating the assembly object and checking sizes.
Font metrics are in src/psp/psp_message_font.h.
