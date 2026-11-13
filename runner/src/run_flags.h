/* Blue/Green run flags: one word in the reserved FPU register $f31
 * (fpu_slots.h FPU_RUN_FLAGS), shared by both CPU threads and the SP
 * interrupt (Red's mailbox doorbell).
 *
 * The interrupt sets bits (run_flags_set_in_interrupt); the threads read
 * them at their safe points (run_flags) and clear the ones they consume
 * (run_flags_clear). A read is one DMFC1, no memory access.
 *
 * Why $f31: libdragon's interrupt path saves and restores $f0-$f19 lazily
 * and never touches $f20-$f31, so a write from the handler stays. The
 * handler runs with the FPU disabled; the setter enables it for its own
 * instructions and puts the status register back as it was (with the FPU
 * left enabled, the interrupt exit would reload $f0-$f19 from slots it
 * never saved).
 *
 * The flags only ask: a thread acts on them at its next safe point, and
 * anything the threads exchange goes through emulated-time sync points, so
 * when a flag arrives never changes emulated results. */
#ifndef SNESRECOMP_RUN_FLAGS_H
#define SNESRECOMP_RUN_FLAGS_H

#include <stdint.h>
#include "fpu_slots.h"

#if !defined(__mips__)
#error "run_flags.h: the run flags live in a VR4300 FPU register"
#endif

/* Flag bits: requests, honoured at the next safe point. */
#define RUN_FLAG_WANT_GREEN 0x1u   /* Blue should yield to Green (Green has work) */
#define RUN_FLAG_DOORBELL   0x2u   /* a mailbox message arrived (diagnostics) */
#define RUN_FLAG_WANT_BLUE  0x4u   /* Green should hand the CPU back to Blue (what it waited for is ready) */

static inline __attribute__((always_inline)) uint32_t run_flags(void) {
  uint64_t v;
  /* DMFC1's result is undefined for one instruction. */
  __asm__ volatile("dmfc1 %0, " FPU_RUN_FLAGS "\n\tnop" : "=r"(v));
  return (uint32_t)v;
}

/* Clear `bits` (thread side). With interrupts off for the read-modify-write,
 * so a bit the interrupt sets meanwhile is not lost. */
static inline void run_flags_clear(uint32_t bits) {
  uint32_t sr, tmp;
  uint64_t v;
  __asm__ volatile(
      "mfc0 %0, $12\n\t"
      "li %1, -2\n\t"            /* ~IE */
      "and %1, %0, %1\n\t"
      "mtc0 %1, $12\n\t"
      "nop\n\tnop\n\t"
      "dmfc1 %2, " FPU_RUN_FLAGS "\n\t"
      "nop\n\t"
      "and %2, %2, %3\n\t"
      "dmtc1 %2, " FPU_RUN_FLAGS "\n\t"
      "mtc0 %0, $12\n\t"
      : "=&r"(sr), "=&r"(tmp), "=&r"(v)
      : "r"((uint64_t)(~bits & 0xffffffffu))
      : "memory");
}

/* Set `bits` from the SP interrupt handler (FPU disabled there). */
static inline void run_flags_set_in_interrupt(uint32_t bits) {
  uint32_t sr, tmp;
  uint64_t v;
  __asm__ volatile(
      "mfc0 %0, $12\n\t"
      "lui %1, 0x2000\n\t"       /* CU1 */
      "or %1, %0, %1\n\t"
      "mtc0 %1, $12\n\t"
      "nop\n\tnop\n\tnop\n\t"    /* the FPU is usable a few instructions on */
      "dmfc1 %2, " FPU_RUN_FLAGS "\n\t"
      "nop\n\t"
      "or %2, %2, %3\n\t"
      "dmtc1 %2, " FPU_RUN_FLAGS "\n\t"
      "nop\n\t"
      "mtc0 %0, $12\n\t"
      : "=&r"(sr), "=&r"(tmp), "=&r"(v)
      : "r"((uint64_t)bits)
      : "memory");
}

/* Start from no flags (once, before the interrupt is enabled). */
static inline void run_flags_reset(void) {
  __asm__ volatile("dmtc1 $0, " FPU_RUN_FLAGS ::: "memory");
}

#endif
