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
	common_cpu_infra.c common_rtl.c widescreen.c recomp_hw.c framedump.c recomp_cache.c \
	state_dump.c host_paths.c rom_image_verify.c rom_identity.c sha1.c \
	crc32.c snapshot_guard.c sha256.c snes_overlay_draw.c snes_runahead.c \
	snes_rewind.c cpu_state.c program_module.c rom_patch.c \
	generic_frame_driver.c beam_frame_driver.c beam_frame_driver_state.c guest_fiber.c \
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

.DEFAULT_GOAL := all

# RSPDSP=1 (default): deferred S-DSP whose batches run on the RSP
# (n64/rspdsp), the C DSP covering what the RSP path does not.
RSPDSP ?= 1
ifeq ($(RSPDSP),1)
N64_CFLAGS += -DSNESRECOMP_APU_DEFER_DSP=1 -DSNESRECOMP_DSP_LAUNCH_HOOK=snesdsp_rsp_apu_launch \
	-DSNESRECOMP_DSP_JOIN_HOOK=snesdsp_rsp_apu_join -DSNESRECOMP_DSP_HOST_ASYNC=1 \
	-DN64_RSPDSP=1 -Irspdsp
OBJS += $(BUILD_DIR)/rspdsp/snesdsp_rsp.o $(BUILD_DIR)/rsp_snesdsp.o
$(BUILD_DIR)/rspdsp/snesdsp_rsp.o: rspdsp/snesdsp_rsp.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<
# libdragon's RSP rule looks in SOURCE_DIR: run it on rspdsp/.
$(BUILD_DIR)/rsp_snesdsp.o: rspdsp/rsp_snesdsp.S rspdsp/snesdsp_rsp_layout.h rspdsp/rsp_snesdsp_tables.inc
	@$(MAKE) --no-print-directory -f $(N64_INST)/include/n64.mk SOURCE_DIR=rspdsp \
		BUILD_DIR=$(BUILD_DIR) RSPASFLAGS="$(N64_RSPASFLAGS)" $@
endif

# ROMPAGING=1: the cartridge reads the SNES ROM through a TLB-mapped window
# paged in from the N64 cartridge on demand (n64_platform_rom_window), so no
# copy of the ROM stays in RAM; ROMPAGE_SLOTS 8 KB slots (default 24 = 192 KB).
# Off by default until checked on hardware; 0 loads the whole ROM into RAM.
ROMPAGING ?= 0
ROMPAGE_SLOTS ?= 24
ifeq ($(ROMPAGING),1)
N64_CFLAGS += -DN64_ROM_PAGING=1 -DN64_ROM_PAGE_SLOTS=$(ROMPAGE_SLOTS)
endif

# RSPBG=1 (default): background layers are expanded on the RSP (n64/rspbg);
# RSPBG=0 keeps the CPU expansion (the reference).
RSPBG ?= 1
ifeq ($(RSPBG),1)
N64_CFLAGS += -DN64_RSPBG=1 -Irspbg
OBJS += $(BUILD_DIR)/rspbg/bg_rsp.o $(BUILD_DIR)/rsp_bg.o
$(BUILD_DIR)/rspbg/bg_rsp.o: rspbg/bg_rsp.c rspbg/bg_rsp.h rspbg/bg_rsp_layout.h
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -o $@ $<
$(BUILD_DIR)/rsp_bg.o: rspbg/rsp_bg.S rspbg/bg_rsp_layout.h rspbg/rsp_bg_full.inc
	@$(MAKE) --no-print-directory -f $(N64_INST)/include/n64.mk SOURCE_DIR=rspbg \
		BUILD_DIR=$(BUILD_DIR) RSPASFLAGS="$(N64_RSPASFLAGS)" $@
endif

# CODECACHE=1: GEN_DIR was generated with SNESRECOMP_EMIT_LEAN=1 and
# SNESRECOMP_EMIT_CODE_CACHE=1. tools/n64/split_code_cache.py moves every
# compiled routine body into its own module object; the main program keeps
# the runtime, the trampolines and routine records, and the host wrappers,
# and keeps every symbol the modules import (code_cache_externs.py).
# tools/n64/pack_code_cache.py then links each module against the linked
# main program (imports resolved at build time) and packs them, with only
# their internal relocations, into rom:/cc.pack, which the code cache
# (runner/src/recomp_cache.h) loads from on first call, staying within
# CODECACHE_BYTES.
CODECACHE ?= 0
CODECACHE_BYTES ?= 1572864
GEN_SRC_DIR := $(GEN_DIR)
ifeq ($(CODECACHE),1)
ifeq ($(GEN_DIR),)
$(error CODECACHE=1 needs GEN_DIR)
endif
CC_SPLIT := $(BUILD_DIR)/ccsplit
CC_SPLIT_OK := $(shell mkdir -p $(BUILD_DIR) && python3 ../tools/n64/split_code_cache.py $(GEN_DIR) $(CC_SPLIT) >&2 && echo ok)
ifneq ($(CC_SPLIT_OK),ok)
$(error split_code_cache.py failed)
endif
GEN_SRC_DIR := $(CC_SPLIT)/resident
CC_MODULES := $(basename $(notdir $(wildcard $(CC_SPLIT)/cc/*.c)))
CC_MODOBJS := $(addprefix $(BUILD_DIR)/cc/,$(addsuffix .o,$(CC_MODULES)))
N64_CFLAGS += -DRECOMP_CACHE_EXTERNAL_BODIES -DN64_CODE_CACHE=1 -DN64_CODE_CACHE_BYTES=$(CODECACHE_BYTES)
$(BUILD_DIR)/cc/%.o: $(CC_SPLIT)/cc/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(CC) -c $(CFLAGS) -Os -G 0 -I$(GEN_INC) -I$(GEN_DIR) -o $@ $<
CC_EXTERNS := $(BUILD_DIR)/snesrecomp_interp.externs
$(CC_EXTERNS): $(CC_MODOBJS)
	@echo "    [EXTERNS] $@"
	python3 ../tools/n64/code_cache_externs.py $@ $(BUILD_DIR)/cc $(N64_INST)/bin/mips64-elf-nm
$(BUILD_DIR)/snesrecomp_interp.elf: $(CC_EXTERNS)
$(BUILD_DIR)/filesystem/cc.pack: $(BUILD_DIR)/snesrecomp_interp.elf $(CC_MODOBJS) cc_module.ld ../tools/n64/pack_code_cache.py
	@mkdir -p $(dir $@)
	@echo "    [PACK] $@"
	python3 ../tools/n64/pack_code_cache.py $@ $(CC_SPLIT)/resident/dispatch_v2.c $(BUILD_DIR)/cc \
		$< $(N64_INST)/bin/mips64-elf-ld cc_module.ld $(BUILD_DIR)/ccmod 8
$(BUILD_DIR)/snesrecomp_interp.dfs: $(BUILD_DIR)/filesystem/cc.pack
endif

ifneq ($(GEN_DIR),)
GEN_SRCS := $(notdir $(wildcard $(GEN_SRC_DIR)/*.c))
GEN_OBJS := $(addprefix $(BUILD_DIR)/gen/,$(GEN_SRCS:.c=.o))
OBJS += $(GEN_OBJS)
$(BUILD_DIR)/n64_stubs.o: CFLAGS += -DSNESRECOMP_N64_HAS_GENERATED_CODE
$(BUILD_DIR)/gen/%.o: $(GEN_SRC_DIR)/%.c
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
	-DSNESRECOMP_FRAME_FINGERPRINTS=0 -DSNESRECOMP_PPU_DMA_HISTORY=0 -DSNESRECOMP_INTERP_DIAG=0 \
	-DSNESRECOMP_GUEST_FIBER=0 \
	-DSNESRECOMP_APU_NATIVE=1 -DSNESRECOMP_AUDIO_TRACE_HOOKS=0 \
	'-DSNESRECOMP_HOT_BSS=__attribute__((section(".sbss.hot")))'

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
PROFILE_US ?= 1000
ifeq ($(PROFILE),1)
N64_CFLAGS += -DN64_PROFILE_START=$(PROFILE_START) -DN64_PROFILE_FRAMES=$(PROFILE_FRAMES) \
	-DN64_PROFILE_US=$(PROFILE_US)
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

# ICACHE_ORDER: an ld --section-ordering-file placing hot code and data for
# the caches. The default joins icache_order.ld (functions, from
# profile/icache_order.py; runtime code only) and data_order.ld (hot static
# variables pinned at fixed cache positions, from profile/dcache_order.py).
# ICACHE_ORDER= (empty) links in the default order.
ICACHE_ORDER ?= $(BUILD_DIR)/cache_order.ld
$(BUILD_DIR)/cache_order.ld: icache_order.ld data_order.ld
	@mkdir -p $(dir $@)
	cat $^ > $@
ifneq ($(ICACHE_ORDER),)
$(BUILD_DIR)/snesrecomp_interp.elf: LDFLAGS += --section-ordering-file=$(abspath $(ICACHE_ORDER))
$(BUILD_DIR)/snesrecomp_interp.elf: $(ICACHE_ORDER)
endif

snesrecomp_interp.z64: $(BUILD_DIR)/snesrecomp_interp.dfs

# After linking, warn when a function icache_order.ld places is not at its
# planned address (one placed before it outgrew its gap and shifted the rest):
# that build's timings are not comparable. The warning does not stop the build.
ifneq ($(ICACHE_ORDER),)
snesrecomp_interp.z64: $(BUILD_DIR)/placement.check
$(BUILD_DIR)/placement.check: $(BUILD_DIR)/snesrecomp_interp.elf icache_order.ld
	@python3 profile/check_placement.py $< icache_order.ld --nm $(N64_INST)/bin/mips64-elf-nm \
		|| echo "*** WARNING: code placement shifted (see above) ***"
	@touch $@
endif

clean:
	rm -rf $(BUILD_DIR) snesrecomp_interp.z64

-include $(wildcard $(BUILD_DIR)/*.d $(BUILD_DIR)/runner/*.d $(BUILD_DIR)/runner/snes/*.d $(BUILD_DIR)/gen/*.d)

.PHONY: all clean
