/* The 65816 registers of the main CPU's interpreter core
 * (interp816_main.c) and of the bridge that drives it: accessors onto the
 * reserved FPU registers compiled code uses (cpu_regs.h). Included only with
 * SNESRECOMP_INTERP_REGS=1. */
#ifndef INTERP816_REGS_H
#define INTERP816_REGS_H
#include "cpu_state.h"
#if !SNESRECOMP_CPU_REGS_FPU
#error "SNESRECOMP_INTERP_REGS needs SNESRECOMP_CPU_REGS_FPU=1"
#endif
#define I816_GET(f, expr) static inline __attribute__((always_inline)) uint32_t i816r_##f(void) { return (expr); }
I816_GET(a, cpu_A())   I816_GET(x, cpu_X())   I816_GET(y, cpu_Y())
I816_GET(sp, cpu_S())  I816_GET(dp, cpu_D())  I816_GET(k, cpu_PB())  I816_GET(db, cpu_DB())
I816_GET(c, cpu_flag_C())  I816_GET(z, cpu_flag_Z())  I816_GET(v, cpu_flag_V())  I816_GET(n, cpu_flag_N())
I816_GET(i, cpu_flag_I())  I816_GET(d, cpu_flag_D())
I816_GET(xf, cpu_x_flag()) I816_GET(mf, cpu_m_flag()) I816_GET(e, cpu_emulation())
#undef I816_GET
/* Writes convert as assignment to the struct field did: the registers to
 * their width (cpu_set_* does that), the flags to 0 or 1. */
#define I816_SET(f, type, stmt) static inline __attribute__((always_inline)) void i816w_##f(type val) { stmt; }
I816_SET(a, uint16_t, cpu_set_A(val))   I816_SET(x, uint16_t, cpu_set_X(val))   I816_SET(y, uint16_t, cpu_set_Y(val))
I816_SET(sp, uint16_t, cpu_set_S(val))  I816_SET(dp, uint16_t, cpu_set_D(val))
I816_SET(k, uint8_t, cpu_set_PB(val))   I816_SET(db, uint8_t, cpu_set_DB(val))
I816_SET(c, bool, cpu_set_flag_C(val))  I816_SET(z, bool, cpu_set_flag_Z(val))
I816_SET(v, bool, cpu_set_flag_V(val))  I816_SET(n, bool, cpu_set_flag_N(val))
I816_SET(i, bool, cpu_set_flag_I(val))  I816_SET(d, bool, cpu_set_flag_D(val))
I816_SET(xf, bool, cpu_set_x_flag(val)) I816_SET(mf, bool, cpu_set_m_flag(val))
I816_SET(e, bool, cpu_set_emulation(val))
#undef I816_SET
#define I816_R(f) i816r_##f()
#define I816_W(f, v) i816w_##f(v)
#endif
