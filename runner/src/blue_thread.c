/* The Blue thread: the 65816 on its own host stack, switched cooperatively
 * with Green (the frame driver and everything else on the CPU). See
 * blue_thread.h. */
#include "blue_thread.h"

#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "fpu_slots.h"
#include "snes/interp_bridge.h"

#if SNESRECOMP_BLUE_THREAD

uint32_t g_blue_pauses, g_blue_resumes, g_blue_unwinds, g_blue_yields;

#ifndef BLUE_STACK_SIZE
#define BLUE_STACK_SIZE (256 * 1024)
#endif

/* ---- Context switch ------------------------------------------------------ */

#if !defined(__mips__)
#error "The Blue thread is the N64 (MIPS) context switch"
#endif
/* The thread not running is parked in reserved FPU registers (fpu_slots.h):
 * its callee-saved GPRs s0-s8, and sp and ra packed into one (both are 32-bit
 * KSEG0 addresses). gp is the same for both threads. A switch swaps each
 * live register with its slot, so the same call goes either way and touches
 * no memory. Everything else is caller-saved, so for the compiler the switch
 * is an ordinary call; that also frees t0-t2 for the swap.
 *
 * DMFC1's result is undefined for one instruction (VR4300 manual, DMFC1), so
 * slots are read two ahead of their use. */
#define PARK_SWAP2(ra_, fa_, rb_, fb_) \
    "  dmfc1 $8, " fa_ "\n  dmfc1 $9, " fb_ "\n" \
    "  dmtc1 " ra_ ", " fa_ "\n  dmtc1 " rb_ ", " fb_ "\n" \
    "  or " ra_ ", $8, $0\n  or " rb_ ", $9, $0\n"
void thread_park_swap(void);
void thread_park_init(uintptr_t sp, uintptr_t ra);
__asm__(
    ".text\n"
    ".set push\n.set noreorder\n.set noat\n"
    ".globl thread_park_swap\n.type thread_park_swap, @function\n"
    "thread_park_swap:\n"
    PARK_SWAP2("$16", FPU_PARK_S0, "$17", FPU_PARK_S1)
    PARK_SWAP2("$18", FPU_PARK_S2, "$19", FPU_PARK_S3)
    PARK_SWAP2("$20", FPU_PARK_S4, "$21", FPU_PARK_S5)
    PARK_SWAP2("$22", FPU_PARK_S6, "$23", FPU_PARK_S7)
    "  dmfc1 $8, " FPU_PARK_S8 "\n"
    "  dmfc1 $9, " FPU_PARK_SP_RA "\n"
    "  dmtc1 $30, " FPU_PARK_S8 "\n"
    "  dsll32 $10, $29, 0\n"           /* sp -> high half */
    "  dsll32 $11, $31, 0\n"
    "  dsrl32 $11, $11, 0\n"           /* ra -> low half, zero-extended */
    "  or $10, $10, $11\n"
    "  dmtc1 $10, " FPU_PARK_SP_RA "\n"
    "  or $30, $8, $0\n"
    "  sll $31, $9, 0\n"               /* ra, sign-extended: return into the other thread */
    "  jr $31\n"
    "  dsra32 $29, $9, 0\n"            /* (delay slot) sp, sign-extended */
    ".size thread_park_swap, .-thread_park_swap\n"
    /* Park a thread that has not run yet: s0-s8 zero, given sp and ra. */
    ".globl thread_park_init\n.type thread_park_init, @function\n"
    "thread_park_init:\n"
    "  dmtc1 $0, " FPU_PARK_S0 "\n  dmtc1 $0, " FPU_PARK_S1 "\n  dmtc1 $0, " FPU_PARK_S2 "\n"
    "  dmtc1 $0, " FPU_PARK_S3 "\n  dmtc1 $0, " FPU_PARK_S4 "\n  dmtc1 $0, " FPU_PARK_S5 "\n"
    "  dmtc1 $0, " FPU_PARK_S6 "\n  dmtc1 $0, " FPU_PARK_S7 "\n  dmtc1 $0, " FPU_PARK_S8 "\n"
    "  dsll32 $8, $4, 0\n"
    "  dsll32 $9, $5, 0\n"
    "  dsrl32 $9, $9, 0\n"
    "  or $8, $8, $9\n"
    "  jr $31\n"
    "  dmtc1 $8, " FPU_PARK_SP_RA "\n"
    ".size thread_park_init, .-thread_park_init\n"
    ".set pop\n");

static void blue_main(void);
static void blue_trampoline(void) {
  blue_main();
  abort();   /* blue_main never returns */
}
static void ctx_init(void *stack_lo, void *stack_top) {
  (void)stack_lo;
  uintptr_t top = (uintptr_t)stack_top & ~(uintptr_t)15;
  thread_park_init(top - 64 /* argument space */, (uintptr_t)blue_trampoline);
}
static inline void switch_to_blue(void) { thread_park_swap(); }
static inline void switch_to_green(void) { thread_park_swap(); }


/* ---- Runs and pauses ----------------------------------------------------- */

enum { GF_IDLE, GF_PAUSED };
static int s_ready, s_state, s_on_blue, s_done, s_resume, s_result, s_yield;
static void (*s_green_service)(void);
static CpuState *s_cpu;
static uint32_t s_entry_pc24, s_paused_pc24;
static uint16_t s_paused_s;
/* Nesting state: as the driver had it when the run started (restored for the
 * driver while paused), and as the paused code had it. */
static void *s_base_bridge, *s_base_recomp, *s_paused_bridge, *s_paused_recomp;

static void blue_main(void) {
  for (;;) {
    s_result = interp_bridge_run_until_quiescent(s_cpu, s_entry_pc24);
    s_done = 1;
    switch_to_green();
  }
}

static void *s_stack_lo, *s_stack_top;
void blue_set_stack(void *lo, void *top) {
  s_stack_lo = lo;
  s_stack_top = top;
}

static void ensure_ready(void) {
  if (s_ready) return;
  s_base_bridge = malloc(interp_bridge_nest_size());
  s_paused_bridge = malloc(interp_bridge_nest_size());
  s_base_recomp = malloc(recomp_nest_state_size());
  s_paused_recomp = malloc(recomp_nest_state_size());
  if (!s_stack_lo) {
    s_stack_lo = malloc(BLUE_STACK_SIZE);
    s_stack_top = (char *)s_stack_lo + BLUE_STACK_SIZE;
  }
  if (!s_base_bridge || !s_paused_bridge || !s_base_recomp || !s_paused_recomp || !s_stack_lo)
    abort();
  ctx_init(s_stack_lo, s_stack_top);
  s_ready = 1;
}

/* Run the Blue thread until it finishes its run or pauses. A yield comes
 * back here too: Green serves and switches straight back. */
static int enter_blue(void) {
  s_done = 0;
  for (;;) {
    s_on_blue = 1;
    switch_to_blue();
    s_on_blue = 0;
    if (!s_yield) break;
    s_yield = 0;
    run_flags_clear(RUN_FLAG_SWITCH);
    if (s_green_service) s_green_service();
  }
  s_state = s_done ? GF_IDLE : GF_PAUSED;
  return s_result;
}

void blue_set_green_service(void (*service)(void)) { s_green_service = service; }

void blue_yield(void) {
  g_blue_yields++;
  s_yield = 1;
  switch_to_green();
}

int blue_is_current(void) { return s_on_blue; }

int blue_pause(uint32_t resume_pc24, uint16_t resume_s) {
  g_blue_pauses++;
  interp_bridge_nest_save(s_paused_bridge);
  recomp_nest_state_save(s_paused_recomp);
  interp_bridge_nest_load(s_base_bridge);
  recomp_nest_state_load(s_base_recomp);
  s_paused_pc24 = resume_pc24;
  s_paused_s = resume_s;
  s_result = 1;   /* what a deadline unwind returns from the run */
  switch_to_green();
  return s_resume;
}

int blue_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  ensure_ready();
  if (s_state == GF_PAUSED) {
    s_resume = (entry_pc24 & 0xFFFFFFu) == s_paused_pc24 && cpu == s_cpu && cpu_S() == s_paused_s;
    interp_bridge_nest_load(s_paused_bridge);
    recomp_nest_state_load(s_paused_recomp);
    if (s_resume) {
      g_blue_resumes++;
      return enter_blue();
    }
    /* Somewhere else: let the paused code unwind as it would have, then
     * start the requested run. */
    g_blue_unwinds++;
    while (s_state == GF_PAUSED) (void)enter_blue();
  }
  s_cpu = cpu;
  s_entry_pc24 = entry_pc24;
  interp_bridge_nest_save(s_base_bridge);
  recomp_nest_state_save(s_base_recomp);
  return enter_blue();
}

#endif
