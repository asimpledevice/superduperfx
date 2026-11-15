/* RSP S-DSP prep test (audio ownership step 2a): random DSP states and
 * random write logs decoded by the C decode (snesdsp_rsp.c, the reference)
 * and by rsp_snesdsp_prep.S; the operations must be byte-identical, and
 * both must reject the same batches. Reports over the debug log. */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>

#include "snesdsp_rsp.h"
#include "dsp.h"
#include "apu_dsp_batch.h"

int snes_frame_counter;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

int snesdsp_rsp_prep_selftest(const Dsp *dsp, const ApuDspLogEntry *log, uint32_t n,
                              uint32_t *nops_out);
const void *snesdsp_rsp_prep_selftest_ops(int rsp);

static uint32_t s_rng = 0x2468aceu;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}

#ifndef PREP_TESTS
#define PREP_TESTS 4000
#endif
enum { kMaxLog = 2048 };
static ApuDspLogEntry s_log[kMaxLog] __attribute__((aligned(8)));

static void random_state(Dsp *d, uint8_t *aram) {
  memset(d, 0, sizeof *d);
  d->apu_ram = aram;
  dsp_reset(d);
  d->apu_ram = aram;
  for (int r = 0; r < 0x80; r++) {
    uint8_t v = (uint8_t)rnd();
    if (r == 0x2d) v &= (rnd() & 3) ? 1 : 0xff;   /* mostly no pitch modulation */
    dsp_write(d, (uint8_t)r, v);
  }
  /* State that is not a function of the register file. */
  for (int c = 0; c < 8; c++) {
    DspChannel *ch = &d->channel[c];
    ch->gainValue = (uint16_t)(rnd() & 0x7ff);
    ch->gainMode = rnd() & 3;
    ch->adsrRates[3] = (uint16_t)rnd();
    ch->pitch = (uint16_t)(rnd() & 0x3fff);
    if (rnd() & 1) ch->useGain = !ch->useGain;
    if (rnd() & 1) ch->directGain = !ch->directGain;
  }
  if (rnd() & 1) {   /* an echo window inside ARAM */
    const unsigned edl = rnd() % 16, pages = edl ? edl * 8 : 1;
    dsp_write(d, 0x6d, (uint8_t)(1 + rnd() % (256 - pages)));
    dsp_write(d, 0x7d, (uint8_t)edl);
  }
  d->echoRemain = (uint16_t)(1 + rnd() % d->echoDelay);
  d->echoBufferIndex = (uint16_t)(rnd() % (d->echoDelay - d->echoRemain + 1));
}

static uint32_t random_log(void) {
  const uint32_t kind = rnd() % 16;
  const uint32_t n = kind == 0 ? 400 + rnd() % 1600 : rnd() % 200;   /* some overflow */
  uint32_t tag = 0;
  for (uint32_t i = 0; i < n; i++) {
    tag += rnd() % 8 == 0 ? rnd() % 16 : 0;
    s_log[i].tag = (uint16_t)tag;
    s_log[i].old = (uint8_t)rnd();
    if (kind == 0 || (rnd() & 1)) {
      uint8_t r = (uint8_t)(rnd() & 0x7f), v = (uint8_t)rnd();
      if (kind == 0 && (rnd() & 1)) r = 0x4d;                 /* the largest decode */
      if (r == 0x2d && (rnd() & 3)) v &= 1;
      if ((r == 0x6d || r == 0x7d || r == 0x6c) && (rnd() & 3)) r = 0x4c;
      s_log[i].isReg = 1; s_log[i].adr = r; s_log[i].val = v;
    } else {
      uint16_t a = (uint16_t)rnd();
      if (rnd() & 1) a &= 0x1f;                                /* incl. the mirror */
      s_log[i].isReg = (rnd() % 8) ? 0 : 2;                    /* 2: verify builds */
      s_log[i].adr = a; s_log[i].val = (uint8_t)rnd();
    }
  }
  return n;
}

int main(void) {
  debug_init_isviewer();
  rspq_init();
  snesdsp_rsp_init();
  static Dsp d;
  static uint8_t aram[0x10000] __attribute__((aligned(16)));
  int same = 0, rejected = 0, fails = 0;
  uint32_t ops = 0;
  for (int t = 0; t < PREP_TESTS; t++) {
    random_state(&d, aram);
    const uint32_t n = random_log();
    uint32_t nops = 0;
    const int r = snesdsp_rsp_prep_selftest(&d, s_log, n, &nops);
    if (r == 1) { same++; ops += nops; continue; }
    if (r == 0) { rejected++; continue; }
    if (fails++ < 8) {
      debugf("[preptest] test %d: result %d (%lu entries, %lu operations)\n", t, r,
             (unsigned long)n, (unsigned long)nops);
      if (r <= -3) {
        const int i = -r - 3;
        const uint16_t *a = (const uint16_t *)snesdsp_rsp_prep_selftest_ops(0) + 4 * i;
        const uint16_t *b = (const uint16_t *)snesdsp_rsp_prep_selftest_ops(1) + 4 * i;
        debugf("[preptest]   op %d: C %04x %04x %04x %04x  RSP %04x %04x %04x %04x\n", i,
               a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
      }
    }
  }
  debugf("[preptest] %d tests: %d identical (%lu operations), %d rejected by both, %d failed\n",
         PREP_TESTS, same, (unsigned long)ops, rejected, fails);
  debugf("[preptest] %s\n", fails ? "FAIL" : "PASS");
  for (;;) {}
}
