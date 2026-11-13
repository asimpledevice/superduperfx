/* Accessors for the 65816 registers of the one CPU state (cpu_state.h).
 *
 *   cpu_A()        read, in the register's C type (uint16 / uint8)
 *   cpu_set_A(v)   write, converting v to that type (as assignment to the
 *                  field did); returns nothing, so it cannot be used as a
 *                  value
 *
 * Generated code (the v2 emitter writes these calls; see
 * recompiler/v2/emitter_helpers.assign) and the runtime use these instead of
 * the fields. Included from cpu_state.h only.
 *
 * SNESRECOMP_CPU_REGS_FPU=1 (n64 CPUFPU=1): A, X, Y, S, D, DB, PB, E and the
 * status flags live in reserved FPU registers (fpu_slots.h) as GCC global
 * register variables, always zero-extended, so a read is one MFC1 and a write
 * one MTC1 (plus the conversion the C type asks for). Otherwise they live in
 * g_cpu's reg_* fields.
 *
 * P is not stored as a byte. Its parts are:
 *   P_mxid   P's M, X, I and D bits, in place (the other bits are 0)
 *   nz       the last result N and Z come from (cpu_set_nz8/16)
 *   flag_C, flag_V   0 or 1
 * cpu_P() puts the byte together and cpu_set_P() takes one apart, for the
 * few places that need the whole byte (PHP, PLP, RTI, interrupts, REP/SEP of
 * mixed bits, the interpreter). Everything else reads and writes one flag.
 *
 * Code that copies g_cpu as a whole (rollback) calls cpu_regs_store() after
 * the copy and cpu_regs_load() before using one, which move the registers
 * between their home and a CpuState's reg_* fields. */
#ifndef SNESRECOMP_CPU_REGS_H
#define SNESRECOMP_CPU_REGS_H

#ifndef SNESRECOMP_CPU_REGS_FPU
#define SNESRECOMP_CPU_REGS_FPU 0
#endif

#define CPU_REG_IN_STRUCT(name, type)                                        \
  static inline __attribute__((always_inline)) type cpu_##name(void) {      \
    return g_cpu.reg_##name;                                                \
  }                                                                         \
  static inline __attribute__((always_inline)) void cpu_set_##name(uint32_t v) { \
    g_cpu.reg_##name = (type)v;                                             \
  }

#if SNESRECOMP_CPU_REGS_FPU
#if !defined(__mips__)
#error "SNESRECOMP_CPU_REGS_FPU keeps the 65816 registers in VR4300 FPU registers"
#endif
#include "fpu_slots.h"
/* Reads move the register into a GPR through an empty asm. Without it GCC 16
 * can fold the FPU register into a narrower access and fail (internal
 * compiler error in mips_load_store_insns; seen reading PB to index a table
 * and D in a condition, so every register gets it). GCC still emits the
 * MFC1 itself, so it keeps the VR4300's one-instruction MFC1 hazard in view.
 * The range check after the barrier lets the compiler drop some masks; it
 * cannot see through the barrier everywhere.
 *
 * Writes convert the value to the type, and with `store_barrier` pass it
 * through the same empty asm: storing a masked read (cpu_set_X(cpu_X() &
 * 0xFF)) fails the same way without it. nz goes without: it takes 32-bit
 * values, never a narrowed one, and the barrier cost about 0.6% of
 * generated code. */
#define CPU_REG_IN_FPU(name, type, slot, max, store_barrier)                \
  register uint32_t g_fpu_cpu_##name __asm__(slot);                         \
  static inline __attribute__((always_inline)) type cpu_##name(void) {      \
    uint32_t v = g_fpu_cpu_##name;                                          \
    __asm__("" : "+r"(v));                                                  \
    if (v > (max)) __builtin_unreachable();                                 \
    return (type)v;                                                         \
  }                                                                         \
  static inline __attribute__((always_inline)) void cpu_set_##name(uint32_t v) { \
    uint32_t t = (type)v;                                                   \
    if (store_barrier) __asm__("" : "+r"(t));                               \
    g_fpu_cpu_##name = t;                                                   \
  }
CPU_REG_IN_FPU(A, uint16, FPU_CPU_A, 0xFFFFu, 1)
CPU_REG_IN_FPU(X, uint16, FPU_CPU_X, 0xFFFFu, 1)
CPU_REG_IN_FPU(Y, uint16, FPU_CPU_Y, 0xFFFFu, 1)
CPU_REG_IN_FPU(S, uint16, FPU_CPU_S, 0xFFFFu, 1)
CPU_REG_IN_FPU(D, uint16, FPU_CPU_D, 0xFFFFu, 1)
CPU_REG_IN_FPU(DB, uint8, FPU_CPU_DB, 0xFFu, 1)
CPU_REG_IN_FPU(PB, uint8, FPU_CPU_PB, 0xFFu, 1)
CPU_REG_IN_FPU(P_mxid, uint8, FPU_CPU_P_MXID, 0x3Cu, 1)
CPU_REG_IN_FPU(nz, uint32_t, FPU_CPU_NZ, 0x1FFFFu, 0)
CPU_REG_IN_FPU(flag_C, uint8, FPU_CPU_C, 1u, 1)
CPU_REG_IN_FPU(flag_V, uint8, FPU_CPU_V, 1u, 1)
#else
CPU_REG_IN_STRUCT(A, uint16)
CPU_REG_IN_STRUCT(X, uint16)
CPU_REG_IN_STRUCT(Y, uint16)
CPU_REG_IN_STRUCT(S, uint16)
CPU_REG_IN_STRUCT(D, uint16)
CPU_REG_IN_STRUCT(DB, uint8)
CPU_REG_IN_STRUCT(PB, uint8)
CPU_REG_IN_STRUCT(P_mxid, uint8)
CPU_REG_IN_STRUCT(nz, uint32_t)
CPU_REG_IN_STRUCT(flag_C, uint8)
CPU_REG_IN_STRUCT(flag_V, uint8)
#endif
#if SNESRECOMP_CPU_REGS_FPU
CPU_REG_IN_FPU(emulation, uint8, FPU_CPU_E, 0xFFu, 1)
#undef CPU_REG_IN_FPU
#else
CPU_REG_IN_STRUCT(emulation, uint8)
#endif
#undef CPU_REG_IN_STRUCT

#define CPU_REG_INLINE static inline __attribute__((always_inline))

/* M, X, I and D: bits of P_mxid. cpu_mx() is the (m << 1) | x index of a
 * function's variants. */
CPU_REG_INLINE uint8 cpu_m_flag(void) { return (uint8)((cpu_P_mxid() >> 5) & 1u); }
CPU_REG_INLINE uint8 cpu_x_flag(void) { return (uint8)((cpu_P_mxid() >> 4) & 1u); }
CPU_REG_INLINE uint8 cpu_flag_D(void) { return (uint8)((cpu_P_mxid() >> 3) & 1u); }
CPU_REG_INLINE uint8 cpu_flag_I(void) { return (uint8)((cpu_P_mxid() >> 2) & 1u); }
CPU_REG_INLINE uint32_t cpu_mx(void) { return (cpu_P_mxid() >> 4) & 3u; }
#define CPU_P_MXID_BIT(name, bit)                                           \
  CPU_REG_INLINE void cpu_set_##name(uint32_t v) {                          \
    cpu_set_P_mxid((cpu_P_mxid() & ~(bit)) | ((uint8)v ? (bit) : 0u));     \
  }
CPU_P_MXID_BIT(m_flag, 0x20u)
CPU_P_MXID_BIT(x_flag, 0x10u)
CPU_P_MXID_BIT(flag_D, 0x08u)
CPU_P_MXID_BIT(flag_I, 0x04u)
#undef CPU_P_MXID_BIT

/* N and Z come from nz, the last result: a 16-bit result as it is, an 8-bit
 * one shifted to the top byte, so that
 *   Z = the low 16 bits are 0      N = nz >= 0x8000.
 * Setting N and Z on their own (PLP, BIT, TSB) stores
 * (N ? 0x10000 : 0) | (Z ? 0 : 1), which both tests read back. */
CPU_REG_INLINE void cpu_set_nz8(uint32_t v) { cpu_set_nz((uint32_t)(uint8)v << 8); }
CPU_REG_INLINE void cpu_set_nz16(uint32_t v) { cpu_set_nz((uint16)v); }
CPU_REG_INLINE uint8 cpu_flag_N(void) { return cpu_nz() >= 0x8000u; }
CPU_REG_INLINE uint8 cpu_flag_Z(void) { return (cpu_nz() & 0xFFFFu) == 0; }
CPU_REG_INLINE void cpu_set_flags_nz(uint32_t n, uint32_t z) {
  cpu_set_nz(((uint8)n ? 0x10000u : 0u) | ((uint8)z ? 0u : 1u));
}
CPU_REG_INLINE void cpu_set_flag_N(uint32_t v) { cpu_set_flags_nz(v, cpu_flag_Z()); }
CPU_REG_INLINE void cpu_set_flag_Z(uint32_t v) { cpu_set_flags_nz(cpu_flag_N(), v); }

/* The whole status byte. Writing it with X set clears the index registers'
 * high bytes, as the hardware's 8-bit index registers have none. */
CPU_REG_INLINE uint8 cpu_P(void) {
  return (uint8)(cpu_P_mxid() | ((uint32_t)cpu_flag_N() << 7) |
                 ((uint32_t)cpu_flag_V() << 6) | ((uint32_t)cpu_flag_Z() << 1) |
                 cpu_flag_C());
}
CPU_REG_INLINE void cpu_set_P(uint32_t v) {
  cpu_set_P_mxid(v & 0x3Cu);
  cpu_set_flags_nz(v & 0x80u, v & 0x02u);
  cpu_set_flag_V((v >> 6) & 1u);
  cpu_set_flag_C(v & 1u);
  if (v & 0x10u) { cpu_set_X(cpu_X() & 0xFFu); cpu_set_Y(cpu_Y() & 0xFFu); }
}

/* The master clocks: plain 64-bit lvalues (no narrowing), so code uses them
 * as variables. CPUFPU=1: GCC global register variables in $f26/$f27. */
#if SNESRECOMP_CPU_REGS_FPU
register uint64_t g_fpu_master_cycles __asm__(FPU_MASTER_CYCLES);
register uint64_t g_fpu_coprocessor_master_cycles __asm__(FPU_COPROCESSOR_MASTER_CYCLES);
#define g_master_cycles g_fpu_master_cycles
#define g_coprocessor_master_cycles g_fpu_coprocessor_master_cycles
#else
#define g_master_cycles (g_cpu.reg_master_cycles)
#define g_coprocessor_master_cycles (g_cpu.reg_coprocessor_master_cycles)
#endif

/* Copy the CPU state from its homes into c's reg_* fields, and back. */
static inline void cpu_regs_store(struct CpuState *c) {
  c->reg_A = cpu_A(); c->reg_X = cpu_X(); c->reg_Y = cpu_Y();
  c->reg_S = cpu_S(); c->reg_D = cpu_D();
  c->reg_DB = cpu_DB(); c->reg_PB = cpu_PB(); c->reg_P = cpu_P();
  c->reg_nz = cpu_nz();
  c->reg_emulation = cpu_emulation();
  c->reg_open_bus = g_open_bus;
  c->reg_master_cycles = g_master_cycles;
  c->reg_coprocessor_master_cycles = g_coprocessor_master_cycles;
}
static inline void cpu_regs_load(const struct CpuState *c) {
  cpu_set_A(c->reg_A); cpu_set_X(c->reg_X); cpu_set_Y(c->reg_Y);
  cpu_set_S(c->reg_S); cpu_set_D(c->reg_D);
  cpu_set_DB(c->reg_DB); cpu_set_PB(c->reg_PB);
  cpu_set_P_mxid(c->reg_P & 0x3Cu);
  cpu_set_flag_V((c->reg_P >> 6) & 1u); cpu_set_flag_C(c->reg_P & 1u);
  cpu_set_nz(c->reg_nz);
  cpu_set_emulation(c->reg_emulation);
  g_open_bus = c->reg_open_bus;
  g_master_cycles = c->reg_master_cycles;
  g_coprocessor_master_cycles = c->reg_coprocessor_master_cycles;
}

#endif
