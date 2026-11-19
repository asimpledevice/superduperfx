/* The main CPU's build of the 65816 interpreter core (interp816.c).
 *
 * SNESRECOMP_INTERP_REGS=1, with the 65816 registers in reserved FPU
 * registers (SNESRECOMP_CPU_REGS_FPU=1): this core reads and writes A, X, Y,
 * S, D, DB, PB and the flags where compiled code keeps them (cpu_regs.h),
 * so the interpreter and compiled code share one home for them. Nothing is
 * copied when one hands over to the other, and none of it is in the data
 * cache to be evicted. The Interp816 struct still carries pc, the interrupt
 * lines, the power state and the bus; its register fields are unused.
 *
 * Coprocessors (the SA-1) keep the struct build, interp816_*; this one is
 * interp816m_*. The opcode tables and the tallies are shared. */
#ifndef SNESRECOMP_INTERP_REGS
#define SNESRECOMP_INTERP_REGS 0
#endif
#if SNESRECOMP_INTERP_REGS
#include "cpu_state.h"
#if !SNESRECOMP_CPU_REGS_FPU
#error "SNESRECOMP_INTERP_REGS needs SNESRECOMP_CPU_REGS_FPU=1"
#endif

#define I816_MAIN 1
#include "interp816_regs.h"

#define interp816_runOpcode interp816m_runOpcode
#define interp816_reset interp816m_reset
#define interp816_getFlags interp816m_getFlags
#define interp816_setFlags interp816m_setFlags
/* (the place the cache layout gives the core: n64/icache_order.ld) */
#include "interp816.h"
int interp816_runOpcode(Interp816 *cpu) __attribute__((section(".text.interp816_runOpcode")));
#include "interp816.c"
#else
typedef int interp816_main_unused;
#endif
