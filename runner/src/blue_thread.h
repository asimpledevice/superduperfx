/* The Blue thread: the 65816 on its own host stack (opt-in for now:
 * SNESRECOMP_BLUE_THREAD=1). Green, the frame driver and the rest of the CPU
 * work, runs on the main stack.
 *
 * The frame driver runs the guest program through
 * blue_run_until_quiescent instead of interp_bridge_run_until_quiescent.
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
#ifndef SNESRECOMP_BLUE_THREAD_H
#define SNESRECOMP_BLUE_THREAD_H

#include <stdint.h>
#include "cpu_state.h"
#if SNESRECOMP_BLUE_THREAD
#include "run_flags.h"
#endif

#ifndef SNESRECOMP_BLUE_THREAD
#define SNESRECOMP_BLUE_THREAD 0
#endif

#if SNESRECOMP_BLUE_THREAD
/* Drop-in for interp_bridge_run_until_quiescent in a frame driver. */
int blue_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24);

/* Called on the Blue thread at a deadline (interp_bridge.c). Returns 1 when
 * the driver resumed this PC (continue), 0 when it wants the unwind. Only
 * valid while blue_is_current(). */
int blue_is_current(void);
int blue_pause(uint32_t resume_pc24, uint16_t resume_s);

/* Yield (Blue, at a safe point, when the run flags ask for a switch): Green
 * clears RUN_FLAG_SWITCH, runs its service and switches straight back; the
 * driver does not see it. The service must not touch emulated state (the
 * 65816's run is mid-flight), so a yield never changes results; it is where
 * Green's independent work goes. Defined with the run flags (run_flags.h). */
void blue_yield(void);
void blue_set_green_service(void (*service)(void));

/* Optional: the host stack the guest runs on, [lo, top) (default: 256 KB
 * from malloc). A host that places its stack for the data cache gives the
 * guest the same placement: the guest's frames are the hot ones. Call
 * before the first run. */
void blue_set_stack(void *lo, void *top);

/* Pauses, resumes, unwound pauses and yields so far (diagnostics). */
extern uint32_t g_blue_pauses, g_blue_resumes, g_blue_unwinds, g_blue_yields;
#else
#include "snes/interp_bridge.h"
static inline int blue_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
  return interp_bridge_run_until_quiescent(cpu, entry_pc24);
}
static inline int blue_is_current(void) { return 0; }
#endif

#endif
