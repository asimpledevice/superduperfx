# RSP SPC700 test ROM: compares the RSP core with the C core (spc.c) on
# random states and programs; reports over the debug log (ares, headless).
BUILD_DIR = build
SOURCE_DIR = ..
include $(N64_INST)/include/n64.mk

RUNNER = ../../../runner/src
SPCRSP_TESTS ?= 2000
DEFER ?= 1   # 1: the deferred DSP (production); 0: milestone 2b, a register-file DSP
N64_CFLAGS += -I.. -I$(BUILD_DIR) -I$(RUNNER) -I$(RUNNER)/snes -I../../include -DSNESRECOMP_TRACE=0 \
	-DSNESRECOMP_APU_DEFER_DSP=$(DEFER) -DSNESRECOMP_APU_NATIVE=1 -DSNESRECOMP_AUDIO_TRACE_HOOKS=0 -DSNESRECOMP_COMPACT_INTERP=1 -DSPCRSP_TESTS=$(SPCRSP_TESTS) \
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
$(BUILD_DIR)/apu.o: $(RUNNER)/snes/apu.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -O2 -o $@ $<
DSP_SRCS = dsp.c dsp_shadow.c audio_shadow.c
DSP_OBJS = $(if $(filter 1,$(DEFER)),$(addprefix $(BUILD_DIR)/runner/,$(DSP_SRCS:.c=.o)) $(BUILD_DIR)/runner/audio_trace.o)
$(BUILD_DIR)/runner/%.o: $(RUNNER)/snes/%.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -O2 -o $@ $<
$(BUILD_DIR)/runner/audio_trace.o: $(RUNNER)/audio_trace.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -O2 -o $@ $<
$(BUILD_DIR)/n64_compat.o: ../../src/n64_compat.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/rsp_spc_tables.inc: ../gen_tables.py $(RUNNER)/snes/spc.c
	@mkdir -p $(dir $@)
	python3 ../gen_tables.py $(RUNNER)/snes/spc.c $@ $(BUILD_DIR)/rsp_spc_traps.h
$(BUILD_DIR)/rsp_spc_syms.inc: $(BUILD_DIR)/rsp_spc.o ../gen_syms.py
	python3 ../gen_syms.py $(BUILD_DIR)/rsp_spc.elf $@ $(N64_INST)/bin/mips64-elf-nm
$(BUILD_DIR)/rsp_spc_ext.o: $(BUILD_DIR)/rsp_spc_syms.inc
$(BUILD_DIR)/rsp_spc_ext.o: RSPASFLAGS += -I$(BUILD_DIR)
$(BUILD_DIR)/ext.check: $(BUILD_DIR)/rsp_spc_ext.o $(BUILD_DIR)/rsp_spc.o
	python3 ../gen_syms.py --check $(BUILD_DIR)/rsp_spc.elf $(BUILD_DIR)/rsp_spc_ext.elf $(N64_INST)/bin/mips64-elf-nm
	@touch $@
$(BUILD_DIR)/rsp_spc.o: $(BUILD_DIR)/rsp_spc_tables.inc
$(BUILD_DIR)/rsp_spc.o: RSPASFLAGS += -I$(BUILD_DIR)

$(BUILD_DIR)/rspspc_test.elf: $(BUILD_DIR)/n64_compat.o $(BUILD_DIR)/test_main.o $(BUILD_DIR)/test_ref.o $(BUILD_DIR)/spc_rsp_run.o $(BUILD_DIR)/spc_rsp.o $(BUILD_DIR)/rsp_spc.o $(BUILD_DIR)/rsp_spc_ext.o $(BUILD_DIR)/ext.check $(BUILD_DIR)/spc.o $(BUILD_DIR)/apu.o $(DSP_OBJS)

rspspc_test.z64: N64_ROM_TITLE = "RSP SPC test"

clean:
	rm -rf $(BUILD_DIR) rspspc_test.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
