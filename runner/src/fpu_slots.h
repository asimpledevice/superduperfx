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

/* Free: $f17-$f19, $f20-$f31. */

#endif
