#ifndef TIME_CATS_H
#define TIME_CATS_H
/* Measurement builds (n64 EXTRA_CFLAGS=-DN64_TIME_CATS): exclusive host
 * time by category, from the VR4300's cycle counter (COP0 Count, half the
 * CPU clock). tcat_enter switches the current category (charging the time
 * since the last switch to the one left) and returns it; tcat_leave(prev)
 * switches back. The categories nest as the code does: interpreted 65816
 * code calls compiled bodies, which call I/O registers, which run the SPC,
 * and so on; each instant is charged to the innermost one. */
#include <stdint.h>
enum { TCAT_OTHER, TCAT_INTERP, TCAT_COMPILED, TCAT_APU, TCAT_IO, TCAT_BEAM, TCAT_OPCODE, TCAT_N };
extern uint32_t g_tcat_iters, g_tcat_ops;   /* (interpreter loop iterations, opcodes) */
#ifdef N64_TIME_CATS
extern uint32_t g_tcat_cur, g_tcat_t;
extern uint64_t g_tcat_acc[TCAT_N];
static inline uint32_t tcat_now(void) {
  uint32_t c;
  __asm__ volatile("mfc0 %0, $9" : "=r"(c));
  return c;
}
#ifdef N64_TIME_SWITCHES
/* How often the frame moves from one category to another: each move leaves
 * the 16 KB caches holding the other side's code and data. */
extern uint32_t g_tcat_sw[TCAT_N][TCAT_N];
#endif
static inline __attribute__((always_inline)) uint32_t tcat_enter(uint32_t cat) {
  const uint32_t now = tcat_now(), prev = g_tcat_cur;
#ifdef N64_TIME_SWITCHES
  g_tcat_sw[prev][cat]++;
#endif
  g_tcat_acc[prev] += now - g_tcat_t;
  g_tcat_t = now;
  g_tcat_cur = cat;
  return prev;
}
static inline void tcat_leave(uint32_t prev) { (void)tcat_enter(prev); }
#else
static inline uint32_t tcat_enter(uint32_t cat) { (void)cat; return 0; }
static inline void tcat_leave(uint32_t prev) { (void)prev; }
#endif
#endif
