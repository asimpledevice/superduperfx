# RSP S-DSP test ROM: compares the RSP microcode against the C DSP on random
# states and reports over the debug log (run headless with ares_run.sh).
BUILD_DIR = build
SOURCE_DIR = ..
include $(N64_INST)/include/n64.mk

RUNNER = ../../../runner/src
N64_CFLAGS += -I.. -I$(RUNNER) -I$(RUNNER)/snes -I../../include -DSNESRECOMP_TRACE=0 -Wno-error

all: rspdsp_test.z64

$(BUILD_DIR)/test_main.o: test_main.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<

$(BUILD_DIR)/rspdsp_test.elf: $(BUILD_DIR)/test_main.o $(BUILD_DIR)/snesdsp_rsp.o $(BUILD_DIR)/rsp_snesdsp.o

rspdsp_test.z64: N64_ROM_TITLE = "RSP DSP test"

clean:
	rm -rf $(BUILD_DIR) rspdsp_test.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
