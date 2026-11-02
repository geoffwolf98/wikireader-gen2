# zimwiki host build: tools (zimprep, zimcat), simulators and tests.
# The WikiReader build lives in device/Makefile.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
           -Isrc -Ithird_party/zstd -Ithird_party/xz -Ifonts -MMD -MP
BUILD   := build/host
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

THIRD := third_party/zstd/zstddeclib.c third_party/xz/xz_crc32.c \
         third_party/xz/xz_dec_stream.c third_party/xz/xz_dec_lzma2.c
CORE  := src/zim.c src/fold.c src/index.c
APP   := src/app.c src/library.c src/blockdev.c src/fat.c src/html.c \
         src/layout.c src/font.c src/gfx.c src/tex.c fonts/fonts.c

obj = $(patsubst %.c,$(BUILD)/%.o,$(1))

TOOLS := $(BUILD)/zimprep $(BUILD)/zimcat
TESTS := $(BUILD)/idxtest $(BUILD)/fattest $(BUILD)/textest
SIMS  := $(BUILD)/zimwiki-headless
ifneq ($(SDL_LIBS),)
SIMS  += $(BUILD)/zimwiki-sdl
endif

all: $(TOOLS) $(SIMS) $(TESTS) build/c33emu

build/c33emu: tools/c33emu.c
	@mkdir -p build
	$(CC) -O2 -std=gnu99 -w -o $@ $< -lz

$(BUILD)/idxtest: $(call obj,tests/idxtest.c src/index.c src/fold.c host/hostio.c)
	$(CC) -o $@ $^

$(BUILD)/textest: $(call obj,tests/textest.c src/tex.c)
	$(CC) -o $@ $^

$(BUILD)/fattest: $(call obj,tests/fattest.c src/fat.c src/blockdev.c host/hostio.c)
	$(CC) -o $@ $^

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -DZSTD_LEGACY_SUPPORT=0 -DZSTD_DISABLE_ASM -c $< -o $@

$(BUILD)/host/plat_sdl.o: host/plat_sdl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

fonts/fonts.c fonts/fonts_gen.h: tools/bdf2c.py fonts/fontlist.txt
	python3 tools/bdf2c.py fonts/fontlist.txt fonts/fonts.c fonts/fonts_gen.h

# The fallback font (WIKIFONT.BIN, prebuilt in the source): WenQuanYi CJK
# plus GNU Unifont for the other scripts.  Rebuild with: make wikifont
# (needs: sudo apt install fonts-unifont; pip install freetype-py)
UNIFONT ?= /usr/share/fonts/opentype/unifont/unifont.otf
wikifont: tools/mkwikifont.py tools/bdf2c.py fonts/bdf/textall.bdf
	@test -f $(UNIFONT) || { echo "no $(UNIFONT): sudo apt install fonts-unifont (or make wikifont UNIFONT=/path/unifont.otf)"; exit 1; }
	python3 tools/mkwikifont.py fonts/bdf/textall.bdf $(UNIFONT) build/wikifont.bdf
	python3 tools/bdf2c.py --bin build/wikifont.bdf WIKIFONT.BIN
	cp WIKIFONT.BIN card/WIKIFONT.BIN

src/fold_table.h: tools/mkfold.py
	python3 tools/mkfold.py > $@

$(BUILD)/zimprep: $(call obj,tools/zimprep.c tools/recluster.c $(CORE) $(THIRD) host/hostio.c)
	$(CC) -o $@ $^ -lzstd -lpthread

$(BUILD)/zimcat: $(call obj,tools/zimcat.c $(CORE) $(THIRD) host/hostio.c)
	$(CC) -o $@ $^

$(BUILD)/zimwiki-headless: $(call obj,host/plat_headless.c host/plat_common.c host/hostio.c $(APP) $(CORE) $(THIRD))
	$(CC) -o $@ $^ -lz

$(BUILD)/zimwiki-sdl: $(call obj,host/plat_sdl.c host/plat_common.c host/hostio.c $(APP) $(CORE) $(THIRD))
	$(CC) -o $@ $^ $(SDL_LIBS) -lz

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

test: all
	sh tests/run.sh $(ZIMS)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean wikifont
