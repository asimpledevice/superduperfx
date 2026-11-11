/* SPC700 on the RSP, C side: converts between the Apu (apu.c, spc.c) and
 * the RSP state block, and runs in C what the RSP leaves to the CPU. */
#include "spc.h"
#include "apu.h"
#include "spc_rsp.h"
#include "spc_rsp_layout.h"

uint32_t g_spc_rsp_runs, g_spc_rsp_traps, g_spc_rsp_insns;

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
  }
  b->cycles = apu->cycles; b->timerSync = apu->timerSyncCycle;
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

void spc_rsp_run_apu(Apu *apu, uint32_t clocks) {
  while (clocks) {
    const uint32_t n = clocks < SPC_RSP_MAX_RUN ? clocks : SPC_RSP_MAX_RUN;
    to_block(apu, &s_blk);
    s_blk.budget = (int32_t)n;
    const int exit = spc_rsp_exec(&s_blk, apu->ram);
    from_block(apu, &s_blk);
    apu->portClock += s_blk.used;
    clocks -= s_blk.used;
    g_spc_rsp_runs++;
    g_spc_rsp_insns += s_blk.count;
    if (exit == SPCEXIT_TRAP && clocks) {  /* one instruction in C, its carry back to the RSP */
      apu_runCycles(apu, 1);
      clocks--;
      g_spc_rsp_traps++;
    }
  }
}
