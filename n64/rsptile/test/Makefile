# Tile cache test ROM: the RSP's tile and TLUT conversions (rsp_tile.S)
# against the C reference (tile_cache_ref.c) on random VRAM and CGRAM.
BUILD_DIR = build
SOURCE_DIR = ..
include ../../libdragon.mk

N64_CFLAGS += -I.. -Wno-error

all: tiletest.z64

$(BUILD_DIR)/tiletest.elf: $(BUILD_DIR)/test_main.o $(BUILD_DIR)/tile_cache.o \
	$(BUILD_DIR)/tile_cache_ref.o $(BUILD_DIR)/rsp_tile.o

$(BUILD_DIR)/test_main.o: test_main.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<

tiletest.z64: N64_ROM_TITLE = "TILE CACHE TEST"
tiletest.z64: $(BUILD_DIR)/tiletest.elf
$(BUILD_DIR)/rsp_tile.o: ../rsp_tile_conv.inc ../tile_cache_layout.h

clean:
	rm -rf $(BUILD_DIR) tiletest.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
