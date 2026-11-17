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
uint32_t g_vm_chunks, g_vm_rollbacks;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static uint32_t s_rng = 0x1234567u;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}
static int16_t rnd15(void) { return (int16_t)((int32_t)(rnd() << 17) >> 17); }

#ifndef RSPDSP_TESTS
#define RSPDSP_TESTS 2000
#endif
enum { kMaxLog = 64 };
static SnesDspLogIn s_log[kMaxLog];

static void random_state(Dsp *d, uint8_t *aram) {
  memset(d, 0, sizeof *d);
  d->apu_ram = aram;
  dsp_reset(d);
  d->apu_ram = aram;
  for (int r = 0; r < 0x80; r++) {
    uint8_t v = (uint8_t)rnd();
#if !RSPDSP_VM
    if (r == 0x2d) v &= 1;                       /* pitch modulation: C path */
#endif
    if (r == 0x7d) v &= 0x0f;
    if (r == 0x4c || r == 0x5c) v &= (uint8_t)rnd();
    if (r == 0x3d) v &= (uint8_t)rnd() & (uint8_t)rnd();
    dsp_write(d, (uint8_t)r, v);
  }
#ifdef RSPDSP_REAL_PITCH
  /* Timing runs: pitches like real music (SMW decodes ~0.47 BRR blocks per
   * sample across its voices). */
  for (int c = 0; c < 8; c++) {
    const unsigned p = 0x200 + rnd() % 0x1400;
    dsp_write(d, (uint8_t)(c * 16 + 2), (uint8_t)p);
    dsp_write(d, (uint8_t)(c * 16 + 3), (uint8_t)(p >> 8));
  }
#endif
  /* A third of the states have echo idle (writes off, volumes 0): the
   * no-echo variant runs those batches unless the log turns echo on. */
  if (rnd() % 3 == 0) {
    dsp_write(d, 0x6c, (uint8_t)(rnd() | 0x20));
    dsp_write(d, 0x2c, 0);
    dsp_write(d, 0x3c, 0);
  }
  /* Echo window inside ARAM, not touching $0000-$000F. */
  const unsigned edl = rnd() % 16, pages = edl ? edl * 8 : 1;
#ifdef RSPDSP_REAL_PITCH   /* (timing: the ring in the upper half, samples and directory below) */
  const unsigned esa = 0x80 + rnd() % (128 - pages);
  dsp_write(d, 0x5d, (uint8_t)(1 + rnd() % 0x6c));
#else
  const unsigned esa = 1 + rnd() % (256 - pages);
#endif
  dsp_write(d, 0x6d, (uint8_t)esa);
  dsp_write(d, 0x7d, (uint8_t)edl);
  d->echoRemain = (uint16_t)(1 + rnd() % d->echoDelay);
  d->echoBufferIndex = (uint16_t)(rnd() % (d->echoDelay - d->echoRemain + 1));
  d->firBufferIndex = rnd() & 7;
  for (int i = 0; i < 8; i++) { d->firBufferL[i] = rnd15(); d->firBufferR[i] = rnd15(); }
  d->evenCycle = rnd() & 1;
  d->noiseSample = rnd15();
  d->noiseCounter = (uint16_t)(rnd() % 64);
#ifdef RSPDSP_REAL_PITCH
  /* Sample data outside the echo ring, as in games: the directory's start
   * and loop addresses, and the voices' block addresses. */
  const unsigned ring0 = esa << 8, ring1 = ring0 + (edl ? edl * 2048u : 4u);
#define OUTSIDE(a) ((unsigned)(a) + 64u <= ring0 || (unsigned)(a) >= ring1 + 16u)
  for (int k = 0; k < 512; k++) {
    unsigned a;
    do a = 0x100 + rnd() % 0x6000; while (!OUTSIDE(a));
    aram[(d->dirPage + 2 * k) & 0xffff] = (uint8_t)a;
    aram[(d->dirPage + 2 * k + 1) & 0xffff] = (uint8_t)(a >> 8);
  }
#endif
  for (int c = 0; c < 8; c++) {
    DspChannel *ch = &d->channel[c];
    ch->pitchCounter = (uint16_t)rnd();
    for (int i = 0; i < 19; i++) ch->decodeBuffer[i] = rnd15();
    ch->decodeOffset = (uint16_t)rnd();
#ifdef RSPDSP_REAL_PITCH
    ch->decodeOffset = (uint16_t)(0x100 + rnd() % 0x6000);
#endif
    ch->previousFlags = rnd() & 3;
    ch->old = rnd15();
    ch->older = rnd15();
    ch->rateCounter = (uint16_t)(rnd() % 2048);
    ch->adsrState = (uint8_t)(rnd() % 5);
    ch->gain = (uint16_t)(rnd() & 0x7ff);
    if ((rnd() & 3) == 0) ch->gain = 0;
    ch->sampleOut = rnd15();
#ifdef RSPDSP_ACTIVE   /* (timing: every voice playing, none released) */
    ch->gain = (uint16_t)(0x100 + rnd() % 0x600);
    ch->adsrState = (uint8_t)(rnd() % 3);
#endif
  }
#ifdef RSPDSP_ACTIVE
  dsp_write(d, 0x5c, 0);
  dsp_write(d, 0x6c, (uint8_t)(d->ram[0x6c] & 0x7f));   /* no reset */
#endif
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
#if !RSPDSP_VM
      if (r == 0x2d) v &= 1;
#endif
      if (r == 0x6d || r == 0x7d) r = 0x4c;      /* keep the echo window */
      if (r == 0x5d && (rnd() & 1)) r = 0x4c;    /* key on often */
#ifdef RSPDSP_ACTIVE
      if (r == 0x5c || r == 0x6c) r = 0x4c;
#endif
#ifdef RSPDSP_REAL_PITCH
      if ((r & 0x0e) == 0x02) r = 0x4c;
#endif
      s_log[i].isReg = 1; s_log[i].adr = r; s_log[i].val = v;
    } else {
      uint16_t a = (uint16_t)rnd();
      switch (rnd() % 4) {
        case 0: a &= 0x1f; break;                                /* incl. mirror */
        case 1: a = (uint16_t)(d->echoBufferAdr + (rnd() & 0xff)); break;
#ifndef RSPDSP_REAL_PITCH   /* (keeps the directory's targets outside the echo ring) */
        case 2: a = (uint16_t)(d->dirPage + (rnd() & 0x3ff)); break;
#endif
        default: break;
      }
      s_log[i].isReg = 0; s_log[i].adr = a; s_log[i].val = (uint8_t)rnd();
    }
  }
  return n;
}

int main(void) {
  debug_init_isviewer();
  rspq_init();
  snesdsp_rsp_init();
  static Dsp a, b;
  uint8_t *aram_ref = memalign(16, 0x10000);
  uint8_t *aram_rsp = memalign(16, RSPDSP_ARAM_SIZE);
  int fails = 0, fallbacks = 0, tests = RSPDSP_TESTS;
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
    { int shown = 0;
      for (size_t o = lo; o < hi; o++) if (pa[o] != pb[o]) {
        if (fails < 3 && shown++ < 12) debugf("  state differs at Dsp+0x%x: ref %02x rsp %02x\n", (unsigned)o, pa[o], pb[o]);
        bad = 1;
      }
#if RSPDSP_BLOCKS
      { extern uint32_t g_snesdsp_rsp_last_nblk, g_snesdsp_rsp_last_nkon;
        if (bad && fails < 3) debugf("  records: %lu blocks, %lu key ons\n", (unsigned long)g_snesdsp_rsp_last_nblk,
                                     (unsigned long)g_snesdsp_rsp_last_nkon); }
#endif
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
  { extern uint32_t g_snesdsp_rsp_ne_batches;
    debugf("no-echo variant: %lu batches\n", (unsigned long)g_snesdsp_rsp_ne_batches); }
#ifdef PROF_VM_COUNT
  debugf("voice-major: %lu chunks, %lu rollbacks\n", (unsigned long)g_vm_chunks, (unsigned long)g_vm_rollbacks);
#endif
#if RSPDSP_VM
  { extern uint32_t g_snesdsp_rsp_vm_batches;
    debugf("voice-major variant: %lu batches\n", (unsigned long)g_snesdsp_rsp_vm_batches); }
#endif
  debugf("RSPDSP_TEST %s: %d/%d pass, %d fell back to C\n", fails ? "FAIL" : "PASS",
         tests - fails - fallbacks, tests - fallbacks, fallbacks);
  for (;;) {}
}
