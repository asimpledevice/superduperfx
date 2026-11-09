/* Guest CPU on its own host stack. See guest_fiber.h. */
#include "guest_fiber.h"

#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "snes/interp_bridge.h"

#if SNESRECOMP_GUEST_FIBER

uint32_t g_guest_fiber_pauses, g_guest_fiber_resumes, g_guest_fiber_unwinds;

#ifndef GUEST_FIBER_STACK_SIZE
#define GUEST_FIBER_STACK_SIZE (256 * 1024)
#endif

/* ---- Context switch ------------------------------------------------------ */

#if defined(__mips__)
/* Callee-saved state of the MIPS ABIs: s0-s7, s8, gp, sp, ra and $f20-$f31
 * (libdragon runs the FPU with 32 64-bit registers). Everything else is
 * caller-saved, so the switch is an ordinary call for the compiler. */
typedef struct { uint64_t r[12]; uint64_t f[12]; } GuestCtx;
void guest_ctx_switch(GuestCtx *save, const GuestCtx *load);
__asm__(
    ".text\n"
    ".set push\n.set noreorder\n.set noat\n"
    ".globl guest_ctx_switch\n.type guest_ctx_switch, @function\n"
    "guest_ctx_switch:\n"
    "  sd $16, 0($4)\n  sd $17, 8($4)\n  sd $18, 16($4)\n  sd $19, 24($4)\n"
    "  sd $20, 32($4)\n  sd $21, 40($4)\n  sd $22, 48($4)\n  sd $23, 56($4)\n"
    "  sd $30, 64($4)\n  sd $28, 72($4)\n  sd $29, 80($4)\n  sd $31, 88($4)\n"
    "  sdc1 $f20, 96($4)\n  sdc1 $f21, 104($4)\n  sdc1 $f22, 112($4)\n  sdc1 $f23, 120($4)\n"
    "  sdc1 $f24, 128($4)\n  sdc1 $f25, 136($4)\n  sdc1 $f26, 144($4)\n  sdc1 $f27, 152($4)\n"
    "  sdc1 $f28, 160($4)\n  sdc1 $f29, 168($4)\n  sdc1 $f30, 176($4)\n  sdc1 $f31, 184($4)\n"
    "  ld $16, 0($5)\n  ld $17, 8($5)\n  ld $18, 16($5)\n  ld $19, 24($5)\n"
    "  ld $20, 32($5)\n  ld $21, 40($5)\n  ld $22, 48($5)\n  ld $23, 56($5)\n"
    "  ld $30, 64($5)\n  ld $28, 72($5)\n  ld $29, 80($5)\n  ld $31, 88($5)\n"
    "  ldc1 $f20, 96($5)\n  ldc1 $f21, 104($5)\n  ldc1 $f22, 112($5)\n  ldc1 $f23, 120($5)\n"
    "  ldc1 $f24, 128($5)\n  ldc1 $f25, 136($5)\n  ldc1 $f26, 144($5)\n  ldc1 $f27, 152($5)\n"
    "  ldc1 $f28, 160($5)\n  ldc1 $f29, 168($5)\n  ldc1 $f30, 176($5)\n  ldc1 $f31, 184($5)\n"
    "  jr $31\n  nop\n"
    ".size guest_ctx_switch, .-guest_ctx_switch\n"
    ".set pop\n");

static GuestCtx s_main_ctx, s_guest_ctx;
static void guest_main(void);
static void guest_trampoline(void) {
  guest_main();
  abort();   /* guest_main never returns */
}
static void ctx_init(void *stack_lo, void *stack_top) {
  (void)stack_lo;
  uintptr_t top = (uintptr_t)stack_top & ~(uintptr_t)15;
  uint64_t gp;
  __asm__ volatile("move %0, $28" : "=r"(gp));
  memset(&s_guest_ctx, 0, sizeof s_guest_ctx);
  s_guest_ctx.r[9] = gp;
  s_guest_ctx.r[10] = (uint64_t)(intptr_t)(top - 64);   /* sp, with argument space */
  s_guest_ctx.r[11] = (uint64_t)(intptr_t)guest_trampoline;
}
static inline void switch_to_guest(void) { guest_ctx_switch(&s_main_ctx, &s_guest_ctx); }
static inline void switch_to_main(void) { guest_ctx_switch(&s_guest_ctx, &s_main_ctx); }

#else
#include <ucontext.h>
static ucontext_t s_main_uc, s_guest_uc;
static void guest_main(void);
static void ctx_init(void *stack_lo, void *stack_top) {
  getcontext(&s_guest_uc);
  s_guest_uc.uc_stack.ss_sp = stack_lo;
  s_guest_uc.uc_stack.ss_size = (size_t)((char *)stack_top - (char *)stack_lo);
  s_guest_uc.uc_link = NULL;
  makecontext(&s_guest_uc, guest_main, 0);
}
static inline void switch_to_guest(void) { swapcontext(&s_main_uc, &s_guest_uc); }
static inline void switch_to_main(void) { swapcontext(&s_guest_uc, &s_main_uc); }
#endif

/* ---- Runs and pauses ----------------------------------------------------- */

enum { GF_IDLE, GF_PAUSED };
static int s_ready, s_state, s_on_guest, s_done, s_resume, s_result;
static CpuState *s_cpu;
static uint32_t s_entry_pc24, s_paused_pc24;
static uint16_t s_paused_s;
/* Nesting state: as the driver had it when the run started (restored for the
 * driver while paused), and as the paused code had it. */
static void *s_base_bridge, *s_base_recomp, *s_paused_bridge, *s_paused_recomp;

static void guest_main(void) {
  for (;;) {
    s_result = interp_bridge_run_until_quiescent(s_cpu, s_entry_pc24);
    s_done = 1;
    switch_to_main();
  }
}

static void *s_stack_lo, *s_stack_top;
void guest_fiber_set_stack(void *lo, void *top) {
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
    s_stack_lo = malloc(GUEST_FIBER_STACK_SIZE);
    s_stack_top = (char *)s_stack_lo + GUEST_FIBER_STACK_SIZE;
  }
  if (!s_base_bridge || !s_paused_bridge || !s_base_recomp || !s_paused_recomp || !s_stack_lo)
    abort();
  ctx_init(s_stack_lo, s_stack_top);
  s_ready = 1;
}

/* Run the guest fiber until it finishes its run or pauses. */
static int enter_guest(void) {
  s_done = 0;
  s_on_guest = 1;
  switch_to_guest();
  s_on_guest = 0;
  s_state = s_done ? GF_IDLE : GF_PAUSED;
  return s_result;
}

int guest_fiber_on_guest(void) { return s_on_guest; }

int guest_fiber_pause(uint32_t resume_pc24, uint16_t resume_s) {
  g_guest_fiber_pauses++;
  interp_bridge_nest_save(s_paused_bridge);
  recomp_nest_state_save(s_paused_recomp);
  interp_bridge_nest_load(s_base_bridge);
  recomp_nest_state_load(s_base_recomp);
  s_paused_pc24 = resume_pc24;
  s_paused_s = resume_s;
  s_result = 1;   /* what a deadline unwind returns from the run */
  switch_to_main();
  return s_resume;
}

int guest_fiber_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  ensure_ready();
  if (s_state == GF_PAUSED) {
    s_resume = (entry_pc24 & 0xFFFFFFu) == s_paused_pc24 && cpu == s_cpu && cpu->S == s_paused_s;
    interp_bridge_nest_load(s_paused_bridge);
    recomp_nest_state_load(s_paused_recomp);
    if (s_resume) {
      g_guest_fiber_resumes++;
      return enter_guest();
    }
    /* Somewhere else: let the paused code unwind as it would have, then
     * start the requested run. */
    g_guest_fiber_unwinds++;
    while (s_state == GF_PAUSED) (void)enter_guest();
  }
  s_cpu = cpu;
  s_entry_pc24 = entry_pc24;
  interp_bridge_nest_save(s_base_bridge);
  recomp_nest_state_save(s_base_recomp);
  return enter_guest();
}

#endif
