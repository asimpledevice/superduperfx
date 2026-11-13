/* Red mailbox test: rspq's one-word mailbox and doorbell, end to end, on
 * real rspq with two overlays.
 *
 *  R1 post: a queued command posts a value; the SP interrupt's mailbox
 *     handler takes it and sets a run flag in $f31; the CPU waits on the
 *     flag (DMFC1, no memory access). Latency from rspq_flush.
 *  R2 serve: one long-running command answers each new down word with
 *     down + 1 while it runs (try-post: none may be lost); round trips.
 *  R3 overlay switches: overlay A echoes the down word, overlay B its
 *     inverse, alternately: the mailbox must survive overlay loads.
 *  R4 the run flags: no bit appears that nobody set, and a cleared bit
 *     stays clear through other interrupts (vblank).
 *  R5 ring post: R1 through Red's DMEM command ring (no flush, no DMA).
 *  R6 ring under load: ring commands for both overlays while a long RDRAM
 *     stream runs; both overlays' sums must come out exact (the stream's
 *     position survives ring commands and the overlay switches they cause).
 *  R7 ring full: back-to-back ring commands, waiting for room, wrapping.
 *  R8 highpri: a highpri sequence still runs and ends with Red spinning.
 *
 * Results on screen (one page, for a photo) and in the debug log. */
#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>
#include "run_flags.h"
#include <rspq_constants.h>

DEFINE_RSP_UCODE(rsp_red_a);
DEFINE_RSP_UCODE(rsp_red_b);

#define CYC(ticks) ((uint32_t)(ticks) * 2u)   /* TICKS run at half the CPU clock */
#define US_TICKS(us) ((uint32_t)(us) * 47u)

enum { A_POST, A_SERVE, A_ECHO, A_ADD, A_REPORT, A_BUSY };
enum { B_ECHO, B_ADD, B_REPORT };

static char s_lines[24][48];
static int s_nlines, s_fails;
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
  if (s_nlines < 23) s_nlines++;
}

/* The doorbell handler, in the SP interrupt. */
static volatile uint32_t s_up, s_bells, s_bell_t;
static void on_bell(uint32_t up) {
  s_bell_t = TICKS_READ();
  s_up = up;
  s_bells++;
  run_flags_set_in_interrupt(RUN_FLAG_DOORBELL);
}

/* Wait for the doorbell flag, then consume it. */
static bool wait_bell(uint32_t t0) {
  while (!(run_flags() & RUN_FLAG_DOORBELL))
    if (TICKS_READ() - t0 > US_TICKS(5000)) return false;
  run_flags_clear(RUN_FLAG_DOORBELL);
  return true;
}

static uint32_t s_ovl_a, s_ovl_b;

static void r_post(void) {
  show("R1 post");
  uint32_t sum = 0, mx = 0, bad = 0, to = 0;
  for (uint32_t i = 1; i <= 2000; i++) {
    const uint32_t v = i * 2654435761u;
    const uint32_t t0 = TICKS_READ();
    rspq_write(s_ovl_a, A_POST, 0, v);
    rspq_flush();
    if (!wait_bell(t0)) { to++; continue; }
    if (s_up != v) bad++;
    const uint32_t dt = s_bell_t - t0;
    sum += dt;
    if (dt > mx) mx = dt;
  }
  rspq_wait();
  const bool ok = bad == 0 && to == 0;
  if (!ok) s_fails++;
  const uint32_t n = 2000 - to;
  line("R1 post->flag: %s cyc avg %lu max %lu", ok ? "ok" : "FAIL", n ? CYC(sum / n) : 0, CYC(mx));
  line("   bad %lu to %lu", bad, to);
}

static void r_serve(void) {
  show("R2 serve");
  const uint32_t n = 20000;
  uint32_t sum = 0, mx = 0, bad = 0, to = 0;
  rspq_mailbox_send(0);
  rspq_write(s_ovl_a, A_SERVE, n);
  rspq_flush();
  wait_ms(1);
  for (uint32_t i = 1; i <= n; i++) {
    const uint32_t v = i * 0x9E3779B9u | 1u;   /* never 0, so always new */
    const uint32_t t0 = TICKS_READ();
    rspq_mailbox_send(v);
    if (!wait_bell(t0)) { to++; break; }
    if (s_up != v + 1) bad++;
    const uint32_t dt = s_bell_t - t0;
    sum += dt;
    if (dt > mx) mx = dt;
  }
  rspq_wait();
  const bool ok = bad == 0 && to == 0;
  if (!ok) s_fails++;
  line("R2 serve rtt: %s cyc avg %lu max %lu", ok ? "ok" : "FAIL", CYC(sum / n), CYC(mx));
  line("   bad %lu to %lu", bad, to);
}

static void r_switch(void) {
  show("R3 overlay switches");
  uint32_t bad = 0, to = 0;
  for (uint32_t i = 1; i <= 500; i++) {
    const uint32_t v = i * 0x01000193u;
    rspq_mailbox_send(v);
    uint32_t t0 = TICKS_READ();
    rspq_write(s_ovl_a, A_ECHO, 0);
    rspq_flush();
    if (!wait_bell(t0)) { to++; continue; }
    if (s_up != v) bad++;
    t0 = TICKS_READ();
    rspq_write(s_ovl_b, B_ECHO, 0);
    rspq_flush();
    if (!wait_bell(t0)) { to++; continue; }
    if (s_up != ~v) bad++;
  }
  rspq_wait();
  const bool ok = bad == 0 && to == 0;
  if (!ok) s_fails++;
  line("R3 overlay A/B x500: %s bad %lu to %lu", ok ? "ok" : "FAIL", bad, to);
}

static void r_flags(void) {
  show("R4 run flags");
  run_flags_clear(~0u);
  const uint32_t f0 = run_flags();
  wait_ms(100);   /* vblank and timer interrupts meanwhile */
  const uint32_t f1 = run_flags();
  const bool ok = f0 == 0 && f1 == 0;
  if (!ok) s_fails++;
  line("R4 run flags stay clear: %s (%08lx %08lx)", ok ? "ok" : "FAIL", f0, f1);
}

static inline uint32_t cmdw(uint32_t ovl, uint32_t cmd, uint32_t arg) {
  return ovl | (cmd << 24) | (arg & 0xffffffu);
}
static void ring1(uint32_t ovl, uint32_t cmd, uint32_t arg) {
  const uint32_t w = cmdw(ovl, cmd, arg);
  rspq_ring_write(&w, 1);
}

/* Ask an overlay for its sum through the ring; ~0 on timeout. */
static uint32_t report(uint32_t ovl, uint32_t cmd) {
  const uint32_t t0 = TICKS_READ();
  ring1(ovl, cmd, 0);
  return wait_bell(t0) ? s_up : ~0u;
}

static void r_ring_post(void) {
  show("R5 ring post");
  uint32_t sum = 0, mx = 0, bad = 0, to = 0;
  for (uint32_t i = 1; i <= 2000; i++) {
    const uint32_t v = i * 2654435761u;
    const uint32_t w[2] = {cmdw(s_ovl_a, A_POST, 0), v};
    const uint32_t t0 = TICKS_READ();
    rspq_ring_write(w, 2);
    if (!wait_bell(t0)) { to++; continue; }
    if (s_up != v) bad++;
    const uint32_t dt = s_bell_t - t0;
    sum += dt;
    if (dt > mx) mx = dt;
  }
  const bool ok = bad == 0 && to == 0;
  if (!ok) s_fails++;
  const uint32_t n = 2000 - to;
  line("R5 ring->flag: %s cyc avg %lu max %lu", ok ? "ok" : "FAIL", n ? CYC(sum / n) : 0, CYC(mx));
  line("   bad %lu to %lu", bad, to);
}

static void r_ring_load(void) {
  show("R6 ring under load");
  const uint32_t a0 = report(s_ovl_a, A_REPORT), b0 = report(s_ovl_b, B_REPORT);
  const uint32_t N = 6000, K = N / 16;
  uint32_t k = 0;
  for (uint32_t i = 0; i < N; i++) {
    rspq_write(s_ovl_a, A_ADD, 1);
    if ((i & 7) == 0) rspq_write(s_ovl_a, A_BUSY, 300);
    if ((i & 15) == 0 && k < K) {
      ring1(s_ovl_b, B_ADD, 7);
      ring1(s_ovl_a, A_ADD, 1000);
      k++;
    }
  }
  rspq_wait();
  const uint32_t a = report(s_ovl_a, A_REPORT) - a0, b = report(s_ovl_b, B_REPORT) - b0;
  const bool ok = k == K && a == N + K * 1000 && b == K * 7;
  if (!ok) s_fails++;
  line("R6 ring+stream: %s x%lu", ok ? "ok" : "FAIL", k);
  line("   A %lu/%lu B %lu/%lu", a, N + K * 1000, b, K * 7);
}

static void r_ring_full(void) {
  show("R7 ring full");
  const uint32_t a0 = report(s_ovl_a, A_REPORT);
  const uint32_t n = 20000, c0 = rspq_ring_commands, w0 = rspq_ring_wait_ticks;
  const uint32_t t0 = TICKS_READ();
  for (uint32_t i = 0; i < n; i++) {
    /* 1-3 words (an add, then no-ops): every wrap position. A 0 word would
       be a WaitNewInput, which must never be written to the ring. */
    const uint32_t w[3] = {cmdw(s_ovl_a, A_ADD, 3), RSPQ_RING_PAD_WORD, RSPQ_RING_PAD_WORD};
    rspq_ring_write(w, 1 + (i % 3));
  }
  const uint32_t dt = TICKS_READ() - t0, c = rspq_ring_commands - c0;
  const uint32_t a = report(s_ovl_a, A_REPORT) - a0;
  const bool ok = a == 3 * n && c == n;
  if (!ok) s_fails++;
  line("R7 ring x%lu: %s sum %lu cyc/cmd %lu", n, ok ? "ok" : "FAIL", a, CYC(dt / n));
  line("   waiting %lu%%", (rspq_ring_wait_ticks - w0) * 100u / (dt ? dt : 1));
}

static void r_highpri(void) {
  show("R8 highpri");
  const uint32_t a0 = report(s_ovl_a, A_REPORT);
  for (int i = 0; i < 50; i++) {
    rspq_write(s_ovl_a, A_BUSY, 2000);
    rspq_highpri_begin();
    rspq_write(s_ovl_a, A_ADD, 5);
    rspq_highpri_end();
    rspq_highpri_sync();
  }
  rspq_wait();
  const uint32_t a = report(s_ovl_a, A_REPORT) - a0;
  const bool ok = a == 250;
  if (!ok) s_fails++;
  line("R8 highpri x50: %s sum %lu", ok ? "ok" : "FAIL", a);
}

int main(void) {
  debug_init_isviewer();
  debug_init_usblog();
  console_init();
  console_set_render_mode(RENDER_MANUAL);
  run_flags_reset();
  rspq_init();
  s_ovl_a = rspq_overlay_register(&rsp_red_a);
  s_ovl_b = rspq_overlay_register(&rsp_red_b);
  rspq_mailbox_set_handler(on_bell);
  line("Red mailbox + ring test v2");
  r_post();
  r_serve();
  r_switch();
  r_flags();
  r_ring_post();
  r_ring_load();
  r_ring_full();
  r_highpri();
  line("doorbells %lu", s_bells);
  line("RESULT: %s (%d failed)", s_fails ? "FAIL" : "ALL OK", s_fails);
  debugf("REDTEST_DONE\n");
  show(NULL);
  while (1) {}
}
