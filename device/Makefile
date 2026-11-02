# zimwiki for the WikiReader (Epson S1C33E07, grifo kernel)
#
#   make WR=/path/to/wikireader        # checkout of github.com/wikireader/wikireader
#
# Needs the C33 toolchain from that tree (make -C $WR toolchain, or the
# build notes in ../README.md) and grifo + mini-libc built:
#   make -C $WR/samo-lib/mini-libc; make -C $WR/samo-lib/fatfs
#   make -C $WR/samo-lib/drivers;  make -C $WR/samo-lib/grifo
#
# Produces build/init.app.  Put it on the card with grifo as kernel.elf.

WR      ?= $(HOME)/wikireader
TC      ?= $(WR)/host-tools/toolchain-install/bin
CC      := $(TC)/c33-epson-elf-gcc
LD      := $(TC)/c33-epson-elf-ld
SIZE    := $(TC)/c33-epson-elf-size
NM      := $(TC)/c33-epson-elf-nm
SAMO    := $(WR)/samo-lib
TOP     := ..

INCS    := -Iinclude -I$(TOP)/src -I$(TOP)/fonts -I$(TOP)/third_party/zstd \
           -I$(TOP)/third_party/xz -I$(SAMO)/grifo/include -I$(SAMO)/mini-libc/include
BASE    := -std=gnu99 -mlong-calls -fno-builtin -mc33pe -MMD -Wall -Wno-unused-parameter \
           -Wno-sign-compare $(INCS)
# size for most of the program, speed for the decoders and drawing
CFLAGS  := $(BASE) -Os
FAST    := $(BASE) -O2
ZSTD    := -DMEM_FORCE_MEMORY_ACCESS=3 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_DISABLE_ASM -DZSTD_NO_INTRINSICS \
           -DZSTD_NO_TRACE -DZSTD_STRIP_ERROR_STRINGS -DXXH_NO_XXH3 -w

SLOW_SRC := src/app.c src/library.c src/fat.c src/index.c src/fold.c \
            src/tex.c fonts/fonts.c device/plat_grifo.c
FAST_SRC := src/zim.c src/blockdev.c src/font.c src/gfx.c src/html.c src/layout.c \
            third_party/xz/xz_crc32.c third_party/xz/xz_dec_stream.c third_party/xz/xz_dec_lzma2.c
ZSTD_SRC := third_party/zstd/zstddeclib.c

OBJS := $(patsubst %.c,build/%.o,$(SLOW_SRC) $(FAST_SRC) $(ZSTD_SRC))

LIBS := $(SAMO)/grifo/lib/libgrifo.a $(SAMO)/mini-libc/lib/libc.a \
        $(shell $(CC) -print-libgcc-file-name 2>/dev/null)
LDS  := $(SAMO)/grifo/lds/application.lds

all: build/init.app

build/third_party/zstd/%.o: $(TOP)/third_party/zstd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(subst -fno-builtin,,$(FAST)) $(ZSTD) -c $< -o $@

build/%.o: $(TOP)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(if $(filter $*.c,$(FAST_SRC)),$(FAST),$(CFLAGS)) $(if $(findstring third_party,$*),-w) -c $< -o $@

# init.elf keeps symbols (for c33emu and debugging); init.app is stripped
build/init.elf: $(OBJS) $(LDS)
	$(LD) -o $@ -e grifo_main -static --no-gc-sections -N $(OBJS) \
	      --start-group $(LIBS) --end-group -T $(LDS) -Map build/init.map

build/init.app: build/init.elf
	$(TC)/c33-epson-elf-strip -o $@ $<
	$(SIZE) $@

-include $(OBJS:.o=.d)

# everything to copy to the root of the SD card (besides the wiki files)
card: build/init.app
	mkdir -p build/card
	cp build/init.app build/card/init.app
	cp $(SAMO)/grifo/grifo.elf build/card/kernel.elf
	cp $(TOP)/WIKIFONT.BIN build/card/ 2>/dev/null || true

clean:
	rm -rf build

.PHONY: all card clean
