.DEFAULT_GOAL := all

TARGET := smpsp

PSPSDK := $(shell psp-config --pspsdk-path)

SRCS := $(filter-out src/opengl.c,$(wildcard src/*.c src/snes/*.c))
SRCS += src/psp/psp_renderer.c
SRCS += src/psp/psp_input.c

OBJS := $(SRCS:%.c=%.o)
OBJS += build/embedded_rom.o

build/embedded_rom.o: sm.smc
	@mkdir -p build
	psp-ld -r -b binary -o $@ $<

CFLAGS := -O2 -G0 -fno-strict-aliasing -I.
CFLAGS += -DSYSTEM_VOLUME_MIXER_AVAILABLE=0

# Optional profiling build: make PSP_PROFILE=1
PSP_PROFILE ?= 0
ifeq ($(PSP_PROFILE),1)
CFLAGS += -DPSP_PROFILE
endif

PSP_PPU_VALIDATE_JOURNAL ?= 0
ifeq ($(PSP_PPU_VALIDATE_JOURNAL),1)
CFLAGS += -DPSP_PPU_VALIDATE_JOURNAL -DPSP_PROFILE
endif

# Debug-only tile atlas validation: make PSP_ATLAS_VALIDATE=1
PSP_ATLAS_VALIDATE ?= 0
ifeq ($(PSP_ATLAS_VALIDATE),1)
CFLAGS += -DPSP_ATLAS_VALIDATE -DPSP_PROFILE
endif

# PSP-only build. PSPSDK supplies its standard libraries through build.mak.
LIBS := -lpspaudiolib \
        -lpspaudio \
        -lpsppower \
        -lpspgu \
        -lm

BUILD_PRX := 1

EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := Super Metroid

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


