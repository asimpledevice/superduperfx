# Red mailbox test ROM: rspq's mailbox and doorbell, end to end, on real
# rspq (see red_main.c). Results on screen and in the debug log.
BUILD_DIR = build
SOURCE_DIR = .
include ../libdragon.mk

N64_CFLAGS += -I../../runner/src -Wno-error

all: redtest.z64

$(BUILD_DIR)/redtest.elf: $(BUILD_DIR)/red_main.o $(BUILD_DIR)/rsp_red_a.o $(BUILD_DIR)/rsp_red_b.o

redtest.z64: N64_ROM_TITLE = "RED MAILBOX TEST"
redtest.z64: $(BUILD_DIR)/redtest.elf

clean:
	rm -rf $(BUILD_DIR) redtest.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
