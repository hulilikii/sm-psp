// Glow whitelist: what glows beyond the built-in sources (sprites, door caps, lava / acid, rain). Add a line, rebuild; nothing else.
// It is read by PspGlow_ListBoxes (sm_84.c); an entry only matters while its thing is in the room and in view.
//
//   GLOW_PLM(setup, mode)    a PLM (station, machine, any object the room's PLM list draws): the blocks it draws.
//                            setup: its setup routine, fnPlmSetup_* from funcs.h (the 16-bit pointer at the start of the PLM's header).
//   GLOW_ENEMY(init, mode)   an enemy species: its hit box. init: its init routine, fn<Name>_Init from funcs.h (EnemyDef.ai_init).
//
//   mode GLOW_LIGHTS: only the coloured (saturated) pixels glow, at full strength; grey metal and dark pixels do not. Colours are the
//                     palette's own; PSP_GE_GLOW_SAT (0..255) sets how coloured a pixel must be.
//   mode GLOW_ALL:    every pixel of the thing that is brighter than the glow threshold (what a door cap does).
//
// Inside its box the thing is redrawn from the scene's own tiles / sprites (so what lies in front of it hides it), then goes through the
// ordinary bright pass and blur.

// Map station, energy and missile recharge machines (the *Access PLMs are the invisible doorway blocks beside them)
GLOW_PLM(fnPlmSetup_B6D3_MapStation, GLOW_LIGHTS)
GLOW_PLM(fnPlmSetup_PlmB6DF_EnergyStation, GLOW_LIGHTS)
GLOW_PLM(fnPlmSetup_PlmB6EB_EnergyStation, GLOW_LIGHTS)  // the missile machine (PLM B6EB); the setup is named after its neighbour

// Shutters (enemies): the lights on beam and super missile shutters
GLOW_ENEMY(fnTimedShutter_Init, GLOW_LIGHTS)
GLOW_ENEMY(fnShootableShutter_Init, GLOW_LIGHTS)
GLOW_ENEMY(fnHorizontalShootableShutter_Init, GLOW_LIGHTS)
