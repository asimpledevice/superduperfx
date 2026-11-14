# SPC700 on the RSP, paged module 1: DECW, INCW, CMPW (dp words), JMP [!abs+X],
# MOV SP,X, SLEEP/STOP. See rsp_spc_m0.S for paged modules.
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

m1:
    li t0, 0x1f
    beq t7, t0, jmpiax
    li t0, 0xbd
    beq t7, t0, movpx
    andi t0, t7, 0xef               # $EF sleep, $FF stop
    li t1, 0xef
    beq t0, t1, sleep
    andi t0, a2, 0xfff0             # a word on the I/O page ($00F0-$00FF): the
    xori t0, t0, 0xf0               # CPU (decw/incw write two bytes; a trap at
    beqz t0, trap                   # the second, e.g. $F3, would re-run the first)
    nop
    jal C_rdw_dp                    # t5 = word at a2 (low), a3 (high)
    nop
    li t0, 0x5a
    beq t7, t0, cmpw
    li t0, 0x3a
    beq t7, t0, 1f
    addiu t1, t5, 1                 # incw
    addiu t1, t5, -1                # decw
1:  andi t5, t1, 0xffff
    andi a1, t5, 0xff
    jal C_wr                        # low byte, then high (keeps t5)
    move a0, a2
    srl a1, t5, 8
    jal C_wr
    move a0, a3
    li ra, %lo(C_loop)
    j C_zn16                        # Z and N from the word
    nop
cmpw:                               # ya - word: C, Z, N
    sll t3, s3, 8
    or t3, t3, s1
    xori t5, t5, 0xffff
    addu t4, t3, t5
    addiu t4, t4, 1
    srl t0, t4, 16
    andi s7, s7, 0xfe
    or s7, s7, t0
    andi t5, t4, 0xffff
    li ra, %lo(C_loop)
    j C_zn16
    nop
jmpiax:                             # PC = word at (abs + X), (abs + X + 1)
    jal C_fetchw
    nop
    addu a2, v0, s2
    andi a2, a2, 0xffff
    addiu a3, a2, 1
    jal C_m_ptrw                    # t5 = [a2] | [a3] << 8
    andi a3, a3, 0xffff
    j C_loop
    move s6, t5
movpx:
    j C_loop
    move s5, s2
sleep:                              # stopped: the rest of the budget goes as
    li t0, 1                        # single-cycle steps (as at a run's start)
    blez fp, 1f
    sb t0, %lo(C_ST + SPCST_STOPPED)
    move fp, zero
1:  j C_loop
    nop
trap:
    j C_OP_TRAP
    nop
