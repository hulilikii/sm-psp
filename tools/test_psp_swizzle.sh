#!/bin/sh
# Host test for the swizzled atlas addressing (Gate 5u). Run from the repository root.
set -e
R=src/psp/psp_renderer.c
# the renderer must contain exactly the expressions the test checks
grep -qF '((((pt >> 5) << 3) + ((pt & 31) >> 2)) << 7) + (pt & 3) * 4' $R
grep -qF '((((pt >> 6) << 4) + ((pt & 63) >> 2)) << 7) + (pt & 3) * 4' $R
grep -qF '((((t >> 4) << 3) + ((t & 15) >> 1)) << 7) + (t & 1) * 8' $R
${CC:-cc} -O1 -Wall -Wextra -o /tmp/test_psp_swizzle tools/test_psp_swizzle.c
/tmp/test_psp_swizzle
