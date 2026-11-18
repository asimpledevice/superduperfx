/* Differential test of the SPC700 translator (n64 Makefile SPCDBT=1
 * SPCDBT_TEST=1): random SPC programs run on two APUs, one translated and
 * one interpreted (g_spc_dbt_enabled off while it runs), with random port
 * writes between slices of random length; after every slice ARAM, ports,
 * timers, SPC registers, the S-DSP registers and its output must match.
 * The ROM prints "[spcdbt-test] PASSED" or the first difference, then
 * stops. Test memory holds generated programs only, never game data.
 *
 * Programs:
 * - structured: 4-40 straight-line instructions drawn from every opcode
 *   but control flow (random operands, so stores land anywhere: on I/O
 *   registers, the direct page, the stack and the program itself), a few
 *   short forward branches, then a conditional branch back to the start
 *   and a bra back (loops for superblocks and back-edges);
 * - random: $0200-$3FFF filled with random bytes (STOP and SLEEP aside).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "snes/apu.h"
#include "snes/spc.h"
#include "snes/dsp.h"

#if SNESRECOMP_SPC_DBT

extern void n64_platform_log(const char *fmt, ...);
extern uint16_t g_spc_dbt_watch[3];   /* (spc_dbt.c: ARAM bytes the failure trace shows) */

static uint32_t s_rng = 0x2545f491u;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}

static bool control_flow(uint8_t op) {
  if ((op & 0x1f) == 0x10) return true;                 /* bpl..beq */
  if ((op & 0x0f) == 0x01 || (op & 0x0f) == 0x03) return true;   /* tcall, bbs/bbc */
  switch (op) {
    case 0x2f: case 0x5f: case 0x1f: case 0x3f: case 0x4f: case 0x6f: case 0x7f:
    case 0x0f: case 0xef: case 0xff: case 0x2e: case 0xde: case 0x6e: case 0xfe:
      return true;
    default:
      return false;
  }
}

/* SPC700 instruction lengths (the opcode byte included). */
static int insn_len(uint8_t op) {
  static const uint8_t k[256] = {
    1,1,2,3,2,3,1,2,2,3,3,2,3,1,3,1, 2,1,2,3,2,3,3,2,3,1,2,2,1,1,3,3,
    1,1,2,3,2,3,1,2,2,3,3,2,3,1,3,2, 2,1,2,3,2,3,3,2,3,1,2,2,1,1,2,3,
    1,1,2,3,2,3,1,2,2,3,3,2,3,1,3,2, 2,1,2,3,2,3,3,2,3,1,2,2,1,1,3,3,
    1,1,2,3,2,3,1,2,2,3,3,2,3,1,3,1, 2,1,2,3,2,3,3,2,3,1,2,2,1,1,2,1,
    1,1,2,3,2,3,1,2,2,3,3,2,3,2,1,3, 2,1,2,3,2,3,3,2,3,1,2,2,1,1,1,1,
    1,1,2,3,2,3,1,2,2,3,3,2,3,2,1,1, 2,1,2,3,2,3,3,2,3,1,2,2,1,1,1,1,
    1,1,2,3,2,3,1,2,2,3,3,2,3,2,1,1, 2,1,2,3,2,3,3,2,2,2,2,2,1,1,3,1,
    1,1,2,3,2,3,1,2,2,3,3,2,3,1,1,1, 2,1,2,3,2,3,3,2,2,2,2,2,1,1,2,1,
  };
  return k[op];
}

static void program_structured(Apu *apu) {
  uint8_t *ram = apu->ram;
  uint16_t pc = 0x0200;
  const uint16_t start = pc;
  const int n = 4 + (int)(rnd() % 37);
  for (int i = 0; i < n; i++) {
    uint8_t op;
    do op = (uint8_t)rnd(); while (control_flow(op));
    const int len = insn_len(op);
    ram[pc] = op;
    for (int k = 1; k < len; k++) ram[(uint16_t)(pc + k)] = (uint8_t)rnd();
    /* Absolute operands mostly in plain memory ($0300-$3FFF), sometimes
     * on the I/O page or the program. */
    if (len == 3 && (rnd() & 3)) ram[(uint16_t)(pc + 2)] = (uint8_t)(0x03 + rnd() % 0x3d);
    pc = (uint16_t)(pc + len);
    if (!(rnd() % 8)) {              /* a short forward branch */
      static const uint8_t br[] = { 0x10, 0x30, 0x50, 0x70, 0x90, 0xb0, 0xd0, 0xf0 };
      ram[pc] = br[rnd() & 7];
      ram[(uint16_t)(pc + 1)] = (uint8_t)(rnd() % 4);
      pc = (uint16_t)(pc + 2);
    }
  }
  static const uint8_t back[] = { 0xd0, 0xf0, 0x10, 0x30, 0x90, 0xb0 };
  ram[pc] = back[rnd() % 6];                          /* conditional back to the start */
  ram[(uint16_t)(pc + 1)] = (uint8_t)(start - (pc + 2));
  pc = (uint16_t)(pc + 2);
  if (rnd() & 1) {                                    /* or a dbnz y loop */
    ram[pc] = 0xfe;
    ram[(uint16_t)(pc + 1)] = (uint8_t)(start - (pc + 2));
    pc = (uint16_t)(pc + 2);
  }
  ram[pc] = 0x2f;                                     /* bra start */
  ram[(uint16_t)(pc + 1)] = (uint8_t)(start - (pc + 2));
}

static void program_random(Apu *apu) {
  for (uint32_t a = 0x0200; a < 0x4000; a++) {
    uint8_t b;
    do b = (uint8_t)rnd(); while (b == 0xef || b == 0xff);
    apu->ram[a] = b;
  }
}

static void setup(Apu *apu, unsigned kind, uint32_t seed) {
  apu_reset(apu);
  s_rng = seed;
  for (uint32_t a = 0; a < 0x10000; a++) apu->ram[a] = (uint8_t)rnd();
  if (!(kind & 1)) program_structured(apu); else program_random(apu);
#ifdef SPC_DBT_TEST_SNIPPET   /* (local experiments only: a code snippet from outside, not committed) */
  {
#include SPC_DBT_TEST_SNIPPET
    memcpy(apu->ram + kSnippetAt, k_snippet, sizeof k_snippet);
    apu->spc->pc = kSnippetPc;
  }
#endif
  apu->romReadable = false;
  Spc *spc = apu->spc;
#ifndef SPC_DBT_TEST_SNIPPET
  spc->pc = 0x0200;
#endif
  spc->a = (uint8_t)rnd(); spc->x = (uint8_t)rnd(); spc->y = (uint8_t)rnd();
  spc->sp = (uint8_t)(0xc0 + (rnd() & 0x3f));
  spc->p = rnd() & 1;
  spc->c = rnd() & 1;
  apu->cpuCyclesLeft = 0;
  apu_cpuWrite(apu, 0xf1, (uint8_t)(rnd() & 7));      /* timers */
  apu_cpuWrite(apu, 0xfa, (uint8_t)rnd());
  apu_cpuWrite(apu, 0xfb, (uint8_t)rnd());
  apu_cpuWrite(apu, 0xfc, (uint8_t)rnd());
  if (kind & 2) {   /* the S-DSP live: echo on (sync and log marks), all voices keyed on */
    static const uint8_t regs[][2] = {
      { 0x6c, 0x20 }, { 0x5d, 0x30 }, { 0x6d, 0x60 }, { 0x7d, 0x02 }, { 0x0d, 0x40 }, { 0x4d, 0xff },
      { 0x0c, 0x7f }, { 0x1c, 0x7f }, { 0x2c, 0x40 }, { 0x3c, 0x40 }, { 0x6c, 0x00 },
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
      apu_cpuWrite(apu, 0xf2, regs[i][0]);
      apu_cpuWrite(apu, 0xf3, regs[i][1]);
    }
    apu->ram[0x3000] = 0x00; apu->ram[0x3001] = 0x31;   /* sample 0: $3100, loops to $3100 */
    apu->ram[0x3002] = 0x00; apu->ram[0x3003] = 0x31;
    for (unsigned v = 0; v < 8; v++) {
      const uint8_t vr[][2] = { { 0x00, 0x7f }, { 0x01, 0x7f }, { 0x02, (uint8_t)rnd() }, { 0x03, (uint8_t)(rnd() & 0x3f) },
                                { 0x04, 0x00 }, { 0x05, 0x8f }, { 0x06, 0xe0 } };
      for (unsigned i = 0; i < 7; i++) {
        apu_cpuWrite(apu, 0xf2, (uint8_t)(v << 4 | vr[i][0]));
        apu_cpuWrite(apu, 0xf3, vr[i][1]);
      }
    }
    apu_cpuWrite(apu, 0xf2, 0x4c);
    apu_cpuWrite(apu, 0xf3, 0xff);   /* KON */
  }
  spc_dbt_flush();   /* (the program was written behind the translator's back) */
}

static bool same(Apu *ref, Apu *dbt, const char **what, uint32_t *where) {
  apu_dspSync(ref);
  apu_dspSync(dbt);
  for (uint32_t a = 0; a < 0x10000; a++)
    if (ref->ram[a] != dbt->ram[a]) { *what = "ARAM"; *where = a; return false; }
  const size_t ap = offsetof(Apu, ram) + sizeof ref->ram, an = offsetof(Apu, pad) + 6u;
  if (memcmp((const uint8_t *)ref + ap, (const uint8_t *)dbt + ap, an - ap)) {
    *what = "ports/timers"; *where = 0; return false;
  }
  const Spc *x = ref->spc, *y = dbt->spc;   /* (cyclesUsed is the interpreter's scratch) */
  if (x->a != y->a || x->x != y->x || x->y != y->y || x->sp != y->sp || x->pc != y->pc ||
      x->c != y->c || x->z != y->z || x->v != y->v || x->n != y->n || x->i != y->i ||
      x->h != y->h || x->p != y->p || x->b != y->b || x->stopped != y->stopped) {
    *what = "SPC registers"; *where = (uint32_t)(x->c | x->z << 1 | x->v << 2 | x->n << 3 | x->i << 4 | x->h << 5 |
                                               x->p << 6) << 8 |
                                    (uint32_t)(y->c | y->z << 1 | y->v << 2 | y->n << 3 | y->i << 4 | y->h << 5 |
                                               y->p << 6);
    return false;
  }
  for (int r = 0; r < 0x80; r++)
    if (dsp_read(ref->dsp, (uint8_t)r) != dsp_read(dbt->dsp, (uint8_t)r)) {
      *what = "S-DSP register"; *where = (uint32_t)r; return false;
    }
  if (ref->dsp->sampleWrite != dbt->dsp->sampleWrite) { *what = "S-DSP sample count"; *where = 0; return false; }
  if (memcmp(ref->dsp->sampleBuffer, dbt->dsp->sampleBuffer, sizeof ref->dsp->sampleBuffer)) {
    *what = "S-DSP output"; *where = 0; return false;
  }
  if (ref->cycles != dbt->cycles || ref->portClock != dbt->portClock) { *what = "cycles"; *where = 0; return false; }
  return true;
}

/* The reference: interpreted, with its S-DSP batches joined (the two APUs
 * share the RSP). The translator's validity epoch is global and its bit 0
 * follows P, so the reference's P changes must not reach it. */
static void run_ref(Apu *ref, uint32_t clocks) {
  const uint32_t epoch = g_spc_dbt_epoch;
  g_spc_dbt_enabled = false;
  apu_runCycles(ref, clocks);
  apu_dspSync(ref);
  g_spc_dbt_enabled = true;
  g_spc_dbt_epoch = epoch + 2;   /* (+2: whatever the reference marked, recheck) */
}

void spc_dbt_test(void) {
  Apu *ref = apu_init(), *dbt = apu_init();
  extern uint32_t g_spc_dbt_stats[8];
  unsigned programs = 0;
  uint64_t translated = 0, total = 0;
#ifndef SPC_DBT_TEST_FIRST
#define SPC_DBT_TEST_FIRST 0
#endif
  for (unsigned prog = SPC_DBT_TEST_FIRST; prog < 256; prog++) {
    const unsigned kind = prog & 3;   /* bit 0 random/structured, bit 1 the S-DSP live */
    const uint32_t seed = 0x9e3779b9u * (prog + 1);
    setup(ref, kind, seed);
    setup(dbt, kind, seed);
    s_rng = seed ^ 0x5bd1e995u;
    for (unsigned step = 0; step < 512; step++) {
      if (!(step & 7)) {
        const unsigned port = rnd() & 3, value = rnd() & 255;
        const uint64_t when = ref->portClock + (rnd() & 127);
        apu_schedulePortWrite(ref, (uint8_t)port, (uint8_t)value, when);
        apu_schedulePortWrite(dbt, (uint8_t)port, (uint8_t)value, when);
      }
      const uint32_t clocks = rnd() % 513;
      /* (Each APU's S-DSP batches are joined before the other runs: they
       * share the RSP.) */
      run_ref(ref, clocks);
      const uint32_t t0 = g_spc_dbt_stats[3];
      apu_runCycles(dbt, clocks);
      apu_dspSync(dbt);
      translated += g_spc_dbt_stats[3] - t0;
      total += clocks;
      const char *what; uint32_t where;
      if (!same(ref, dbt, &what, &where)) {
        n64_platform_log("[spcdbt-test] FAILED: program %u (%s, seed %08lx), step %u: %s differs at %04lx "
                         "(ref pc %04x a%02x x%02x y%02x sp%02x, dbt pc %04x a%02x x%02x y%02x sp%02x)\n",
                         prog, (kind & 1) ? "random" : "structured", (unsigned long)seed, step, what,
                         (unsigned long)where, ref->spc->pc, ref->spc->a, ref->spc->x, ref->spc->y,
                         ref->spc->sp, dbt->spc->pc, dbt->spc->a, dbt->spc->x, dbt->spc->y, dbt->spc->sp);
        /* Replay to this slice, then trace it: the reference one cycle at
         * a time (each instruction start), the translator by its runs. */
        extern int g_spc_dbt_trace_runs;
        setup(ref, kind, seed); setup(dbt, kind, seed);
        s_rng = seed ^ 0x5bd1e995u;
        for (unsigned st2 = 0; st2 <= step; st2++) {
          if (!(st2 & 7)) {
            const unsigned port = rnd() & 3, value = rnd() & 255;
            const uint64_t when = ref->portClock + (rnd() & 127);
            apu_schedulePortWrite(ref, (uint8_t)port, (uint8_t)value, when);
            apu_schedulePortWrite(dbt, (uint8_t)port, (uint8_t)value, when);
          }
          const uint32_t cl = rnd() % 513;
          if (st2 < step) {
            run_ref(ref, cl);
            apu_runCycles(dbt, cl); apu_dspSync(dbt);
            continue;
          }
          n64_platform_log("[trace] slice of %lu cycles from %lu\n", (unsigned long)cl, (unsigned long)ref->cycles);
          const uint32_t epoch = g_spc_dbt_epoch;
          g_spc_dbt_enabled = false;
          for (uint32_t k = 0; k < cl; k++) {
            if (!ref->cpuCyclesLeft) {
              const Spc *r = ref->spc;
              n64_platform_log("[ref] c%lu %04x: %02x %02x %02x  a%02x x%02x y%02x sp%02x c%d z%d n%d v%d h%d p%d  w %02x %02x %02x\n",
                               (unsigned long)ref->cycles, r->pc, ref->ram[r->pc], ref->ram[(uint16_t)(r->pc + 1)],
                               ref->ram[(uint16_t)(r->pc + 2)], r->a, r->x, r->y, r->sp, r->c, r->z, r->n, r->v, r->h,
                               r->p, ref->ram[g_spc_dbt_watch[0]], ref->ram[g_spc_dbt_watch[1]], ref->ram[g_spc_dbt_watch[2]]);
            }
            apu_runCycles(ref, 1);
          }
          apu_dspSync(ref);
          g_spc_dbt_epoch = epoch;
          g_spc_dbt_enabled = true;
          g_spc_dbt_trace_runs = 1;
          apu_runCycles(dbt, cl);
          g_spc_dbt_trace_runs = 0;
        }
        for (;;) {}
      }
    }
    programs++;
    if (!(programs % 32))
      n64_platform_log("[spcdbt-test] %u programs ok (%lu%% of cycles translated)\n", programs,
                       (unsigned long)(total ? translated * 100 / total : 0));
  }
  n64_platform_log("[spcdbt-test] PASSED: %u programs, %lu%% of %lu cycles translated\n", programs,
                   (unsigned long)(total ? translated * 100 / total : 0), (unsigned long)total);
  for (;;) {}
}

#endif
