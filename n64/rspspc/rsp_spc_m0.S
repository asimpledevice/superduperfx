# SPC700 on the RSP, paged module 0: operations sound drivers use but that do
# not fit the core: DBNZ Y, DBNZ dp, ROL, ROR (memory), ROLA, RETI.
#
# Paged modules (spc_rsp_layout.h EXT_SLOT): each is linked at the slot in
# the borrowed IMEM and DMA'd there when an operation needs it (x_page_id in
# rsp_spc_ext.S); they call the core (C_*) and the resident borrowed code
# (X_*) by absolute address. An operation enters at the slot with its
# addressing mode done (a2 the address) and the opcode in t7, as the core's
# handlers do, and ends like them (C_loop, C_zn_loop, C_branch, ...).
#
# Two rules, because a trap restores only PC, cycles, SP and PSW and the C
# core then runs the whole instruction: every check that can trap comes
# before the instruction's first write, and register changes come after its
# last access that can trap. A write to $00F1 would page module 7 over the
# module making it, so modules leave those to the CPU (checked first).
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

m0:
    li t0, 0xfe
    beq t7, t0, dbnzy
    li t0, 0x3c
    beq t7, t0, rola
    li t0, 0x7f
    beq t7, t0, reti
    li t0, 0xf1                     # memory forms write a2: $00F1 would page in
    beq a2, t0, trap                # module 7 over this one; left to the CPU
    li t0, 0x6e
    beq t7, t0, dbnz
    andi t0, t7, 0x40               # rol: $2B $2C $3B; ror: $6B $6C $7B
    bnez t0, ror_m
    nop
    jal C_rd                        # rol [a2]
    move a0, a2
    move t5, zero
rol_c:                              # v0 -> a1 = v0 << 1 | C, C = bit 7
    srl t0, v0, 7
    sll a1, v0, 1
    andi t1, s7, 1
    or a1, a1, t1
    j C_rmw_cc
    andi a1, a1, 0xff
rola:
    move v0, s1
    b rol_c
    li t5, 1
ror_m:
    jal C_rd                        # ror [a2]
    move a0, a2
    j C_ror_c
    move t5, zero
dbnzy:                              # Y--, branch if not 0
    addiu s3, s3, -1
    andi s3, s3, 0xff
    j C_branch
    move t3, s3
dbnz:                               # [a2]--, branch if not 0
    srl t1, s6, 6                   # the offset in the IPL ROM: its fetch would
    xori t1, t1, 0x3ff              # trap after the write, so the CPU runs it
    bnez t1, 1f
    lbu t0, %lo(C_ST + SPCST_ROMREADABLE)
    bnez t0, trap
1:  jal C_rd
    move a0, a2
    addiu a1, v0, -1
    andi a1, a1, 0xff
    jal C_wr                        # (keeps t3)
    move t3, a1
    j C_branch
    move a0, a2
reti:                               # PSW, then PC from the stack
    jal C_pull
    nop
    move s7, v0
    jal C_pull
    andi k1, s7, 0x20               # direct page
    sll k1, k1, 3
    jal C_pull
    move t5, v0
    sll v0, v0, 8
    j C_loop
    or s6, t5, v0
trap:
    j C_OP_TRAP
    nop
