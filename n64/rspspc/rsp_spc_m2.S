# SPC700 on the RSP, paged module 2: OR/AND/EOR/ADC/SBC on memory with two
# operands (dp,dp  dp,#imm  (X),(Y)), MOV dp,dp, MOV A,(X)+, MOV (X)+,A, XCN.
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

m2:
    li t0, 0x9f
    beq t7, t0, xcn
    li t0, 0xbf
    beq t7, t0, movindp
    li t0, 0xaf
    beq t7, t0, movsindp
    li t0, 0xf1
    beq a2, t0, trap                # the rest write a2 (the destination)
    li t0, 0xfa
    beq t7, t0, movmd
    nop
    jal C_m2_rd                     # t5 = [a3] (source), v0 = [a2]
    nop
    srl t0, t7, 5                   # 0 or 1 and 2 eor 4 adc 5 sbc
    beqz t0, 1f
    or a1, v0, t5
    addiu t0, t0, -1
    beqz t0, 1f
    and a1, v0, t5
    addiu t0, t0, -1
    beqz t0, 1f
    xor a1, v0, t5
    addiu t0, t0, -2                # adc ($8x: 4) or sbc ($Ax: 5)
    beqz t0, 2f
    move t3, v0
    xori t5, t5, 0xff
2:  move t7, t4                     # adc8 uses t4: the instruction's cycles, which
    jal C_adc8                      # the write's log entry needs (t3 + t5 + C, V H C)
    move v0, t5
    move t4, t7
    move a1, v0
1:  jal C_wr
    move a0, a2
    j C_zn_loop
    move v0, a1
movmd:                              # [a2] = [a3]
    jal C_rd
    move a0, a3
    move a1, v0
    li ra, %lo(C_loop)
    j C_wr
    move a0, a2
movindp:                            # A = [X | dp], X++ (after the read: it can trap)
    jal C_rd
    or a0, s2, k1
    addiu s2, s2, 1
    andi s2, s2, 0xff
    j C_zn_loop
    move s1, v0
movsindp:                           # [X | dp] = A, X++ (after the write)
    or a0, s2, k1
    li t0, 0xf1
    beq a0, t0, trap
    move a1, s1
    jal C_wr
    nop
    addiu s2, s2, 1
    j C_loop
    andi s2, s2, 0xff
xcn:                                # A's nibbles swapped
    srl t0, s1, 4
    sll s1, s1, 4
    or s1, s1, t0
    andi s1, s1, 0xff
    j C_zn_loop
    move v0, s1
trap:
    j C_OP_TRAP
    nop
