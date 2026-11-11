# RSP SPC700 test ROM: compares the RSP core with the C core (spc.c) on
# random states and programs; reports over the debug log (ares, headless).
BUILD_DIR = build
SOURCE_DIR = ..
include $(N64_INST)/include/n64.mk

RUNNER = ../../../runner/src
SPCRSP_TESTS ?= 2000
N64_CFLAGS += -I.. -I$(BUILD_DIR) -I$(RUNNER) -I$(RUNNER)/snes -I../../include -DSNESRECOMP_TRACE=0 \
	-DSNESRECOMP_APU_DEFER_DSP=0 -DSNESRECOMP_COMPACT_INTERP=1 -DSPCRSP_TESTS=$(SPCRSP_TESTS) \
	-include ../../include/n64_compat.h -Wno-error $(TEST_CFLAGS)

all: rspspc_test.z64

$(BUILD_DIR)/test_main.o: test_main.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/test_ref.o: test_ref.c $(BUILD_DIR)/rsp_spc_tables.inc
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/spc_rsp_run.o: ../spc_rsp_run.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/spc.o: $(RUNNER)/snes/spc.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -O2 -o $@ $<
$(BUILD_DIR)/n64_compat.o: ../../src/n64_compat.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/rsp_spc_tables.inc: ../gen_tables.py $(RUNNER)/snes/spc.c
	@mkdir -p $(dir $@)
	python3 ../gen_tables.py $(RUNNER)/snes/spc.c $@ $(BUILD_DIR)/rsp_spc_traps.h
$(BUILD_DIR)/rsp_spc.o: $(BUILD_DIR)/rsp_spc_tables.inc
$(BUILD_DIR)/rsp_spc.o: RSPASFLAGS += -I$(BUILD_DIR)

$(BUILD_DIR)/rspspc_test.elf: $(BUILD_DIR)/n64_compat.o $(BUILD_DIR)/test_main.o $(BUILD_DIR)/test_ref.o $(BUILD_DIR)/spc_rsp_run.o $(BUILD_DIR)/spc_rsp.o $(BUILD_DIR)/rsp_spc.o $(BUILD_DIR)/spc.o

rspspc_test.z64: N64_ROM_TITLE = "RSP SPC test"

clean:
	rm -rf $(BUILD_DIR) rspspc_test.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
