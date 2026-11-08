# N64 build of the snesrecomp runtime (libdragon).
#
#   make ROM=/path/to/game.sfc                      # interpreter only
#   make ROM=game.sfc GEN_DIR=gen GEN_INC=recomp     # plus generated code
#
# GEN_DIR is a v2_emit output directory; GEN_INC is the cfg directory that
# holds its funcs.h. Generated code is compiled with -Os.
#
# The ROM is copied into the cartridge filesystem (build/filesystem) at build
# time. Nothing ROM-derived is written into the source tree.

BUILD_DIR = build
SOURCE_DIR = src
include $(N64_INST)/include/n64.mk

RUNNER = ../runner/src

# Shared runtime units. Desktop-only units are left out: launcher, keybinds,
# OSD, save-state menu, netplay, Lua, debug server, trace/replay writers,
# host report, MSU-1 and render workers.
RUNNER_SRCS = \
	common_cpu_infra.c common_rtl.c widescreen.c recomp_hw.c framedump.c \
	state_dump.c host_paths.c rom_image_verify.c rom_identity.c sha1.c \
	crc32.c snapshot_guard.c sha256.c snes_overlay_draw.c snes_runahead.c \
	snes_rewind.c cpu_state.c program_module.c rom_patch.c \
	generic_frame_driver.c beam_frame_driver.c beam_frame_driver_state.c \
	content_variant.c variant_selector.c cpu_trace.c audio_trace.c \
	ppu_dma_trace.c execution_mode.c mod_audio.c host_mesh.c cpu_fast_map.c \
	host_mesh_builder.c guarded_patch.c util.c \
	snes/apu.c snes/cart.c snes/cpu.c snes/dma.c snes/dsp.c snes/dsp1.c \
	snes/dsp1_hle.c snes/joypad.c snes/audio_shadow.c snes/dsp_shadow.c \
	snes/color_lut.c snes/ppu.c snes/hd_jobs.c snes/ppu_legacy.c snes/sa1.c \
	snes/sdd1.c snes/ws_shadow.c snes/snes.c snes/snes_other.c snes/spc.c \
	snes/superfx.c snes/interp816.c snes/tier2_capture.c snes/interp_bridge.c \
	snes/cx4.c

HOST_SRCS = n64_host.c n64_platform.c n64_compat.c n64_stubs.c \
	n64_ppu_build.c n64_ppu_draw.c

OBJS = $(addprefix $(BUILD_DIR)/runner/,$(RUNNER_SRCS:.c=.o)) \
       $(addprefix $(BUILD_DIR)/,$(HOST_SRCS:.c=.o))

ifneq ($(GEN_DIR),)
GEN_SRCS := $(notdir $(wildcard $(GEN_DIR)/*.c))
GEN_OBJS := $(addprefix $(BUILD_DIR)/gen/,$(GEN_SRCS:.c=.o))
OBJS += $(GEN_OBJS)
$(BUILD_DIR)/n64_stubs.o: CFLAGS += -DSNESRECOMP_N64_HAS_GENERATED_CODE
$(BUILD_DIR)/gen/%.o: $(GEN_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -Os -I$(GEN_INC) -I$(GEN_DIR) -o $@ $<
endif

N64_CFLAGS += -Iinclude -I$(RUNNER) -I$(RUNNER)/snes -I$(RUNNER)/desktop \
	-include include/n64_compat.h \
	-DSNESRECOMP_TRACE=0 \
	-DSNESRECOMP_EXECUTION_DEFAULT_MODE=SNESRECOMP_EXECUTION_MODE_OFF \
	-Wno-error -Wno-unused-parameter -Wno-unused-variable \
	-Wno-unused-function -Wno-sign-compare -Wno-unused-but-set-variable
N64_CFLAGS += -O2
N64_CFLAGS += -DSNESRECOMP_FAST_READ_MAP=1 -DSNESRECOMP_QUIESCENT_BACKEDGE=1 \
	-DSNESRECOMP_LAZY_BEAM=1 -DSNESRECOMP_REVERSE_DEBUG=0 -DSNESRECOMP_INTERP_EDGE_TRACE=0 \
	-DSNESRECOMP_WRAM_POLL_QUIESCENT=1 -DSNESRECOMP_PARK_TO_VBLANK=1 \
	-DSNESRECOMP_FRAME_FINGERPRINTS=0 -DSNESRECOMP_PPU_DMA_HISTORY=0 \
	-DSNESRECOMP_APU_NATIVE=1

# Diagnostic history buffers sized for desktop debugging total about 5.5 MB
# of .bss. Shrink them to token sizes (powers of two: the rings index with
# masks) so the runtime fits in 8 MB. Behaviour is unchanged; only how much
# history the diagnostics keep.
N64_CFLAGS += -DSNESRECOMP_AUDIO_TRACE_HISTORY=0 -DSNESRECOMP_INTERP_PAGE_MAP=0 \
	-DAUDIO_TRACE_PORT_RING=16u -DFP_RING=16 -DDMA_RING_LEN=16 \
	-DPPU_RING_LEN=16 -DSUPERFX_JOB_CAP=16u -DPPU_WLOG_CAP=16u \
	-DSPC_TRACE_RING=16 -DITRACE_RECENT_LEN=16 -DINTERP_EDGE_RING_LEN=16 \
	-DINTERP_SCOPE_NAMES=256u

# libdragon exports dma_read, dma_write and cart_init too. Rename the
# runtime's copies in every unit that sees runtime headers; n64_platform.c
# (the only libdragon-facing unit) keeps libdragon's names.
RUNTIME_RENAMES = -Ddma_read=snes_dma_read -Ddma_write=snes_dma_write \
	-Dcart_init=snes_cart_init
$(BUILD_DIR)/runner/%.o $(BUILD_DIR)/gen/%.o $(BUILD_DIR)/n64_host.o $(BUILD_DIR)/n64_stubs.o \
	$(BUILD_DIR)/n64_ppu_build.o: \
	CFLAGS += $(RUNTIME_RENAMES)

# make PROFILE=1: sample PCs for PROFILE_FRAMES frames starting at frame
# PROFILE_START and dump them over the debug log (n64/profile/symbolize.py).
# RENDER: 0 software PPU, 1 RDP backend (default), 2 RDP + software compare.
RENDER ?= 1
N64_CFLAGS += -DN64_RENDER=$(RENDER) $(EXTRA_CFLAGS)

PROFILE_START ?= 3
PROFILE_FRAMES ?= 10
ifeq ($(PROFILE),1)
N64_CFLAGS += -DN64_PROFILE_START=$(PROFILE_START) -DN64_PROFILE_FRAMES=$(PROFILE_FRAMES)
endif

N64_MKDFS_ROOT = $(BUILD_DIR)/filesystem
N64_ROM_TITLE = "snesrecomp interp"
N64_ROM_EXPANSIONPAK = required

all: snesrecomp_interp.z64

$(BUILD_DIR)/runner/%.o: $(RUNNER)/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/filesystem/game.sfc: $(ROM)
	@test -n "$(ROM)" || { echo "Set ROM=/path/to/game.sfc"; exit 1; }
	@mkdir -p $(dir $@)
	cp "$(ROM)" $@

$(BUILD_DIR)/snesrecomp_interp.dfs: $(BUILD_DIR)/filesystem/game.sfc
$(BUILD_DIR)/snesrecomp_interp.elf: $(OBJS)

snesrecomp_interp.z64: $(BUILD_DIR)/snesrecomp_interp.dfs

clean:
	rm -rf $(BUILD_DIR) snesrecomp_interp.z64

-include $(wildcard $(BUILD_DIR)/*.d $(BUILD_DIR)/runner/*.d $(BUILD_DIR)/runner/snes/*.d $(BUILD_DIR)/gen/*.d)

.PHONY: all clean
