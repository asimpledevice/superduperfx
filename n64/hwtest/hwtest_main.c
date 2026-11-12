/* CPU/RSP signalling hardware test (EverDrive). Answers, on real hardware:
 *  - can the CPU read and write DMEM while the RSP runs, safely, and at
 *    what cost, and does that slow the RSP or corrupt its DMA;
 *  - does the semaphore give mutual exclusion between CPU and RSP;
 *  - can the 8 SIG bits carry a byte in one write, both ways;
 *  - how long a doorbell takes (polled, and through the SP interrupt).
 * Results go to the screen (one page, made to be photographed) and to the
 * debug log. The line of the test in progress is on screen before it runs,
 * so a hang shows where it happened. */
#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include "hwtest_layout.h"

DEFINE_RSP_UCODE(rsp_hwtest);

#define DM(off) (SP_DMEM[(off) / 4])
#define CYC(ticks) ((uint32_t)(ticks) * 2u)   /* TICKS run at half the CPU clock */
#define US_TICKS(us) ((uint32_t)(us) * 47u)   /* ~46.875 ticks per microsecond */

static char s_lines[32][48];
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
  if (s_nlines < 31) s_nlines++;
}

static void fail(void) { s_fails++; }

static void sig_clear_all(void) { *SP_STATUS = 0xAAAA00; }   /* clear SIG0-SIG7 */

static bool rsp_halted(void) { return (*SP_STATUS & SP_STATUS_HALTED) != 0; }

static bool wait_halt(uint32_t us) {
  const uint32_t t0 = TICKS_READ();
  while (!rsp_halted())
    if (TICKS_READ() - t0 > US_TICKS(us)) return false;
  return true;
}

/* Force the RSP to stop if it ignored the stop signal. */
static bool stop_rsp(void) {
  *SP_STATUS = SP_WSTATUS_SET_SIG7;
  bool ok = wait_halt(100000);
  if (!ok) *SP_STATUS = SP_WSTATUS_SET_HALT;
  sig_clear_all();
  return ok;
}

static void start(uint32_t mode, uint32_t param, uint32_t param2) {
  for (int o = 0; o < 0x100; o += 4) DM(o) = 0;   /* RSP halted here */
  DM(HT_MODE) = mode;
  DM(HT_PARAM) = param;
  DM(HT_PARAM2) = param2;
  sig_clear_all();
  rsp_run_async();
}

static void spin_us(uint32_t us) {   /* no bus traffic: the COP0 count only */
  const uint32_t t0 = TICKS_READ();
  while (TICKS_READ() - t0 < US_TICKS(us)) {}
}

/* RSP loop passes per millisecond while the CPU is idle, reads DMEM, or
 * writes DMEM, for 10 ms each. */
static void rsp_rates(uint32_t out[3]) {
  for (int phase = 0; phase < 3; phase++) {
    const uint32_t c0 = DM(HT_COUNT), t0 = TICKS_READ();
    volatile uint32_t sink = 0;
    uint32_t i = 0;
    while (TICKS_READ() - t0 < US_TICKS(10000)) {
      if (phase == 1) sink += DM(HT_SCRATCH);
      else if (phase == 2) DM(HT_SCRATCH) = i++;
    }
    (void)sink;
    out[phase] = (DM(HT_COUNT) - c0) / 10;
  }
}

/* CPU cycles per access, 4096 accesses. */
static uint32_t cost_rd(volatile uint32_t *p) {
  volatile uint32_t sink = 0;
  const uint32_t t0 = TICKS_READ();
  for (int i = 0; i < 4096; i++) sink += *p;
  (void)sink;
  return CYC(TICKS_READ() - t0) / 4096;
}
static uint32_t cost_wr(volatile uint32_t *p) {
  const uint32_t t0 = TICKS_READ();
  for (int i = 0; i < 4096; i++) *p = i;
  return CYC(TICKS_READ() - t0) / 4096;
}

static volatile uint32_t s_irq_t, s_irq_n;
static void sp_irq(void) { s_irq_t = TICKS_READ(); s_irq_n++; }

static uint32_t s_cost_dmem_rd_run, s_cost_dmem_wr_run;

/* 1: CPU reads DMEM while the RSP writes it. */
static void t_read(void) {
  show("1 DMEM read during a run");
  start(HT_M_COUNT, 0, 0);
  spin_us(1000);
  uint32_t prev = DM(HT_COUNT), back = 0, changes = 0, torn = 0;
  for (int i = 0; i < 200000; i++) {
    const uint32_t v = DM(HT_COUNT);
    if (v < prev) back++;
    if (v != prev) changes++;
    prev = v;
  }
  for (int i = 0; i < 20000; i++) {
    const uint32_t k = DM(HT_REC), a = DM(HT_REC + 4), b = DM(HT_REC + 8), c = DM(HT_REC + 12);
    if (a != ~k || b != k * 3u || c != (k ^ 0x5a5a5a5au)) torn++;
  }
  uint32_t r[3];
  rsp_rates(r);
  s_cost_dmem_rd_run = cost_rd(&DM(HT_SCRATCH));
  s_cost_dmem_wr_run = cost_wr(&DM(HT_SCRATCH));
  const bool halted = stop_rsp();
  const bool ok = back == 0 && changes > 1000 && halted;
  if (!ok) fail();
  line("1 DMEM rd: %s back=%lu chg=%lu", ok ? "ok" : "FAIL", back, changes);
  line("  torn rec=%lu/20000 (no seq)", torn);
  line("  RSP passes/ms idle %lu rd %lu wr %lu", r[0], r[1], r[2]);
}

/* 2: sequence counter: no torn record may be accepted. */
static void t_seq(void) {
  show("2 sequence counter");
  start(HT_M_SEQ, 0, 0);
  spin_us(1000);
  uint32_t good = 0, retry = 0, bad = 0;
  for (int i = 0; i < 100000; i++) {
    const uint32_t s1 = DM(HT_SEQ);
    if (s1 & 1) { retry++; continue; }
    const uint32_t k = DM(HT_REC), a = DM(HT_REC + 4), b = DM(HT_REC + 8), c = DM(HT_REC + 12);
    if (DM(HT_SEQ) != s1) { retry++; continue; }
    if (a != ~k || b != k * 3u || c != (k ^ 0x5a5a5a5au)) bad++;
    else good++;
  }
  const bool halted = stop_rsp();
  const bool ok = bad == 0 && good > 0 && halted;
  if (!ok) fail();
  line("2 seq: %s g=%lu r=%lu bad=%lu", ok ? "ok" : "FAIL", good, retry, bad);
}

/* 3: CPU writes DMEM while the RSP runs: echo round trips, and a 2 KB
 * guard area nobody writes must stay intact. */
static void t_echo(void) {
  show("3 DMEM write during a run");
  uint32_t expect = 0;
  for (int o = HT_GUARD; o < HT_GUARD_END; o += 4) {
    const uint32_t v = (uint32_t)o * 0x9E3779B9u;
    DM(o) = v;
    expect += v;
  }
  start(HT_M_ECHO, 0, 0);
  spin_us(1000);
  uint32_t miss = 0, rt_sum = 0, rt_max = 0;
  for (uint32_t i = 1; i <= 1000; i++) {
    const uint32_t t0 = TICKS_READ();
    DM(HT_IN) = i;
    while (DM(HT_OUT) != i)
      if (TICKS_READ() - t0 > US_TICKS(1000)) { miss++; break; }
    const uint32_t dt = TICKS_READ() - t0;
    rt_sum += dt;
    if (dt > rt_max) rt_max = dt;
  }
  const bool halted = stop_rsp();
  uint32_t bad_words = 0;
  for (int o = HT_GUARD; o < HT_GUARD_END; o += 4)
    if (DM(o) != (uint32_t)o * 0x9E3779B9u) bad_words++;
  const bool rsp_sum_ok = halted && DM(HT_RES) == expect;
  const bool ok = miss == 0 && bad_words == 0 && rsp_sum_ok;
  if (!ok) fail();
  line("3 DMEM wr: %s miss=%lu guard=%s/%s", ok ? "ok" : "FAIL", miss, bad_words ? "BAD" : "ok",
       rsp_sum_ok ? "ok" : "BAD");
  line("  echo cyc avg %lu max %lu", CYC(rt_sum / 1000), CYC(rt_max));
}

/* 4: the semaphore's semantics, then CPU and RSP each adding 100,000 to a
 * shared counter under it. */
static void t_sem(void) {
  show("4 semaphore");
  *SP_SEMAPHORE = 0;
  const uint32_t r1 = *SP_SEMAPHORE, r2 = *SP_SEMAPHORE, r3 = *SP_SEMAPHORE;
  *SP_SEMAPHORE = 0;
  const uint32_t r4 = *SP_SEMAPHORE;
  *SP_SEMAPHORE = 0;
  const bool sem_ok = r1 == 0 && r2 == 1 && r3 == 1 && r4 == 0;
  const uint32_t n = 100000;
  start(HT_M_SEM, n, 0);
  uint32_t cpu_retry = 0;
  const uint32_t t0 = TICKS_READ();
  for (uint32_t i = 0; i < n; i++) {
    while (*SP_SEMAPHORE) cpu_retry++;
    const uint32_t v = DM(HT_SHARED);
    DM(HT_SHARED) = v + 1;
    *SP_SEMAPHORE = 0;
  }
  const uint32_t cpu_cyc = CYC(TICKS_READ() - t0) / n;
  const bool halted = wait_halt(2000000);
  if (!halted) *SP_STATUS = SP_WSTATUS_SET_HALT;
  const uint32_t shared = DM(HT_SHARED), rspn = DM(HT_RSPN), rsp_retry = DM(HT_RES + 4);
  *SP_SEMAPHORE = 0;
  const bool ok = sem_ok && halted && shared == 2 * n && rspn == n;
  if (!ok) fail();
  line("4 sema: %s reads %lu%lu%lu%lu (want 0110)", ok ? "ok" : "FAIL", r1, r2, r3, r4);
  line("  sum %lu/%lu retry c%lu r%lu", shared, 2 * n, cpu_retry, rsp_retry);
  line("  CPU locked inc: %lu cyc each", cpu_cyc);
}

static uint32_t sig_mask(uint32_t v) {
  uint32_t m = 0;
  for (int i = 0; i < 8; i++) m |= (v >> i & 1) ? 1u << (10 + 2 * i) : 1u << (9 + 2 * i);
  return m;
}

/* 5: the SIG bits as a byte, CPU to RSP and RSP to CPU. */
static void t_sig(void) {
  show("5 SIG byte");
  start(HT_M_SIGIN, 0, 0);
  uint32_t in_bad = 0, cpu_bad = 0, in_to = 0;
  for (uint32_t v = 0; v < 256; v++) {
    *SP_STATUS = sig_mask(v);
    if ((*SP_STATUS >> 7 & 0xff) != v) cpu_bad++;
    DM(HT_IN) = v + 1;
    const uint32_t t0 = TICKS_READ();
    while ((DM(HT_OUT) >> 8) != v + 1)
      if (TICKS_READ() - t0 > US_TICKS(1000)) { in_to++; break; }
    if ((DM(HT_OUT) & 0xff) != v) in_bad++;
  }
  DM(HT_IN) = 0xFFFFFFFFu;
  bool halted = wait_halt(100000);
  if (!halted) *SP_STATUS = SP_WSTATUS_SET_HALT;
  sig_clear_all();

  start(HT_M_SIGOUT, 0, 0);
  uint32_t out_bad = 0, out_to = 0;
  for (uint32_t v = 0; v < 256; v++) {
    const uint32_t t0 = TICKS_READ();
    while (DM(HT_COUNT) != v + 1)
      if (TICKS_READ() - t0 > US_TICKS(1000)) { out_to++; break; }
    if ((*SP_STATUS >> 7 & 0xff) != v) out_bad++;
    DM(HT_IN) = v + 1;
  }
  const bool halted2 = wait_halt(100000);
  if (!halted2) *SP_STATUS = SP_WSTATUS_SET_HALT;
  sig_clear_all();
  const bool ok = in_bad == 0 && cpu_bad == 0 && in_to == 0 && out_bad == 0 && out_to == 0 && halted && halted2;
  if (!ok) fail();
  line("5 SIG byte: %s", ok ? "ok" : "FAIL");
  line("  cpu->rsp bad %lu to %lu (cpu view bad %lu)", in_bad, in_to, cpu_bad);
  line("  rsp->cpu bad %lu to %lu", out_bad, out_to);
}

/* 6: doorbell round trip: CPU sets SIG3, RSP answers with SIG0 (polled),
 * then with SIG0 plus the SP interrupt. */
static void t_bell(void) {
  show("6 doorbell");
  uint32_t mn[2] = {~0u, ~0u}, mx[2] = {0, 0}, sum[2] = {0, 0}, to[2] = {0, 0};
  register_SP_handler(sp_irq);
  set_SP_interrupt(1);
  for (int irq = 0; irq < 2; irq++) {
    start(HT_M_BELL, irq, 0);
    spin_us(1000);
    for (int i = 0; i < 1000; i++) {
      *SP_STATUS = SP_WSTATUS_CLEAR_SIG0;
      const uint32_t n0 = s_irq_n;
      const uint32_t t0 = TICKS_READ();
      *SP_STATUS = SP_WSTATUS_SET_SIG3;
      uint32_t dt;
      if (!irq) {
        while (!(*SP_STATUS & SP_STATUS_SIG0))
          if (TICKS_READ() - t0 > US_TICKS(1000)) break;
        dt = TICKS_READ() - t0;
      } else {
        while (s_irq_n == n0)
          if (TICKS_READ() - t0 > US_TICKS(1000)) break;
        dt = s_irq_t - t0;
      }
      if (dt > US_TICKS(1000)) { to[irq]++; continue; }
      if (dt < mn[irq]) mn[irq] = dt;
      if (dt > mx[irq]) mx[irq] = dt;
      sum[irq] += dt;
    }
    stop_rsp();
  }
  set_SP_interrupt(0);
  unregister_SP_handler(sp_irq);
  const bool ok = to[0] == 0 && to[1] == 0;
  if (!ok) fail();
  line("6 bell: %s (CPU cyc min/avg/max)", ok ? "ok" : "FAIL");
  for (int irq = 0; irq < 2; irq++) {
    const uint32_t n = 1000 - to[irq];
    line("  %s %lu/%lu/%lu to=%lu", irq ? "irq " : "poll", CYC(mn[irq]), n ? CYC(sum[irq] / n) : 0,
         CYC(mx[irq]), to[irq]);
  }
}

/* 7: RSP DMA from RDRAM while the CPU reads or writes DMEM. */
static void t_dma(void) {
  show("7 DMA under CPU traffic");
  uint32_t *buf = memalign(16, HT_DMABUF_SIZE);
  uint32_t sum = 0;
  for (int i = 0; i < HT_DMABUF_SIZE / 4; i++) { buf[i] = i * 0x01010101u + 0x1234567u; sum += buf[i]; }
  data_cache_hit_writeback(buf, HT_DMABUF_SIZE);
  start(HT_M_DMA, PhysicalAddr(buf), sum);
  spin_us(1000);
  uint32_t r[3];
  rsp_rates(r);
  /* Reads of the buffer the DMA is writing. */
  volatile uint32_t sink = 0;
  for (int i = 0; i < 20000; i++) sink += DM(HT_DMABUF + (i & 0xff) * 4);
  (void)sink;
  const bool halted = stop_rsp();
  const uint32_t passes = DM(HT_COUNT), bad = DM(HT_RES + 8);
  free(buf);
  const bool ok = halted && bad == 0 && passes > 100;
  if (!ok) fail();
  line("7 DMA: %s passes %lu bad %lu", ok ? "ok" : "FAIL", passes, bad);
  line("  passes/ms idle %lu rd %lu wr %lu", r[0], r[1], r[2]);
}

/* 8: access costs, RSP halted (running: from test 1). */
static void t_cost(void) {
  show("8 access costs");
  uint32_t *buf = memalign(16, 64);
  buf[0] = 1;
  volatile uint32_t *unc = UncachedAddr(buf);
  const uint32_t dr = cost_rd(&DM(HT_SCRATCH)), dw = cost_wr(&DM(HT_SCRATCH));
  const uint32_t ur = cost_rd(unc), uw = cost_wr(unc);
  const uint32_t cr = cost_rd((volatile uint32_t *)buf);
  const uint32_t sr = cost_rd(SP_STATUS);
  uint32_t t0 = TICKS_READ();
  for (int i = 0; i < 4096; i++) { (void)*SP_SEMAPHORE; *SP_SEMAPHORE = 0; }
  const uint32_t sem = CYC(TICKS_READ() - t0) / 4096;
  free(buf);
  line("8 CPU cyc/access (halted / running)");
  line("  DMEM rd %lu / %lu  wr %lu / %lu", dr, s_cost_dmem_rd_run, dw, s_cost_dmem_wr_run);
  line("  RDRAM uncached rd %lu wr %lu", ur, uw);
  line("  RDRAM cached rd %lu", cr);
  line("  SP_STATUS rd %lu  sema take+rel %lu", sr, sem);
}

int main(void) {
  debug_init_isviewer();
  debug_init_usblog();
  console_init();
  console_set_render_mode(RENDER_MANUAL);
  rsp_init();
  rsp_load(&rsp_hwtest);
  line("RSP/CPU signalling test v1");
  t_read();
  t_seq();
  t_echo();
  t_sem();
  t_sig();
  t_bell();
  t_dma();
  t_cost();
  line("RESULT: %s (%d failed)", s_fails ? "FAIL" : "ALL OK", s_fails);
  debugf("HWTEST_DONE\n");
  show(NULL);
  while (1) {}
}
