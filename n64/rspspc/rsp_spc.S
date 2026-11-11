# SPC700 on the RSP: runs the sound CPU for a cycle budget.
#
# The CPU queues SpcCmd_Run with an SpcRspState block (spc_rsp_layout.h);
# the RSP runs whole instructions until the budget is used, exactly as
# runner/src/snes/spc.c does (n64/rspspc/test compares the two on random
# states), and writes the block back. Opcodes the RSP leaves to the CPU
# (rare ones: TCALL, BRK, RETI, DAA, DAS, the carry-bit ops, ...) end the
# run before them with SPCEXIT_TRAP; the glue runs that one instruction in C.
#
# ARAM stays in RDRAM. Pages 0 and 1 (direct page 0 and the stack) are held
# in DMEM for the whole run; the rest goes through a 2 KB direct-mapped
# write-back cache of 64-byte lines. Milestone 1: the I/O page ($F0-$FF) and
# the IPL ROM are plain RAM here and in the test's reference.
#
# Registers during a run:
#   s1 A, s2 X, s3 Y, s5 SP, s6 PC, s7 PSW (N V P B H I Z C),
#   fp cycles left (budget minus used), k1 direct-page base (P ? 0x100 : 0),
#   sp instructions executed, t7 opcode, a2/a3 operand addresses.
# rd/wr (memory) clobber t0-t2 and, on a miss, t8, t9, k0, s0, s4, at.

#include <rsp_queue.inc>
#include "spc_rsp_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand SpcCmd_Run, 4
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8               # rspq needs a non-empty saved state
RSPQ_EndSavedState

#include "rsp_spc_tables.inc"

BRMASK:     .byte 0x80, 0x40, 0x01, 0x02    # bpl/bmi, bvc/bvs, bcc/bcs, bne/beq
FLMASK:     .byte 0, 0x20, 0x20, 0x01, 0x01, 0x04, 0x04, 0   # by opcode >> 5
FLSET:      .byte 0, 0,    0x20, 0,    0x01, 0x04, 0,    0

    .bss
    .align 4
ST:         .ds.b SPCST_SIZE
RET_RA:     .ds.b 4
REGS:       .ds.b 4                 # psw a x y, for pushes
    .align 4
TAGS:       .ds.b 64                # line number per slot, bit 15 dirty; 0x7fff empty
    .align 4
ZP:         .ds.b 0x200             # ARAM $0000-$01FF
    .align 4
LINES:      .ds.b 2048              # 32 slots x 64 bytes

    .text

SpcCmd_Run:
    sw ra, %lo(RET_RA)
    li t0, 0xffffff
    and s0, a0, t0                  # top byte of a0 is the command id
    move s7, s0                     # state block address, kept until done
    li s4, %lo(ST)
    jal DMAIn
    li t0, DMA_SIZE(SPCST_SIZE, 1)
    sw s7, %lo(ST + SPCST_USED)     # remember the block address (USED is output)
    lw s0, %lo(ST + SPCST_ARAM)
    li s4, %lo(ZP)
    jal DMAIn
    li t0, DMA_SIZE(0x200, 1)
    li t0, 0x7fff                   # all line slots empty
    li t1, %lo(TAGS)
1:  sh t0, 0(t1)
    addiu t1, t1, 2
    li t2, %lo(TAGS) + 64
    bne t1, t2, 1b
    nop
    lbu s1, %lo(ST + SPCST_A)
    lbu s2, %lo(ST + SPCST_X)
    lbu s3, %lo(ST + SPCST_Y)
    lbu s5, %lo(ST + SPCST_SP)
    lhu s6, %lo(ST + SPCST_PC)
    lbu s7, %lo(ST + SPCST_PSW)
    lw fp, %lo(ST + SPCST_BUDGET)
    lbu t0, %lo(ST + SPCST_STOPPED)
    beqz t0, 2f
    andi k1, s7, 0x20
    blez fp, 2f                     # stopped: each step is one cycle, so
    nop                             # a positive budget is used exactly
    move fp, zero
2:  sll k1, k1, 3
    move sp, zero
    sw zero, %lo(ST + SPCST_EXIT)

loop:
    blez fp, done
    move a0, s6
    jal rd
    addiu s6, s6, 1
    andi s6, s6, 0xffff
    sll t3, v0, 1
    lhu t3, %lo(DISPATCH)(t3)
    move t7, v0                     # opcode
    srl t4, t3, 12                  # cycles
    subu fp, fp, t4
    addiu sp, sp, 1
    srl t5, t3, 6
    andi t5, t5, 0x1e               # mode * 2
    lhu t5, %lo(MODETAB)(t5)
    andi t6, t3, 0x7f
    sll t6, t6, 1
    jr t5
    lhu t6, %lo(OPTAB)(t6)          # t6 = the operation's address
M_NONE:
    jr t6
    nop

#----------------------------------------------------------------------
# Memory. rd: a0 address -> v0 byte. wr: a0 address, a1 byte.

fetch:                              # v0 = byte at PC, PC++
    move a0, s6
    addiu s6, s6, 1
    j rd
    andi s6, s6, 0xffff

rd:
    sltiu t0, a0, 0x200
    beqz t0, rd_hi
    srl t0, a0, 6
    jr ra
    lbu v0, %lo(ZP)(a0)
rd_hi:
    andi t1, t0, 31
    sll t1, t1, 1
    lhu t2, %lo(TAGS)(t1)
    andi t2, t2, 0x7fff
    bne t2, t0, rd_miss
    sll t1, t1, 5                   # slot * 64
    andi t2, a0, 63
    addu t1, t1, t2
    jr ra
    lbu v0, %lo(LINES)(t1)
rd_miss:
    move t8, ra
    jal fill
    move t9, t0
    move ra, t8
    b rd_hi
    srl t0, a0, 6

wr:
    sltiu t0, a0, 0x200
    beqz t0, wr_hi
    srl t0, a0, 6
    jr ra
    sb a1, %lo(ZP)(a0)
wr_hi:
    andi t1, t0, 31
    sll t1, t1, 1
    lhu t2, %lo(TAGS)(t1)
    andi t9, t2, 0x7fff
    bne t9, t0, wr_miss
    ori t2, t2, 0x8000
    sh t2, %lo(TAGS)(t1)            # dirty
    sll t1, t1, 5
    andi t2, a0, 63
    addu t1, t1, t2
    jr ra
    sb a1, %lo(LINES)(t1)
wr_miss:
    move t8, ra
    jal fill
    move t9, t0
    move ra, t8
    b wr_hi
    srl t0, a0, 6

# fill: bring line t9 into its slot (writing back a dirty occupant).
# Keeps a0, a1, t3-t7, v1.
fill:
    move k0, ra
    andi t1, t9, 31
    sll t1, t1, 1
    lhu t2, %lo(TAGS)(t1)
    andi t0, t2, 0x8000
    beqz t0, 1f
    andi t2, t2, 0x7fff
    sll s4, t1, 5
    addiu s4, s4, %lo(LINES)
    sll t2, t2, 6
    lw s0, %lo(ST + SPCST_ARAM)
    addu s0, s0, t2
    jal DMAOut
    li t0, DMA_SIZE(64, 1)
1:  andi s4, t9, 31
    sll s4, s4, 6
    addiu s4, s4, %lo(LINES)
    sll t2, t9, 6
    lw s0, %lo(ST + SPCST_ARAM)
    addu s0, s0, t2
    jal DMAIn
    li t0, DMA_SIZE(64, 1)
    andi t1, t9, 31
    sll t1, t1, 1
    jr k0
    sh t9, %lo(TAGS)(t1)

push:                               # a1 -> [$100 + SP], SP--
    ori a0, s5, 0x100
    addiu s5, s5, -1
    j wr
    andi s5, s5, 0xff

pull:                               # SP++, v0 <- [$100 + SP]
    addiu s5, s5, 1
    andi s5, s5, 0xff
    j rd
    ori a0, s5, 0x100

fetchw:                             # v0 = word at PC, PC += 2 (ra kept in v1)
    move v1, ra
    jal fetch
    nop
    move t5, v0
    jal fetch
    nop
    sll v0, v0, 8
    jr v1
    or v0, v0, t5

rdw_dp:                             # t5 = word at a2 (low) and a3 = ((a2+1)&$FF)|dp
    move v1, ra
    addiu a3, a2, 1
    andi a3, a3, 0xff
    or a3, a3, k1
    jal rd
    move a0, a2
    move t5, v0
    jal rd
    move a0, a3
    sll v0, v0, 8
    jr v1
    or t5, t5, v0

zn16:                               # Z and N from the 16-bit t5
    andi s7, s7, 0x7d
    srl t0, t5, 8
    andi t0, t0, 0x80
    or s7, s7, t0
    sltiu t0, t5, 1
    sll t0, t0, 1
    jr ra
    or s7, s7, t0

# adc8: v0 = (t3 + v0 + C) & $FF with V, H, C set (as spc_adc).
adc8:
    andi t0, s7, 1
    addu t1, t3, v0
    addu t1, t1, t0
    andi t2, t3, 0xf
    andi t4, v0, 0xf
    addu t2, t2, t4
    addu t2, t2, t0
    sltiu t2, t2, 0x10
    xori t2, t2, 1
    sll t2, t2, 3                   # H
    xor t4, t3, v0
    nor t4, t4, zero
    xor t0, v0, t1
    and t4, t4, t0
    andi t4, t4, 0x80
    srl t4, t4, 1                   # V
    srl t0, t1, 8                   # C
    andi s7, s7, 0xb6
    or s7, s7, t2
    or s7, s7, t4
    or s7, s7, t0
    jr ra
    andi v0, t1, 0xff

#----------------------------------------------------------------------
# Addressing modes: a2 = address (and a3 = source for the two-operand ones).
# Each ends by dispatching the operation (t6 = op * 2).

M_DP:
    jal fetch
    nop
    jr t6
    or a2, v0, k1
M_ABS:
    jal fetchw
    nop
    jr t6
    move a2, v0
M_IND:
    jr t6
    or a2, s2, k1
M_IDX:
    jal fetch
    nop
    addu a2, v0, s2
    addiu a3, a2, 1
    andi a2, a2, 0xff
    or a2, a2, k1
    andi a3, a3, 0xff
    or a3, a3, k1
    jal m_ptrw
    nop
    jr t6
    move a2, t5
M_IMM:
    move a2, s6
    addiu s6, s6, 1
    jr t6
    andi s6, s6, 0xffff
M_DPX:
    jal fetch
    nop
    addu a2, v0, s2
m_dp8:
    andi a2, a2, 0xff
    jr t6
    or a2, a2, k1
M_DPY:
    jal fetch
    nop
    b m_dp8
    addu a2, v0, s3
M_ABX:
    jal fetchw
    nop
    addu a2, v0, s2
m_ab16:
    jr t6
    andi a2, a2, 0xffff
M_ABY:
    jal fetchw
    nop
    b m_ab16
    addu a2, v0, s3
M_IDY:
    jal fetch
    nop
    or a2, v0, k1
    addiu a3, v0, 1
    andi a3, a3, 0xff
    or a3, a3, k1
    jal m_ptrw
    nop
    b m_ab16
    addu a2, t5, s3
m_ptrw:                             # t5 = [a2] | [a3] << 8 (ra kept in v1)
    move v1, ra
    jal rd
    move a0, a2
    move t5, v0
    jal rd
    move a0, a3
    sll v0, v0, 8
    jr v1
    or t5, t5, v0
M_DPDP:                             # a3 = source dp, a2 = destination dp
    jal fetch
    nop
    b M_DP
    or a3, v0, k1
M_DPIMM:                            # a3 = immediate address, a2 = dp
    move a3, s6
    addiu s6, s6, 1
    b M_DP
    andi s6, s6, 0xffff
M_INDIND:
    or a3, s3, k1
    jr t6
    or a2, s2, k1
m2_rd:                              # t5 = [a3] (source), v0 = [a2] (ra kept in v1)
    move v1, ra
    jal rd
    move a0, a3
    move t5, v0
    jal rd
    move a0, a2
    jr v1
    nop

#----------------------------------------------------------------------
# Operations.

zn_loop:                            # Z and N from the byte v0, next opcode
    andi s7, s7, 0x7d
    andi t0, v0, 0x80
    or s7, s7, t0
    sltiu t0, v0, 1
    sll t0, t0, 1
    b loop
    or s7, s7, t0

OP_NOP:
    j loop
    nop
OP_OR:
    jal rd
    move a0, a2
    or s1, s1, v0
    j zn_loop
    move v0, s1
OP_AND:
    jal rd
    move a0, a2
    and s1, s1, v0
    j zn_loop
    move v0, s1
OP_EOR:
    jal rd
    move a0, a2
    xor s1, s1, v0
    j zn_loop
    move v0, s1
OP_MOV:
    jal rd
    move a0, a2
    j zn_loop
    move s1, v0
OP_MOVX:
    jal rd
    move a0, a2
    j zn_loop
    move s2, v0
OP_MOVY:
    jal rd
    move a0, a2
    j zn_loop
    move s3, v0
OP_CMP:
    jal rd
    move a0, a2
    b cmp_c
    move t3, s1
OP_CMPX:
    jal rd
    move a0, a2
    b cmp_c
    move t3, s2
OP_CMPY:
    jal rd
    move a0, a2
    move t3, s3
cmp_c:                              # t3 - v0: C and Z/N
    xori v0, v0, 0xff
    addu v0, t3, v0
    addiu v0, v0, 1
    srl t0, v0, 8
    andi s7, s7, 0xfe
    or s7, s7, t0
    j zn_loop
    andi v0, v0, 0xff
OP_ADC:
    jal rd
    move a0, a2
    jal adc8
    move t3, s1
    j zn_loop
    move s1, v0
OP_SBC:
    jal rd
    move a0, a2
    xori v0, v0, 0xff
    jal adc8
    move t3, s1
    j zn_loop
    move s1, v0
OP_MOVS:
    move a1, s1
movs_c:
    jal rd                          # the dummy read, as spc_movs
    move a0, a2
wr_loop:                            # [a2] = a1, next opcode
    jal wr
    move a0, a2
    j loop
    nop
OP_MOVSX:
    b movs_c
    move a1, s2
OP_MOVSY:
    b movs_c
    move a1, s3

# Read-modify-write: v0 in, a1 out; t5 = 1 when the operand is A.
OP_ASL:
    jal rd
    move a0, a2
    move t5, zero
asl_c:
    srl t0, v0, 7
    sll a1, v0, 1
    b rmw_cc
    andi a1, a1, 0xff
OP_LSR:
    jal rd
    move a0, a2
    move t5, zero
lsr_c:
    andi t0, v0, 1
    b rmw_cc
    srl a1, v0, 1
OP_ROL:
    jal rd
    move a0, a2
    move t5, zero
rol_c:
    sll a1, v0, 1
    andi t0, s7, 1
    or a1, a1, t0
    andi a1, a1, 0xff
    b rmw_cc
    srl t0, v0, 7
OP_ROR:
    jal rd
    move a0, a2
    move t5, zero
ror_c:
    srl a1, v0, 1
    andi t0, s7, 1
    sll t0, t0, 7
    or a1, a1, t0
    andi t0, v0, 1
rmw_cc:                             # C = t0, then store a1
    andi s7, s7, 0xfe
    or s7, s7, t0
rmw_st:
    bnez t5, a_st
    nop
m_store:
    jal wr
    move a0, a2
    j zn_loop
    move v0, a1
a_st:
    move s1, a1
    j zn_loop
    move v0, a1
OP_INC:
    jal rd
    move a0, a2
    addiu a1, v0, 1
    b m_store
    andi a1, a1, 0xff
OP_DEC:
    jal rd
    move a0, a2
    addiu a1, v0, -1
    b m_store
    andi a1, a1, 0xff
OP_ASLA:
    move v0, s1
    b asl_c
    li t5, 1
OP_LSRA:
    move v0, s1
    b lsr_c
    li t5, 1
OP_ROLA:
    move v0, s1
    b rol_c
    li t5, 1
OP_RORA:
    move v0, s1
    b ror_c
    li t5, 1

# Two operands: a2 destination, a3 source.
OP_ORMM2:
    jal m2_rd
    nop
    b m_store
    or a1, v0, t5
OP_ANDMM2:
    jal m2_rd
    nop
    b m_store
    and a1, v0, t5
OP_EORMM2:
    jal m2_rd
    nop
    b m_store
    xor a1, v0, t5
OP_CMPMM2:
    jal m2_rd
    nop
    move t3, v0
    b cmp_c
    move v0, t5
OP_SBCMM2:
    jal m2_rd
    nop
    b adcm_c
    xori t5, t5, 0xff
OP_ADCMM2:
    jal m2_rd
    nop
adcm_c:
    move t3, v0
    jal adc8
    move v0, t5
    b m_store
    move a1, v0
OP_MOVMI:                           # mov dp, #imm: read source, dummy-read dp, write
    jal m2_rd
    nop
    b wr_loop
    move a1, t5
OP_MOVMD:                           # mov dp, dp: no dummy read
    jal rd
    move a0, a3
    b wr_loop
    move a1, v0

# Bits, branches.
OP_CLR1:
    b bit1_c
    li t3, 1
OP_SET1:
    move t3, zero
bit1_c:
    jal rd
    move a0, a2
    srl t0, t7, 5
    li t1, 1
    sllv t1, t1, t0
    or a1, v0, t1
    beqz t3, wr_loop
    nop
    b wr_loop
    xor a1, a1, t1
OP_BBS:
    jal rd
    move a0, a2
    srl t0, t7, 5
    srlv t3, v0, t0
    b branch
    andi t3, t3, 1
OP_BBC:
    jal rd
    move a0, a2
    srl t0, t7, 5
    srlv t3, v0, t0
    andi t3, t3, 1
    b branch
    xori t3, t3, 1
OP_CBNE:
    jal rd
    move a0, a2
    xori v0, v0, 0xff
    addu v0, s1, v0
    addiu v0, v0, 1
    b branch
    andi t3, v0, 0xff
OP_DBNZ:
    jal rd
    move a0, a2
    addiu a1, v0, -1
    andi a1, a1, 0xff
    jal wr
    move a0, a2
    b branch
    move t3, a1
OP_DBNZY:
    addiu s3, s3, -1
    andi s3, s3, 0xff
    b branch
    move t3, s3
OP_BRANCH:                          # bpl bmi bvc bvs bcc bcs bne beq
    srl t0, t7, 6
    lbu t0, %lo(BRMASK)(t0)
    and t0, s7, t0
    sltu t0, zero, t0
    srl t1, t7, 5
    andi t1, t1, 1
    xor t3, t0, t1
    xori t3, t3, 1
branch:                             # fetch rel; taken (t3 != 0): PC += rel, 2 cycles
    jal fetch
    nop
    beqz t3, loop
    sll v0, v0, 24
    sra v0, v0, 24
    addu s6, s6, v0
    andi s6, s6, 0xffff
    b loop
    addiu fp, fp, -2
OP_BRA:
    jal fetch
    nop
    sll v0, v0, 24
    sra v0, v0, 24
    addu s6, s6, v0
    j loop
    andi s6, s6, 0xffff

# Jumps, calls, stack.
OP_JMP:
    j loop
    move s6, a2
OP_JMPIAX:                          # jmp [abs+X]
    jal fetchw
    nop
    addu a2, v0, s2
    addiu a3, a2, 1
    andi a2, a2, 0xffff
    andi a3, a3, 0xffff
    jal m_ptrw
    nop
    j loop
    move s6, t5
OP_CALL:
    srl a1, s6, 8
    jal push
    nop
    andi a1, s6, 0xff
    jal push
    nop
    j loop
    move s6, a2
OP_RET:
    jal pull
    nop
    move t5, v0
    jal pull
    nop
    sll v0, v0, 8
    j loop
    or s6, t5, v0
OP_PUSH:                            # pushp pusha pushx pushy
    sb s7, %lo(REGS) + 0
    sb s1, %lo(REGS) + 1
    sb s2, %lo(REGS) + 2
    sb s3, %lo(REGS) + 3
    srl t0, t7, 5
    lbu a1, %lo(REGS)(t0)
    jal push
    nop
    j loop
    nop
OP_POP:                             # popp popa popx popy
    jal pull
    nop
    srl t0, t7, 5
    li t1, 5
    beq t0, t1, 1f
    li t1, 6
    beq t0, t1, 2f
    li t1, 7
    beq t0, t1, 3f
    nop
    move s7, v0                     # popp
    andi k1, s7, 0x20
    j loop
    sll k1, k1, 3
1:  j loop
    move s1, v0
2:  j loop
    move s2, v0
3:  j loop
    move s3, v0

# Registers, flags.
OP_FLAGS:                           # clrp setp clrc setc ei di
    srl t0, t7, 5
    lbu t1, %lo(FLMASK)(t0)
    lbu t2, %lo(FLSET)(t0)
    nor t1, t1, zero
    and s7, s7, t1
    or s7, s7, t2
    andi k1, s7, 0x20
    j loop
    sll k1, k1, 3
OP_INCDEC:                          # decx incx deca inca decy incy
    srl t0, t7, 4
    andi t0, t0, 2
    addiu t0, t0, -1                # +1 when bit 5 of the opcode is set
    andi t1, t7, 0x0f
    li t2, 0x0d
    beq t1, t2, 1f                  # x
    sltiu t2, t7, 0xc0
    bnez t2, 2f                     # a
    addu s3, s3, t0                 # y (undone below for a)
    andi s3, s3, 0xff
    j zn_loop
    move v0, s3
1:  addu s2, s2, t0
    andi s2, s2, 0xff
    j zn_loop
    move v0, s2
2:  subu s3, s3, t0
    addu s1, s1, t0
    andi s1, s1, 0xff
    j zn_loop
    move v0, s1
OP_XFER:                            # movxa movax movxp movay movya
    li t0, 0x5d
    beq t7, t0, 1f
    li t0, 0x7d
    beq t7, t0, 2f
    li t0, 0x9d
    beq t7, t0, 3f
    li t0, 0xdd
    beq t7, t0, 4f
    nop
    move s3, s1                     # fd: y = a
    j zn_loop
    move v0, s3
1:  move s2, s1
    j zn_loop
    move v0, s2
2:  move s1, s2
    j zn_loop
    move v0, s1
3:  move s2, s5
    j zn_loop
    move v0, s2
4:  move s1, s3
    j zn_loop
    move v0, s1
OP_MOVPX:
    j loop
    move s5, s2
OP_MOVSINDP:                        # mov (x)+, a
    or a0, s2, k1
    addiu s2, s2, 1
    andi s2, s2, 0xff
    jal wr
    move a1, s1
    j loop
    nop
OP_MOVINDP:                         # mov a, (x)+
    or a0, s2, k1
    addiu s2, s2, 1
    jal rd
    andi s2, s2, 0xff
    j zn_loop
    move s1, v0
OP_CLRV:
    j loop
    andi s7, s7, 0xb7
OP_NOTC:
    j loop
    xori s7, s7, 1
OP_SLEEP:                           # sleep, stop: no interrupts, so both stop
    li t0, 1
    sb t0, %lo(ST + SPCST_STOPPED)
    blez fp, loop
    nop
    j loop
    move fp, zero
OP_XCN:
    srl t0, s1, 4
    sll s1, s1, 4
    or s1, s1, t0
    andi s1, s1, 0xff
    j zn_loop
    move v0, s1
OP_MUL:                             # ya = y * a
    move t0, zero
    move t1, s1
    li t2, 8
1:  andi t3, s3, 1
    beqz t3, 2f
    srl s3, s3, 1
    addu t0, t0, t1
2:  addiu t2, t2, -1
    bnez t2, 1b
    sll t1, t1, 1
    andi s1, t0, 0xff
    srl s3, t0, 8
    j zn_loop
    move v0, s3
OP_DIV:                             # a = ya / x, y = ya % x (as spc.c)
    andi t3, s7, 0xb7               # V and H recomputed
    andi t0, s2, 0xf
    andi t1, s3, 0xf
    sltu t0, t1, t0                 # (y & f) < (x & f)
    xori t0, t0, 1
    sll t0, t0, 3                   # H = (x & f) <= (y & f)
    or t3, t3, t0
    sll t1, s3, 8
    bnez s2, 1f
    or t1, t1, s1                   # ya
    li t4, 0xffff                   # x = 0: quotient $FFFF, remainder a
    b 3f
    move t5, s1
1:  move t4, zero                   # restoring division, 16 bits
    move t5, zero
    li t2, 16
2:  sll t5, t5, 1
    srl t0, t1, 15
    or t5, t5, t0
    sll t1, t1, 1
    andi t1, t1, 0xffff
    sll t4, t4, 1
    sltu t0, t5, s2
    bnez t0, 4f
    addiu t2, t2, -1
    subu t5, t5, s2
    ori t4, t4, 1
4:  bnez t2, 2b
    nop
3:  sltiu t0, t4, 0x100
    xori t0, t0, 1
    sll t0, t0, 6                   # V = quotient > $FF
    or s7, t3, t0
    andi s1, t4, 0xff
    andi s3, t5, 0xff
    j zn_loop
    move v0, s1

# Words (direct page): a2 low address, a3 high.
OP_DECW:
    jal rdw_dp
    nop
    b incw_c
    addiu t5, t5, -1
OP_INCW:
    jal rdw_dp
    nop
    addiu t5, t5, 1
incw_c:
    andi t5, t5, 0xffff
    jal zn16
    nop
    andi a1, t5, 0xff
    jal wr
    move a0, a2
    srl a1, t5, 8
    jal wr
    move a0, a3
    j loop
    nop
OP_MOVW:
    jal rdw_dp
    nop
    andi s1, t5, 0xff
    srl s3, t5, 8
    jal zn16
    nop
    j loop
    nop
OP_MOVWS:
    addiu a3, a2, 1
    andi a3, a3, 0xff
    or a3, a3, k1
    jal rd                          # dummy read, as spc.c
    move a0, a2
    move a1, s1
    jal wr
    move a0, a2
    move a1, s3
    jal wr
    move a0, a3
    j loop
    nop
OP_CMPW:
    jal rdw_dp
    nop
    xori t5, t5, 0xffff
    sll t0, s3, 8
    or t0, t0, s1                   # ya
    addu t5, t0, t5
    addiu t5, t5, 1
    srl t0, t5, 16                  # C
    andi s7, s7, 0xfe
    or s7, s7, t0
    andi t5, t5, 0xffff
    jal zn16
    nop
    j loop
    nop
OP_SUBW:
    jal rdw_dp
    nop
    xori t5, t5, 0xffff
    b addw_c
    li t6, 1
OP_ADDW:
    jal rdw_dp
    nop
    move t6, zero
addw_c:                             # ya + t5 + t6, H as spc.c (always +1)
    sll t3, s3, 8
    or t3, t3, s1                   # ya
    addu t4, t3, t5
    addu t4, t4, t6                 # result
    andi t0, t3, 0xfff
    andi t1, t5, 0xfff
    addu t0, t0, t1
    addiu t0, t0, 1
    sltiu t0, t0, 0x1000
    xori t0, t0, 1
    sll t0, t0, 3                   # H
    xor t1, t3, t5
    nor t1, t1, zero
    xor t2, t5, t4
    and t1, t1, t2
    srl t1, t1, 9
    andi t1, t1, 0x40               # V
    srl t2, t4, 16                  # C
    andi s7, s7, 0xb6
    or s7, s7, t0
    or s7, s7, t1
    or s7, s7, t2
    andi t5, t4, 0xffff
    andi s1, t5, 0xff
    srl s3, t5, 8
    jal zn16
    nop
    j loop
    nop

# Left to the CPU: stop before this opcode.
OP_TRAP:
    addiu s6, s6, -1
    andi s6, s6, 0xffff
    addu fp, fp, t4
    addiu sp, sp, -1
    li t0, SPCEXIT_TRAP
    sw t0, %lo(ST + SPCST_EXIT)

#----------------------------------------------------------------------
done:
    sb s1, %lo(ST + SPCST_A)
    sb s2, %lo(ST + SPCST_X)
    sb s3, %lo(ST + SPCST_Y)
    sb s5, %lo(ST + SPCST_SP)
    sh s6, %lo(ST + SPCST_PC)
    sb s7, %lo(ST + SPCST_PSW)
    lw t0, %lo(ST + SPCST_BUDGET)
    lw s7, %lo(ST + SPCST_USED)     # the state block address
    subu t0, t0, fp
    sw t0, %lo(ST + SPCST_USED)
    sw sp, %lo(ST + SPCST_COUNT)
    li s5, 0                        # write back dirty lines
1:  lhu t2, %lo(TAGS)(s5)
    andi t0, t2, 0x8000
    beqz t0, 2f
    andi t2, t2, 0x7fff
    sll s4, s5, 5
    addiu s4, s4, %lo(LINES)
    sll t2, t2, 6
    lw s0, %lo(ST + SPCST_ARAM)
    addu s0, s0, t2
    jal DMAOut
    li t0, DMA_SIZE(64, 1)
2:  addiu s5, s5, 2
    li t0, 64
    bne s5, t0, 1b
    nop
    lw s0, %lo(ST + SPCST_ARAM)
    li s4, %lo(ZP)
    jal DMAOut
    li t0, DMA_SIZE(0x200, 1)
    move s0, s7
    li s4, %lo(ST)
    lw ra, %lo(RET_RA)
    j DMAOut                        # returns to rspq
    li t0, DMA_SIZE(SPCST_SIZE, 1)

