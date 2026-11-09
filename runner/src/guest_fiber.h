/* Guest CPU on its own host stack (opt-in: SNESRECOMP_GUEST_FIBER=1).
 *
 * The frame driver runs the guest program through
 * guest_fiber_run_until_quiescent instead of interp_bridge_run_until_quiescent.
 * The run executes on a second host stack, so when generated code reaches the
 * scheduler's master-clock deadline it can PAUSE there (switch back to the
 * driver) instead of unwinding every native frame: the driver delivers its
 * interrupt as before, and the next run at the same PC continues the paused
 * native code where it stopped, rather than interpreting the rest of the
 * routine and its callers.
 *
 * To the driver a pause looks exactly like a deadline unwind: the run returns
 * the same value with the same resume PC, APU flush and beam catch-up, and
 * the bridge's nesting state and the generated-code shadow stack are back at
 * their run-start values while the driver runs. A run at any other PC or S
 * (reset, state load, an interrupt handler that rewrote its return address)
 * first unwinds the paused code the ordinary way. */
#ifndef SNESRECOMP_GUEST_FIBER_H
#define SNESRECOMP_GUEST_FIBER_H

#include <stdint.h>
#include "cpu_state.h"

#ifndef SNESRECOMP_GUEST_FIBER
#define SNESRECOMP_GUEST_FIBER 0
#endif

#if SNESRECOMP_GUEST_FIBER
/* Drop-in for interp_bridge_run_until_quiescent in a frame driver. */
int guest_fiber_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24);

/* Called on the guest fiber at a deadline (interp_bridge.c). Returns 1 when
 * the driver resumed this PC (continue), 0 when it wants the unwind. Only
 * valid while guest_fiber_on_guest(). */
int guest_fiber_on_guest(void);
int guest_fiber_pause(uint32_t resume_pc24, uint16_t resume_s);

/* Optional: the host stack the guest runs on, [lo, top) (default: 256 KB
 * from malloc). A host that places its stack for the data cache gives the
 * guest the same placement: the guest's frames are the hot ones. Call
 * before the first run. */
void guest_fiber_set_stack(void *lo, void *top);

/* Pauses, resumes and unwound pauses so far (diagnostics). */
extern uint32_t g_guest_fiber_pauses, g_guest_fiber_resumes, g_guest_fiber_unwinds;
#else
#include "snes/interp_bridge.h"
static inline int guest_fiber_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  return interp_bridge_run_until_quiescent(cpu, entry_pc24);
}
static inline int guest_fiber_on_guest(void) { return 0; }
#endif

#endif
