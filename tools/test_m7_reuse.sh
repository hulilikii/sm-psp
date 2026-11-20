#!/bin/sh
# Host test for Gate 5t. Run from the repository root. Needs a host C compiler.
set -e
R=src/psp/psp_renderer.c
a=$(grep -n '^/\* One mode 7 row of the image path' $R | cut -d: -f1)
b=$(grep -n '^/\* Mode 7 frame as textured quads' $R | cut -d: -f1)
sed -n "${a},$((b-1))p" $R > /tmp/m7_extract.inc
for LOG in 1 0; do
  ${CC:-cc} -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -DSM_PSP_LOG=$LOG -o /tmp/test_m7_reuse_$LOG tools/test_m7_reuse.c
  echo "SM_PSP_LOG=$LOG"; /tmp/test_m7_reuse_$LOG
done
echo "negative control (reuse without the stale flag must fail):"; /tmp/test_m7_reuse_1 --negative
