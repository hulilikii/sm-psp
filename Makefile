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
PSP_SW_PATH ?= -1
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

OBJS := $(SRCS:%.c=%.o)
OBJS += build/embedded_rom.o

build/embedded_rom.o: sm.smc
	@mkdir -p build
	psp-ld -r -b binary -o $@ $<

CFLAGS := -O2 -G0 -fno-strict-aliasing -I.
CFLAGS += -DSYSTEM_VOLUME_MIXER_AVAILABLE=0
CFLAGS += -DSM_PSP_LOG=$(PSP_LOG) -DSM_PSP_LOG_FLUSH_FRAMES=$(PSP_LOG_FLUSH_FRAMES)
CFLAGS += -DSM_PSP_GE_STRIPS=$(PSP_GE_STRIPS) -DSM_PSP_SW_PATH=$(PSP_SW_PATH)
CFLAGS += -DPSP_AUDIO_ENABLED=$(PSP_AUDIO) -DPSP_AUDIO_ME=$(PSP_AUDIO_ME)

# SPC/DSP code runs on the Media Engine, which has no libc/syscalls: an assert()
# that fires there would hang the ME instead of reporting.
src/spc_player.o src/snes/dsp.o: CFLAGS += -DNDEBUG

# PSP-only build. PSPSDK supplies its standard libraries through build.mak.
LIBS :=
# me-core must precede me-core-lib (it references meLibLoadPrx/kcall/kinit).
ifeq ($(PSP_AUDIO_ME),1)
LIBS += -lme-core -lme-core-lib
endif
LIBS := -lpspaudiolib \
        -lpspaudio \
        -lpsppower \
        -lpspgu \
        -lm

BUILD_PRX := 1

EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := スーパーメトロイド

# Objects don't track CFLAGS, so a stamp file forces a rebuild when these change.
BUILD_STAMP := build/flags_log$(PSP_LOG)_flush$(PSP_LOG_FLUSH_FRAMES)_strips$(PSP_GE_STRIPS)_sw$(PSP_SW_PATH)_aud$(PSP_AUDIO)_me$(PSP_AUDIO_ME).stamp
$(BUILD_STAMP):
	@mkdir -p build
	@rm -f build/flags_*.stamp
	@touch $@
$(filter-out build/embedded_rom.o,$(OBJS)): $(BUILD_STAMP)

include $(PSPSDK)/lib/build.mak

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


