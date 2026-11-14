/* SPC700 on the RSP, C side: converts between the Apu (apu.c, spc.c) and
 * the RSP state block, and runs in C what the RSP leaves to the CPU. */
#include "spc.h"
#include "apu.h"
#include "spc_rsp.h"
#include "spc_rsp_layout.h"
#if SNESRECOMP_APU_DEFER_DSP
#include "dsp.h"
#include "apu_dsp_watch.h"
#include "apu_dsp_batch.h"
#endif

uint32_t g_spc_rsp_runs, g_spc_rsp_traps, g_spc_rsp_insns, g_spc_rsp_cycles, g_spc_rsp_c_cycles;

/* Longest single RSP run: the timers' closed form divides 16-bit tick counts. */
#define SPC_RSP_MAX_RUN (1u << 19)

static SpcRspRun s_run;
#define s_blk (s_run.b)

static void to_block(Apu *apu, SpcRspBlock *b) {
  const Spc *s = apu->spc;
  b->a = s->a; b->x = s->x; b->y = s->y; b->sp = s->sp; b->pc = s->pc;
  b->psw = (uint8_t)(s->n << 7 | s->v << 6 | s->p << 5 | s->b << 4 | s->h << 3 | s->i << 2 |
                     s->z << 1 | s->c);
  b->stopped = s->stopped;
  b->ccl = apu->cpuCyclesLeft; b->dspAdr = apu->dspAdr; b->romReadable = apu->romReadable;
  memcpy(b->inPorts, apu->inPorts, 6); memcpy(b->outPorts, apu->outPorts, 4);
  for (int i = 0; i < 3; i++) {
    b->timer[i].cycles = apu->timer[i].cycles; b->timer[i].divider = apu->timer[i].divider;
    b->timer[i].target = apu->timer[i].target; b->timer[i].counter = apu->timer[i].counter;
    b->timer[i].enabled = apu->timer[i].enabled;
    b->timer[i].pad[0] = i == 2 ? 4 : 7;   /* prescaler period shift (rsp_spc_ext.S) */
  }
  b->cycles = apu->cycles; b->timerSync = apu->timerSyncCycle;
  b->undoFp = 0;
  memcpy(b->dspRegs, spc_rsp_dsp_regs(apu), 128);
}

static void from_block(Apu *apu, const SpcRspBlock *b) {
  Spc *s = apu->spc;
  s->a = b->a; s->x = b->x; s->y = b->y; s->sp = b->sp; s->pc = b->pc;
  const uint8_t f = b->psw;
  s->n = f & 0x80; s->v = f & 0x40; s->p = f & 0x20; s->b = f & 0x10;
  s->h = f & 8; s->i = f & 4; s->z = f & 2; s->c = f & 1;
  s->stopped = b->stopped;
  apu->cpuCyclesLeft = b->ccl; apu->dspAdr = b->dspAdr; apu->romReadable = b->romReadable;
  memcpy(apu->inPorts, b->inPorts, 6); memcpy(apu->outPorts, b->outPorts, 4);
  for (int i = 0; i < 3; i++) {
    apu->timer[i].cycles = b->timer[i].cycles; apu->timer[i].divider = b->timer[i].divider;
    apu->timer[i].target = b->timer[i].target; apu->timer[i].counter = b->timer[i].counter;
    apu->timer[i].enabled = b->timer[i].enabled;
  }
  apu->cycles = b->cycles; apu->timerSyncCycle = b->timerSync;
  memcpy(spc_rsp_dsp_regs(apu), b->dspRegs, 128);
}

#if SNESRECOMP_APU_DEFER_DSP
/* DSP sample edges (multiples of 32 APU cycles) in [c0, c0 + n). */
static uint32_t edges_in(uint32_t c0, uint32_t n) {
  const uint32_t f = (32u - (c0 & 31u)) & 31u;
  return n > f ? ((n - f - 1u) >> 5) + 1u : 0u;
}

/* The RSP logged every ARAM and DSP register write of a run while a batch
 * was open, with its cycle offset. Keep what apu_spcWriteRam and the $F3
 * path would have logged (the watch marks cannot change during such a run:
 * what changes them traps), with its sample tag. */
static void take_log(Apu *apu, uint32_t c0, uint32_t p, uint32_t bytes) {
  const ApuDspLogEntry *raw = spc_rsp_raw_log();
  for (uint32_t i = 0; i < bytes / 8; i++) {
    const ApuDspLogEntry r = raw[i];
    if (!r.isReg && !APU_DSP_PAGE_BIT(apu->spc->dspWatchLog, r.adr)) continue;
    ApuDspLogEntry *e = &apu->dspLog[apu->dspLogN++];
    *e = r;
    e->tag = (uint16_t)(p + edges_in(c0, r.tag));
  }
}
#endif

uint8_t *spc_rsp_dsp_regs(Apu *apu) {
#if SNESRECOMP_APU_DEFER_DSP
  return apu->dspRegs;
#else
  return (uint8_t *)apu->dsp;   /* the test's register-file DSP (milestone 2b) */
#endif
}

/* Shorter calls stay in C: a run costs a queue round trip and a D-cache flush. */
#ifndef SPC_RSP_MIN_RUN
#define SPC_RSP_MIN_RUN 256u
#endif

extern uint32_t g_spc_rsp_hook_ticks;
uint32_t g_spc_rsp_lane_posts, g_spc_rsp_lane_busy, g_spc_rsp_lane_joins;
#ifdef SPC_RSP_TRAP_STATS
uint32_t g_spc_rsp_trap_op[256], g_spc_rsp_trap_opnd[256];   /* opcode, first operand byte */
/* Cycles run in C, by why: 0 a DSP edge, 1 an echo page in $0000-$01FF,
 * 2 the sample directory over the I/O page, 3 no log room, 4 short calls. */
uint32_t g_spc_rsp_c_why[5];
#define C_WHY(i, n) (g_spc_rsp_c_why[i] += (n))
#else
#define C_WHY(i, n) ((void)0)
#endif

/* The run in flight (at most one): what its collection needs. */
static struct {
  bool active;
  uint32_t c0, p;   /* apu->cycles and dspPending when posted */
} s_fl;

#if SNESRECOMP_APU_DEFER_DSP
/* Pages the run must not touch without the C core: those the DSP may write
 * (echo) while a batch is open, and those a batch in flight writes (its echo
 * bytes come back to ARAM when it is joined). The RSP never caches them, so
 * any access there traps. */
static void sync_pages(Apu *apu, uint8_t out[32]) {
  for (int i = 0; i < 32; i++)
    out[i] = apu->spc->dspWatchSync[i] | (apu->dspInFlight ? apu->dspFlightWrite[i] : 0);
}
#endif

/* The next run from the current state for at most `clocks` cycles: its
 * length, or 0 when the next cycle must run in C (a DSP edge the C path
 * takes, or a state the RSP cannot run). Applies the port writes due now. */
static uint32_t plan(Apu *apu, uint32_t clocks) {
  uint32_t n = clocks < SPC_RSP_MAX_RUN ? clocks : SPC_RSP_MAX_RUN;
  /* CPU port writes: those due now apply before the next instruction, as
   * in apu_runCycles_native, and the run stops at the next one's cycle. */
  apu_drainDuePortWrites(apu);
  if (apu->portQHead != apu->portQTail) {
    const uint64_t d = apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)].target_cycle -
                       apu->portClock;
    if (d < n) n = (uint32_t)d;
  }
#if SNESRECOMP_APU_DEFER_DSP
  /* No join with an S-DSP batch in flight (step B2): as the C core does, the
   * run only stops where the batch matters, its trap pages (sync_pages) hold
   * the pages the batch may write, so any access there joins in C. */
  /* Sample edges the C path takes: a batch start (none pending) and the
   * edge that fills a batch. The RSP run stops just before. */
  const uint32_t c0 = apu->cycles, f = (32u - (c0 & 31u)) & 31u, p = apu->dspPending;
  const uint32_t special = p ? f + 32u * (APU_DSP_MAX_PENDING - p - 1u) : f;
  if (special < n) n = special;
  if (!n) C_WHY(0, 1);
  uint8_t sync[32];
  sync_pages(apu, sync);
  if (sync[0] & 3) { if (n) C_WHY(1, 1); n = 0; }   /* pages 0/1 stay in DMEM: no trap there */
  /* The RSP checks the sample directory at the RAM write after an I/O
   * register write's effect: a directory over $00F0-$00FF stays in C. It
   * knows one directory: while the register file's DIR differs from the
   * Dsp's (a DIR write logged, or a batch in flight), runs stay in C. */
  const uint16_t dir = apu->dsp->dirPage;
  if ((p || apu->dspInFlight) &&
      (dir == 0 || dir >= 0xfd00 || dir != (uint16_t)(spc_rsp_dsp_regs(apu)[0x5d] << 8)))
    { if (n) C_WHY(2, 1); n = 0; }
  if (p) {
    /* Room for every write the run can log (at most 3 entries in 5 cycles:
     * MOVWS to $F2/$F3), in the RSP's buffer and in apu->dspLog. */
    uint32_t cap = APU_DSP_LOG_LEN - apu->dspLogN;
    if (cap > SPC_RSP_RAW_BYTES / 8) cap = SPC_RSP_RAW_BYTES / 8;
    const uint32_t most = cap > 8 ? (cap - 8) * 5 / 3 : 0;
    const uint32_t carry = apu->cpuCyclesLeft;
    const uint32_t room = most > carry + 16 ? most - carry - 16 : 0;
    if (room < n) { if (!room && n) C_WHY(3, 1); n = room; }
  }
#endif
  return n;
}

static void c_step(Apu *apu) {
  apu_runCyclesC(apu, 1);
  g_spc_rsp_c_cycles++;
}

static void post(Apu *apu, uint32_t n) {
  to_block(apu, &s_blk);
  s_blk.budget = (int32_t)n;
#if SNESRECOMP_APU_DEFER_DSP
  const uint32_t p = apu->dspPending;
  s_blk.logging = p != 0;
  sync_pages(apu, s_blk.sync);
  s_blk.dirPage = apu->dsp->dirPage;
  s_fl.p = p;
#else
  s_blk.logging = 0;
  memset(s_blk.sync, 0, 32);
#endif
  s_fl.c0 = apu->cycles;
  s_fl.active = true;
  memset(s_run.tags, 0, sizeof s_run.tags);       /* all line slots empty */
  memset(s_run.wpages, 0, sizeof s_run.wpages);
  spc_rsp_post(&s_blk, apu->ram);
}

/* The pages the run wrote are no longer current in the S-DSP's view of
 * ARAM (the C core clears these bits in its write path): pages 0-1, which
 * go back whole, those of the lines still dirty at the end, and those of
 * dirty lines evicted during the run. */
static void mark_written(Apu *apu) {
#if SNESRECOMP_APU_DEFER_DSP
  uint8_t *fresh = apu->spc->dspViewFresh;
  fresh[0] &= (uint8_t)~3u;
  for (int i = 0; i < 32; i++) {
    const uint16_t t = s_run.tags[i];
    if (t & 0x8000) {
      const uint32_t page = (t & 0x7fffu) >> 2;
      fresh[page >> 3] &= (uint8_t)~(1u << (page & 7));
    }
    fresh[i] &= (uint8_t)~s_run.wpages[i];
  }
#else
  (void)apu;
#endif
}

/* Collect the run in flight: its state back into the Apu, its log, and the
 * instruction a trap leaves to C. Returns the cycles it advanced. */
static uint32_t collect(Apu *apu) {
  s_fl.active = false;
  const int exit = spc_rsp_wait(&s_blk);
  mark_written(apu);
  from_block(apu, &s_blk);
  /* A trap re-runs the instruction in C: undo its timer reads' clears. */
  if (exit == SPCEXIT_TRAP && s_blk.undoFp && s_blk.undoFp == (uint32_t)(s_blk.budget - (int32_t)s_blk.used))
    for (int i = 0; i < 3; i++) apu->timer[i].counter = s_blk.timer[i].pad[1];
  apu->portClock += s_blk.used;
  g_spc_rsp_cycles += s_blk.used;
#if SNESRECOMP_APU_DEFER_DSP
  if (s_fl.p) take_log(apu, s_fl.c0, s_fl.p, s_blk.rawN);
  apu->dspPending = s_fl.p + edges_in(s_fl.c0, s_blk.used);
#endif
  g_spc_rsp_runs++;
  uint32_t adv = s_blk.used;
  if (exit == SPCEXIT_TRAP) {  /* one instruction in C, its carry back to the RSP */
#ifdef SPC_RSP_TRAP_STATS
    { extern uint32_t g_spc_rsp_trap_op[256], g_spc_rsp_trap_opnd[256];
      const uint16_t pc = apu->spc->pc;
      g_spc_rsp_trap_op[apu->ram[pc]]++;
      g_spc_rsp_trap_opnd[apu->ram[(uint16_t)(pc + 1)]]++; }
#endif
    c_step(apu);
    adv++;
    g_spc_rsp_traps++;
  }
  return adv;
}

/* Inside the SPC's state on the CPU (the lane, a synchronous run): Blue can
 * yield from in there (the S-DSP join's wait loop), and Green's pump must
 * not touch the state then. */
static int s_in_apu;
/* RtlApuLock / RtlApuUnlock (n64_host.c): the holder may run 65816 code. */
void spc_rsp_lane_hold(Apu *apu) { spc_rsp_lane_join(apu); s_in_apu++; }
void spc_rsp_lane_release(void) { if (s_in_apu > 0) s_in_apu--; }

void spc_rsp_lane_join(Apu *apu) {
  if (!s_fl.active) return;
  g_spc_rsp_lane_joins++;
  s_in_apu++;
  (void)collect(apu);
  s_in_apu--;
}

static void run_apu(Apu *apu, uint32_t clocks);

void spc_rsp_run_apu(Apu *apu, uint32_t clocks) {
  const uint32_t t0 = spc_rsp_ticks();
  spc_rsp_lane_join(apu);
  s_in_apu++;
  if (clocks < SPC_RSP_MIN_RUN) {
    C_WHY(4, clocks);
    g_spc_rsp_c_cycles += clocks;
    apu_runCyclesC(apu, clocks);
  } else {
    run_apu(apu, clocks);
  }
  s_in_apu--;
  g_spc_rsp_hook_ticks += spc_rsp_ticks() - t0;
}

static void run_apu(Apu *apu, uint32_t clocks) {
  while (clocks) {
    const uint32_t n = plan(apu, clocks);
    if (!n) {
      c_step(apu);
      clocks--;
      continue;
    }
    post(apu, n);
    const uint32_t adv = collect(apu);
    clocks = adv < clocks ? clocks - adv : 0;
  }
}

/* The last target a catch-up gave: the doorbell's pump continues toward it
 * only. (The time read at a later safe point can be ahead of where that
 * block's own port accesses land, so a pump to it could let the SPC pass a
 * port write's cycle.) */
static Apu *s_lane_apu;
static uint64_t s_horizon;

static void lane_advance(Apu *apu, uint64_t target);
void spc_rsp_lane_advance(Apu *apu, uint64_t target) {
  s_lane_apu = apu;
  if (target > s_horizon) s_horizon = target;
  lane_advance(apu, target);
}

static void lane_advance(Apu *apu, uint64_t target) {
  const uint32_t t0 = spc_rsp_ticks();
  s_in_apu++;
  if (s_fl.active) {
    if (!spc_rsp_done()) { g_spc_rsp_lane_busy++; s_in_apu--; return; }
    (void)collect(apu);
  }
  for (;;) {
    apu_drainDuePortWrites(apu);
    if (apu->portClock >= target) break;
    const uint64_t owed = target - apu->portClock;
    if (owed < SPC_RSP_MIN_RUN) break;   /* later, with more */
    const uint32_t n = plan(apu, owed > SPC_RSP_MAX_RUN ? SPC_RSP_MAX_RUN : (uint32_t)owed);
    if (!n) { c_step(apu); continue; }
    post(apu, n);
    g_spc_rsp_lane_posts++;
#if SPC_RSP_ASYNC
    spc_rsp_arm_doorbell();
#endif
    break;
  }
  s_in_apu--;
  g_spc_rsp_hook_ticks += spc_rsp_ticks() - t0;
}

uint32_t g_spc_rsp_lane_pumps;
void spc_rsp_lane_pump(void) {
  extern volatile uint32_t g_red_blue_waits;   /* Blue in the S-DSP join's wait */
  if (!s_lane_apu || s_in_apu || g_red_blue_waits) return;
  g_spc_rsp_lane_pumps++;
  lane_advance(s_lane_apu, s_horizon);
}
