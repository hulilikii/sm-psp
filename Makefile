TARGET := smpsp

PSPSDK := $(shell psp-config --pspsdk-path)

SRCS := $(filter-out src/opengl.c,$(wildcard src/*.c src/snes/*.c))
SRCS += src/platform/psp/psp_renderer.c
SRCS += src/platform/psp/psp_input.c
PSP_DEBUG_LOGGING ?= 1
ifeq ($(PSP_DEBUG_LOGGING),1)
SRCS += src/platform/psp/psp_debug_log.c
endif

OBJS := $(SRCS:%.c=%.o)
OBJS += build/embedded_rom.o

build/embedded_rom.o: sm.smc
	@mkdir -p build
	psp-ld -r -b binary -o $@ $<

CFLAGS := -O2 -G0 -fno-strict-aliasing -I.
CFLAGS += -DSYSTEM_VOLUME_MIXER_AVAILABLE=0


ifeq ($(PSP_DEBUG_LOGGING),1)
PSP_DEBUG_BUILD_ID := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)-$(shell date +%Y%m%d_%H%M%S)
CFLAGS += -DPSP_DEBUG_LOGGING
CFLAGS += -DPSP_DEBUG_BUILD_ID=\"$(PSP_DEBUG_BUILD_ID)\"
endif

# PSP-only build. PSPSDK supplies its standard libraries through build.mak.
LIBS := -lpspaudiolib \
        -lpspaudio \
        -lpsppower \
        -lpspgu \
        -lm

BUILD_PRX := 1

EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE := Super Metroid PSP

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


