# sm-psp

Early version. It has bugs and the code is messy.

For building instructions, see: [BUILDING.md](https://github.com/hulilikii/sm-psp/blob/main/BUILDING.md)

To run on PSP:\
Put `sm.smc` (sha1 hash da957f0d63d14cb441d215462904c4fa8519c613) in the same folder as the EBOOT.

Optional:\
Place sm MSU-1 PCM files inside a folder named `audio` in the same folder as the EBOOT. Without pcm files it will play the original audio tracks.

Changes:\
Button icons redrawn to represent PSP controls, all action buttons are mappable including to `ANGLE UP` and `ANGLE DOWN`, `dash` button is a toggle on/off with xray moved to this routine, `ITEM SELECT` only selects item and `ITEM CANCEL` toggles selected item on/off, HUD is moved to the side black bars with added `SPEEDBOOSTER` icon to indicate toggle state, all message boxes are redrawn by PSP to allow button and text changes, gameplay area keeps its original 4:3 ratio but is scaled to fit the display better.

Credits:\
[snesrev](https://github.com/snesrev/sm) - sm decompliation,\
[z2442](https://github.com/z2442/zelda3-PSP) - Zelda3 PSP port,\
[PJBoy](https://patrickjohnston.org/bank/index.html) - sm bank logs,\
[mcidclan](https://github.com/mcidclan) - Media engine implementation,\
[mlarouche](https://github.com/mlarouche/SuperMetroid-MSU1) - MSU-1,\
[NimbleBeasts](https://nimblebeastscollective.itch.io/magosfonts) - mago pixel font,\
[omegaserpent](https://www.deviantart.com/omegaserpent/gallery) - home screen background
