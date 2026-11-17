# Audio split on the N64: tests/audio/echo_in_place_test.c (the host's random
# APU script) built with the microcode S-DSP back end (RSPDSP_STREAM). Run it
# in ares (debug log) and compare its lines with the host reference's:
#   make EIP_STEPS=3000 EIP_EVERY=50 [EIP_BASIC=1]
#   host: echo_in_place_ref built with the same EIP_BASIC, args STEPS EVERY.
BUILD_DIR = build
SOURCE_DIR = ..
include ../../libdragon.mk

ROOT = ../../..
RUNNER = $(ROOT)/runner/src
EIP_STEPS ?= 3000
EIP_EVERY ?= 50
EIP_BASIC ?= 1
DEFS = -DSNESRECOMP_APU_NATIVE=1 -DSNESRECOMP_APU_DEFER_DSP=1 -DSNESRECOMP_DSP_FRONTEND=1 \
	-DSNESRECOMP_ECHO_IN_PLACE=1 -DSNESRECOMP_DSP_RECORDS=1 -DSNESRECOMP_DSP_STREAM=1 -DRSPDSP_BLOCKS=1 \
	-DN64_SPLITTEST=1 -DEIP_STEPS=$(EIP_STEPS) -DEIP_EVERY=$(EIP_EVERY) -DEIP_BASIC=$(EIP_BASIC)
N64_CFLAGS += -I.. -I../../src -I$(RUNNER) -I$(RUNNER)/snes -I../../include -DSNESRECOMP_TRACE=0 \
	-DSNESRECOMP_AUDIO_TRACE_HOOKS=0 -include ../../include/n64_compat.h -Wno-error $(DEFS) -O2
RSP_DEFS = -DRSPDSP_BLOCKS=1 -DRSPDSP_STREAM=1

all: splittest.z64

$(BUILD_DIR)/main.o: $(ROOT)/tests/audio/echo_in_place_test.c FORCE
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/snesdsp_rsp.o: ../snesdsp_rsp.c FORCE
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/red.o: ../../src/red.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
SRCS = spc.c dsp.c dsp_frontend.c dsp_shadow.c audio_shadow.c
OBJS = $(addprefix $(BUILD_DIR)/runner/,$(SRCS:.c=.o)) $(BUILD_DIR)/runner/audio_trace.o
$(BUILD_DIR)/runner/%.o: $(RUNNER)/snes/%.c FORCE
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/runner/audio_trace.o: $(RUNNER)/audio_trace.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<
$(BUILD_DIR)/n64_compat.o: ../../src/n64_compat.c
	@mkdir -p $(dir $@)
	$(N64_CC) -c $(CFLAGS) $(N64_CFLAGS) -o $@ $<

$(BUILD_DIR)/splittest.elf: $(BUILD_DIR)/n64_compat.o $(BUILD_DIR)/main.o $(BUILD_DIR)/snesdsp_rsp.o \
	$(BUILD_DIR)/red.o $(BUILD_DIR)/rsp_snesdsp.o $(BUILD_DIR)/rsp_snesdsp_ne.o $(OBJS)

splittest.z64: N64_ROM_TITLE = "Audio split test"
$(BUILD_DIR)/rsp_snesdsp.o $(BUILD_DIR)/rsp_snesdsp_ne.o: RSPASFLAGS += $(RSP_DEFS)
$(BUILD_DIR)/rsp_snesdsp_ne.o: ../rsp_snesdsp.S

clean:
	rm -rf $(BUILD_DIR) splittest.z64
FORCE:
-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean FORCE
