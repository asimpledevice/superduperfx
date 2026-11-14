# SPC700 on the RSP, paged module 4: the carry-bit operations on abs.bit (OR1, OR1 /, AND1, AND1 /, EOR1, MOV1 C,m.b; MOV1 m.b,C; NOT1).
# See rsp_spc_m0.S for paged modules.
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

m4:
    jal C_fetchw                    # abs.bit
    nop
    srl t6, v0, 13                  # bit (t3-t7 survive rd/wr)
    andi a2, v0, 0x1fff
    jal C_rd                        # (reads before any check: as spc.c)
    move a0, a2
    srlv t3, v0, t6
    andi t3, t3, 1                  # the bit
    li t0, 0xca
    beq t7, t0, mov1s
    li t0, 0xea
    beq t7, t0, not1
    srl t0, t7, 5                   # 0 or1 1 or1n 2 and1 3 and1n 4 eor1 5 mov1
    andi t1, t0, 1
    sltiu t2, t0, 4
    and t1, t1, t2
    xor t3, t3, t1                  # or1n, and1n: the inverted bit
    andi t2, s7, 1
    sltiu t1, t0, 2
    bnez t1, 1f
    or t4, t2, t3                   # or
    sltiu t1, t0, 4
    bnez t1, 1f
    and t4, t2, t3                  # and
    li t1, 4
    beq t0, t1, 1f
    xor t4, t2, t3                  # eor
    move t4, t3                     # mov1
1:  andi s7, s7, 0xfe
    j C_loop
    or s7, s7, t4
mov1s:                              # bit = C
    li t0, 0xf1
    beq a2, t0, trap
    li t1, 1
    sllv t1, t1, t6
    nor t2, t1, zero
    and a1, v0, t2
    andi t0, s7, 1
    sllv t0, t0, t6
    b wr_loop
    or a1, a1, t0
not1:
    li t0, 0xf1
    beq a2, t0, trap
    li t1, 1
    sllv t1, t1, t6
    xor a1, v0, t1
wr_loop:
    li ra, %lo(C_loop)
    j C_wr
    move a0, a2
trap:
    j C_OP_TRAP
    nop
