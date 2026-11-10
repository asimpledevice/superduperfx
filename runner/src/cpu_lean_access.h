/* Addressing-mode helpers for lean generated code (SNESRECOMP_EMIT_LEAN).
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
 * cpu_state.h includes the declarations. Definitions: define
 * CPU_LEAN_ACCESS_DEFINE and include this header again, once per program
 * (cpu_state.c; test harnesses that supply their own cpu_read8 etc.). */
#ifndef CPU_LEAN_ACCESS_H
#define CPU_LEAN_ACCESS_H

#define CPU_LEAN_ACCESS_FORMS(FORM) \
  FORM(dp,    0x00, (uint16)(cpu->D + k)) \
  FORM(dpx,   0x00, (uint16)(cpu->D + k + cpu->X)) \
  FORM(absX,  (uint8)((((uint32)cpu->DB << 16) + (uint32)k + (uint32)cpu->X) >> 16), \
                   (uint16)(((uint32)cpu->DB << 16) + (uint32)k + (uint32)cpu->X)) \
  FORM(absY,  (uint8)((((uint32)cpu->DB << 16) + (uint32)k + (uint32)cpu->Y) >> 16), \
                   (uint16)(((uint32)cpu->DB << 16) + (uint32)k + (uint32)cpu->Y)) \
  FORM(longx, (uint8)(((uint32)k + (uint32)cpu->X) >> 16), \
                   (uint16)((uint32)k + (uint32)cpu->X))

#define CPU_LEAN_ACCESS_DECLARE(name, bank, addr) \
  uint8  cpu_read_##name##8(CpuState *cpu, uint32 k); \
  uint16 cpu_read_##name##16(CpuState *cpu, uint32 k); \
  void   cpu_write_##name##8(CpuState *cpu, uint32 k, uint8 v); \
  void   cpu_write_##name##16(CpuState *cpu, uint32 k, uint16 v);
CPU_LEAN_ACCESS_FORMS(CPU_LEAN_ACCESS_DECLARE)
void   cpu_push8(CpuState *cpu, uint8 v);
uint8  cpu_pull8(CpuState *cpu);
uint32 cpu_host_return_pc24(CpuState *cpu, uint16 entry_s, uint8 hrv, uint32 skip);

#endif

#if defined(CPU_LEAN_ACCESS_DEFINE) && !defined(CPU_LEAN_ACCESS_DEFINED)
#define CPU_LEAN_ACCESS_DEFINED
#define CPU_LEAN_ACCESS_DEFINE_FORM(name, bank, addr) \
  uint8 cpu_read_##name##8(CpuState *cpu, uint32 k) { \
    return cpu_read8(cpu, bank, addr); } \
  uint16 cpu_read_##name##16(CpuState *cpu, uint32 k) { \
    return cpu_read16(cpu, bank, addr); } \
  void cpu_write_##name##8(CpuState *cpu, uint32 k, uint8 v) { \
    cpu_write8(cpu, bank, addr, v); } \
  void cpu_write_##name##16(CpuState *cpu, uint32 k, uint16 v) { \
    cpu_write16(cpu, bank, addr, v); }
CPU_LEAN_ACCESS_FORMS(CPU_LEAN_ACCESS_DEFINE_FORM)
void cpu_push8(CpuState *cpu, uint8 v) {
  cpu_write8(cpu, 0x00, cpu->S, v);
  cpu->S = (uint16)(cpu->S - 1);
}
uint8 cpu_pull8(CpuState *cpu) {
  cpu->S = (uint16)(cpu->S + 1);
  return cpu_read8(cpu, 0x00, cpu->S);
}
/* The return address a paired host caller left on the stack (hrv 2: JSR
 * frame, 3: JSL frame), plus 1 and the inline argument bytes the function
 * consumes; 0xFFFFFFFF when there is no such frame. */
uint32 cpu_host_return_pc24(CpuState *cpu, uint16 entry_s, uint8 hrv, uint32 skip) {
  if (hrv != 2 && hrv != 3) return 0xFFFFFFFFu;
  uint16 rpcl = cpu_read8(cpu, 0x00, (uint16)(entry_s + 1u));
  uint16 rpch = cpu_read8(cpu, 0x00, (uint16)(entry_s + 2u));
  uint8 rpb = (hrv == 3) ? cpu_read8(cpu, 0x00, (uint16)(entry_s + 3u)) : cpu->PB;
  return ((uint32)rpb << 16) | (uint16)((((rpch << 8) | rpcl) + 1u + skip) & 0xFFFFu);
}
#endif
