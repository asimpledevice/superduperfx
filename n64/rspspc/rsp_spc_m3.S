# SPC700 on the RSP, paged module 3: TCALL n, BRK, PCALL, NOTC, CLRV. See
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

m3:
    li t0, 0xed
    beq t7, t0, notc
    li t0, 0xe0
    beq t7, t0, clrv
    li t0, 0x4f
    beq t7, t0, pcall
    lbu t0, %lo(C_ST + SPCST_ROMREADABLE)
    bnez t0, trap                   # tcall/brk vectors in the IPL ROM: the CPU
    srl a1, s6, 8                   # push PC (high, low)
    jal C_push
    nop
    jal C_push
    andi a1, s6, 0xff
    li a2, 0xffde                   # brk: $FFDE; tcall n: $FFDE - 2n
    li t0, 0x0f
    bne t7, t0, 1f
    srl t1, t7, 3
    jal C_push                      # push PSW; B set, I clear
    move a1, s7
    andi s7, s7, 0xfb
    ori s7, s7, 0x10
    b 2f
    nop
1:  subu a2, a2, t1                 # (opcode >> 4) * 2
2:  addiu a3, a2, 1
    jal C_m_ptrw
    nop
    j C_loop
    move s6, t5
pcall:                              # push PC, PC = $FF00 | imm
    jal C_fetch
    nop
    move t3, v0                     # (t3-t7 survive rd/wr)
    srl a1, s6, 8
    jal C_push
    nop
    jal C_push
    andi a1, s6, 0xff
    j C_loop
    ori s6, t3, 0xff00
notc:
    j C_loop
    xori s7, s7, 1
clrv:                               # V and H
    j C_loop
    andi s7, s7, 0xb7
trap:
    j C_OP_TRAP
    nop
