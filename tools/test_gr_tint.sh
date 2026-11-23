#!/bin/sh
# Host test for the Gate 5x Gouraud tint planner (GrFit / GrPlan). Run from the repository root.
set -e
R=src/psp/psp_renderer.c
a=$(grep -n '^/\* Gate 5x: Gouraud tint. A segment' $R | cut -d: -f1)
b=$(grep -n '^static uint32_t g_gr_vn, ' $R | cut -d: -f1)
sed -n "${a},${b}p" $R > /tmp/gr_data.inc
a=$(grep -n '^#ifndef SM_PSP_GR_RADIUS$' $R | cut -d: -f1)
sed -n "${a},$((a+14))p" $R > /tmp/gr_cfg.inc
a=$(grep -n '^ */\* Gate 5x: Gouraud tint planning' $R | cut -d: -f1)
b=$(grep -n '^//software patch' $R | cut -d: -f1)
sed -n "${a},$((b-1))p" $R > /tmp/gr_plan.inc
${CC:-cc} ${TESTFLAGS} -O2 -Wall -Wextra -Wno-unused-function -o /tmp/test_gr_tint tools/test_gr_tint.c -lm
/tmp/test_gr_tint
