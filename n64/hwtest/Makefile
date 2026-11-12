# CPU/RSP signalling hardware test ROM (see hwtest_main.c). Results on
# screen, made to be photographed, and in the debug log.
BUILD_DIR = build
SOURCE_DIR = .
include $(N64_INST)/include/n64.mk

N64_CFLAGS += -Wno-error

all: hwtest.z64

$(BUILD_DIR)/rsp_hwtest.o: rsp_hwtest.S hwtest_layout.h
$(BUILD_DIR)/hwtest.elf: $(BUILD_DIR)/hwtest_main.o $(BUILD_DIR)/rsp_hwtest.o

hwtest.z64: N64_ROM_TITLE = "RSP CPU HW TEST"
hwtest.z64: $(BUILD_DIR)/hwtest.elf

clean:
	rm -rf $(BUILD_DIR) hwtest.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
