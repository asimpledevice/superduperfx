/* The Blue thread: the 65816 on its own host stack, switched cooperatively
 * with Green (the frame driver and everything else on the CPU). See
 * blue_thread.h. */
#include "blue_thread.h"

#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "snes/interp_bridge.h"

#if SNESRECOMP_BLUE_THREAD

uint32_t g_blue_pauses, g_blue_resumes, g_blue_unwinds;

#ifndef BLUE_STACK_SIZE
#define BLUE_STACK_SIZE (256 * 1024)
#endif

/* ---- Context switch ------------------------------------------------------ */

#if !defined(__mips__)
#error "The Blue thread is the N64 (MIPS) context switch"
#endif
/* Callee-saved state of the MIPS ABIs: s0-s7, s8, gp, sp and ra. The
 * callee-saved FPU registers ($f20-$f31) are not switched: every FPU register
 * compiled code may keep is caller-saved, and the rest belong to state both
 * threads share (n64/fpu_reserve.mk). Everything else is caller-saved, so the
 * switch is an ordinary call for the compiler. */
typedef struct { uint64_t r[12]; } ThreadCtx;
void thread_ctx_switch(ThreadCtx *save, const ThreadCtx *load);
__asm__(
    ".text\n"
    ".set push\n.set noreorder\n.set noat\n"
    ".globl thread_ctx_switch\n.type thread_ctx_switch, @function\n"
    "thread_ctx_switch:\n"
    "  sd $16, 0($4)\n  sd $17, 8($4)\n  sd $18, 16($4)\n  sd $19, 24($4)\n"
    "  sd $20, 32($4)\n  sd $21, 40($4)\n  sd $22, 48($4)\n  sd $23, 56($4)\n"
    "  sd $30, 64($4)\n  sd $28, 72($4)\n  sd $29, 80($4)\n  sd $31, 88($4)\n"
    "  ld $16, 0($5)\n  ld $17, 8($5)\n  ld $18, 16($5)\n  ld $19, 24($5)\n"
    "  ld $20, 32($5)\n  ld $21, 40($5)\n  ld $22, 48($5)\n  ld $23, 56($5)\n"
    "  ld $30, 64($5)\n  ld $28, 72($5)\n  ld $29, 80($5)\n  ld $31, 88($5)\n"
    "  jr $31\n  nop\n"
    ".size thread_ctx_switch, .-thread_ctx_switch\n"
    ".set pop\n");

static ThreadCtx s_green_ctx, s_blue_ctx;
static void blue_main(void);
static void blue_trampoline(void) {
  blue_main();
  abort();   /* blue_main never returns */
}
static void ctx_init(void *stack_lo, void *stack_top) {
  (void)stack_lo;
  uintptr_t top = (uintptr_t)stack_top & ~(uintptr_t)15;
  uint64_t gp;
  __asm__ volatile("move %0, $28" : "=r"(gp));
  memset(&s_blue_ctx, 0, sizeof s_blue_ctx);
  s_blue_ctx.r[9] = gp;
  s_blue_ctx.r[10] = (uint64_t)(intptr_t)(top - 64);   /* sp, with argument space */
  s_blue_ctx.r[11] = (uint64_t)(intptr_t)blue_trampoline;
}
static inline void switch_to_blue(void) { thread_ctx_switch(&s_green_ctx, &s_blue_ctx); }
static inline void switch_to_green(void) { thread_ctx_switch(&s_blue_ctx, &s_green_ctx); }


/* ---- Runs and pauses ----------------------------------------------------- */

enum { GF_IDLE, GF_PAUSED };
static int s_ready, s_state, s_on_blue, s_done, s_resume, s_result;
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

/* Run the Blue thread until it finishes its run or pauses. */
static int enter_blue(void) {
  s_done = 0;
  s_on_blue = 1;
  switch_to_blue();
  s_on_blue = 0;
  s_state = s_done ? GF_IDLE : GF_PAUSED;
  return s_result;
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
    s_resume = (entry_pc24 & 0xFFFFFFu) == s_paused_pc24 && cpu == s_cpu && cpu->S == s_paused_s;
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
