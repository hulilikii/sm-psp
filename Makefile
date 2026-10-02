TARGET := smpsp

PSPSDK := $(shell psp-config --pspsdk-path)

SRCS := $(filter-out src/opengl.c,$(wildcard src/*.c src/snes/*.c))
SRCS += src/platform/psp/psp_renderer.c
SRCS += src/platform/psp/psp_input.c

OBJS := $(SRCS:%.c=%.o)

CFLAGS := -O2 -G0 -fno-strict-aliasing -I.
CFLAGS += -DSYSTEM_VOLUME_MIXER_AVAILABLE=0

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

.PHONY: all package cleanbuild

all: $(TARGET).prx EBOOT.PBP

package: EBOOT.PBP
	@mkdir -p  Build/SMPSP
	cp EBOOT.PBP Build/SMPSP/
	cp sm.smc Build/SMPSP/
	
cleanbuild:
	rm -rf Build
