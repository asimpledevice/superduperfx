# FPU registers SuperDuperFX reserves for its own state (BRINGUP_NOTES.md,
# "The FPU register split"). Compiled code keeps $f0-$f4 (scratch and the
# return value) and $f12/$f13 (the first two floating-point arguments under
# libdragon's o64 ABI); everything else is ours. Every unit that runs in an
# N64 build must be compiled with SDFX_FPU_FIXED: our code, our libdragon and
# newlib (third_party/newlib/build.sh). This file is the single list. The
# register names have no "$" (GCC accepts f5), so make and the shell leave them be.
SDFX_FPU_RESERVED := 5 6 7 8 9 10 11 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
SDFX_FPU_FIXED := $(foreach r,$(SDFX_FPU_RESERVED),-ffixed-f$(r))

# The only functions allowed to touch reserved registers
# (tools/n64/check_fpu_reserved.py). libdragon's exception path saves and
# restores the whole FPU file around a handler (inthandler.S); the Blue
# thread parks the idle thread in its slots (runner/src/fpu_slots.h).
SDFX_FPU_OWNERS := save_fpu_regs end_interrupt finalize_exception_frame end_exception_restore_all \
	thread_park_swap thread_park_init

# The 65816 registers' slots in CPUFPU=1 builds (runner/src/cpu_regs.h,
# runner/src/fpu_slots.h): GCC global register variables, which any function
# that includes cpu_regs.h may read and write. Code compiled without that
# header cannot name them (-ffixed), so the check lets every function use
# them in those builds only.
SDFX_FPU_SHARED := 17 18 19 20 21 22 23 24

# For scripts: make -s -f n64/fpu_reserve.mk print-fixed
.PHONY: print-fixed print-reserved
print-fixed: ; @echo '$(SDFX_FPU_FIXED)'
print-reserved: ; @echo '$(SDFX_FPU_RESERVED)'
