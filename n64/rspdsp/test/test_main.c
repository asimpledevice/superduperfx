/* RSP S-DSP test: random batches through the C DSP (dsp_cycle/dsp_write,
 * the reference) and through the RSP microcode; compares the DSP state, the
 * ARAM and every output sample. Reports over the debug log. */
#include <libdragon.h>
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "snesdsp_rsp.h"
#include "dsp.h"

/* Symbols the DSP's translation units expect from the runtime. */
int snes_frame_counter;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static uint32_t s_rng = 0x1234567u;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}
static int16_t rnd15(void) { return (int16_t)((int32_t)(rnd() << 17) >> 17); }

enum { kMaxLog = 64 };
static SnesDspLogIn s_log[kMaxLog];

static void random_state(Dsp *d, uint8_t *aram) {
  memset(d, 0, sizeof *d);
  d->apu_ram = aram;
  dsp_reset(d);
  d->apu_ram = aram;
  for (int r = 0; r < 0x80; r++) {
    uint8_t v = (uint8_t)rnd();
    if (r == 0x2d) v &= 1;                       /* pitch modulation: C path */
    if (r == 0x7d) v &= 0x0f;
    if (r == 0x4c || r == 0x5c) v &= (uint8_t)rnd();
    if (r == 0x3d) v &= (uint8_t)rnd() & (uint8_t)rnd();
    dsp_write(d, (uint8_t)r, v);
  }
  /* Echo window inside ARAM, not touching $0000-$000F. */
  const unsigned edl = rnd() % 16, pages = edl ? edl * 8 : 1;
  const unsigned esa = 1 + rnd() % (256 - pages);
  dsp_write(d, 0x6d, (uint8_t)esa);
  dsp_write(d, 0x7d, (uint8_t)edl);
  d->echoRemain = (uint16_t)(1 + rnd() % d->echoDelay);
  d->echoBufferIndex = (uint16_t)(rnd() % (d->echoDelay - d->echoRemain + 1));
  d->firBufferIndex = rnd() & 7;
  for (int i = 0; i < 8; i++) { d->firBufferL[i] = rnd15(); d->firBufferR[i] = rnd15(); }
  d->evenCycle = rnd() & 1;
  d->noiseSample = rnd15();
  d->noiseCounter = (uint16_t)(rnd() % 64);
  for (int c = 0; c < 8; c++) {
    DspChannel *ch = &d->channel[c];
    ch->pitchCounter = (uint16_t)rnd();
    for (int i = 0; i < 19; i++) ch->decodeBuffer[i] = rnd15();
    ch->decodeOffset = (uint16_t)rnd();
    ch->previousFlags = rnd() & 3;
    ch->old = rnd15();
    ch->older = rnd15();
    ch->rateCounter = (uint16_t)(rnd() % 2048);
    ch->adsrState = (uint8_t)(rnd() % 5);
    ch->gain = (uint16_t)(rnd() & 0x7ff);
    if ((rnd() & 3) == 0) ch->gain = 0;
    ch->sampleOut = rnd15();
  }
}

static uint32_t random_log(Dsp *d, uint32_t samples) {
  uint32_t n = rnd() % kMaxLog;
  uint32_t tag = 0;
  for (uint32_t i = 0; i < n; i++) {
    tag += rnd() % 8 == 0 ? rnd() % 16 : 0;
    if (tag > samples) tag = samples;
    s_log[i].tag = (uint16_t)tag;
    if (rnd() & 1) {
      uint8_t r = (uint8_t)(rnd() & 0x7f), v = (uint8_t)rnd();
      if (r == 0x2d) v &= 1;
      if (r == 0x6d || r == 0x7d) r = 0x4c;      /* keep the echo window */
      if (r == 0x5d && (rnd() & 1)) r = 0x4c;    /* key on often */
      s_log[i].isReg = 1; s_log[i].adr = r; s_log[i].val = v;
    } else {
      uint16_t a = (uint16_t)rnd();
      switch (rnd() % 4) {
        case 0: a &= 0x1f; break;                                /* incl. mirror */
        case 1: a = (uint16_t)(d->echoBufferAdr + (rnd() & 0xff)); break;
        case 2: a = (uint16_t)(d->dirPage + (rnd() & 0x3ff)); break;
        default: break;
      }
      s_log[i].isReg = 0; s_log[i].adr = a; s_log[i].val = (uint8_t)rnd();
    }
  }
  return n;
}

int main(void) {
  debug_init_isviewer();
  snesdsp_rsp_init();
  static Dsp a, b;
  uint8_t *aram_ref = memalign(16, 0x10000);
  uint8_t *aram_rsp = memalign(16, RSPDSP_ARAM_SIZE);
  int fails = 0, fallbacks = 0, tests = 2000;
  extern uint32_t g_snesdsp_rsp_ticks, g_snesdsp_rsp_samples;
  uint32_t c_ticks = 0, c_samples = 0;
  for (int t = 0; t < tests; t++) {
    for (int i = 0; i < 0x10000; i += 4) *(uint32_t *)(aram_ref + i) = rnd();
    random_state(&a, aram_ref);
    memcpy(aram_rsp, aram_ref, 0x10000);
    memcpy(aram_rsp + 0x10000, aram_ref, 16);
    b = a;
    const uint32_t samples = 1 + rnd() % ((t & 7) ? 256 : 1024);
    const uint32_t n = random_log(&a, samples);

    if (!snesdsp_rsp_batch(&b, aram_rsp, s_log, n, samples)) {
      fallbacks++;
      continue;
    }
    /* Reference: writes tagged k, sample k, ...; then writes tagged `samples`. */
    uint32_t i = 0;
    const uint32_t ct0 = get_ticks();
    for (uint32_t k = 0; k <= samples; k++) {
      for (; i < n && s_log[i].tag == k; i++) {
        if (s_log[i].isReg) dsp_write(&a, (uint8_t)s_log[i].adr, s_log[i].val);
        else aram_ref[s_log[i].adr] = s_log[i].val;
      }
      if (k < samples) dsp_cycle(&a);
    }
    c_ticks += get_ticks() - ct0;
    c_samples += samples;

    const size_t lo = offsetof(Dsp, ram), hi = offsetof(Dsp, sampleBuffer);
    const uint8_t *pa = (const uint8_t *)&a, *pb = (const uint8_t *)&b;
    int bad = 0;
    for (size_t o = lo; o < hi; o++) if (pa[o] != pb[o]) {
      if (fails < 6) debugf("  state differs at Dsp+0x%x: ref %02x rsp %02x\n", (unsigned)o, pa[o], pb[o]);
      bad = 1; break;
    }
    if (a.sampleWrite != b.sampleWrite) {
      if (fails < 6) debugf("  sampleWrite ref %lu rsp %lu\n", (unsigned long)a.sampleWrite, (unsigned long)b.sampleWrite);
      bad = 1;
    }
    for (uint32_t s = 0; s < samples * 2 && !bad; s++)
      if (a.sampleBuffer[s] != b.sampleBuffer[s]) {
        if (fails < 6) debugf("  output differs at sample %lu %s: ref %d rsp %d\n", (unsigned long)(s / 2),
                              (s & 1) ? "R" : "L", a.sampleBuffer[s], b.sampleBuffer[s]);
        bad = 1;
      }
    data_cache_hit_invalidate(aram_rsp, RSPDSP_ARAM_SIZE);
    for (int x = 0; x < 0x10000 && !bad; x++) if (aram_ref[x] != aram_rsp[x]) {
      if (fails < 6) debugf("  ARAM differs at %04x: ref %02x rsp %02x\n", x, aram_ref[x], aram_rsp[x]);
      bad = 1;
    }
    if (!bad && memcmp(aram_rsp, aram_rsp + 0x10000, 16)) { if (fails < 6) debugf("  mirror stale\n"); bad = 1; }
    if (bad && fails++ < 6) debugf("FAIL test %d: %lu samples, %lu log entries\n", t, (unsigned long)samples, (unsigned long)n);
  }
  debugf("timing: C dsp_cycle %lu CPU cycles/sample; RSP batch %lu RSP cycles/sample (%lu samples)\n",
         (unsigned long)((uint64_t)c_ticks * 2 / c_samples),
         (unsigned long)((uint64_t)g_snesdsp_rsp_ticks * 4 / 3 / g_snesdsp_rsp_samples),
         (unsigned long)g_snesdsp_rsp_samples);
  debugf("RSPDSP_TEST %s: %d/%d pass, %d fell back to C\n", fails ? "FAIL" : "PASS",
         tests - fails - fallbacks, tests - fallbacks, fallbacks);
  for (;;) {}
}
