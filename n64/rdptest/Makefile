# RDP facts test ROM (docs/n64/PPU_DESIGN.md, step 0): texture-rectangle
# flips in copy and 1-cycle mode for RGBA16, CI8 and CI4 textures, checked
# against the expected pixels; then a 256x240 display page.
BUILD_DIR = build
SOURCE_DIR = .
include ../libdragon.mk

N64_CFLAGS += -Wno-error $(RDPTEST_FLAGS)

all: rdptest.z64

$(BUILD_DIR)/rdptest.elf: $(BUILD_DIR)/rdp_main.o

rdptest.z64: N64_ROM_TITLE = "RDP FLIP TEST"
rdptest.z64: $(BUILD_DIR)/rdptest.elf

clean:
	rm -rf $(BUILD_DIR) rdptest.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
