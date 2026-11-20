#!/bin/sh
# Host test for the block-window mode 7 path (Gate 5w). Run from the repository root; needs a host C compiler and libm.
set -e
R=src/psp/psp_renderer.c
a=$(grep -n '^/\* One mode 7 row of the image path' $R | cut -d: -f1)
b=$(grep -n "^/\* The part of a row's state that M7RenderRow reads" $R | cut -d: -f1)
sed -n "${a},$((b-1))p" $R > /tmp/m7row_extract.inc
a=$(grep -n '^/\* M7BLK-BEGIN \*/' $R | cut -d: -f1)
b=$(grep -n '^/\* M7BLK-END \*/' $R | cut -d: -f1)
sed -n "${a},${b}p" $R > /tmp/m7blk_extract.inc
${CC:-cc} -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -o /tmp/test_m7_blk tools/test_m7_blk.c -lm
/tmp/test_m7_blk
echo "negative control:"; /tmp/test_m7_blk --negative
