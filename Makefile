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
ifeq ($(PSP_LOG),1)
SRCS += src/psp/psp_log.c
endif

OBJS := $(SRCS:%.c=%.o)
OBJS += build/embedded_rom.o

build/embedded_rom.o: sm.smc
	@mkdir -p build
	psp-ld -r -b binary -o $@ $<

CFLAGS := -O2 -G0 -fno-strict-aliasing -I.
CFLAGS += -DSYSTEM_VOLUME_MIXER_AVAILABLE=0
CFLAGS += -DSM_PSP_LOG=$(PSP_LOG) -DSM_PSP_LOG_FLUSH_FRAMES=$(PSP_LOG_FLUSH_FRAMES)
CFLAGS += -DSM_PSP_GE_STRIPS=$(PSP_GE_STRIPS)

# PSP-only build. PSPSDK supplies its standard libraries through build.mak.
LIBS := -lpspaudiolib \
        -lpspaudio \
        -lpsppower \
        -lpspgu \
        -lm

BUILD_PRX := 1

EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := Super Metroid

# Objects don't track CFLAGS, so a stamp file forces a rebuild when these change.
BUILD_STAMP := build/flags_log$(PSP_LOG)_flush$(PSP_LOG_FLUSH_FRAMES)_strips$(PSP_GE_STRIPS).stamp
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


