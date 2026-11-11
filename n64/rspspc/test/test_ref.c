/* RSP SPC700 test ROM: runs random states and programs on the RSP core and
 * on the C core (runner/src/snes/spc.c) and compares registers, flags, PC,
 * cycles and all of ARAM. Milestone 1: the I/O page and IPL ROM are plain
 * RAM on both sides (apu_cpuRead/apu_cpuWrite below). */
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "spc.h"
#include "spc_rsp.h"

uint8_t apu_cpuRead(Apu *apu, uint16_t adr) { return apu->ram[adr]; }
void apu_cpuWrite(Apu *apu, uint16_t adr, uint8_t val) { apu->ram[adr] = val; }

static uint32_t s_rng = 0x2468aceu;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

#include "rsp_spc_traps.h"
static int is_trap(uint8_t op) {   /* opcodes the RSP leaves to the C core */
  for (unsigned i = 0; i < sizeof k_spc_rsp_traps; i++) if (k_spc_rsp_traps[i] == op) return 1;
  return 0;
}

static void random_spc(Spc *s) {
  const uint32_t r = rnd(), f = rnd();
  s->a = r; s->x = r >> 8; s->y = r >> 16; s->sp = r >> 24; s->pc = rnd();
  s->n = f & 1; s->v = f & 2; s->p = f & 4; s->b = f & 8; s->h = f & 16; s->i = f & 32;
  s->z = f & 64; s->c = f & 128;
  s->stopped = (rnd() % 64) == 0;
}

static int cmp(const Spc *a, const Spc *b) {
  return a->a != b->a || a->x != b->x || a->y != b->y || a->sp != b->sp || a->pc != b->pc ||
         a->n != b->n || a->v != b->v || a->p != b->p || a->b != b->b || a->h != b->h ||
         a->i != b->i || a->z != b->z || a->c != b->c || a->stopped != b->stopped;
}

uint32_t test_ticks(void);
#define get_ticks test_ticks
#define debugf(...) fprintf(stderr, __VA_ARGS__)
void data_cache_hit_invalidate(volatile void *, unsigned long);
void run_tests(void) {
  Apu *aa = memalign(16, sizeof(Apu)), *ab = memalign(16, sizeof(Apu));
  memset(aa, 0, sizeof(Apu)); memset(ab, 0, sizeof(Apu));
  static Spc sa, sb;
  int fails = 0;
  uint64_t c_ticks = 0, r_ticks = 0, c_cycles = 0;
  const int tests = SPCRSP_TESTS;
  for (int t = 0; t < tests; t++) {
    if (t % 250 == 0) debugf("test %d (%d failed)\n", t, fails);
    for (int i = 0; i < 0x10000; i += 4) *(uint32_t *)(aa->ram + i) = rnd();
    if (t & 1)   /* long runs: mostly no trapped opcodes */
      for (int i = 0; i < 0x10000; i++) while (is_trap(aa->ram[i])) aa->ram[i] = rnd();
    memcpy(ab->ram, aa->ram, 0x10000);
    random_spc(&sa);
    sa.apu = aa; sb = sa; sb.apu = ab;
    const int budget = 1 + (int)(rnd() % ((t & 7) ? 200 : 5000));
    uint32_t t0 = get_ticks();
    int ua = 0;
    while (ua < budget) ua += spc_runOpcode(&sa);
    uint32_t t1 = get_ticks();
    const int ub = spc_rsp_run(&sb, budget);
    uint32_t t2 = get_ticks();
    c_ticks += t1 - t0; r_ticks += t2 - t1; c_cycles += ua;
    /* spc_rsp_exec wrote back and invalidated ARAM before the RSP ran. */
    int bad = ua != ub || cmp(&sa, &sb);
    int at = -1;
    if (!bad) for (int i = 0; i < 0x10000; i++) if (aa->ram[i] != ab->ram[i]) { at = i; bad = 1; break; }
    if (bad && fails++ < 8)
      debugf("FAIL test %d budget %d: cycles C %d RSP %d; pc %04x/%04x a %02x/%02x x %02x/%02x y %02x/%02x "
             "sp %02x/%02x stop %d/%d%s%04x\n", t, budget, ua, ub, sa.pc, sb.pc, sa.a, sb.a, sa.x, sb.x,
             sa.y, sb.y, sa.sp, sb.sp, sa.stopped, sb.stopped, at >= 0 ? " ARAM differs at " : " ", at < 0 ? 0 : at);
  }
  /* RSP speed alone: long runs of trap-free random code (no per-run flush
   * in the measured part beyond one exec). */
  {
    uint64_t rt = 0, rc = 0, ri = 0;
    for (int k = 0; k < 8; k++) {
      for (int i = 0; i < 0x10000; i += 4) *(uint32_t *)(ab->ram + i) = rnd();
      for (int i = 0; i < 0x10000; i++) while (is_trap(ab->ram[i])) ab->ram[i] = rnd();
      random_spc(&sb); sb.stopped = 0; sb.apu = ab;
      extern uint32_t g_spc_rsp_insns;
      const uint32_t i0 = g_spc_rsp_insns;
      const uint32_t t0 = get_ticks();
      rc += spc_rsp_run(&sb, 100000);
      rt += get_ticks() - t0;
      ri += g_spc_rsp_insns - i0;
    }
    debugf("RSP alone: %lu SPC cycles, %lu instructions; %lu RSP cycles per instruction (random code: pessimistic cache)\n",
           (unsigned long)rc, (unsigned long)ri, (unsigned long)(rt * 4 / 3 / (ri ? ri : 1)));
  }
  debugf("timing: C core %lu CPU ticks per 1000 SPC cycles; RSP path %lu (incl. sync, 64 KB cache flush per run)\n",
         (unsigned long)(c_ticks * 1000 / c_cycles), (unsigned long)(r_ticks * 1000 / c_cycles));
  debugf("runs %lu, traps %lu, RSP instructions %lu\n", (unsigned long)g_spc_rsp_runs,
         (unsigned long)g_spc_rsp_traps, (unsigned long)g_spc_rsp_insns);
  debugf("SPCRSP_TEST %s: %d/%d pass\n", fails ? "FAIL" : "PASS", tests - fails, tests);
}
