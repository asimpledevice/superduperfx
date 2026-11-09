# RSP BG test ROM: runs random jobs through rsp_bg.S and compares every row
# with a CPU expansion; reports over the debug log (run headless in ares).
BUILD_DIR = build
SOURCE_DIR = ..
include $(N64_INST)/include/n64.mk

N64_CFLAGS += -I.. -Wno-error $(TEST_CFLAGS)

all: rspbg_test.z64

$(BUILD_DIR)/test_main.o: test_main.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<

$(BUILD_DIR)/rspbg_test.elf: $(BUILD_DIR)/test_main.o $(BUILD_DIR)/bg_rsp.o $(BUILD_DIR)/rsp_bg.o

rspbg_test.z64: N64_ROM_TITLE = "RSP BG test"
$(BUILD_DIR)/rsp_bg.o: RSPASFLAGS += $(RSP_DEFS)
$(BUILD_DIR)/rsp_bg.o: ../rsp_bg_full.inc ../bg_rsp_layout.h

clean:
	rm -rf $(BUILD_DIR) rspbg_test.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
