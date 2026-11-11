.DEFAULT_GOAL := all

TARGET := smpsp

PSPSDK := $(shell psp-config --pspsdk-path)

SRCS := $(filter-out src/opengl.c,$(wildcard src/*.c src/snes/*.c))
SRCS += src/psp/psp_renderer.c
SRCS += src/psp/psp_input.c

# Logging and profiling. PSP_LOG_FLUSH_FRAMES is how often the buffered log is written to the memory stick; 0 flushes every line (survives a crash, but every write stalls the frame).
PSP_LOG ?= 1
PSP_LOG_FLUSH_FRAMES ?= 1800
# PSP_GE_STRIPS=1 draws large 2D rectangles as 64-pixel-wide vertical strips (a PPSSPP docs suggestion). no gain, slightly more CPU, so it is off by default.
PSP_GE_STRIPS ?= 0
# How software frames reach the screen: -1 measures both paths at run time and keeps the faster one (logged), 0 = CPU conversion to a 565 VRAM texture, 1 = GE copies the 8888 frame to VRAM and samples it there.
# Default 1 (measured 4.1 ms vs 5.7 ms). -1 times its calibration with the log clock, so it needs PSP_LOG=1.
PSP_SW_PATH ?= 1
# PSP_GE_WIN=1 draws frames with window-1 masks (BG1-3, OBJ) on the GE, checked against software at run time. 0: they stay in software.
PSP_GE_WIN ?= 1
# PSP_VTX_POOL=1: big vertex buffers are built in a cached pool and written back once. 0: inside the display list.
# Gate 5f hardware A/B: the pool was ~20% slower per vertex (0.45 vs 0.37 us), so it is off by default.
PSP_VTX_POOL ?= 0
# PSP_TRIPLE=1: three frame buffers. Below 60 fps the vblank wait is skipped, so the next frame must not draw into the buffer still on screen. 0: libgu double buffering.
PSP_TRIPLE ?= 1
# PSP_PROBES=1: per-line diagnostic probes (window, mode 7, flicker) in log builds. They cost CPU and skew the profile.
PSP_PROBES ?= 0
# PSP_VRAM_DIRTY=1: copy only the VRAM pages written since the last frame. 0: copy all 64 KB.
PSP_VRAM_DIRTY ?= 1
# PSP_CAP_REUSE=1: a line whose PPU registers equal the previous line's copies that line's snapshot (BG scroll and CGRAM 0 are re-read). Log builds recheck every 16th frame. 0: capture every line in full.
PSP_CAP_REUSE ?= 1
# PSP_VTX16=1: BG and sprite quads use a 10-byte vertex with 16-bit texture coordinates (Gate 5h-c). 0: the 16-byte float vertex.
PSP_VTX16 ?= 1
# PSP_STATS=1: Gate 5i-a statistics (quads, runs, cache probes per layer) in the log. Log builds only; costs about 0.1-0.2 ms per frame, 0 removes it.
PSP_STATS ?= 1
# PSP_CAP_WIN=1: window edge writes ($2126-$2129) no longer count as register changes in ppu_write; a line whose only change is the edges re-derives just its window fields (Gate 5k). 0: they force a full capture, as before.
PSP_CAP_WIN ?= 1
# PSP_BENCH=1: Gate 5i-b boot-time micro-benchmark of layer-image strips, about 1 s at boot (log builds only). Done: off by default.
PSP_BENCH ?= 0
# PSP_BENCH_M7=1: Gate 5r-a boot-time benchmark of GE-native mode 7 rotation (row strips from a 1024x1024 plane), about 3 s at boot (log builds only). Off by default.
PSP_BENCH_M7 ?= 0
# PSP_MOS_GE=1: Gate 5s: the mosaic source image is rendered by the GE (no CPU pixels). 0: previous CPU-built image (MosBuild).
PSP_MOS_GE ?= 1
# PSP_LAYER_IMG: BG2/BG3 layers split into many scroll runs are drawn from a cached 8 bit image (one strip per run).
# 0: tile quads. 1: Gate 5l (32x32 / 64x32 maps). 2: also 32x64 maps (Gate 5m). 3: also skip the image scan when no VRAM page of the map or tiles was written (Gate 5m).
PSP_LAYER_IMG ?= 3
ifeq ($(PSP_LOG),1)
SRCS += src/psp/psp_log.c
endif

# Audio. PSP_AUDIO=1 plays SPC/DSP audio on the CPU; PSP_AUDIO_ME=1 moves SPC+DSP
# to the Media Engine (needs psp-media-engine-custom-core installed: make install).
PSP_AUDIO ?= 1
PSP_AUDIO_ME ?= 1
ifeq ($(PSP_AUDIO_ME),1)
override PSP_AUDIO := 1
SRCS += src/psp/psp_audio_me.c
endif

# ME audio statistics in the log. Only takes effect with PSP_LOG=1 and PSP_AUDIO_ME=1.
PSP_ME_LOG ?= 1
ifeq ($(PSP_LOG)$(PSP_AUDIO_ME)$(PSP_ME_LOG),111)
ME_LOG_ON := 1
else
ME_LOG_ON := 0
endif

OBJS := $(SRCS:%.c=%.o)
OBJS += build/embedded_rom.o

build/embedded_rom.o: sm.smc
	@mkdir -p build
	psp-ld -r -b binary -o $@ $<

CFLAGS := -O2 -G0 -fno-strict-aliasing -I.
CFLAGS += -DSYSTEM_VOLUME_MIXER_AVAILABLE=0
CFLAGS += -DSM_PSP_LOG=$(PSP_LOG) -DSM_PSP_LOG_FLUSH_FRAMES=$(PSP_LOG_FLUSH_FRAMES)
CFLAGS += -DSM_PSP_GE_STRIPS=$(PSP_GE_STRIPS) -DSM_PSP_SW_PATH=$(PSP_SW_PATH) -DSM_PSP_GE_WIN=$(PSP_GE_WIN)
CFLAGS += -DSM_PSP_VTX_POOL=$(PSP_VTX_POOL)
CFLAGS += -DSM_PSP_TRIPLE=$(PSP_TRIPLE)
CFLAGS += -DSM_PSP_PROBES=$(PSP_PROBES) -DSM_PSP_VRAM_DIRTY=$(PSP_VRAM_DIRTY)
CFLAGS += -DSM_PSP_CAP_REUSE=$(PSP_CAP_REUSE)
CFLAGS += -DSM_PSP_VTX16=$(PSP_VTX16)
CFLAGS += -DSM_PSP_STATS=$(PSP_STATS)
CFLAGS += -DSM_PSP_CAP_WIN=$(PSP_CAP_WIN) -DSM_PSP_BENCH=$(PSP_BENCH)
CFLAGS += -DSM_PSP_BENCH_M7=$(PSP_BENCH_M7)
CFLAGS += -DSM_PSP_MOS_GE=$(PSP_MOS_GE)
CFLAGS += -DSM_PSP_LAYER_IMG=$(PSP_LAYER_IMG)
CFLAGS += -DPSP_AUDIO_ENABLED=$(PSP_AUDIO) -DPSP_AUDIO_ME=$(PSP_AUDIO_ME) -DSM_PSP_ME_LOG=$(ME_LOG_ON)

# SPC/DSP code runs on the Media Engine, which has no libc/syscalls: an assert()
# that fires there would hang the ME instead of reporting.
src/spc_player.o src/snes/dsp.o: CFLAGS += -DNDEBUG

# PSP-only build. PSPSDK supplies its standard libraries through build.mak.
LIBS :=
# me-core must precede me-core-lib (it references meLibLoadPrx/kcall/kinit).
ifeq ($(PSP_AUDIO_ME),1)
LIBS += -lme-core -lme-core-lib
endif
LIBS += -lpspaudiolib \
        -lpspaudio \
        -lpsppower \
        -lpspgu \
        -lm

BUILD_PRX := 1

EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := スーパーメトロイド

# XMB artwork and sound packed into EBOOT.PBP (pspsdk build.mak hands these to pack-pbp).
# Each file is only used if it exists, so a missing asset never breaks the build.
PSP_ASSET_DIR := assets/psp
PSP_ASSET_FILES :=
ifneq ($(wildcard $(PSP_ASSET_DIR)/ICON0.PNG),)
PSP_EBOOT_ICON := $(PSP_ASSET_DIR)/ICON0.PNG
PSP_ASSET_FILES += $(PSP_EBOOT_ICON)
endif
ifneq ($(wildcard $(PSP_ASSET_DIR)/PIC1.PNG),)
PSP_EBOOT_PIC1 := $(PSP_ASSET_DIR)/PIC1.PNG
PSP_ASSET_FILES += $(PSP_EBOOT_PIC1)
endif
ifneq ($(wildcard $(PSP_ASSET_DIR)/SND0.AT3),)
PSP_EBOOT_SND0 := $(PSP_ASSET_DIR)/SND0.AT3
PSP_ASSET_FILES += $(PSP_EBOOT_SND0)
endif

# Objects don't track CFLAGS, so a stamp file forces a rebuild when these change.
BUILD_STAMP := build/flags_log$(PSP_LOG)_flush$(PSP_LOG_FLUSH_FRAMES)_strips$(PSP_GE_STRIPS)_sw$(PSP_SW_PATH)_win$(PSP_GE_WIN)_vp$(PSP_VTX_POOL)_tb$(PSP_TRIPLE)_pr$(PSP_PROBES)_vd$(PSP_VRAM_DIRTY)_cr$(PSP_CAP_REUSE)_v16$(PSP_VTX16)_st$(PSP_STATS)_cw$(PSP_CAP_WIN)_bn$(PSP_BENCH)_li$(PSP_LAYER_IMG)_aud$(PSP_AUDIO)_me$(PSP_AUDIO_ME)_melog$(ME_LOG_ON).stamp
$(BUILD_STAMP):
	@mkdir -p build
	@rm -f build/flags_*.stamp
	@touch $@
$(filter-out build/embedded_rom.o,$(OBJS)): $(BUILD_STAMP)

# Its own stamp (not flags_*, which the main stamp rule deletes): toggling PSP_BENCH_M7 rebuilds the renderer.
BENCH_M7_STAMP := build/bench_m7_$(PSP_BENCH_M7).stamp
$(BENCH_M7_STAMP):
	@mkdir -p build
	@rm -f build/bench_m7_*.stamp
	@touch $@
src/psp/psp_renderer.o: $(BENCH_M7_STAMP)

include $(PSPSDK)/lib/build.mak

# Its own stamp (like BENCH_M7_STAMP): toggling PSP_MOS_GE rebuilds the renderer.
MOS_GE_STAMP := build/mos_ge_$(PSP_MOS_GE).stamp
$(MOS_GE_STAMP):
	@mkdir -p build
	@rm -f build/mos_ge_*.stamp
	@touch $@
src/psp/psp_renderer.o: $(MOS_GE_STAMP)

# Rebuild the EBOOT when an asset changes.
EBOOT.PBP: $(PSP_ASSET_FILES)

.PHONY: all package

all: $(TARGET).prx EBOOT.PBP

package: EBOOT.PBP
	@mkdir -p PSP/GAME/SMPSP
	cp EBOOT.PBP PSP/GAME/SMPSP/
	
# Clean the build
clean: clean_obj clean_data
clean_obj:
	@$(RM) $(OBJS) $(TARGET).elf $(TARGET).prx EBOOT.PBP PARAM.SFO
clean_data:
	@rm -rf PSP/ build/


