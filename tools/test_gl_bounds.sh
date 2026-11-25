#!/bin/sh
# Host test of src/psp/psp_gl_guard.h against the real PSPSDK libgu sources.
#   PSPSDK_SRC=/path/to/pspsdk/src  tools/test_gl_bounds.sh      (the directory that contains gu/ and ge/)
# Needs a 64-bit host cc (list buffers are mapped below 4 GB with MAP_32BIT). It does not need a PSP toolchain.
set -e
cd "$(dirname "$0")/.."
SDK=${PSPSDK_SRC:-}
if [ -z "$SDK" ] || [ ! -f "$SDK/gu/guInternal.h" ] || [ ! -f "$SDK/ge/pspge.h" ]; then
  echo "test_gl_bounds: set PSPSDK_SRC to the PSPSDK source directory (it must contain gu/guInternal.h and ge/pspge.h)" >&2
  exit 2
fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
for f in "$SDK"/gu/*.c; do
  b=$(basename "$f")
  case "$b" in sceGuInit.c|sceGuTerm.c|sceGuDisplay.c|sceGuSwapBuffers.c|sceGuDispBuffer.c|vram.c) continue ;; esac
  # the only PSP-specific address trick: sceGuStart maps the list uncached (| 0x40000000); the host has no such alias
  sed 's/| *0x40000000//' "$f" > "$T/$b"
done
INC="-Itools/gl_host_stub -I$SDK/gu -I$SDK/ge -I$SDK/display -I."
for f in "$T"/*.c; do
  cc -O1 -w -std=gnu11 $INC -c "$f" -o "${f%.c}.o"
done
cc -O1 -g -Wall -Wextra -Wno-unused-function -std=gnu11 $INC tools/test_gl_bounds.c "$T"/*.o -o "$T/test_gl_bounds"
"$T/test_gl_bounds"
python3 tools/check_gl_guard.py "$SDK"
