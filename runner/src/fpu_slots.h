/* Which reserved FPU register holds what (n64/fpu_reserve.mk reserves them;
 * docs/n64/BRINGUP_NOTES.md, "The FPU register split", has the budget).
 *
 * Names are assembler strings, for the hand-written code that owns each slot.
 * Compiled code never names a reserved register (it is built with -ffixed-fN),
 * and tools/n64/check_fpu_reserved.py fails a build in which a function
 * outside SDFX_FPU_OWNERS touches one.
 *
 * $f5-$f19 are in the range libdragon's interrupt path saves and restores
 * when a handler uses the FPU, so their values survive an interrupt but a
 * handler cannot change them. $f20-$f31 are not saved there: state an
 * interrupt handler must write (the run flags) goes in that range. */
#ifndef SNESRECOMP_FPU_SLOTS_H
#define SNESRECOMP_FPU_SLOTS_H

/* The parked thread (Blue or Green, whichever is not running): its s0-s8,
 * then sp (high 32 bits) and ra (low 32 bits) in one register. gp is the
 * same for both threads and is not parked. runner/src/blue_thread.c. */
#define FPU_PARK_S0 "$f5"
#define FPU_PARK_S1 "$f6"
#define FPU_PARK_S2 "$f7"
#define FPU_PARK_S3 "$f8"
#define FPU_PARK_S4 "$f9"
#define FPU_PARK_S5 "$f10"
#define FPU_PARK_S6 "$f11"
#define FPU_PARK_S7 "$f14"
#define FPU_PARK_S8 "$f15"
#define FPU_PARK_SP_RA "$f16"

/* The 65816 registers (runner/src/cpu_regs.h, CPUFPU=1 builds): one each,
 * held zero-extended. Compiled code reaches them through GCC global register
 * variables, so these slots are not limited to SDFX_FPU_OWNERS
 * (n64/fpu_reserve.mk, SDFX_FPU_SHARED); code compiled without cpu_regs.h,
 * libdragon and newlib included, cannot name them (-ffixed). */
#define FPU_CPU_A "$f17"
#define FPU_CPU_X "$f18"
#define FPU_CPU_Y "$f19"
#define FPU_CPU_S "$f20"
#define FPU_CPU_D "$f21"
#define FPU_CPU_DB "$f22"
#define FPU_CPU_PB "$f23"
#define FPU_CPU_P_MXID "$f24" /* P's M, X, I and D bits (the rest are 0) */
#define FPU_CPU_E "$f25"   /* the emulation bit, 0 or 1 */

/* The master clocks (64-bit; cpu_regs.h g_master_cycles). */
#define FPU_MASTER_CYCLES "$f26"
#define FPU_COPROCESSOR_MASTER_CYCLES "$f27"

/* The other status flags (cpu_regs.h): the last N/Z result, then C and V,
 * each 0 or 1. */
#define FPU_CPU_NZ "$f28"
#define FPU_CPU_C "$f29"
#define FPU_CPU_V "$f30"

/* Blue/Green run flags (run_flags.h): set by the SP interrupt (Red's
 * mailbox doorbell), read by both threads at their safe points. In the range
 * the interrupt path does not save, so the handler's write stays. */
#define FPU_RUN_FLAGS "$f31"

#endif
