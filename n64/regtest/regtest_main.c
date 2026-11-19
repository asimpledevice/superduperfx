/* CPU register hardware test (EverDrive). Answers, on real hardware:
 *  - does a 32-bit MTC1 leave the high half of a 64-bit FPU register alone
 *    (so rarely-written state can live there next to a hot low half);
 *  - do the reserved FPU registers keep all 64 bits across interrupts;
 *  - is COP0 ErrorEPC a free 64-bit register (round trip, across interrupts),
 *    and which bits of Context, XContext and LLAddr hold a value;
 *  - what a read+write of each of those costs next to a cached load+store.
 * Results go to the screen (one page, made to be photographed) and to the
 * debug log. The line of the test in progress is on screen before it runs,
 * so a hang shows where it happened. */
#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static char s_lines[30][52];
static int s_nlines;
static int s_fails;

static void show(const char *running) {
  console_clear();
  for (int i = 0; i < s_nlines; i++) printf("%s\n", s_lines[i]);
  if (running) printf("running: %s\n", running);
  console_render();
}

static void line(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_lines[s_nlines], sizeof s_lines[0], fmt, ap);
  va_end(ap);
  debugf("%s\n", s_lines[s_nlines]);
  if (s_nlines < 29) s_nlines++;
}

static void check(bool ok) { if (!ok) s_fails++; }
static const char *okstr(bool ok) { return ok ? "ok" : "FAIL"; }

#define HI(v) ((unsigned long)((uint64_t)(v) >> 32))
#define LO(v) ((unsigned long)((uint64_t)(v) & 0xFFFFFFFFu))

/* Every reserved register (n64/fpu_reserve.mk); nothing compiled touches them. */
#define FREGS(X) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(14) X(15) X(16) X(17) X(18) X(19) \
  X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

static void fset(int r, uint64_t v) {
  switch (r) {
#define X(n) case n: __asm__ volatile("dmtc1 %0, $f" #n "\n nop" : : "r"(v)); break;
    FREGS(X)
#undef X
  }
}
static uint64_t fget(int r) {
  uint64_t v = 0;
  switch (r) {
#define X(n) case n: __asm__ volatile("dmfc1 %0, $f" #n "\n nop" : "=r"(v)); break;
    FREGS(X)
#undef X
  }
  return v;
}
static void fset32(int r, uint32_t v) {
  switch (r) {
#define X(n) case n: __asm__ volatile("mtc1 %0, $f" #n "\n nop" : : "r"(v)); break;
    FREGS(X)
#undef X
  }
}
static int64_t fget32(int r) {   /* MFC1 sign-extends into the 64-bit GPR */
  int64_t v = 0;
  switch (r) {
#define X(n) case n: __asm__ volatile("mfc1 %0, $f" #n "\n nop" : "=r"(v)); break;
    FREGS(X)
#undef X
  }
  return v;
}
static const uint8_t k_fregs[] = {
#define X(n) n,
  FREGS(X)
#undef X
};
#define NFREGS ((int)sizeof k_fregs)

static uint64_t pat(int i) { return 0x0123456789ABCDEFull * (uint64_t)(2 * i + 3) ^ 0xA5A5A5A55A5A5A5Aull; }

static volatile uint32_t s_vblanks;
static void on_vi(void) { s_vblanks++; }
static volatile uint32_t s_timer_hits;
static void on_timer(int ovfl) { (void)ovfl; s_timer_hits++; }

/* Let interrupts run: vertical blanks and a 1 kHz timer, for n vblanks. */
static uint32_t run_interrupts(uint32_t n) {
  const uint32_t v0 = s_vblanks, t0 = s_timer_hits;
  while (s_vblanks - v0 < n) {}
  return s_timer_hits - t0;
}

/* 1. MTC1 and the high half. */
static void t_mtc1(void) {
  show("mtc1 high half");
  int bad = 0, badlo = 0, badsx = 0;
  uint64_t seen = 0;
  for (int i = 0; i < NFREGS; i++) {
    const int r = k_fregs[i];
    for (int k = 0; k < 4; k++) {
      const uint64_t init = pat(i + 7 * k);
      const uint32_t lo = (uint32_t)pat(i + 7 * k + 100) | (k & 1 ? 0x80000000u : 0);
      fset(r, init);
      fset32(r, lo);
      const uint64_t got = fget(r);
      const int64_t got32 = fget32(r);
      if (HI(got) != HI(init)) { if (!bad) seen = got; bad++; }
      if (LO(got) != lo) badlo++;
      if (got32 != (int64_t)(int32_t)lo) badsx++;
    }
  }
  line("1 MTC1 keeps high half: %s (%d of %d)", bad ? "NO" : "YES", NFREGS * 4 - bad, NFREGS * 4);
  if (bad) line("  first bad read %08lx:%08lx", HI(seen), LO(seen));
  line("  low half %s, MFC1 sign-extends %s", okstr(!badlo), okstr(!badsx));
  check(!bad && !badlo && !badsx);
}

/* 2. Reserved registers across interrupts, all 64 bits. $f5-$f19 go through
 * libdragon's save and restore; $f20-$f31 are simply never touched. */
static void t_fpu_irq(void) {
  show("fpu regs across interrupts");
  for (int i = 0; i < NFREGS; i++) fset(k_fregs[i], pat(i));
  const uint32_t timers = run_interrupts(30);
  int bad = 0, first = -1;
  for (int i = 0; i < NFREGS; i++)
    if (fget(k_fregs[i]) != pat(i)) { if (first < 0) first = k_fregs[i]; bad++; }
  line("2 FPU regs over 30 vblanks, %lu timer irqs:", (unsigned long)timers);
  if (bad) line("  %d of %d CHANGED (first $f%d)", bad, NFREGS, first);
  else line("  all %d kept 64 bits", NFREGS);
  check(!bad);
  /* The same with a low half rewritten after the high half was set. */
  for (int i = 0; i < NFREGS; i++) { fset(k_fregs[i], pat(i)); fset32(k_fregs[i], (uint32_t)pat(i + 50)); }
  run_interrupts(10);
  bad = 0;
  for (int i = 0; i < NFREGS; i++)
    if (fget(k_fregs[i]) != ((pat(i) & 0xFFFFFFFF00000000ull) | (uint32_t)pat(i + 50))) bad++;
  line("  split halves over 10 vblanks: %s", bad ? "CHANGED" : "kept");
  check(!bad);
}

#define C0_CONTEXT 4
#define C0_LLADDR 17
#define C0_XCONTEXT 20
#define C0_ERROREPC 30
#define DMTC0(v, n) __asm__ volatile("dmtc0 %0, $" #n "\n nop\n nop" : : "r"((uint64_t)(v)))
#define DMFC0(n) ({ uint64_t v_; __asm__ volatile("dmfc0 %0, $" #n "\n nop" : "=r"(v_)); v_; })

/* 3. ErrorEPC as a 64-bit register. Nothing but a reset, an NMI or a cache
 * error writes it, and ERET reads it only with Status.ERL set. */
static void t_errorepc(void) {
  show("ErrorEPC");
  static const uint64_t pats[] = { 0x0123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull, 0, 0x8000000080000001ull,
                                   0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull, 0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull };
  int bad = 0; uint64_t wrote = 0, got = 0;
  for (unsigned i = 0; i < sizeof pats / sizeof pats[0]; i++) {
    DMTC0(pats[i], 30);
    const uint64_t g = DMFC0(30);
    if (g != pats[i]) { if (!bad) { wrote = pats[i]; got = g; } bad++; }
  }
  line("3 ErrorEPC 64-bit round trip: %s", bad ? "NO" : "YES");
  if (bad) line("  wrote %08lx:%08lx got %08lx:%08lx", HI(wrote), LO(wrote), HI(got), LO(got));
  check(!bad);
  DMTC0(0x0123456789ABCDEFull, 30);
  const uint32_t timers = run_interrupts(30);
  uint64_t g = DMFC0(30);
  line("  over 30 vblanks, %lu timer irqs: %s", (unsigned long)timers, g == 0x0123456789ABCDEFull ? "kept" : "CHANGED");
  check(g == 0x0123456789ABCDEFull);
  /* Across code that leaves kernel-visible traces: a DMA-less cache flush,
   * console output and a memory allocation. */
  DMTC0(0xFEDCBA9876543210ull, 30);
  data_cache_hit_writeback_invalidate(s_lines, sizeof s_lines);
  show("ErrorEPC (after flush)");
  g = DMFC0(30);
  line("  over cache ops and console: %s", g == 0xFEDCBA9876543210ull ? "kept" : "CHANGED");
  check(g == 0xFEDCBA9876543210ull);
}

/* 4. Which bits of the other candidates hold a value (information only). */
static void t_others(void) {
  show("Context, XContext, LLAddr");
  disable_interrupts();
  const uint64_t c_old = DMFC0(4), x_old = DMFC0(20), l_old = DMFC0(17);
  DMTC0(~0ull, 4);  const uint64_t c1 = DMFC0(4);  DMTC0(0, 4);  const uint64_t c0 = DMFC0(4);
  DMTC0(~0ull, 20); const uint64_t x1 = DMFC0(20); DMTC0(0, 20); const uint64_t x0 = DMFC0(20);
  DMTC0(~0ull, 17); const uint64_t l1 = DMFC0(17); DMTC0(0, 17); const uint64_t l0 = DMFC0(17);
  DMTC0(c_old, 4); DMTC0(x_old, 20); DMTC0(l_old, 17);
  enable_interrupts();
  line("4 writable bits (wrote ones ^ wrote zeros):");
  line("  Context  %08lx:%08lx", HI(c1 ^ c0), LO(c1 ^ c0));
  line("  XContext %08lx:%08lx", HI(x1 ^ x0), LO(x1 ^ x0));
  line("  LLAddr   %08lx:%08lx", HI(l1 ^ l0), LO(l1 ^ l0));
}

/* 5. Cost of a read+write, in CPU cycles, against a cached load+store. */
#define N_LOOP 4096
#define COST(body) ({ \
  disable_interrupts(); \
  uint64_t v_ = 0x1234; (void)v_; \
  const uint32_t t0_ = TICKS_READ(); \
  for (int i_ = 0; i_ < N_LOOP; i_++) { body; __asm__ volatile(""); } \
  const uint32_t t1_ = TICKS_READ(); \
  enable_interrupts(); \
  (uint32_t)(t1_ - t0_) * 2u; })

static volatile uint64_t s_mem;

static void t_cost(void) {
  show("costs");
  uint64_t keep = DMFC0(30);
  const uint32_t empty = COST(__asm__ volatile("nop"));
  const uint32_t fpu = COST(__asm__ volatile("dmfc1 %0, $f20\n nop\n daddiu %0, %0, 1\n dmtc1 %0, $f20" : "+r"(v_)));
  const uint32_t fpu32 = COST(__asm__ volatile("mfc1 %0, $f20\n nop\n addiu %0, %0, 1\n mtc1 %0, $f20" : "+r"(v_)));
  const uint32_t c0 = COST(__asm__ volatile("dmfc0 %0, $30\n nop\n daddiu %0, %0, 1\n dmtc0 %0, $30" : "+r"(v_)));
  const uint32_t mem = COST(s_mem = s_mem + 1);
  DMTC0(keep, 30);
  line("5 read+add+write, cycles (incl. loop %lu.%02lu):", (unsigned long)(empty / N_LOOP), (unsigned long)(empty % N_LOOP * 100 / N_LOOP));
#define CY(x) (unsigned long)((x) / N_LOOP), (unsigned long)((x) % N_LOOP * 100 / N_LOOP)
  line("  FPU 64 %lu.%02lu  FPU 32 %lu.%02lu", CY(fpu), CY(fpu32));
  line("  ErrorEPC %lu.%02lu  cached RAM %lu.%02lu", CY(c0), CY(mem));
}

int main(void) {
  debug_init_isviewer();
  debug_init_usblog();
  console_init();
  console_set_render_mode(RENDER_MANUAL);
  timer_init();
  register_VI_handler(on_vi);
  set_VI_interrupt(1, 2);
  new_timer(TIMER_TICKS(1000), TF_CONTINUOUS, on_timer);
  line("CPU register test v1");
  t_mtc1();
  t_fpu_irq();
  t_errorepc();
  t_others();
  t_cost();
  line("RESULT: %s (%d failed)", s_fails ? "FAIL" : "ALL OK", s_fails);
  debugf("REGTEST_DONE\n");
  show(NULL);
  while (1) {}
}
