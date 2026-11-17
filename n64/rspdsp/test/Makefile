# RSP S-DSP test ROM: compares the RSP microcode against the C DSP on random
# states and reports over the debug log (run headless with ares_run.sh).
BUILD_DIR = build
SOURCE_DIR = ..
include ../../libdragon.mk

RUNNER = ../../../runner/src
N64_CFLAGS += -I.. -I$(RUNNER) -I$(RUNNER)/snes -I../../include -DSNESRECOMP_TRACE=0 -DSNESRECOMP_AUDIO_TRACE_HOOKS=0 -include ../../include/n64_compat.h -Wno-error $(TEST_CFLAGS)

all: rspdsp_test.z64

$(BUILD_DIR)/test_main.o: test_main.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<

DSP_SRCS = dsp.c dsp_frontend.c dsp_shadow.c audio_shadow.c
DSP_OBJS = $(addprefix $(BUILD_DIR)/runner/,$(DSP_SRCS:.c=.o)) $(BUILD_DIR)/runner/audio_trace.o
$(BUILD_DIR)/runner/%.o: $(RUNNER)/snes/%.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -O2 -o $@ $<
$(BUILD_DIR)/runner/audio_trace.o: $(RUNNER)/audio_trace.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -O2 -o $@ $<

$(BUILD_DIR)/n64_compat.o: ../../src/n64_compat.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<

# The voice-major variant (rsp_snesdsp_vm.S) takes the pitch-modulated
# batches; TEST_CFLAGS=-DRSPDSP_VM_ALL runs every batch on it.
RSPDSP_VM ?= 1
N64_CFLAGS += -DRSPDSP_VM=$(RSPDSP_VM)
$(BUILD_DIR)/rspdsp_test.elf: $(BUILD_DIR)/n64_compat.o $(BUILD_DIR)/test_main.o $(BUILD_DIR)/snesdsp_rsp.o $(BUILD_DIR)/rsp_snesdsp.o $(BUILD_DIR)/rsp_snesdsp_ne.o $(BUILD_DIR)/rsp_snesdsp_vm.o $(DSP_OBJS)

rspdsp_test.z64: N64_ROM_TITLE = "RSP DSP test"
$(BUILD_DIR)/rsp_snesdsp.o $(BUILD_DIR)/rsp_snesdsp_ne.o $(BUILD_DIR)/rsp_snesdsp_vm.o: RSPASFLAGS += $(RSP_DEFS)
$(BUILD_DIR)/rsp_snesdsp_ne.o $(BUILD_DIR)/rsp_snesdsp_vm.o: ../rsp_snesdsp.S ../snesdsp_rsp_layout.h

clean:
	rm -rf $(BUILD_DIR) rspdsp_test.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
