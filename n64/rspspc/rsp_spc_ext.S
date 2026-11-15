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
    j x_page_id                     # 0x10 $F1 writes: module 8
    li t0, 8
    j x_page_id                     # 0x18 DIV (also through OP_PG6): module 6
    li t0, 6
    j x_page_id                     # 0x20 the end of a budget slice: module 9
    li t0, 9
    j x_wmark                       # 0x28
    nop
    j x_page_op                     # 0x30 an OP_PG<k> operation
    nop

# Paged modules (spc_rsp_layout.h EXT_SLOT): bring module t0 into the slot
# unless it is there, then enter it. Keeps every register but t0-t2, s0,
# s4 and $1 (ra is kept in DMEM across the load).
x_page_op:                          # t6 = OP_PG0 + 4 k
    addiu t0, t6, -(C_OP_PG0 - C__start)
    srl t0, t0, 2
    li t1, 5                        # OP_PG5: MUL, resident
    beq t0, t1, x_mul
x_page_id:
    lbu t1, %lo(C_ST + SPCRUN_MOD)
    beq t0, t1, 1f
    sw ra, %lo(C_ST + SPCRUN_RA)
    sb t0, %lo(C_ST + SPCRUN_MOD)
    sll t1, t0, 8                   # EXT_MOD_SIZE 0x100
    lw s0, %lo(C_EXTBUF)
    addu s0, s0, t1
    addiu s0, s0, EXT_MOD_OFS
    li s4, 0x1000 + EXT_SLOT
    jal C_SDmaIn
    li t0, DMA_SIZE(EXT_MOD_SIZE, 1)
    lw ra, %lo(C_ST + SPCRUN_RA)
1:  j EXT_SLOT
    nop

x_wmark:                            # line t2 written back: mark its page
    srl s0, t2, 5                   # bitmap byte (page >> 3)
    srl t0, t2, 2
    andi t0, t0, 7
    li t1, 1
    sllv t1, t1, t0
    lbu t0, %lo(C_ST + SPCRUN_WPAGES)(s0)
    or t0, t0, t1
    jr ra
    sb t0, %lo(C_ST + SPCRUN_WPAGES)(s0)

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
