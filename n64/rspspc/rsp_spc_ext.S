# SPC700 on the RSP: the code that runs in the borrowed IMEM.
#
# rsp_spc.S loads this over rspq's code (IMEM 0x000 up to the overlay) at
# the start of a run, saving rspq's bytes, and puts them back before it
# returns to rspq, which runs none of its code during a command. Linked at
# IMEM 0 like any ucode; calls into the core through C_* (gen_syms.py), and
# the core enters through the table below. Registers as in rsp_spc.S.

#include <rsp.inc>
#include "spc_rsp_layout.h"
#include "rsp_spc_syms.inc"

    .set noreorder
    .set at

    .data
    .ds.b 8                         # (the ucode build wants a data segment; unused)

    .text
ext_begin:
    j x_tsync_now                   # 0x00 (EXT_TSYNC_NOW in rsp_spc.S)
    nop
    j x_tsync                       # 0x08
    nop
    j x_f1                          # 0x10
    nop
    j x_div                         # 0x18
    nop
    j x_mul                         # 0x20
    nop

x_f1:                               # $F1: timer enables, port clears, IPL ROM
    move t8, ra
    jal x_tsync_now
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
x_tsync_now:
    lw t2, %lo(C_END)
    subu t2, t2, fp
    subu t2, t2, t4                 # fp was charged this instruction's cycles
x_tsync:
    lw t0, %lo(C_ST + SPCST_TSYNC)
    subu s0, t2, t0                 # clocks
    beqz s0, 9f
    sw t2, %lo(C_ST + SPCST_TSYNC)
    li s4, %lo(C_ST + SPCST_TIMERS)
tm_one:
    lbu k0, 5(s4)                   # prescaler period shift (7, 7, 4; spc_rsp_run.c)
    lbu t0, 0(s4)                   # prescaler
    sltu t1, t0, s0
    bnez t1, 1f
    subu t2, t0, s0
    b tm_next
    sb t2, 0(s4)
1:  subu t1, s0, t0                 # rest
    addiu t2, t1, -1
    srlv t2, t2, k0
    addiu t2, t2, 1                 # ticks
    sllv t0, t2, k0
    subu t0, t0, t1
    sb t0, 0(s4)
    lbu t0, 4(s4)
    beqz t0, tm_next                # disabled: only the prescaler runs
    lbu t1, 2(s4)                   # target
    lbu t0, 1(s4)                   # divider
    subu t9, t1, t0
    addiu t9, t9, -1
    andi t9, t9, 0xff
    addiu t9, t9, 1                 # first
    sltu v0, t2, t9
    beqz v0, 2f
    addu t0, t0, t2
    b tm_next
    sb t0, 1(s4)                    # divider += ticks
2:  subu t2, t2, t9                 # after
    bnez t1, 3f
    sll t1, t1, 16
    li t1, 0x1000000                # target 0: every 256
3:  li v0, 16                       # after / every, after % every
4:  sll t2, t2, 1
    sltu t0, t2, t1
    bnez t0, 5f
    addiu v0, v0, -1
    subu t2, t2, t1
    ori t2, t2, 1
5:  bnez v0, 4b
    nop
    srl t0, t2, 16
    sb t0, 1(s4)                    # divider = after % every
    andi t2, t2, 0xffff
    lbu t0, 3(s4)
    addu t0, t0, t2
    addiu t0, t0, 1                 # counter += 1 + after / every
    andi t0, t0, 15
    sb t0, 3(s4)
tm_next:
    addiu s4, s4, 8
    li t0, %lo(C_ST + SPCST_TIMERS) + 24
    bne s4, t0, tm_one
    nop
9:  jr ra
    nop

# DIV: a = ya / x, y = ya % x, as spc.c, the quotient on the vector unit.
# VRCP gives R ~ 2^31 / x; R16 = R >> 15; q = (ya * R16) >> 16 (VMUDL) is the
# quotient or one less (checked for every ya and x: n64/rspspc/vdiv_model.c),
# so one remainder check (VMUDN gives the low 16 bits of q * x) finishes it.
x_div:
    andi t3, s7, 0xb7               # V and H recomputed
    andi t0, s2, 0xf
    andi t1, s3, 0xf
    sltu t0, t1, t0                 # (y & f) < (x & f)
    xori t0, t0, 1
    sll t0, t0, 3                   # H = (x & f) <= (y & f)
    or t3, t3, t0
    sll t2, s3, 8
    or t2, t2, s1                   # ya
    bnez s2, 1f
    nop
    li t4, 0xffff                   # x = 0: quotient $FFFF, remainder a
    b 3f
    move t5, s1
1:  mtc2 s2, $v02.e0
    vrcp $v03.e0, $v02.e0           # R, low half
    vrcph $v04.e0, $v02.e0          # R, high half
    mtc2 t2, $v06.e0
    mfc2 t0, $v03.e0
    mfc2 t1, $v04.e0
    srl t0, t0, 15
    andi t0, t0, 1
    sll t1, t1, 1
    or t0, t0, t1                   # R16 = R >> 15
    mtc2 t0, $v05.e0
    vmudl $v07, $v06, $v05, e(0)    # q = (ya * R16) >> 16
    vmudn $v08, $v07, $v02, e(0)    # q * x, low 16 bits
    mfc2 t4, $v07.e0
    mfc2 t1, $v08.e0
    andi t4, t4, 0xffff
    subu t5, t2, t1
    andi t5, t5, 0xffff             # remainder (or remainder + x)
    sltu t0, t5, s2
    bnez t0, 3f
    nop
    addiu t4, t4, 1
    subu t5, t5, s2
3:  sltiu t0, t4, 0x100
    xori t0, t0, 1
    sll t0, t0, 6                   # V = quotient > $FF
    or s7, t3, t0
    andi s1, t4, 0xff
    andi s3, t5, 0xff
    j C_zn_loop
    move v0, s1

x_mul:                              # ya = y * a, on the vector unit
    mtc2 s1, $v01.e0
    mtc2 s3, $v02.e0
    vmudn $v03, $v01, $v02, e(0)    # low 16 bits of the product, unclamped
    mfc2 t0, $v03.e0
    andi t0, t0, 0xffff
    andi s1, t0, 0xff
    srl s3, t0, 8
    j C_zn_loop
    move v0, s3
