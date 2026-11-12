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

static SpcRspBlock s_blk;

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
static void run_apu(Apu *apu, uint32_t clocks);

void spc_rsp_run_apu(Apu *apu, uint32_t clocks) {
  const uint32_t t0 = spc_rsp_ticks();
  if (clocks < SPC_RSP_MIN_RUN) {
    g_spc_rsp_c_cycles += clocks;
    apu_runCyclesC(apu, clocks);
  } else {
    run_apu(apu, clocks);
  }
  g_spc_rsp_hook_ticks += spc_rsp_ticks() - t0;
}

static void run_apu(Apu *apu, uint32_t clocks) {
  while (clocks) {
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
    apu_dspJoinNow(apu);
    /* Sample edges the C path takes: a batch start (none pending) and the
     * edge that fills a batch. The RSP run stops just before. */
    const uint32_t c0 = apu->cycles, f = (32u - (c0 & 31u)) & 31u, p = apu->dspPending;
    const uint32_t special = p ? f + 32u * (APU_DSP_MAX_PENDING - p - 1u) : f;
    if (special < n) n = special;
    if (p && (apu->spc->dspWatchSync[0] & 3)) n = 0;   /* page 0/1 reads would need the DSP */
    /* The RSP checks the sample directory at the RAM write after an I/O
     * register write's effect: a directory over $00F0-$00FF stays in C. */
    if (p && (apu->dsp->dirPage == 0 || apu->dsp->dirPage >= 0xfd00)) n = 0;
    if (p) {
      /* Room for every write the run can log (at most 3 entries in 5 cycles:
       * MOVWS to $F2/$F3), in the RSP's buffer and in apu->dspLog. */
      uint32_t cap = APU_DSP_LOG_LEN - apu->dspLogN;
      if (cap > SPC_RSP_RAW_BYTES / 8) cap = SPC_RSP_RAW_BYTES / 8;
      const uint32_t most = cap > 8 ? (cap - 8) * 5 / 3 : 0;
      const uint32_t carry = apu->cpuCyclesLeft;
      const uint32_t room = most > carry + 16 ? most - carry - 16 : 0;
      if (room < n) n = room;
    }
    if (!n) {
      apu_runCyclesC(apu, 1);
      clocks--;
      g_spc_rsp_c_cycles++;
      continue;
    }
#endif
    to_block(apu, &s_blk);
    s_blk.budget = (int32_t)n;
#if SNESRECOMP_APU_DEFER_DSP
    s_blk.logging = p != 0;
    memcpy(s_blk.sync, apu->spc->dspWatchSync, 32);
    s_blk.dirPage = apu->dsp->dirPage;
#else
    s_blk.logging = 0;
    memset(s_blk.sync, 0, 32);
#endif
    const int exit = spc_rsp_exec(&s_blk, apu->ram);
    from_block(apu, &s_blk);
    /* A trap re-runs the instruction in C: undo its timer reads' clears. */
    if (exit == SPCEXIT_TRAP && s_blk.undoFp && s_blk.undoFp == (uint32_t)(s_blk.budget - (int32_t)s_blk.used))
      for (int i = 0; i < 3; i++) apu->timer[i].counter = s_blk.timer[i].pad[1];
    apu->portClock += s_blk.used;
    clocks -= s_blk.used;
    g_spc_rsp_cycles += s_blk.used;
#if SNESRECOMP_APU_DEFER_DSP
    if (p) take_log(apu, c0, p, s_blk.rawN);
    apu->dspPending = p + edges_in(c0, s_blk.used);
#endif
    g_spc_rsp_runs++;

    if (exit == SPCEXIT_TRAP && clocks) {  /* one instruction in C, its carry back to the RSP */
      apu_runCyclesC(apu, 1);
      clocks--;
      g_spc_rsp_c_cycles++;
      g_spc_rsp_traps++;
    }
  }
}
