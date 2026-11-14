# SPC700 on the RSP, paged module 8: $F1 writes (timer enables, port
# clears, the IPL ROM switch). See rsp_spc_m0.S for paged modules.
#include <rsp.inc>
#include "spc_rsp_layout.h"
#include "rsp_spc_syms.inc"
#include "rsp_spc_ext_syms.inc"

    .set noreorder
    .set at

    .data
    .ds.b 8                         # (the ucode build wants a data segment; unused)

    .text
    .org EXT_SLOT                   # linked where it runs; only the slot is copied

x_f1:                               # $F1: timer enables, port clears, IPL ROM
    move t8, ra
    jal X_x_tsync_now
    nop
    move ra, t8
    li s4, %lo(C_ST + SPCST_TIMERS)
    li t2, 1
7:  lbu t0, 4(s4)                   # enabled before
    and t1, a1, t2
    sltu t1, zero, t1
    bnez t0, 8f
    sb t1, 4(s4)
    beqz t1, 8f
    nop
    sb zero, 1(s4)                  # newly enabled: divider and counter reset
    sb zero, 3(s4)
8:  addiu s4, s4, 8
    sll t2, t2, 1
    li t0, 8
    bne t2, t0, 7b
    nop
    andi t0, a1, 0x10
    beqz t0, 9f
    andi t0, a1, 0x20
    sh zero, %lo(C_ST + SPCST_INPORTS)
9:  beqz t0, 9f
    srl t0, a1, 7
    sh zero, %lo(C_ST + SPCST_INPORTS + 2)
9:  j C_wr_zp
    sb t0, %lo(C_ST + SPCST_ROMREADABLE)

# Timers, as apu_native_timer: bring all three to cycle t2 (tsync) or to the
# start of the instruction in progress (tsync_now). Uses t0-t2, t9, v0, s0,
# s4, k0.
