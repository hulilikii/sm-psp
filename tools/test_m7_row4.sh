#!/bin/sh
# Host test for the four-pixel mode 7 row loop (Gate 5v). Run from the repository root.
set -e
R=src/psp/psp_renderer.c
a=$(grep -n '^/\* One mode 7 row of the image path' $R | cut -d: -f1)
b=$(grep -n "^/\* The part of a row's state that M7RenderRow reads" $R | cut -d: -f1)
sed -n "${a},$((b-1))p" $R > /tmp/m7row_extract.inc
${CC:-cc} -O2 -Wall -Wextra -Wno-unused-function -o /tmp/test_m7_row4 tools/test_m7_row4.c
/tmp/test_m7_row4
