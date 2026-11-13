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
 * SNESRECOMP_CPU_REGS_FPU=1 (n64 CPUFPU=1): A, X, Y, S, D, DB, PB and P live
 * in reserved FPU registers (fpu_slots.h) as GCC global register variables,
 * always zero-extended, so a read is one MFC1 and a write one MTC1 (plus the
 * conversion the C type asks for). Otherwise they live in g_cpu's reg_*
 * fields. E is in an FPU register too (CPUFPU=1); m and x are P's bits.
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
 * cannot see through the barrier everywhere. Every write stores a value
 * already converted to the type. The `barrier` argument is kept so a
 * register can go without it once a compiler handles that. */
#define CPU_REG_IN_FPU(name, type, slot, max, barrier)                              \
  register uint32_t g_fpu_cpu_##name __asm__(slot);                         \
  static inline __attribute__((always_inline)) type cpu_##name(void) {      \
    uint32_t v = g_fpu_cpu_##name;                                          \
    if (barrier) __asm__("" : "+r"(v));                                     \
    if (v > (max)) __builtin_unreachable();                                 \
    return (type)v;                                                         \
  }                                                                         \
  static inline __attribute__((always_inline)) void cpu_set_##name(uint32_t v) { \
    g_fpu_cpu_##name = (type)v;                                             \
  }
CPU_REG_IN_FPU(A, uint16, FPU_CPU_A, 0xFFFFu, 1)
CPU_REG_IN_FPU(X, uint16, FPU_CPU_X, 0xFFFFu, 1)
CPU_REG_IN_FPU(Y, uint16, FPU_CPU_Y, 0xFFFFu, 1)
CPU_REG_IN_FPU(S, uint16, FPU_CPU_S, 0xFFFFu, 1)
CPU_REG_IN_FPU(D, uint16, FPU_CPU_D, 0xFFFFu, 1)
CPU_REG_IN_FPU(DB, uint8, FPU_CPU_DB, 0xFFu, 1)
CPU_REG_IN_FPU(PB, uint8, FPU_CPU_PB, 0xFFu, 1)
CPU_REG_IN_FPU(P, uint8, FPU_CPU_P, 0xFFu, 1)
#else
CPU_REG_IN_STRUCT(A, uint16)
CPU_REG_IN_STRUCT(X, uint16)
CPU_REG_IN_STRUCT(Y, uint16)
CPU_REG_IN_STRUCT(S, uint16)
CPU_REG_IN_STRUCT(D, uint16)
CPU_REG_IN_STRUCT(DB, uint8)
CPU_REG_IN_STRUCT(PB, uint8)
CPU_REG_IN_STRUCT(P, uint8)
#endif
#if SNESRECOMP_CPU_REGS_FPU
CPU_REG_IN_FPU(emulation, uint8, FPU_CPU_E, 0xFFu, 1)
#undef CPU_REG_IN_FPU
#else
CPU_REG_IN_STRUCT(emulation, uint8)
#endif
#undef CPU_REG_IN_STRUCT

/* M and X are P's bits 5 and 4: every writer of P keeps them (REP/SEP, PLP,
 * RTI, XCE, interrupt entry, the interpreter's sync), so they have no copy of
 * their own. cpu_mx() is the (m << 1) | x index of a function's variants. */
static inline __attribute__((always_inline)) uint8 cpu_m_flag(void) {
  return (uint8)((cpu_P() >> 5) & 1u);
}
static inline __attribute__((always_inline)) uint8 cpu_x_flag(void) {
  return (uint8)((cpu_P() >> 4) & 1u);
}
static inline __attribute__((always_inline)) uint32_t cpu_mx(void) {
  return (cpu_P() >> 4) & 3u;
}
static inline __attribute__((always_inline)) void cpu_set_m_flag(uint32_t v) {
  cpu_set_P((uint8)((cpu_P() & ~0x20u) | ((uint8)v ? 0x20u : 0u)));
}
static inline __attribute__((always_inline)) void cpu_set_x_flag(uint32_t v) {
  cpu_set_P((uint8)((cpu_P() & ~0x10u) | ((uint8)v ? 0x10u : 0u)));
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
  c->reg_emulation = cpu_emulation();
  c->reg_open_bus = g_open_bus;
  c->reg_master_cycles = g_master_cycles;
  c->reg_coprocessor_master_cycles = g_coprocessor_master_cycles;
}
static inline void cpu_regs_load(const struct CpuState *c) {
  cpu_set_A(c->reg_A); cpu_set_X(c->reg_X); cpu_set_Y(c->reg_Y);
  cpu_set_S(c->reg_S); cpu_set_D(c->reg_D);
  cpu_set_DB(c->reg_DB); cpu_set_PB(c->reg_PB); cpu_set_P(c->reg_P);
  cpu_set_emulation(c->reg_emulation);
  g_open_bus = c->reg_open_bus;
  g_master_cycles = c->reg_master_cycles;
  g_coprocessor_master_cycles = c->reg_coprocessor_master_cycles;
}

#endif
