/* Helpers for lean generated code (SNESRECOMP_EMIT_LEAN).
 *
 * Each helper is exactly the call the default emitter writes inline at every
 * site (same bus call, same address arithmetic, same order), moved out of
 * line so a site passes only its constant:
 *
 *   cpu_read_dp8(cpu, k)    = cpu_read8(cpu, 0x00, (uint16)(cpu->D + k))
 *   cpu_read_dpx8(cpu, k)   = cpu_read8(cpu, 0x00, (uint16)(cpu->D + k + cpu->X))
 *   cpu_read_absX8(cpu, k)  = cpu_read8(cpu, bank, addr) of (DB << 16) + k + X
 *   cpu_read_absY8(cpu, k)  = the same with Y
 *   cpu_read_longx8(cpu, k) = the same of k (24-bit) + X
 *   ... and the 16-bit and write forms of each;
 *   cpu_push8(cpu, v)       = cpu_write8(cpu, 0x00, cpu->S, v); S -= 1
 *   cpu_pull8(cpu)          = S += 1; cpu_read8(cpu, 0x00, cpu->S)
 *   cpu_host_return_pc24    = the function prologue's return-frame read.
 *
 * and the emitter's other fixed sequences, each in the order it writes them:
 *
 *   cpu_charge(cpu, n)          = cycles += n; master_cycles += n * speed
 *   cpu_charge_dl(cpu)          = if (D & 0xFF) cpu_charge(cpu, 1)
 *   cpu_charge_page(cpu, k, i)  = if ((k & 0xFF00) != ((k + i) & 0xFF00)) cpu_charge(cpu, 1)
 *   cpu_sync_p_nz(cpu)          = P's N and Z bits from the flag mirrors
 *   recomp_block_enter(cpu, pc) = WatchdogCheck(); deadline test at PB:pc
 *   recomp_block_enter_charge   = the same, then (no yield) the coprocessor
 *                                 clock store and the block's static charge
 *   recomp_enter(name)          = g_last_recomp_func, RecompStackPush, cpu_dbg_funcname
 *   recomp_frame(...)           = entry S, tail-call context, host return frame,
 *                                 entry-S record
 *
 * speed is g_code_speed[PB >> 7] (cpu_state.h).
 *
 * cpu_state.h includes the declarations. Definitions: define
 * CPU_LEAN_ACCESS_DEFINE and include this header again, once per program
 * (cpu_state.c; test harnesses that supply their own cpu_read8 etc.). */
#ifndef CPU_LEAN_ACCESS_H
#define CPU_LEAN_ACCESS_H

#define CPU_LEAN_ACCESS_FORMS(FORM) \
  FORM(dp,    0x00, (uint16)(cpu_D() + k)) \
  FORM(dpx,   0x00, (uint16)(cpu_D() + k + cpu_X())) \
  FORM(absX,  (uint8)((((uint32)cpu_DB() << 16) + (uint32)k + (uint32)cpu_X()) >> 16), \
                   (uint16)(((uint32)cpu_DB() << 16) + (uint32)k + (uint32)cpu_X())) \
  FORM(absY,  (uint8)((((uint32)cpu_DB() << 16) + (uint32)k + (uint32)cpu_Y()) >> 16), \
                   (uint16)(((uint32)cpu_DB() << 16) + (uint32)k + (uint32)cpu_Y())) \
  FORM(longx, (uint8)(((uint32)k + (uint32)cpu_X()) >> 16), \
                   (uint16)((uint32)k + (uint32)cpu_X()))

#define CPU_LEAN_ACCESS_DECLARE(name, bank, addr) \
  uint8  cpu_read_##name##8(CpuState *cpu, uint32 k); \
  uint16 cpu_read_##name##16(CpuState *cpu, uint32 k); \
  void   cpu_write_##name##8(CpuState *cpu, uint32 k, uint32 v); \
  void   cpu_write_##name##16(CpuState *cpu, uint32 k, uint32 v);
CPU_LEAN_ACCESS_FORMS(CPU_LEAN_ACCESS_DECLARE)
void   cpu_push8(CpuState *cpu, uint32 v);   /* stores (uint8)v */
uint8  cpu_pull8(CpuState *cpu);
void   cpu_push16(CpuState *cpu, uint32 v);   /* S-1, word at S, S-1 */
uint16 cpu_pull16(CpuState *cpu);              /* S+1, word at S, S+1 */
uint32 cpu_host_return_pc24(CpuState *cpu, uint16 entry_s, uint8 hrv, uint32 skip);
void   cpu_charge(CpuState *cpu, uint32 n);
void   cpu_charge_dl(CpuState *cpu);
void   cpu_charge_page(CpuState *cpu, uint32 k, uint32 i);
void   cpu_sync_p_nz(CpuState *cpu);
int    recomp_block_enter(CpuState *cpu, uint32 pc16);
int    recomp_block_enter_charge(CpuState *cpu, uint32 pc16, uint32 n);
int    recomp_block_enter_charge_c(CpuState *cpu, uint32 pc16, uint32 n, uint32 master);
void   recomp_enter(const char *name);
struct RecompRoutine;
/* recomp_enter with the name from a code-cache record (recomp_cache.c). */
void   recomp_enter_rt(const struct RecompRoutine *rt);
uint32 recomp_frame(CpuState *cpu, int s_offset, uint32 skip, uint16 *entry_s, uint8 *hrv);
RecompReturn recomp_return_slow(CpuState *cpu, uint32 rpc24, uint16 ret_s, uint16 entry_s,
                                uint8 hrv, uint32 host_return_pc24, uint32 frame_sz,
                                uint32 site);

#endif

#if defined(CPU_LEAN_ACCESS_DEFINE) && !defined(CPU_LEAN_ACCESS_DEFINED)
#define CPU_LEAN_ACCESS_DEFINED
#define CPU_LEAN_ACCESS_DEFINE_FORM(name, bank, addr) \
  uint8 cpu_read_##name##8(CpuState *cpu, uint32 k) { \
    return cpu_read8(cpu, bank, addr); } \
  uint16 cpu_read_##name##16(CpuState *cpu, uint32 k) { \
    return cpu_read16(cpu, bank, addr); } \
  void cpu_write_##name##8(CpuState *cpu, uint32 k, uint32 v) { \
    cpu_write8(cpu, bank, addr, v); } \
  void cpu_write_##name##16(CpuState *cpu, uint32 k, uint32 v) { \
    cpu_write16(cpu, bank, addr, v); }
CPU_LEAN_ACCESS_FORMS(CPU_LEAN_ACCESS_DEFINE_FORM)
void cpu_push8(CpuState *cpu, uint32 v) {
  cpu_write8(cpu, 0x00, cpu_S(), v);
  cpu_set_S((uint16)(cpu_S() - 1));
}
uint8 cpu_pull8(CpuState *cpu) {
  cpu_set_S((uint16)(cpu_S() + 1));
  return cpu_read8(cpu, 0x00, cpu_S());
}
void cpu_push16(CpuState *cpu, uint32 v) {
  cpu_set_S((uint16)(cpu_S() - 1));
  cpu_write16(cpu, 0x00, cpu_S(), v);
  cpu_set_S((uint16)(cpu_S() - 1));
}
uint16 cpu_pull16(CpuState *cpu) {
  cpu_set_S((uint16)(cpu_S() + 1));
  uint16 v = cpu_read16(cpu, 0x00, cpu_S());
  cpu_set_S((uint16)(cpu_S() + 1));
  return v;
}
/* The return address a paired host caller left on the stack (hrv 2: JSR
 * frame, 3: JSL frame), plus 1 and the inline argument bytes the function
 * consumes; 0xFFFFFFFF when there is no such frame. */
uint32 cpu_host_return_pc24(CpuState *cpu, uint16 entry_s, uint8 hrv, uint32 skip) {
  if (hrv != 2 && hrv != 3) return 0xFFFFFFFFu;
  uint16 rpcl = cpu_read8(cpu, 0x00, (uint16)(entry_s + 1u));
  uint16 rpch = cpu_read8(cpu, 0x00, (uint16)(entry_s + 2u));
  uint8 rpb = (hrv == 3) ? cpu_read8(cpu, 0x00, (uint16)(entry_s + 3u)) : cpu_PB();
  return ((uint32)rpb << 16) | (uint16)((((rpch << 8) | rpcl) + 1u + skip) & 0xFFFFu);
}

/* The runtime symbols the sequences below call (common_cpu_infra.h). */
void WatchdogCheck(void);
void RecompStackPush(const char *name);
void RecompStackPop(void);
void RecompStackPopYield(void);
int cpu_resolve_ancestor_skip(uint16_t ret_s);
int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv);
extern int g_recomp_stack_top;
extern uint16_t g_cpu_entry_s[];
extern const char *g_last_recomp_func;

void cpu_charge(CpuState *cpu, uint32 n) {
  cpu->cycles += n;
  cpu->master_cycles += n * (uint32)g_code_speed[cpu_PB() >> 7];
}
void cpu_charge_dl(CpuState *cpu) {
  if (cpu_D() & 0xFF) cpu_charge(cpu, 1u);
}
void cpu_charge_page(CpuState *cpu, uint32 k, uint32 i) {
  if ((k & 0xFF00) != ((k + i) & 0xFF00)) cpu_charge(cpu, 1u);
}
void cpu_sync_p_nz(CpuState *cpu) {
  cpu_set_P((uint8)((cpu_P() & ~0x82) | (cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0)));
}
int recomp_block_enter(CpuState *cpu, uint32 pc16) {
  WatchdogCheck();
  return interp_bridge_lle_deadline_pause(cpu, ((uint32)cpu_PB() << 16) | pc16);
}
int recomp_block_enter_charge(CpuState *cpu, uint32 pc16, uint32 n) {
  if (recomp_block_enter(cpu, pc16)) return 1;
  cpu->coprocessor_master_cycles = cpu->master_cycles;
  cpu_charge(cpu, n);
  return 0;
}
/* The same for code whose region speed is fixed: master = n * that speed. */
int recomp_block_enter_charge_c(CpuState *cpu, uint32 pc16, uint32 n, uint32 master) {
  if (recomp_block_enter(cpu, pc16)) return 1;
  cpu->coprocessor_master_cycles = cpu->master_cycles;
  cpu->cycles += n;
  cpu->master_cycles += master;
  return 0;
}
void recomp_enter(const char *name) {
  g_last_recomp_func = name;
  RecompStackPush(name);
  cpu_dbg_funcname(name);
}
uint32 recomp_frame(CpuState *cpu, int s_offset, uint32 skip, uint16 *entry_s, uint8 *hrv) {
  uint16 s = (uint16)(cpu_S() + s_offset);
  uint8 h = cpu->host_return_valid;
  if (cpu_take_tailcall_return_context(&s, &h)) cpu->host_return_valid = h;
  uint32 host_return_pc24 = cpu_host_return_pc24(cpu, s, h, skip);
  if (g_recomp_stack_top >= 1) g_cpu_entry_s[g_recomp_stack_top - 1] = s;
  *entry_s = s;
  *hrv = h;
  return host_return_pc24;
}
/* An RTS/RTL that is not the paired, balanced host return (the call site
 * tests that one first): the default emitter's decision tree for the popped
 * frame, test for test, each exit preceded by the recomp-stack pop the
 * emitter writes before it (codegen.py _emit_return). frame_sz is 2 for RTS,
 * 3 for RTL; site is the RTS/RTL PC. */
RecompReturn recomp_return_slow(CpuState *cpu, uint32 rpc24, uint16 ret_s, uint16 entry_s,
                                uint8 hrv, uint32 host_return_pc24, uint32 frame_sz,
                                uint32 site) {
  /* A paired, balanced frame whose return address was rewritten to code with
   * no compiled entry. */
  if (hrv == frame_sz && ret_s == entry_s &&
      rpc24 != host_return_pc24 && !cpu_dispatch_has_entry(cpu, rpc24)) {
    RecompStackPop();
    return interp_tier_dispatch_rewritten_return(cpu, rpc24, site);
  }
  if (ret_s != entry_s) {
    /* Return to an ancestor's continuation (multi-level non-local return). */
    int anc_skip = cpu_resolve_ancestor_skip(ret_s);
    if (anc_skip >= 0) {
      cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);
      RecompStackPop();
      return (RecompReturn)anc_skip;
    }
    /* The callee moved its own frame above stack arguments and dropped them. */
    if (hrv == frame_sz && rpc24 == host_return_pc24 &&
        (uint16)(ret_s - entry_s) < 0x8000u) {
      RecompStackPop();
      return RECOMP_RETURN_NORMAL;
    }
    /* The skipped caller runs in the interpreter. */
    if (interp_bridge_return_targets_owner(ret_s, cpu_S())) {
      cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);
      RecompStackPopYield();
      return interp_bridge_lle_yield_unwind(cpu, rpc24);
    }
    if ((uint16)(ret_s - entry_s) < 0x8000u &&
        interp_bridge_has_direct_paired_bounce()) {
      RecompStackPop();
      return interp_tier_dispatch_rewritten_return(cpu, rpc24, site);
    }
  }
  /* Deeper than entry: the RTS/RTL consumed a frame this routine pushed. */
  if (ret_s != entry_s &&
      (uint16)(entry_s - ret_s) < 0x8000u &&
      interp_bridge_has_direct_paired_bounce()) {
    RecompStackPop();
    return interp_tier_dispatch_rewritten_return(cpu, rpc24, site);
  }
  if (ret_s != entry_s &&
      (uint16)(entry_s - ret_s) < 0x8000u &&
      cpu_S() != entry_s &&
      (uint16)(cpu_S() - entry_s) < 0x8000u) {
    RecompStackPop();
    return interp_tier_dispatch_rewritten_return(cpu, rpc24, site);
  }
  if (ret_s != entry_s &&
      (uint16)(entry_s - ret_s) < 0x8000u &&
      !cpu_dispatch_has_entry(cpu, rpc24)) {
    RecompStackPop();
    return interp_tier_dispatch_popped_return(cpu, rpc24, site, (uint16)(entry_s + frame_sz));
  }
  cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);
  RecompStackPop();
  return cpu_dispatch_pc_from(cpu, rpc24, (uint16)(entry_s + frame_sz), site);
}
#endif
