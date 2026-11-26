#!/bin/sh
# Host test for ppu_drawLineScratch (src/snes/ppu.c): BG3 is not drawn, register state is restored. Run from the repository root.
set -e
${CC:-cc} -O1 -w -I. -o /tmp/test_ppu_scratch_bg3 tools/test_ppu_scratch_bg3.c src/snes/ppu.c
/tmp/test_ppu_scratch_bg3
