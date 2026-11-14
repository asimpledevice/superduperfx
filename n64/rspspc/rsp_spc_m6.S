# SPC700 on the RSP, paged module 6: DIV, on the vector unit (see
# BRINGUP_NOTES.md "borrowed IMEM and DIV on the vector unit"). See
# rsp_spc_m0.S for paged modules.
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

