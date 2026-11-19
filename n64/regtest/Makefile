# CPU register hardware test ROM (see regtest_main.c). Results on screen,
# made to be photographed, and in the debug log.
BUILD_DIR = build
SOURCE_DIR = .
include ../libdragon.mk

N64_CFLAGS += -Wno-error

all: regtest.z64

$(BUILD_DIR)/regtest.elf: $(BUILD_DIR)/regtest_main.o

regtest.z64: N64_ROM_TITLE = "CPU REG HW TEST"
regtest.z64: $(BUILD_DIR)/regtest.elf

clean:
	rm -rf $(BUILD_DIR) regtest.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
