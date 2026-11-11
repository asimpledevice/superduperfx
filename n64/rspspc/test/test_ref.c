/* RSP SPC700 test: random states and programs, run by apu_runCycles (apu.c,
 * spc.c) and by spc_rsp_run_apu, compared field by field. The DSP is a
 * plain register file here (milestone 2b): DSP sample edges do nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "spc.h"
#include "apu.h"
#include "dsp.h"
#include "spc_rsp.h"
#include "rsp_spc_traps.h"

uint32_t test_ticks(void);
#define debugf(...) fprintf(stderr, __VA_ARGS__)

/* DSP stub: a register file (Dsp* points at 128 bytes). */
Dsp *dsp_init(uint8_t *ram) { (void)ram; return (Dsp *)calloc(1, 128); }
void dsp_free(Dsp *d) { free(d); }
void dsp_reset(Dsp *d) { memset(d, 0, 128); }
void dsp_cycle(Dsp *d) { (void)d; }
uint8_t dsp_read(Dsp *d, uint8_t adr) { return ((uint8_t *)d)[adr & 0x7f]; }
void dsp_write(Dsp *d, uint8_t adr, uint8_t val) { ((uint8_t *)d)[adr & 0x7f] = val; }
void dsp_saveload(Dsp *d, SaveLoadInfo *sli) { (void)d; (void)sli; }
uint64_t g_apu_timer0_total_ticks;
uint8_t *spc_rsp_dsp_regs(struct Apu *apu) { return (uint8_t *)apu->dsp; }

static uint32_t s_rng = 0x2468aceu;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static int is_trap(uint8_t op) {
  for (unsigned i = 0; i < sizeof k_spc_rsp_traps; i++) if (k_spc_rsp_traps[i] == op) return 1;
  return 0;
}

static Apu *new_apu(void) {
  Apu *a = memalign(16, sizeof(Apu));
  memset(a, 0, sizeof(Apu));
  a->spc = spc_init(a);
  a->dsp = dsp_init(a->ram);
  return a;
}

static void random_state(Apu *a) {
  Spc *s = a->spc;
  const uint32_t r = rnd(), f = rnd();
  s->a = r; s->x = r >> 8; s->y = r >> 16; s->sp = r >> 24; s->pc = rnd();
  s->n = f & 1; s->v = f & 2; s->p = f & 4; s->b = f & 8; s->h = f & 16; s->i = f & 32;
  s->z = f & 64; s->c = f & 128;
  s->stopped = (rnd() % 64) == 0;
  a->romReadable = (rnd() % 8) == 0;
  a->dspAdr = rnd();
  for (int i = 0; i < 6; i++) a->inPorts[i] = rnd();
  for (int i = 0; i < 4; i++) a->outPorts[i] = rnd();
  for (int i = 0; i < 3; i++) {
    const uint32_t t = rnd();
    a->timer[i].cycles = t & (i == 2 ? 15 : 127);   /* the prescaler: below its period */
    a->timer[i].divider = t >> 8; a->timer[i].target = t >> 16;
    a->timer[i].counter = (t >> 24) & 15; a->timer[i].enabled = rnd() & 1;
  }
  a->cycles = rnd();
  a->timerSyncCycle = a->cycles - rnd() % 2000;
  a->cpuCyclesLeft = (rnd() & 1) ? rnd() % 12 : 0;
  for (int i = 0; i < 128; i++) ((uint8_t *)a->dsp)[i] = rnd();
}

static void copy_apu(Apu *dst, const Apu *src) {   /* state and ARAM; keep dst's own objects */
  Spc *ds = dst->spc; Dsp *dd = dst->dsp;
  memcpy(dst, src, sizeof(Apu));
  dst->spc = ds; dst->dsp = dd;
  Apu *keep = ds->apu; *ds = *src->spc; ds->apu = keep;
  memcpy(dd, src->dsp, 128);
}

static const char *differs(const Apu *a, const Apu *b) {
  const Spc *x = a->spc, *y = b->spc;
  if (x->a != y->a || x->x != y->x || x->y != y->y || x->sp != y->sp || x->pc != y->pc) return "registers";
  if (x->n != y->n || x->v != y->v || x->p != y->p || x->b != y->b || x->h != y->h || x->i != y->i ||
      x->z != y->z || x->c != y->c) return "flags";
  if (x->stopped != y->stopped) return "stopped";
  if (a->cpuCyclesLeft != b->cpuCyclesLeft) return "cpuCyclesLeft";
  if (a->cycles != b->cycles || a->portClock != b->portClock) return "cycles";
  if (a->timerSyncCycle != b->timerSyncCycle) return "timerSyncCycle";
  if (a->dspAdr != b->dspAdr || a->romReadable != b->romReadable) return "dspAdr/romReadable";
  if (memcmp(a->inPorts, b->inPorts, 6) || memcmp(a->outPorts, b->outPorts, 4)) return "ports";
  for (int i = 0; i < 3; i++)
    if (a->timer[i].cycles != b->timer[i].cycles || a->timer[i].divider != b->timer[i].divider ||
        a->timer[i].target != b->timer[i].target || a->timer[i].counter != b->timer[i].counter ||
        a->timer[i].enabled != b->timer[i].enabled) return "timers";
  if (memcmp(a->dsp, b->dsp, 128)) return "DSP registers";
  if (memcmp(a->ram, b->ram, 0x10000)) return "ARAM";
  return NULL;
}

/* Replay a failing test one cycle at a time and report the first step after
 * which the two differ. */
static void step_diagnose(Apu *init, Apu *aa, Apu *ab, uint32_t clocks) {
  copy_apu(aa, init); copy_apu(ab, init);
  for (uint32_t k = 0; k < clocks; k++) {
    const uint16_t pc = aa->spc->pc; const uint8_t op = aa->ram[pc], ccl = aa->cpuCyclesLeft;
    const uint8_t b1 = aa->ram[(uint16_t)(pc + 1)], b2 = aa->ram[(uint16_t)(pc + 2)];
    const uint8_t psw = (uint8_t)(aa->spc->n << 7 | aa->spc->v << 6 | aa->spc->p << 5 | aa->spc->b << 4 |
                                  aa->spc->h << 3 | aa->spc->i << 2 | aa->spc->z << 1 | aa->spc->c);
    const uint8_t A = aa->spc->a, X = aa->spc->x, Y = aa->spc->y, SP = aa->spc->sp;
    apu_runCycles(aa, 1);
    spc_rsp_run_apu(ab, 1);
    const char *d = differs(aa, ab);
    if (d) {
      debugf("  step %lu: %s differ after pc %04x op %02x %02x %02x (ccl %u) a %02x x %02x y %02x sp %02x psw %02x\n",
             (unsigned long)k, d, pc, op, b1, b2, ccl, A, X, Y, SP, psw);
      debugf("  after: C a %02x x %02x y %02x sp %02x pc %04x / RSP a %02x x %02x y %02x sp %02x pc %04x\n",
             aa->spc->a, aa->spc->x, aa->spc->y, aa->spc->sp, aa->spc->pc,
             ab->spc->a, ab->spc->x, ab->spc->y, ab->spc->sp, ab->spc->pc);
      return;
    }
  }
  debugf("  step replay: no difference\n");
}

void run_tests(void) {
  Apu *aa = new_apu(), *ab = new_apu(), *ai = new_apu();
  int fails = 0;
  uint64_t c_ticks = 0, r_ticks = 0, c_cycles = 0;
  const int tests = SPCRSP_TESTS;
  for (int t = 0; t < tests; t++) {
    if (t % 250 == 0) debugf("test %d (%d failed)\n", t, fails);
    for (int i = 0; i < 0x10000; i += 4) *(uint32_t *)(aa->ram + i) = rnd();
    if (t & 1)   /* long runs: no opcodes left to the CPU */
      for (int i = 0; i < 0x10000; i++) while (is_trap(aa->ram[i])) aa->ram[i] = rnd();
    if (t % 4 == 1)   /* timer-heavy: many reads of $FD-$FF */
      for (int i = 0; i < 0xfffe; i += 3) { aa->ram[i] = 0xe4; aa->ram[i + 1] = 0xfd + rnd() % 3; }
    random_state(aa);
    copy_apu(ab, aa);
    copy_apu(ai, aa);
    const uint32_t clocks = 1 + rnd() % ((t & 7) ? 300 : 20000);
    uint32_t t0 = test_ticks();
    apu_runCycles(aa, clocks);
    uint32_t t1 = test_ticks();
    spc_rsp_run_apu(ab, clocks);
    uint32_t t2 = test_ticks();
    c_ticks += t1 - t0; r_ticks += t2 - t1; c_cycles += clocks;
    const char *d = differs(aa, ab);
    if (d && fails < 8) {
      debugf("  in %02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x out %02x%02x%02x%02x/%02x%02x%02x%02x x %02x/%02x y %02x/%02x sp %02x/%02x f %d%d%d%d/%d%d%d%d\n",
        aa->inPorts[0],aa->inPorts[1],aa->inPorts[2],aa->inPorts[3],aa->inPorts[4],aa->inPorts[5],
        ab->inPorts[0],ab->inPorts[1],ab->inPorts[2],ab->inPorts[3],ab->inPorts[4],ab->inPorts[5],
        aa->outPorts[0],aa->outPorts[1],aa->outPorts[2],aa->outPorts[3],ab->outPorts[0],ab->outPorts[1],ab->outPorts[2],ab->outPorts[3],
        aa->spc->x, ab->spc->x, aa->spc->y, ab->spc->y, aa->spc->sp, ab->spc->sp,
        aa->spc->n, aa->spc->z, aa->spc->c, aa->spc->v, ab->spc->n, ab->spc->z, ab->spc->c, ab->spc->v);
    }
    if (d && fails < 8) debugf("  portClock %llu/%llu stopped %d/%d romReadable %d\n", (unsigned long long)aa->portClock,
                               (unsigned long long)ab->portClock, aa->spc->stopped, ab->spc->stopped, aa->romReadable);
    if (d && fails < 4) step_diagnose(ai, aa, ab, clocks);
    if (d && fails++ < 8)
      debugf("FAIL test %d (%lu clocks): %s differ; pc %04x/%04x a %02x/%02x ccl %u/%u cycles %lu/%lu\n", t,
             (unsigned long)clocks, d, aa->spc->pc, ab->spc->pc, aa->spc->a, ab->spc->a, aa->cpuCyclesLeft,
             ab->cpuCyclesLeft, (unsigned long)aa->cycles, (unsigned long)ab->cycles);
  }
  debugf("timing: C %lu CPU ticks per 1000 APU cycles; RSP path %lu (incl. sync and a 64 KB flush per run)\n",
         (unsigned long)(c_ticks * 1000 / c_cycles), (unsigned long)(r_ticks * 1000 / c_cycles));
  debugf("runs %lu, traps %lu, RSP instructions %lu\n", (unsigned long)g_spc_rsp_runs,
         (unsigned long)g_spc_rsp_traps, (unsigned long)g_spc_rsp_insns);
  debugf("SPCRSP_TEST %s: %d/%d pass\n", fails ? "FAIL" : "PASS", tests - fails, tests);
}
