# Scene builder test ROM: the RSP's background command (rsp_scene.S,
# SceneCmd_Bg) drawn by the RDP, against the C reference (n64_ppu_scene.c)
# drawn on the CPU, for random VRAM, scroll, tilemap layouts, 8x8 and 16x16
# tiles, row ranges and spans.
BUILD_DIR = build
SOURCE_DIR = ..
include ../../libdragon.mk

N64_CFLAGS += -I.. -I../../include -Wno-error -DN64PPU_SCENE_RSP=1 $(SCENETEST_FLAGS)

all: scenetest.z64

$(BUILD_DIR)/scenetest.elf: $(BUILD_DIR)/test_main.o $(BUILD_DIR)/tile_cache.o \
	$(BUILD_DIR)/tile_cache_ref.o $(BUILD_DIR)/rsp_tile.o $(BUILD_DIR)/rsp_scene.o \
	$(BUILD_DIR)/scene_rsp.o $(BUILD_DIR)/n64_ppu_scene.o

$(BUILD_DIR)/test_main.o: test_main.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/n64_ppu_scene.o: ../../src/n64_ppu_scene.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<

scenetest.z64: N64_ROM_TITLE = "SCENE TEST"
scenetest.z64: $(BUILD_DIR)/scenetest.elf
$(BUILD_DIR)/rsp_tile.o: ../rsp_tile_conv.inc ../tile_cache_layout.h
$(BUILD_DIR)/rsp_scene.o: ../scene_layout.h

clean:
	rm -rf $(BUILD_DIR) scenetest.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
