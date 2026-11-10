/* Measurement only: per-frame ROM page touches (page protection) and
 * per-frame compiled-function entries (-finstrument-functions on gen code). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "snes/snes.h"
#include "snes/cart.h"
extern Snes *g_snes;
extern const uint8_t *g_rom;
extern int snes_frame_counter;
#define PG 4096u
static uint8_t *s_base; static uint32_t s_size, s_pages;
static uint8_t s_touched[4096];
static FILE *s_out;
static int s_frame = -1;
__attribute__((no_instrument_function)) void rt_anchor(void) {}
static __attribute__((no_instrument_function)) void on_segv(int sig, siginfo_t *si, void *uc_) {
  uint8_t *a = (uint8_t *)si->si_addr;
  if (!s_base || a < s_base || a >= s_base + s_pages * PG) { signal(SIGSEGV, SIG_DFL); raise(SIGSEGV); return; }
  uint32_t p = (uint32_t)((a - s_base) / PG);
  ucontext_t *uc = uc_;
  long rip = (long)uc->uc_mcontext.gregs[16] /* REG_RIP */ - (long)(uintptr_t)&rt_anchor;
  s_touched[p] = 1;
  mprotect(s_base + (size_t)p * PG, PG, PROT_READ);
  fprintf(s_out, "R %d %u %ld\n", s_frame, p, rip);
}
__attribute__((no_instrument_function)) void rt_frame(void) {
  if (!s_out) {
    s_out = fopen(getenv("RT_OUT") ? getenv("RT_OUT") : "/dev/null", "w");
    setvbuf(s_out, NULL, _IOFBF, 1 << 20);
  }
  if (!s_base && g_snes && g_snes->cart && g_snes->cart->rom) {
    Cart *c = g_snes->cart;
    s_size = c->romSize; s_pages = (s_size + PG - 1) / PG;
    s_base = mmap(NULL, (size_t)s_pages * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(s_base, c->rom, s_size);
    c->rom = s_base; g_rom = s_base;   /* fast map rebuilds on the pointer change */
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    fprintf(s_out, "ROM %u\n", s_size);
  }
  s_frame = snes_frame_counter;
  fprintf(s_out, "F %d\n", s_frame);
  if (s_base) mprotect(s_base, (size_t)s_pages * PG, PROT_NONE);
}
/* Compiled-function entries, once per function per frame. */
#define FH (1u << 16)
static uintptr_t s_fn[FH]; static int s_fn_frame[FH];
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site) {
  (void)site;
  if (!s_out) return;
  uintptr_t f = (uintptr_t)fn; uint32_t h = (uint32_t)((f >> 4) * 2654435761u) >> 16;
  while (s_fn[h] && s_fn[h] != f) h = (h + 1) & (FH - 1);
  if (s_fn[h] == f && s_fn_frame[h] == s_frame) return;
  s_fn[h] = f; s_fn_frame[h] = s_frame;
  fprintf(s_out, "C %d %ld\n", s_frame, (long)(f - (uintptr_t)&rt_anchor));
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site) { (void)fn; (void)site; }
