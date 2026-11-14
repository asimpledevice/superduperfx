# SPC700 on the RSP, paged module 7: DAA, DAS, TSET1, TCLR1. See rsp_spc_m0.S
# for paged modules.
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

m7:
    andi t0, t7, 0xb0               # $0E tset1, $4E tclr1 (0); $BE das, $DF daa
    bnez t0, daa
    nop
    jal C_fetchw                    # abs
    nop
    move a2, v0
    jal C_rd
    move a0, a2
    li t0, 0xf1
    beq a2, t0, trap
    xori t5, v0, 0xff
    addu t5, s1, t5
    addiu t5, t5, 1
    andi t5, t5, 0xff               # Z/N later (t5 survives the write)
    or a1, v0, s1                   # tset1
    li t0, 0x4e
    bne t7, t0, 1f
    nor t0, s1, zero
    and a1, v0, t0                  # tclr1
1:  jal C_wr
    move a0, a2
    j C_zn_loop
    move v0, t5
daa:                                # DAA ($DF), DAS ($BE, d = 1): if A > $99 or
    andi t2, t7, 1                  # C ^ d: A += +-$60, C = !d; then if (A & $F) > 9
    xori t2, t2, 1                  # or H ^ d: A += +-6. Z/N from A. d: $BE bit 0 clear
    subu t3, zero, t2               # 0 or -1: (x ^ t3) - t3 negates x when d
    sltiu t0, s1, 0x9a
    xori t0, t0, 1                  # A > $99
    andi t1, s7, 1
    xor t1, t1, t2                  # C ^ d
    or t0, t0, t1
    beqz t0, 1f
    li t1, 0x60
    xor t1, t1, t3
    subu t1, t1, t3
    addu s1, s1, t1
    andi s1, s1, 0xff
    andi s7, s7, 0xfe
    xori t1, t2, 1
    or s7, s7, t1                   # C = !d
1:  andi t0, s1, 0xf
    sltiu t0, t0, 10
    xori t0, t0, 1                  # (A & $F) > 9
    srl t1, s7, 3
    andi t1, t1, 1
    xor t1, t1, t2                  # H ^ d
    or t0, t0, t1
    beqz t0, 9f
    li t1, 6
    xor t1, t1, t3
    subu t1, t1, t3
    addu s1, s1, t1
    andi s1, s1, 0xff
9:  j C_zn_loop
    move v0, s1
trap:
    j C_OP_TRAP
    nop
