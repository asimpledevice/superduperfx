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

static void apu_runCycles_native(Apu *apu, uint32_t clocks) {
  while (clocks) {
    apu_drainPortQueue(apu);
    if (!apu->cpuCyclesLeft) {
#if SNESRECOMP_SPC_DIAGNOSTICS
      g_spc_pc_histogram[apu->spc->pc]++;
      if (apu->spc->pc > g_spc_pc_max_seen)
        g_spc_pc_max_seen = apu->spc->pc;
#endif
      apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
    }
    unsigned sample_phase = apu->cycles & 31u;
    if (!sample_phase) APU_NATIVE_DSP_CYCLE(apu->dsp);
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
