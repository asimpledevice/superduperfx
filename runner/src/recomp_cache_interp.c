/* Interpreter fallback for the code cache's background loading (see
 * recomp_cache.h): runs a routine whose compiled body is still loading in the
 * interpreter, and returns what that body would have.
 *
 * The body's prologue runs first -- its recomp stack frame, entry S with the
 * tail-call return context, the entry-S record -- then the frame is popped
 * and the interpreter runs the routine from its entry PC, as the emitter's AOT
 * deny gate does: a non-local return inside it (SMW's GetDrawInfo PLA/PLA/RTS
 * abort of its caller) then lands in the interpreter, which owns that
 * continuation. A return past the routine's own frame (an SMW pointer-table
 * call returning to its caller's caller) resolves into the same SKIP_N its
 * body's RTS/RTL would return (interp_tier_dispatch_routine).
 *
 * Needs g_recomp_fallback (tools/n64/split_code_cache.py) and rt->id. */
#include "recomp_cache.h"
#include "common_cpu_infra.h"

RecompReturn recomp_cache_interp_fallback(RecompRoutine *rt, CpuState *cpu) {
  const RecompFallback *fb = &g_recomp_fallback[rt->id];
  recomp_enter_rt(rt);
  uint16 entry_s; uint8 hrv;
  (void)recomp_frame(cpu, fb->s_offset, 0u, &entry_s, &hrv);
  const int tail = cpu_top_frame_tail_entered();
  RecompStackPop();
  /* A short entry keeps the live PB, as the body's transfers do. */
  const uint32 target = ((uint32)cpu->PB << 16) | (fb->pc24 & 0xFFFFu);
  return interp_tier_dispatch_routine(cpu, target, fb->pc24, entry_s, hrv, tail);
}
