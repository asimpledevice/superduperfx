# Every N64 build includes this instead of libdragon's installed n64.mk.
#
# libdragon is part of this repository (third_party/libdragon). It is built
# and installed into build/libdragon, which becomes N64_INST. Only the GCC
# toolchain (mips64-elf) is external: N64_GCCPREFIX, or N64_INST as set in the
# environment (a libdragon toolchain install such as /opt/libdragon).
#
# The install is redone when the vendored sources change; "make libdragon"
# forces it.

SDFX_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
# The rules below must not become the including Makefile's default goal.
SDFX_SAVED_GOAL := $(.DEFAULT_GOAL)
LIBDRAGON_SRC := $(SDFX_ROOT)/third_party/libdragon

# Reserved FPU registers: every unit, libdragon included, is compiled with
# SDFX_FPU_FIXED, and programs link newlib rebuilt with it (third_party/newlib),
# found first through SDFX_NEWLIB_LIBDIR. n64.mk refuses to build without them.
include $(SDFX_ROOT)/n64/fpu_reserve.mk
SDFX_NEWLIB := $(SDFX_ROOT)/build/newlib
SDFX_NEWLIB_LIBDIR := $(SDFX_NEWLIB)/mips64-elf/lib
export SDFX_FPU_FIXED SDFX_NEWLIB_LIBDIR
NEWLIB_DEPS = $(wildcard $(SDFX_ROOT)/third_party/newlib/*) $(SDFX_FPU_STAMP)

# Captured now (:=), before N64_INST is redirected to the repo install below.
N64_GCCPREFIX := $(or $(N64_GCCPREFIX),$(N64_INST))
ifeq ($(strip $(N64_GCCPREFIX)),)
$(error Set N64_INST (or N64_GCCPREFIX) to the mips64-elf GCC toolchain, e.g. /opt/libdragon)
endif
override N64_INST := $(SDFX_ROOT)/build/libdragon
export N64_INST N64_GCCPREFIX

# Sources only: a prebuilt file as a prerequisite (boot/bin's IPL3 images, say)
# would match n64.mk's pattern rules and be "remade".
LIBDRAGON_DEPS := $(shell find $(LIBDRAGON_SRC)/src $(LIBDRAGON_SRC)/include -type f \
	\( -name '*.[chsS]' -o -name '*.cpp' -o -name '*.inc' -o -name '*.ld' \) 2>/dev/null) \
	$(wildcard $(LIBDRAGON_SRC)/*.mk $(LIBDRAGON_SRC)/*.ld $(LIBDRAGON_SRC)/Makefile)
LIBDRAGON_TOOL_DEPS := $(shell find $(LIBDRAGON_SRC)/tools -name '*.c' -o -name '*.cpp' -o -name '*.h' \
	-o -name '*.hpp' -o -name Makefile 2>/dev/null)

# Library, headers, n64.mk and linker scripts; then the host tools n64.mk runs.
# libdragon's objects depend on its sources only, so a change of reserved
# registers rebuilds it from clean (the install records the flags it used).
SDFX_FPU_STAMP := $(SDFX_ROOT)/build/fpu_fixed.stamp
ifneq ($(shell cat $(SDFX_FPU_STAMP) 2>/dev/null),$(SDFX_FPU_FIXED))
$(shell mkdir -p $(SDFX_ROOT)/build && echo '$(SDFX_FPU_FIXED)' > $(SDFX_FPU_STAMP))
endif
$(N64_INST)/include/n64.mk: $(LIBDRAGON_DEPS) $(SDFX_FPU_STAMP) $(N64_INST)/bin/n64tool \
		$(SDFX_NEWLIB_LIBDIR)/libc.a
	@echo "    [LIBDRAGON] $(LIBDRAGON_SRC) -> $(N64_INST)"
	+@if [ "$$(cat $(N64_INST)/fpu_fixed.txt 2>/dev/null)" != "$(SDFX_FPU_FIXED)" ]; then \
		$(MAKE) --no-print-directory -C $(LIBDRAGON_SRC) clean >/dev/null; fi
	+@$(MAKE) --no-print-directory -C $(LIBDRAGON_SRC) install >$(SDFX_ROOT)/build/libdragon-build.log 2>&1 \
		|| { tail -40 $(SDFX_ROOT)/build/libdragon-build.log; exit 1; }
	@echo '$(SDFX_FPU_FIXED)' > $(N64_INST)/fpu_fixed.txt
	@touch $@

$(N64_INST)/bin/n64tool: $(LIBDRAGON_TOOL_DEPS)
	@mkdir -p $(SDFX_ROOT)/build
	@echo "    [LIBDRAGON] tools -> $(N64_INST)/bin"
	+@$(MAKE) --no-print-directory -C $(LIBDRAGON_SRC) tools-install >$(SDFX_ROOT)/build/libdragon-tools.log 2>&1 \
		|| { tail -40 $(SDFX_ROOT)/build/libdragon-tools.log; exit 1; }
	@touch $@

# Downloads the newlib source on first use (third_party/newlib/build.sh).
$(SDFX_NEWLIB_LIBDIR)/libc.a: $(NEWLIB_DEPS)
	@mkdir -p $(SDFX_ROOT)/build
	@echo "    [NEWLIB] -> $(SDFX_NEWLIB)"
	@$(SDFX_ROOT)/third_party/newlib/build.sh $(SDFX_NEWLIB)
	@touch $@

.PHONY: libdragon
libdragon:
	+$(MAKE) --no-print-directory -C $(LIBDRAGON_SRC) install tools-install

include $(N64_INST)/include/n64.mk

.DEFAULT_GOAL := $(SDFX_SAVED_GOAL)
