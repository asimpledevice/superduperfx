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

/* Timer wait loops. Sound drivers spend much of their time in
 *     loop: MOV reg, Tn-OUT   ; $FD-$FF, dp or absolute, into A, X or Y
 *           BEQ loop
 * Each pass reads 0 (which leaves the counter 0), sets reg = 0, Z = 1,
 * N = 0 and branches back; nothing else changes. Passes are skipped in
 * closed form up to the first one whose read sees the next counter
 * increment, which runs normally. The skip never crosses a queued CPU port
 * write or the end of the run, and sends the DSP the sample edges the
 * skipped cycles contain, in order, so the result is identical to running
 * every pass (SNESRECOMP_SPC_WAIT_SKIP=0 runs them). Returns the cycles
 * skipped (0: not at such a loop, or nothing to skip). */
#ifndef SNESRECOMP_SPC_WAIT_SKIP
#define SNESRECOMP_SPC_WAIT_SKIP (!SNESRECOMP_SPC_DIAGNOSTICS)
#endif
#if SNESRECOMP_SPC_WAIT_SKIP
static uint32_t apu_native_waitSkip(Apu *apu, uint32_t clocks) {
  Spc *spc = apu->spc;
  const uint16_t pc = spc->pc;
  const uint8_t *ram = apu->ram;
  unsigned len, cyc;
  uint16_t adr;
  switch (ram[pc]) {
    case 0xe4: case 0xf8: case 0xeb:   /* MOV A/X/Y, dp */
      if (spc->p) return 0;
      adr = ram[(uint16_t)(pc + 1)];
      len = 2; cyc = 3;
      break;
    case 0xe5: case 0xe9: case 0xec:   /* MOV A/X/Y, !abs */
      adr = (uint16_t)(ram[(uint16_t)(pc + 1)] | ram[(uint16_t)(pc + 2)] << 8);
      len = 3; cyc = 4;
      break;
    default:
      return 0;
  }
  if (adr < 0xfd || adr > 0xff) return 0;
  const uint16_t bpc = (uint16_t)(pc + len);
  if (ram[bpc] != 0xf0 || (uint16_t)(bpc + 2 + (int8_t)ram[(uint16_t)(bpc + 1)]) != pc)
    return 0;
  if (pc >= 0xffb0) return 0;               /* keep clear of the IPL ROM overlay */
#if SNESRECOMP_APU_DEFER_DSP
  /* Code fetches from pages the DSP may write would sync it first. */
  if (APU_DSP_PAGE_BIT(spc->dspWatchSync, pc) ||
      APU_DSP_PAGE_BIT(spc->dspWatchSync, (uint16_t)(bpc + 1)))
    return 0;
#endif
  apu_native_timers_sync(apu);
  const Timer *t = &apu->timer[adr - 0xfd];
  if (t->counter) return 0;                 /* this read sees a count */
  const uint32_t pass = cyc + 4u;           /* the read, then BEQ taken */
  uint64_t passes = UINT64_MAX;             /* passes whose read sees 0 */
  if (t->enabled) {
    /* The counter first increments `fire` cycles from now (the inverse of
     * apu_native_timer): tick n comes when cycles + (n - 1) * period + 1
     * clocks have passed, and the first increment is tick `first`. */
    const uint32_t shift = adr == 0xff ? 4u : 7u;
    const uint32_t first = ((uint32_t)(t->target - t->divider - 1u) & 0xffu) + 1u;
    const uint64_t fire = (uint64_t)t->cycles + ((uint64_t)(first - 1u) << shift) + 1u;
    passes = (fire + pass - 1u) / pass;     /* reads at j * pass < fire */
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
  switch (ram[pc]) {
    case 0xe4: case 0xe5: spc->a = 0; break;
    case 0xf8: case 0xe9: spc->x = 0; break;
    default: spc->y = 0; break;
  }
  spc->z = true;
  spc->n = false;
  /* The sample edges at the multiples of 32 in [cycles, cycles + skip). */
  const uint32_t to_edge = (32u - (apu->cycles & 31u)) & 31u;
  if (skip > to_edge) {
    for (uint32_t n = (skip - 1u - to_edge) / 32u + 1u; n; --n) {
#if SNESRECOMP_APU_DEFER_DSP
      apu_dspEdge(apu);
#else
      APU_NATIVE_DSP_CYCLE(apu->dsp);
#endif
    }
  }
  apu->cycles += skip;
  apu->portClock += skip;
  return skip;
}
#endif

static void apu_runCycles_native(Apu *apu, uint32_t clocks) {
  while (clocks) {
    apu_drainPortQueue(apu);
#if SNESRECOMP_SPC_WAIT_SKIP
    if (!apu->cpuCyclesLeft && !apu->spc->stopped) {
      const uint32_t skipped = apu_native_waitSkip(apu, clocks);
      if (skipped) {
        clocks -= skipped;
        continue;
      }
    }
#endif
    if (!apu->cpuCyclesLeft) {
#if SNESRECOMP_SPC_DIAGNOSTICS
      g_spc_pc_histogram[apu->spc->pc]++;
      if (apu->spc->pc > g_spc_pc_max_seen)
        g_spc_pc_max_seen = apu->spc->pc;
#endif
      apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
    }
    unsigned sample_phase = apu->cycles & 31u;
#if SNESRECOMP_APU_DEFER_DSP
    if (!sample_phase) apu_dspEdge(apu);
#else
    if (!sample_phase) APU_NATIVE_DSP_CYCLE(apu->dsp);
#endif
    uint32_t span = apu->cpuCyclesLeft;
    unsigned to_sample = 32u - sample_phase;
    if (span > to_sample) span = to_sample;
    if (span > clocks) span = clocks;
    if (apu->portQHead != apu->portQTail) {
      const ApuPortWrite *write =
          &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1u)];
      uint64_t distance = write->target_cycle - apu->portClock;
      if (span > distance) span = (uint32_t)distance;
    }
    apu->cpuCyclesLeft -= (uint8_t)span;
    apu->cycles += span;
    apu->portClock += span;
    clocks -= span;
  }
  apu_native_timers_sync(apu);
}
