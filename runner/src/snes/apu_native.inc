/* Native APU time service. Included by apu.c after its private queue helper.
 *
 * The SPC core already executes an instruction atomically at its first clock.
 * Between instruction starts, DSP sample edges and queued CPU port writes,
 * timer advancement is the only work. The timers advance lazily: only when
 * the SPC touches a timer register, and once at the end of each run.
 * Preserve the reference order at an edge: ports, SPC, DSP, then timers.
 * All state stays in Apu; no cached host pointer or unfinished job is saved.
 */
/* Advance one timer by `clocks` APU cycles in closed form. Equivalent to the
 * reference per-cycle step: when the prescaler reaches 0 it reloads with
 * `period` and, if enabled, ticks the divider; the 4-bit counter increments
 * when the 8-bit divider equals target (target 0 means every 256 ticks). */
static void apu_native_timer(Timer *timer, uint32_t clocks, unsigned period_shift,
                             int first_timer) {
  if (clocks <= timer->cycles) {
    timer->cycles -= (uint8_t)clocks;
    return;
  }
  const uint32_t period = 1u << period_shift;
  const uint32_t rest = clocks - timer->cycles;
  const uint32_t ticks = ((rest - 1u) >> period_shift) + 1u;
  timer->cycles = (uint8_t)(ticks * period - rest);
  if (!timer->enabled) return;
  const uint32_t first = ((uint32_t)(timer->target - timer->divider - 1u) & 0xffu) + 1u;
  if (ticks < first) {
    timer->divider = (uint8_t)(timer->divider + ticks);
    return;
  }
  const uint32_t every = timer->target ? timer->target : 256u;
  const uint32_t after = ticks - first;
  const uint32_t fires = 1u + after / every;
  timer->divider = (uint8_t)(after % every);
  timer->counter = (uint8_t)((timer->counter + fires) & 15u);
  if (first_timer) {
    extern uint64_t g_apu_timer0_total_ticks;
    g_apu_timer0_total_ticks += fires;
  }
}

/* Bring the timers up to date with apu->cycles. Called before any SPC access
 * to a timer register and at the end of every native run, so the timers are
 * exact wherever the SPC or the host can observe them. */
static inline void apu_native_timers_sync(Apu *apu) {
  const uint32_t clocks = apu->cycles - apu->timerSyncCycle;
  if (!clocks) return;
  apu->timerSyncCycle = apu->cycles;
  apu_native_timer(&apu->timer[0], clocks, 7, 1);
  apu_native_timer(&apu->timer[1], clocks, 7, 0);
  apu_native_timer(&apu->timer[2], clocks, 4, 0);
}

/* SNESRECOMP_DSP_CYCLE_HOOK names a host function to call in place of
 * dsp_cycle (for example, to time it). It must leave the same state as one
 * dsp_cycle call. */
#ifdef SNESRECOMP_DSP_CYCLE_HOOK
void SNESRECOMP_DSP_CYCLE_HOOK(Dsp *dsp);
#define APU_NATIVE_DSP_CYCLE SNESRECOMP_DSP_CYCLE_HOOK
#else
#define APU_NATIVE_DSP_CYCLE dsp_cycle
#endif

/* Wait loops. Sound drivers spend much of their time in loops of one read
 * of a value that cannot change on its own, then branches back:
 *     loop: MOV reg, Tn-OUT   ; $FD-$FF: a timer counter, reads 0 until it ticks
 *           BEQ loop
 *     loop: CMP X, !$00F4     ; $F4-$F7: a CPU port, changes only when a
 *           BNE loop          ;   queued CPU write lands
 * (MOV A/X/Y or CMP A/X/Y from dp or !abs, or CMP dp,#imm as the IPL ROM's
 * handshake does; then up to three branches, BRA included, that lead back
 * to the read.) Every pass reads the same value, so every pass leaves the
 * same registers and flags; the reads of a timer counter that is 0 leave it
 * 0. Passes are skipped in closed form: for a timer, up to the first pass
 * whose read sees the next increment (which runs normally); for a port, up
 * to the next queued CPU write. A skip never crosses a queued CPU write or
 * the end of the run, and sends the DSP the sample edges the skipped cycles
 * contain, in order, so the result is identical to running every pass
 * (SNESRECOMP_SPC_WAIT_SKIP=0 runs them). Returns the cycles skipped (0: not
 * at such a loop, or nothing to skip). */
#ifndef SNESRECOMP_SPC_WAIT_SKIP
#define SNESRECOMP_SPC_WAIT_SKIP (!SNESRECOMP_SPC_DIAGNOSTICS)
#endif
#if SNESRECOMP_SPC_WAIT_SKIP
/* A code byte as the SPC fetches it (the IPL ROM overlays $FFC0-$FFFF). */
static inline uint8_t apu_native_fetch(const Apu *apu, uint16_t adr) {
  return apu->romReadable && adr >= 0xffc0 ? bootRom[adr - 0xffc0] : apu->ram[adr];
}
#if SNESRECOMP_APU_DEFER_DSP
/* Code in a page the DSP may write would sync it at the fetch. */
#define APU_NATIVE_FETCH_SYNCS(apu, adr) \
  (!((apu)->romReadable && (adr) >= 0xffc0) && APU_DSP_PAGE_BIT((apu)->spc->dspWatchSync, (adr)))
#else
#define APU_NATIVE_FETCH_SYNCS(apu, adr) 0
#endif
/* The reads a wait loop can start with, by where their address is: 1 dp at
 * pc + 1, 2 !abs at pc + 1, 3 dp at pc + 2 (CMP dp,#imm). The run loop
 * checks the opcode and the address inline and calls apu_native_waitSkip
 * only for a read of a port or timer counter: other instructions pay a table
 * load, common reads (MOV A,dp, ...) a few more. */
static const uint8_t kApuWaitRead[256] = {
  [0xe4] = 1, [0xf8] = 1, [0xeb] = 1, [0x64] = 1, [0x3e] = 1, [0x7e] = 1,
  [0xe5] = 2, [0xe9] = 2, [0xec] = 2, [0x65] = 2, [0x1e] = 2, [0x5e] = 2,
  [0x78] = 3,
};
static inline bool apu_native_waitCandidate(const Apu *apu) {
  const uint16_t pc = apu->spc->pc;
  const unsigned form = kApuWaitRead[apu_native_fetch(apu, pc)];
  if (!form) return false;
  uint16_t adr;
  if (form == 2) {
    if (apu_native_fetch(apu, (uint16_t)(pc + 2))) return false;   /* not page 0 */
    adr = apu_native_fetch(apu, (uint16_t)(pc + 1));
  } else {
    if (apu->spc->p) return false;                                 /* dp in page 1 */
    adr = apu_native_fetch(apu, (uint16_t)(pc + (form == 3 ? 2 : 1)));
  }
  return adr >= 0xf4 && adr != 0xf8 && adr != 0xf9 && adr != 0xfa && adr != 0xfb && adr != 0xfc;
}
/* Out of line: the run loop stays the size its I-cache slot was planned for
 * (n64/icache_order.ld). */
static __attribute__((noinline)) uint32_t apu_native_waitSkip(Apu *apu, uint32_t clocks) {
  Spc *spc = apu->spc;
  const uint16_t pc = spc->pc;
  const uint8_t op = apu_native_fetch(apu, pc);
  /* The read: register (0 A, 1 X, 2 Y, 3 memory vs immediate), MOV or CMP,
   * address, length and cycles (spc.c's cyclesPerOpcode). */
  unsigned reg, len, cyc;
  bool cmp;
  uint16_t adr;
  uint8_t imm = 0;
  switch (op) {
    case 0xe4: reg = 0; cmp = false; len = 2; cyc = 3; break;   /* MOV A, dp */
    case 0xf8: reg = 1; cmp = false; len = 2; cyc = 3; break;   /* MOV X, dp */
    case 0xeb: reg = 2; cmp = false; len = 2; cyc = 3; break;   /* MOV Y, dp */
    case 0xe5: reg = 0; cmp = false; len = 3; cyc = 4; break;   /* MOV A, !abs */
    case 0xe9: reg = 1; cmp = false; len = 3; cyc = 4; break;   /* MOV X, !abs */
    case 0xec: reg = 2; cmp = false; len = 3; cyc = 4; break;   /* MOV Y, !abs */
    case 0x64: reg = 0; cmp = true;  len = 2; cyc = 3; break;   /* CMP A, dp */
    case 0x3e: reg = 1; cmp = true;  len = 2; cyc = 3; break;   /* CMP X, dp */
    case 0x7e: reg = 2; cmp = true;  len = 2; cyc = 3; break;   /* CMP Y, dp */
    case 0x65: reg = 0; cmp = true;  len = 3; cyc = 4; break;   /* CMP A, !abs */
    case 0x1e: reg = 1; cmp = true;  len = 3; cyc = 4; break;   /* CMP X, !abs */
    case 0x5e: reg = 2; cmp = true;  len = 3; cyc = 4; break;   /* CMP Y, !abs */
    case 0x78: reg = 3; cmp = true;  len = 3; cyc = 5; break;   /* CMP dp, #imm */
    default: return 0;
  }
  if (APU_NATIVE_FETCH_SYNCS(apu, pc)) return 0;
  if (op == 0x78) {
    if (spc->p) return 0;
    imm = apu_native_fetch(apu, (uint16_t)(pc + 1));
    adr = apu_native_fetch(apu, (uint16_t)(pc + 2));
  } else if (len == 2) {
    if (spc->p) return 0;                     /* dp is page 1: not the I/O page */
    adr = apu_native_fetch(apu, (uint16_t)(pc + 1));
  } else {
    adr = (uint16_t)(apu_native_fetch(apu, (uint16_t)(pc + 1)) |
                     apu_native_fetch(apu, (uint16_t)(pc + 2)) << 8);
  }
  const bool timer = adr >= 0xfd && adr <= 0xff;
  if (!timer && (adr < 0xf4 || adr > 0xf7)) return 0;
  /* The value every pass reads, and the state a pass leaves. */
  uint8_t v;
  if (timer) {
    apu_native_timers_sync(apu);
    if (apu->timer[adr - 0xfd].counter) return 0;   /* this read sees a count */
    v = 0;
  } else {
    v = apu->inPorts[adr - 0xf4];
  }
  uint8_t r8 = spc->a, nz;
  bool c = spc->c;
  if (reg == 1) r8 = spc->x;
  else if (reg == 2) r8 = spc->y;
  if (!cmp) {
    nz = v;
  } else {
    const int result = reg == 3 ? v + (imm ^ 0xff) + 1 : r8 + (v ^ 0xff) + 1;
    c = result > 0xff;
    nz = (uint8_t)result;
  }
  const bool z = nz == 0, n = (nz & 0x80) != 0;
  /* The branches: they must lead back to the read. */
  uint32_t pass = cyc;
  uint16_t at = (uint16_t)(pc + len);
  int k;
  for (k = 0; k < 3; k++) {
    const uint8_t b = apu_native_fetch(apu, at);
    if (APU_NATIVE_FETCH_SYNCS(apu, at) || APU_NATIVE_FETCH_SYNCS(apu, (uint16_t)(at + 1)))
      return 0;
    const int8_t rel = (int8_t)apu_native_fetch(apu, (uint16_t)(at + 1));
    bool taken;
    if (b == 0x2f) {
      taken = true;                           /* BRA: 4 cycles */
      pass += 4;
    } else if ((b & 0x1f) == 0x10) {          /* BPL BMI BVC BVS BCC BCS BNE BEQ */
      const unsigned sel = b >> 6;
      const bool flag = sel == 0 ? n : sel == 1 ? spc->v : sel == 2 ? c : z;
      taken = flag == ((b >> 5) & 1);
      pass += taken ? 4u : 2u;
    } else {
      return 0;
    }
    at = taken ? (uint16_t)(at + 2 + rel) : (uint16_t)(at + 2);
    if (at == pc) break;
  }
  if (k == 3) return 0;
  /* Passes that read the same value. */
  uint64_t passes = UINT64_MAX;
  if (timer) {
    const Timer *t = &apu->timer[adr - 0xfd];
    if (t->enabled) {
      /* The counter first increments `fire` cycles from now (the inverse of
       * apu_native_timer): tick n comes when cycles + (n - 1) * period + 1
       * clocks have passed, and the first increment is tick `first`. */
      const uint32_t shift = adr == 0xff ? 4u : 7u;
      const uint32_t first = ((uint32_t)(t->target - t->divider - 1u) & 0xffu) + 1u;
      const uint64_t fire = (uint64_t)t->cycles + ((uint64_t)(first - 1u) << shift) + 1u;
      passes = (fire + pass - 1u) / pass;     /* reads at j * pass < fire */
    }
  }
  uint64_t room = clocks;
  if (apu->portQHead != apu->portQTail) {
    const ApuPortWrite *write = &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
    const uint64_t distance = write->target_cycle - apu->portClock;
    if (distance < room) room = distance;
  }
  if (passes > room / pass) passes = room / pass;
  if (!passes) return 0;
  const uint32_t skip = (uint32_t)passes * pass;
  if (!cmp) {
    if (reg == 0) spc->a = v;
    else if (reg == 1) spc->x = v;
    else spc->y = v;
  } else {
    spc->c = c;
  }
  spc->z = z;
  spc->n = n;
  /* The sample edges at the multiples of 32 in [cycles, cycles + skip). */
  const uint32_t to_edge = (32u - (apu->cycles & 31u)) & 31u;
  if (skip > to_edge) {
    for (uint32_t e = (skip - 1u - to_edge) / 32u + 1u; e; --e) {
#if SNESRECOMP_APU_DEFER_DSP
      apu_dspEdge(apu);
#else
      APU_NATIVE_DSP_CYCLE(apu->dsp);
      APU_FE_EDGE(apu);
#endif
    }
  }
  apu->cycles += skip;
  apu->portClock += skip;
  return skip;
}
#endif

#ifdef SNESRECOMP_SPC_PC_PROFILE
/* Profiling builds: SPC cycles executed by instruction address, and cycles
 * the wait skip fast-forwarded (n64 dumps the busiest addresses). */
uint32_t g_spc_pc_cycles[0x10000];
uint32_t g_spc_skip_cycles, g_spc_skip_count, g_spc_run_cycles;
#endif

#if SNESRECOMP_SPC_DBT
/* The runtime's side of the SPC translator (apu.h). */
uint32_t g_spc_dbt_edges;   /* edges run by translated code (self-check) */
uint8_t g_spc_dbt_wait[8192];   /* pcs where the wait skip fast-forwarded a loop */
int spc_dbt_edge(Apu *apu, uint32_t cycle) {
  apu->cycles = cycle;
  g_spc_dbt_edges++;
#if SNESRECOMP_APU_DEFER_DSP
  const uint32_t pending = apu->dspPending;
  apu_dspEdge(apu);
  return pending == 0 || apu->dspPending != pending + 1;
#else
  APU_NATIVE_DSP_CYCLE(apu->dsp);
  APU_FE_EDGE(apu);
  return 0;
#endif
}
void spc_dbt_fe(Apu *apu) {
  APU_FE_BEFORE_ARAM_WRITE(apu);
#if SNESRECOMP_DSP_STREAM
  /* A voice is untracked (the front end may just have found it so): every
   * store goes in the ledger, which translated code leaves to the slow
   * path. The exact test keeps these bits while it lasts. */
  if (apu->fe.untracked) memset(apu->spc->dspWatchLog, 0xff, sizeof apu->spc->dspWatchLog);
#endif
}
void spc_dbt_slow_write(Apu *apu, uint32_t adr, uint32_t val, uint32_t cycle) {
  /* A store to a page the deferred S-DSP watches: apu_spcWriteRam's slow
   * path (the front end has been caught up). */
  apu->cycles = cycle;
#if SNESRECOMP_DSP_STREAM
  /* The audio split: the page's bit says "maybe". (apu_spcWriteRam after
   * its front-end and code-line steps, which translated code has made.) */
  if (apu_streamWriteIsSlow(apu, adr >> 8)) {
    apu_dspWriteSlow(apu, (uint16_t)adr, (uint8_t)val);
  } else {
    if (apu->fe.untracked) apu_streamLedger(apu, (uint16_t)adr);
    apu->ram[adr] = (uint8_t)val;
  }
#else
  apu_dspWriteSlow(apu, (uint16_t)adr, (uint8_t)val);
#endif
}
uint32_t spc_dbt_read_any(Apu *apu, uint32_t adr, uint32_t cycle) {
  apu->cycles = cycle;   /* a read of a page the S-DSP may write: it syncs */
  return apu_spcReadRam(apu, (uint16_t)adr);
}
uint32_t spc_dbt_io_read(Apu *apu, uint32_t adr, uint32_t cycle) {
  apu->cycles = cycle;   /* (a sync it causes starts a new epoch itself) */
  return apu_cpuRead(apu, (uint16_t)adr);
}
void spc_dbt_io_write(Apu *apu, uint32_t adr, uint32_t val, uint32_t cycle) {
  apu->cycles = cycle;
  apu_cpuWrite(apu, (uint16_t)adr, (uint8_t)val);
}
uint32_t g_spc_dbt_total;   /* SPC cycles run (statistics) */
/* Whether the run loop should hand the instruction at pc to the
 * translator: a block is there, or nobody has tried yet and pc is where a
 * block may start, a control-flow target (reached by a jump, branch, call
 * or return, or where translated code left at a block boundary). A pc the
 * interpreter reached by stepping through straight-line code (a block that
 * did not fit in the time left, say) gets no block of its own: every
 * suffix of a block would. */
#ifdef SPC_DBT_START_OFF
bool g_spc_dbt_enabled = false;   /* (bisection: the translator built in but never used) */
#else
bool g_spc_dbt_enabled = true;
#endif   /* (the translator's self-test runs a reference with it off) */
static inline bool apu_native_dbtWants(uint16_t pc, bool at_target) {
  if (__builtin_expect(!g_spc_dbt_enabled, 0)) return false;
  void **const pg = g_spc_dbt_map[pc >> 8];
  if (!pg) return at_target;
  void *const e = pg[pc & 255];
  return e ? e != (void *)1 : at_target;
}
/* After the interpreter ran the instruction at ppc: whether pc may start a
 * block: ppc transferred control (SPC instructions are 1-3 bytes long), or
 * ppc is one the translator does not cover (marked (void *)1), so a block
 * stopped before it. */
static inline bool apu_native_dbtTransfer(uint16_t ppc, uint16_t pc) {
  if ((uint16_t)(pc - ppc) > 3u) return true;
  void **const pg = g_spc_dbt_map[ppc >> 8];
  return pg && pg[ppc & 255] == (void *)1;
}
#endif

static void apu_runCycles_native(Apu *apu, uint32_t clocks) {
#if SNESRECOMP_SPC_DBT
  g_spc_dbt_total += clocks;
#endif
#if SNESRECOMP_SPC_WAIT_SKIP
  /* A wait loop's read is reached at the start of a run (a catch-up that
   * stopped inside the loop) or by a taken branch back to it: only then is
   * it looked for, and only with room for a pass (at least 7 cycles). */
  bool wait_check = true;
#endif
#if SNESRECOMP_SPC_DBT
  bool at_target = false;   /* (a catch-up starts wherever the last one stopped) */
#endif
  while (clocks) {
    apu_drainPortQueue(apu);
#if SNESRECOMP_SPC_WAIT_SKIP
    if (wait_check && !apu->cpuCyclesLeft && clocks >= 7 && apu_native_waitCandidate(apu) &&
        !apu->spc->stopped) {
      const uint32_t skipped = apu_native_waitSkip(apu, clocks);
      if (skipped) {
#if SNESRECOMP_SPC_DBT
        /* A wait loop the skip handles: translated code leaves before it
         * (spc_dbt_may_run), so the skip gets to fast-forward it. */
        const uint16_t wpc = apu->spc->pc;
#ifndef SPC_DBT_NO_WAITMARK
        if (!(g_spc_dbt_wait[wpc >> 3] & (1u << (wpc & 7)))) {
          g_spc_dbt_wait[wpc >> 3] |= (uint8_t)(1u << (wpc & 7));
          g_spc_dbt_epoch += 2;   /* links into it go back through C */
        }
#endif
        at_target = true;      /* (the loop's pc: a branch target) */
#endif
#ifdef SNESRECOMP_SPC_PC_PROFILE
        g_spc_skip_cycles += skipped;
        g_spc_skip_count++;
#endif
        clocks -= skipped;
        continue;
      }
    }
#endif
#if SNESRECOMP_SPC_DBT
    /* A translated block: it runs to its end or an exit, handling the sample
     * edges inside it itself, but never past a queued CPU write or the end
     * of the run. */
    if (!apu->cpuCyclesLeft && !apu->spc->stopped && apu_native_dbtWants(apu->spc->pc, at_target)) {
      uint32_t limit = clocks;
      if (apu->portQHead != apu->portQTail) {
        const ApuPortWrite *write =
            &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
        const uint64_t distance = write->target_cycle - apu->portClock;
        if (limit > distance) limit = (uint32_t)distance;
      }
      const uint16_t bpc = apu->spc->pc;
      const uint32_t c0 = apu->cycles;
      const uint32_t ran = spc_dbt_run(apu, limit);
      at_target = g_spc_dbt_exit_o == 0;   /* left at a block boundary */
      if (ran) {
        apu->cycles = c0 + ran;
        apu->portClock += ran;
        clocks -= ran;
#if SNESRECOMP_SPC_WAIT_SKIP
        { /* (or it stopped before a wait loop the skip handled before) */
          const uint16_t npc = apu->spc->pc;
          wait_check = npc <= bpc || (g_spc_dbt_wait[npc >> 3] & (1u << (npc & 7)));
        }
#endif
        continue;
      }
      apu->cycles = c0;
    }
#endif
    if (!apu->cpuCyclesLeft) {
#if SNESRECOMP_SPC_DIAGNOSTICS
      g_spc_pc_histogram[apu->spc->pc]++;
      if (apu->spc->pc > g_spc_pc_max_seen)
        g_spc_pc_max_seen = apu->spc->pc;
#endif
      const uint16_t ppc = apu->spc->pc;
      apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
#if SNESRECOMP_SPC_WAIT_SKIP
      wait_check = apu->spc->pc < ppc;
#endif
#if SNESRECOMP_SPC_DBT
      at_target = apu_native_dbtTransfer(ppc, apu->spc->pc);
#endif
#ifdef SNESRECOMP_SPC_PC_PROFILE
      g_spc_pc_cycles[ppc] += apu->cpuCyclesLeft;
      g_spc_run_cycles += apu->cpuCyclesLeft;
#endif
    }
    unsigned sample_phase = apu->cycles & 31u;
#if SNESRECOMP_APU_DEFER_DSP
    if (!sample_phase) apu_dspEdge(apu);
#else
    if (!sample_phase) { APU_NATIVE_DSP_CYCLE(apu->dsp); APU_FE_EDGE(apu); }
#endif
    /* The window to the next event: the next sample edge, the next queued
     * CPU port write, or the end of the run. Inside it nothing but the SPC
     * itself happens, so the instructions that start in it run back to back
     * here, each with apu->cycles at its own start, as the loop above would
     * run them one iteration at a time. */
    uint32_t win = 32u - sample_phase;
    if (win > clocks) win = clocks;
    if (apu->portQHead != apu->portQTail) {
      const ApuPortWrite *write =
          &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
      uint64_t distance = write->target_cycle - apu->portClock;
      if (win > distance) win = (uint32_t)distance;
    }
    uint32_t left = apu->cpuCyclesLeft;
    uint32_t used = left < win ? left : win;
    left -= used;
#if !SNESRECOMP_SPC_DIAGNOSTICS
    if (!left && used < win) {
      const uint32_t c0 = apu->cycles;
      Spc *const spc = apu->spc;
      do {
#if SNESRECOMP_SPC_WAIT_SKIP
        /* A possible wait loop: back to the loop above, which looks for a
         * skip (or runs the instruction). */
        if (wait_check && apu_native_waitCandidate(apu)) break;
#endif
#if SNESRECOMP_SPC_DBT
        if (apu_native_dbtWants(spc->pc, at_target)) break;   /* a translated block, or one to try */
#endif
        apu->cycles = c0 + used;
        const uint16_t ppc = spc->pc;
        left = (uint32_t)spc_runOpcode(spc);
#if SNESRECOMP_SPC_WAIT_SKIP
        wait_check = spc->pc < ppc;
#endif
#if SNESRECOMP_SPC_DBT
        at_target = apu_native_dbtTransfer(ppc, spc->pc);
#endif
#ifdef SNESRECOMP_SPC_PC_PROFILE
        g_spc_pc_cycles[ppc] += left;
        g_spc_run_cycles += left;
#else
        (void)ppc;
#endif
        const uint32_t room = win - used;
        const uint32_t step = left < room ? left : room;
        left -= step;
        used += step;
      } while (!left && used < win);
      apu->cycles = c0;
    }
#endif
    apu->cpuCyclesLeft = (uint8_t)left;
    apu->cycles += used;
    apu->portClock += used;
    clocks -= used;
  }
  apu_native_timers_sync(apu);
}
