/* Echo in place (SNESRECOMP_ECHO_IN_PLACE): the deferred S-DSP owns its echo
 * ring inside ARAM and runs behind the SPC; SPC writes into lines it has
 * still to touch are diverted to it, reads of lines it has still to write
 * wait for it.
 *
 * Built twice by tests/run_c_tests.sh with the same random script: once
 * with the S-DSP in lockstep (the reference) and once deferred with echo in
 * place, where a stand-in for the RSP runs each launched batch at its join,
 * on the live ARAM, applying only register writes and diverted writes, as
 * the microcode does. Joining as late as possible is the hardest case for
 * the rules. Both print a hash of every SPC read, and of ARAM and the S-DSP
 * (state and output) at checkpoints; the script compares the outputs.
 *
 * The SPC itself runs an idle loop; the script's writes and reads go
 * through the SPC's own bus (apu_cpuWrite/apu_cpuRead) between run slices,
 * which is where SPC accesses happen. The script moves the echo ring
 * (ESA, EDL), turns echo writes on and off, clears and pokes the ring and
 * reads it, while voices with echo enabled play. Sample data and the
 * directory stay outside every ring. Test memory holds no retail data. */
#include <stddef.h>
#include <stdio.h>
#ifndef SNESRECOMP_APU_NATIVE
#define SNESRECOMP_APU_NATIVE 1
#endif

#if SNESRECOMP_ECHO_IN_PLACE
#define SNESRECOMP_DSP_LAUNCH_HOOK test_launch
#define SNESRECOMP_DSP_JOIN_HOOK test_join
#endif
#include "../../runner/src/snes/apu.c"

uint64_t g_apu_timer0_total_ticks;
int snes_frame_counter;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

#if SNESRECOMP_ECHO_IN_PLACE
/* The RSP stand-in. */
static ApuDspLogEntry s_log[APU_DSP_LOG_LEN];
static uint32_t s_n, s_samples, s_launches, s_rejects, s_diverted;
static Dsp *s_dsp;
static uint8_t *s_aram;
static uint32_t s_reject_rng = 0x9e3779b9u;
bool test_launch(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log, uint32_t n, uint32_t samples,
                 const uint8_t *marks, uint8_t *fresh) {
  (void)marks; (void)fresh;
  s_reject_rng ^= s_reject_rng << 13; s_reject_rng ^= s_reject_rng >> 17; s_reject_rng ^= s_reject_rng << 5;
  /* The microcode takes its blocks from the records; this stand-in reads
   * ARAM, which the script keeps unchanged where samples live. */
  if (!g_apu_dsp_records.valid || (s_reject_rng & 3) == 0) { s_rejects++; return false; }
  s_n = 0;
  for (uint32_t i = 0; i < n; i++)
    if (log[i].isReg == 1 || log[i].isReg == 3) {
      s_log[s_n++] = log[i];
      s_diverted += log[i].isReg == 3;
    }
  s_dsp = dsp; s_aram = aram; s_samples = samples;
  s_launches++;
  return true;
}
void test_join(void) {
  uint32_t i = 0;
  for (uint32_t k = 0; k <= s_samples; k++) {
    for (; i < s_n && s_log[i].tag == k; i++) {
      if (s_log[i].isReg == 1) dsp_write(s_dsp, (uint8_t)s_log[i].adr, s_log[i].val);
      else s_aram[s_log[i].adr] = s_log[i].val;
    }
    if (k < s_samples) dsp_cycle(s_dsp);
  }
}
void apu_echo_cache_drop(const void *p, uint32_t len) { (void)p; (void)len; }
uint8_t apu_echo_read_uncached(const uint8_t *p) { return *p; }
#endif

static uint32_t h = 2166136261u;
static void mix(const void *p, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 16777619u; }
}

static void dspw(Apu *apu, uint8_t r, uint8_t v) {
  apu_cpuWrite(apu, 0xf2, r);
  apu_cpuWrite(apu, 0xf3, v);
}

static void checkpoint(Apu *apu, unsigned step) {
#if SNESRECOMP_APU_DEFER_DSP
  apu_dspSync(apu);
#endif
  uint32_t hr = 2166136261u, hd = 2166136261u;
  for (int i = 0; i < 0x10000; i++) { hr ^= apu->ram[i]; hr *= 16777619u; }
  const uint8_t *d = (const uint8_t *)apu->dsp->ram;
  for (size_t i = 0; i < sizeof(Dsp) - offsetof(Dsp, ram); i++) { hd ^= d[i]; hd *= 16777619u; }
  printf("step %u: reads %08x ram %08x dsp %08x\n", step, h, hr, hd);
  if (getenv("EIP_RAW") && step == (unsigned)atoi(getenv("EIP_RAW"))) {
    FILE *f = fopen(getenv("EIP_RAWF"), "wb");
    fwrite(apu->dsp, sizeof(Dsp), 1, f); fclose(f);
  }
  if (getenv("EIP_DUMP")) {
    const Dsp *x = apu->dsp;
    printf("  idx %u rem %u fi %u firL", x->echoBufferIndex, x->echoRemain, x->firBufferIndex);
    for (int i = 0; i < 8; i++) printf(" %d", x->firBufferL[i]);
    printf(" firR");
    for (int i = 0; i < 8; i++) printf(" %d", x->firBufferR[i]);
    printf(" ram0 %02x %02x %02x %02x\n", apu->ram[0], apu->ram[1], apu->ram[2], apu->ram[3]);
  }
}

int main(int argc, char **argv) {
  const unsigned steps = argc > 1 ? (unsigned)atoi(argv[1]) : 6000;
  const unsigned every = argc > 2 ? (unsigned)atoi(argv[2]) : 500;
  Apu *apu = apu_init();
  apu_reset(apu);
  /* Idle loop at $0400: bra $0400. */
  apu->ram[0x400] = 0x2f; apu->ram[0x401] = 0xfe;
  apu->romReadable = false;
  apu->spc->pc = 0x400;
  apu->cpuCyclesLeft = 0;
  /* Sample data at $2000: 16 looping blocks of random BRR; directory at
   * $1000 (DIR $10), 4 entries, all start $2000, loop at $2000 + 9k. */
  for (int b = 0; b < 16; b++) {
    uint8_t *p = apu->ram + 0x2000 + 9 * b;
    p[0] = (uint8_t)((rnd() % 12) << 4 | (rnd() % 4) << 2 | (b == 15 ? 3 : 0));
    for (int k = 1; k < 9; k++) p[k] = (uint8_t)rnd();
  }
  for (int e = 0; e < 4; e++) {
    const uint16_t loop = (uint16_t)(0x2000 + 9 * (e * 3));
    apu->ram[0x1000 + 4 * e] = 0x00; apu->ram[0x1001 + 4 * e] = 0x20;
    apu->ram[0x1002 + 4 * e] = (uint8_t)loop; apu->ram[0x1003 + 4 * e] = (uint8_t)(loop >> 8);
  }
  dspw(apu, 0x6c, 0x20);                    /* echo writes off, unmuted */
  dspw(apu, 0x5d, 0x10);
  dspw(apu, 0x0c, 0x50); dspw(apu, 0x1c, 0x50);
  for (int v = 0; v < 4; v++) {
    dspw(apu, (uint8_t)(v * 16 + 0), 0x40); dspw(apu, (uint8_t)(v * 16 + 1), 0x40);
    dspw(apu, (uint8_t)(v * 16 + 2), (uint8_t)rnd()); dspw(apu, (uint8_t)(v * 16 + 3), (uint8_t)(rnd() & 0x1f));
    dspw(apu, (uint8_t)(v * 16 + 4), (uint8_t)v);
    dspw(apu, (uint8_t)(v * 16 + 5), 0x8f); dspw(apu, (uint8_t)(v * 16 + 6), 0xe0);
  }
  dspw(apu, 0x4d, 0x0f);                    /* EON */
  dspw(apu, 0x0d, 0x50);                    /* EFB */
  dspw(apu, 0x2c, 0x30); dspw(apu, 0x3c, 0x30);
  for (int c = 0; c < 8; c++) dspw(apu, (uint8_t)(c * 16 + 15), (uint8_t)(rnd() % 64 - 32));
  dspw(apu, 0x6d, 0x60); dspw(apu, 0x7d, 0x01);
  dspw(apu, 0x4c, 0x0f);                    /* key on */

  uint8_t esa = 0x60, edl = 1, flg = 0x20;
  for (unsigned step = 0; step < steps; step++) {
    apu_runCycles(apu, rnd() % 1500);
    const uint32_t r = rnd() % 100;
    if (getenv("EIP_TRACE") && step + 3 >= (unsigned)atoi(getenv("EIP_TRACE")) && step <= (unsigned)atoi(getenv("EIP_TRACE")))
      fprintf(stderr, "step %u: r %u esa %02x edl %u flg %02x cycles %u\n", step + 1, r, esa, edl, flg, apu->cycles);
    const uint16_t ring = (uint16_t)(esa << 8), len = edl ? (uint16_t)(edl * 2048) : 4;
    if (r < 3) {                 /* move the ring (pages $30-$d0; rings stay clear of
                                    $0000-$2fff, the code and the stand-in's samples) */
      esa = (uint8_t)(0x30 + rnd() % 0xa1);
      dspw(apu, 0x6d, esa);
    } else if (r < 6) {
      edl = (uint8_t)(rnd() % 5);
      dspw(apu, 0x7d, edl);
    } else if (r < 14) {         /* echo writes on/off */
      flg ^= 0x20;
      dspw(apu, 0x6c, flg);
    } else if (r < 18) {         /* clear part of the ring */
      const uint16_t at = (uint16_t)(ring + (rnd() % len & ~3u));
      const unsigned n = 4 + rnd() % 64;
      for (unsigned k = 0; k < n; k++) {
        apu_cpuWrite(apu, (uint16_t)(at + k), 0);
        if (k % 8 == 7) apu_runCycles(apu, rnd() % 40);
      }
    } else if (r < 45) {         /* poke the ring, often near where the S-DSP is */
      uint16_t at = (uint16_t)(ring + rnd() % len);
      if (rnd() & 1) at = (uint16_t)(ring + ((apu->cycles / 32u * 4u + rnd() % 64) % len));
      if (at >= 0x0400 && at < 0x3000) at = (uint16_t)(0x3000 + (at & 0xff));
      apu_cpuWrite(apu, at, (uint8_t)rnd());
    } else if (r < 80) {         /* read the ring */
      uint16_t at = (uint16_t)(ring + rnd() % len);
      if (rnd() & 1) at = (uint16_t)(ring + ((apu->cycles / 32u * 4u + rnd() % 64) % len));
      const uint8_t v = apu_cpuRead(apu, at);
      mix(&v, 1);
    } else if (r < 83) {
      dspw(apu, 0x4c, (uint8_t)(1u << (rnd() % 4)));
    } else if (r < 85) {
      dspw(apu, 0x0d, (uint8_t)rnd());
    }
    if (step % every == every - 1) checkpoint(apu, step + 1);
  }
  checkpoint(apu, steps);
#if SNESRECOMP_ECHO_IN_PLACE
  fprintf(stderr, "echo in place: %u batches launched, %u left to C, %u diverted writes, %u echo conflicts\n",
          s_launches, s_rejects, s_diverted, g_dsp_fe_echo_conflicts);
#endif
  apu_free(apu);
  return 0;
}
