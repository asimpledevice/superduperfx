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
# write-back cache of 64-byte lines.
#
# A run is apu_runCycles (apu_native.inc): BUDGET APU cycles, an instruction
# executing at its first cycle, CCL cycles of it carried across runs. The
# I/O page ($F0-$FF) is as apu_cpuRead/apu_cpuWrite: control, ports, timers
# (brought up to date in closed form when the SPC touches them and at the
# end), and the DSP registers as the SPC reads them back. An IPL ROM read
# ends the run before that instruction (SPCEXIT_TRAP); the CPU runs it.
#
# Registers during a run:
#   s1 A, s2 X, s3 Y, s5 SP, s6 PC, s7 PSW (N V P B H I Z C),
#   fp cycles left (budget minus used), k1 direct-page base (P ? 0x100 : 0),
#   t7 opcode, a2/a3 operand addresses.
# rd/wr (memory) clobber t0-t2 and, on a miss, t8, t9, k0, s0, s4, at.

#include <rsp_queue.inc>
#include "spc_rsp_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand SpcCmd_Run, 8   # a0 state block, a1 ext buffer (spc_rsp.c)
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8               # rspq needs a non-empty saved state
RSPQ_EndSavedState

#include "rsp_spc_tables.inc"

BRMASK:     .byte 0x80, 0x40, 0x01, 0x02    # bpl/bmi, bvc/bvs, bcc/bcs, bne/beq
FLMASK:     .byte 0, 0x20, 0x20, 0x01, 0x01, 0x04, 0x04, 0   # by opcode >> 5
FLSET:      .byte 0, 0,    0x20, 0,    0x01, 0x04, 0,    0
# Per DSP register: 1 a write the CPU must make (FLG ESA EDL DIR, SRCN,
# ENDX), 2 a read the CPU must make (ENVX, OUTX, ENDX).
DSPF:       .byte 0,0,0,0,1,0,0,0, 2,2,0,0,0,0,0,0
            .byte 0,0,0,0,1,0,0,0, 2,2,0,0,0,0,0,0
            .byte 0,0,0,0,1,0,0,0, 2,2,0,0,0,0,0,0
            .byte 0,0,0,0,1,0,0,0, 2,2,0,0,0,0,0,0
            .byte 0,0,0,0,1,0,0,0, 2,2,0,0,0,0,0,0
            .byte 0,0,0,0,1,0,0,0, 2,2,0,0,0,1,0,0
            .byte 0,0,0,0,1,0,0,0, 2,2,0,0,1,1,0,0
            .byte 0,0,0,0,1,0,0,0, 2,2,0,0,3,1,0,0
# $F0-$FF handlers (rd_io, wr_io).
    .align 1
RIOT:       .half rio_zero - _start, rio_zero - _start, rio_f2 - _start, rio_f3 - _start
            .half rio_port - _start, rio_port - _start, rio_port - _start, rio_port - _start
            .half rio_port - _start, rio_port - _start, rio_zero - _start, rio_zero - _start
            .half rio_zero - _start, rd_tim - _start, rd_tim - _start, rd_tim - _start
WIOT:       .half wr_zp - _start, wio_f1 - _start, wio_f2 - _start, wio_f3 - _start
            .half wio_out - _start, wio_out - _start, wio_out - _start, wio_out - _start
            .half wio_in - _start, wio_in - _start, wio_tgt - _start, wio_tgt - _start
            .half wio_tgt - _start, wr_zp - _start, wr_zp - _start, wr_zp - _start
# REGS slots (0 psw, 1 a, 2 x, 3 y, 4 sp) by opcode >> 5.
XFT:        .byte 0,0, 0,0, 1,2, 2,1, 4,2, 0,0, 3,1, 1,3   # 5d x=a 7d a=x 9d x=sp dd a=y fd y=a
IDT:        .byte 2,-1, 2,1, 0,0, 0,0, 1,-1, 1,1, 3,-1, 3,1 # 1d 3d x, 9c bc a, dc fc y

    .bss
    .align 4
EXTBUF:     .ds.b 4                 # RDRAM: ext code, then the IMEM and DMEM save areas
RET_RA:     .ds.b 4
END:        .ds.b 4                 # CYCLES + BUDGET: the cycle the run ends at
PC0:        .ds.b 2                 # PC of the instruction in progress
    .align 2
FP0:        .ds.b 4                 # fp before it (a trap rolls back to both)
REGS:       .ds.b 8                 # psw a x y sp, for register families
    .align 4
ZP:         .ds.b 0x200             # ARAM $0000-$01FF
    .align 4
LINES:      .ds.b 2048              # 32 slots x 64 bytes

    .text

/* The borrowed IMEM: rspq's code, from _start to the overlay's code, which
 * rspq does not run during a command. rsp_spc_ext.S is linked there and
 * starts with an entry table. */
#define RSPQ_IMEM_SIZE  (_ovl_text_start - _start)
#define EXT_SAVE_OFS    0x300           /* rspq IMEM save area after the code (spc_rsp.c) */
#define EXT_DSAVE_OFS   0x600           /* rspq DMEM save area */
/* The borrowed DMEM: rspq's, below the overlay's data (its command buffer
 * and state; untouched during a command), except Red's resident words at
 * its start (mailbox, command ring), which the CPU may write at any time.
 * The state block lives there, then the run's scratch (spc_rsp_layout.h
 * SPCRUN_*); gen_syms.py --check checks it all fits. */
#define BORROW_DMEM_SIZE (_ovl_data_start - RSPQ_RESIDENT_END)
    .set ST, 0x50                   /* above RSPQ_RESIDENT_END, 16-aligned */
    .set TAGS, ST + SPCRUN_TAGS     /* line number per slot, bit 15 dirty; 0 empty */
#define EXT_TSYNC_NOW   0x1000
#define EXT_TSYNC       0x1008
#define EXT_F1          0x1010
#define EXT_DIV         0x1018
#define EXT_MUL         0x1020
#define EXT_WMARK       0x1028

SpcCmd_Run:
    sw ra, %lo(RET_RA)
    sw a1, %lo(EXTBUF)
    move s7, a0                     # state block (the DMA ignores the top byte: command id)
    addiu s0, a1, EXT_DSAVE_OFS     # rspq's DMEM out first, then the block in
    li s4, %lo(RSPQ_RESIDENT_END)
    jal SDmaOutA
    li t0, DMA_SIZE(BORROW_DMEM_SIZE, 1)
    move s0, s7
    li s4, %lo(ST)
    jal SDmaIn                      # with the run's area: the tags and page bits zeroed
    li t0, DMA_SIZE(SPCRUN_IO, 1)
    sw s7, %lo(ST + SPCST_USED)     # remember the block address (USED is output)
    lw s0, %lo(ST + SPCST_ARAM)
    li s4, %lo(ZP)
    jal SDmaIn
    li t0, DMA_SIZE(0x200, 1)
    lw s0, %lo(EXTBUF)              # rspq's IMEM out to its save area, our
    addiu s0, s0, EXT_SAVE_OFS      # code in (both while the run starts)
    li s4, 0x1000
    jal SDmaOutA
    li t0, DMA_SIZE(RSPQ_IMEM_SIZE, 1)
    lw s0, %lo(EXTBUF)
    li s4, 0x1000
    jal SDmaInA
    li t0, DMA_SIZE(RSPQ_IMEM_SIZE, 1)
    lbu s1, %lo(ST + SPCST_A)
    lbu s2, %lo(ST + SPCST_X)
    lbu s3, %lo(ST + SPCST_Y)
    lbu s5, %lo(ST + SPCST_SP)
    lhu s6, %lo(ST + SPCST_PC)
    lbu s7, %lo(ST + SPCST_PSW)
    lw fp, %lo(ST + SPCST_BUDGET)
    lw t0, %lo(ST + SPCST_CYCLES)
    addu t0, t0, fp
    sw t0, %lo(END)
    lbu t0, %lo(ST + SPCST_CCL)
    subu fp, fp, t0                 # the carried instruction finishes first
    lbu t0, %lo(ST + SPCST_STOPPED)
    beqz t0, 2f
    andi k1, s7, 0x20
    blez fp, 2f                     # stopped: each step is one cycle, so
    nop                             # a positive budget is used exactly
    move fp, zero
2:  sll k1, k1, 3
    sw zero, %lo(ST + SPCST_EXIT)
    sw zero, %lo(ST + SPCST_RAWN)   # raw log bytes so far (staging: the last RAWN & 63)

loop:
OP_NOP:                             # (also the NOP handler)
    blez fp, done
    sh s6, %lo(PC0)
    sw fp, %lo(FP0)
    sb s5, ST + SPCRUN_SP0               # a trap inside a store rolls these back too
    sb s7, ST + SPCRUN_PSW0
    move t4, zero                   # this instruction's cycles: none charged yet
    move a0, s6
    jal rd
    addiu s6, s6, 1
    andi s6, s6, 0xffff
    sll t3, v0, 1
    lhu t3, %lo(DISPATCH)(t3)
    move t7, v0                     # opcode
    srl t4, t3, 12                  # cycles
    subu fp, fp, t4
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
    andi t1, a0, 0x1f0
    xori t1, t1, 0xf0
    beqz t1, rd_io                  # $00F0-$00FF
    nop
    jr ra
    lbu v0, %lo(ZP)(a0)
rd_hi:
    srl t0, a0, 6
    xori t1, t0, 0x3ff
    bnez t1, rd_hc                  # not $FFC0-$FFFF
    lbu t1, %lo(ST + SPCST_ROMREADABLE)
    bnez t1, OP_TRAP                # the IPL ROM: left to the CPU
    nop
rd_hc:
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
rd_miss:                            # fill, then the lookup again
    li k0, rd_hi
    b fill
    move t9, t0

wr:
    sltiu t0, a0, 0x200
    beqz t0, wr_hi
    andi t1, a0, 0x1f0
    xori t1, t1, 0xf0
    beqz t1, wr_io                  # $00F0-$00FF (then RAM, as apu_cpuWrite)
    nop
wr_zp:
    addiu t1, a0, %lo(ZP)
wr_st:                              # store a1 at DMEM t1 (logged while a batch is open)
    lbu t0, %lo(ST + SPCST_LOGGING)
    bnez t0, wr_log
    nop
    jr ra
    sb a1, 0(t1)
wr_hi:
    srl t0, a0, 6
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
    b wr_st
    addiu t1, t1, %lo(LINES)
wr_miss:
    li k0, wr_hi
    b fill
    move t9, t0

# Logging while a DSP batch is open (apu_dspLogRam, the $F3 path): every
# ARAM write and DSP register write, with the instruction's cycle offset.
# Checked before anything is stored, since a trap re-runs the instruction.
wr_log:
    lhu t0, %lo(ST + SPCST_DIRPAGE)
    subu t0, a0, t0
    andi t0, t0, 0xffff
    sltiu t0, t0, 0x400
    bnez t0, OP_TRAP                # the sample directory: the CPU (watch marks)
    move t8, ra
    lbu v0, 0(t1)                   # old value
    move t9, a0
    jal log_put
    move t2, zero                   # an ARAM write
    move ra, t8
    jr ra
    sb a1, 0(t1)
# log_put: entry (t9 address or register, a1 value, v0 old, t2 1 for a
# register). Keeps a0, a1, t1, t3-t8. The run is sized so it cannot fill.
log_put:
    lw t0, %lo(ST + SPCST_RAWN)
    andi s4, t0, 63
    lw s0, %lo(ST + SPCST_BUDGET)
    subu s0, s0, fp
    subu s0, s0, t4                 # cycle offset of this instruction in the run
    sh t9, ST + SPCLOG_STAGE + 0(s4)
    sb a1, ST + SPCLOG_STAGE + 2(s4)
    sb v0, ST + SPCLOG_STAGE + 3(s4)
    sh s0, ST + SPCLOG_STAGE + 4(s4)
    sb t2, ST + SPCLOG_STAGE + 6(s4)
    addiu t0, t0, 8
    andi s4, t0, 63
    bnez s4, 1f
    sw t0, %lo(ST + SPCST_RAWN)
    move k0, ra                     # 8 entries: out to the raw log
    lw s0, %lo(ST + SPCST_RAWBUF)
    addu s0, s0, t0
    addiu s0, s0, -64
    li s4, ST + SPCLOG_STAGE
    jal SDmaOut
    li t0, DMA_SIZE(64, 1)
    move ra, k0
1:  jr ra
    nop

# The I/O page. rd_io: v0 = apu_cpuRead(a0). wr_io: apu_cpuWrite(a0, a1)
# before the RAM write. Keep a0, a1, a2, a3, v1, t3-t7.
rd_io:                              # by RIOT ($F0-$FF)
    andi t1, a0, 0xf
    sll t2, t1, 1
    lhu t2, %lo(RIOT)(t2)
    jr t2
    nop
rio_zero:                           # $F0 $F1 $FA-$FC read 0
    jr ra
    move v0, zero
rio_port:                           # $F4-$F7 from the CPU, $F8-$F9
    jr ra
    lbu v0, %lo(ST + SPCST_INPORTS - 4)(t1)
rio_f2:
    jr ra
    lbu v0, %lo(ST + SPCST_DSPADR)
rio_f3:
    lbu t0, %lo(ST + SPCST_DSPADR)
    andi t0, t0, 0x7f
    lbu t1, %lo(DSPF)(t0)
    andi t1, t1, 2
    bnez t1, OP_TRAP                # ENVX OUTX ENDX: the CPU syncs the DSP
    nop
    jr ra
    lbu v0, %lo(ST + SPCST_DSPREGS)(t0)
rd_tim:                             # sync the timers, read and clear the counter
    beqz t4, OP_TRAP                # an opcode fetch from $FD-$FF: the CPU runs
    move t8, ra                     # it (the read clears; a later trap could not undo it)
    jal tsync_now
    nop
    move ra, t8
    lw t0, %lo(FP0)                 # the instruction's first timer read: keep the
    lw t1, %lo(ST + SPCST_UNDOFP)   # counters, which spc_rsp_run.c puts back if
    beq t0, t1, 1f                  # it traps (the C core then runs it whole)
    lbu t1, %lo(ST + SPCST_TIMERS + 3)
    sw t0, %lo(ST + SPCST_UNDOFP)
    sb t1, %lo(ST + SPCST_TIMERS + 6)
    lbu t1, %lo(ST + SPCST_TIMERS + 11)
    sb t1, %lo(ST + SPCST_TIMERS + 14)
    lbu t1, %lo(ST + SPCST_TIMERS + 19)
    sb t1, %lo(ST + SPCST_TIMERS + 22)
1:    addiu t2, a0, -0xfd
    sll t2, t2, 3
    lbu v0, %lo(ST + SPCST_TIMERS + 3)(t2)
    jr ra
    sb zero, %lo(ST + SPCST_TIMERS + 3)(t2)

wr_io:                              # by WIOT ($F0-$FF), then the RAM write
    andi t1, a0, 0xf
    sll t2, t1, 1
    lhu t2, %lo(WIOT)(t2)
    jr t2
    nop
wio_out:                            # $F4-$F7: ports to the CPU
    b wr_zp
    sb a1, %lo(ST + SPCST_OUTPORTS - 4)(t1)
wio_in:                             # $F8-$F9
    b wr_zp
    sb a1, %lo(ST + SPCST_INPORTS - 4)(t1)
wio_tgt:                            # $FA-$FC: timer targets
    move t8, ra
    jal tsync_now
    nop
    move ra, t8
    addiu t2, a0, -0xfa
    sll t2, t2, 3
    b wr_zp
    sb a1, %lo(ST + SPCST_TIMERS + 2)(t2)
wio_f2:
    b wr_zp
    sb a1, %lo(ST + SPCST_DSPADR)
wio_f3:
  lbu t0, %lo(ST + SPCST_DSPADR)
    sltiu t1, t0, 0x80
    beqz t1, wr_zp
    lbu t1, %lo(ST + SPCST_LOGGING)
    beqz t1, OP_TRAP                # no batch open: the CPU writes the DSP
    lbu t1, %lo(DSPF)(t0)
    andi t1, t1, 1
    bnez t1, OP_TRAP                # FLG ESA EDL DIR, SRCN, ENDX: the CPU
    move t8, ra
    sb a1, %lo(ST + SPCST_DSPREGS)(t0)
    move t9, t0
    move v0, zero
    jal log_put
    li t2, 1                        # a register write
    b wr_zp
    move ra, t8
wio_f1:                             # $F1: in the borrowed IMEM (rsp_spc_ext.S)
    j ext_go
    li t1, EXT_F1

# Code in the borrowed IMEM (rsp_spc_ext.S, loaded over rspq's code for the
# run): wait for that load, then enter through the table at its start.
wmark:                              # page of line t2 into SPCRUN_WPAGES (uses s0, t0, t1)
    j ext_go
    li t1, EXT_WMARK
tsync_now:                          # timers to the start of this instruction
    j ext_go
    li t1, EXT_TSYNC_NOW
tsync:                              # timers to cycle t2
    li t1, EXT_TSYNC
ext_go:
    .set noat
1:  mfc0 $1, COP0_SP_STATUS
    andi $1, $1, SP_STATUS_DMA_BUSY | SP_STATUS_DMA_FULL
    bnez $1, 1b
    nop
    .set at
    jr t1
    nop

# DMA. rspq's own routines sit in the borrowed IMEM, so the run has its own.
# s0 RDRAM, s4 DMEM or IMEM (0x1000 | offset), t0 DMA_SIZE. *A: asynchronous.
SDmaIn:
    b sdma
    li t2, DMA_IN
SDmaOut:
    b sdma
    li t2, DMA_OUT
SDmaInA:
    b sdma
    li t2, DMA_IN_ASYNC
SDmaOutA:
    li t2, DMA_OUT_ASYNC
sdma:
    .set noat
1:  mfc0 $1, COP0_DMA_FULL
    bnez $1, 1b
    mtc0 s0, COP0_DMA_RAMADDR
    bgez t2, 2f
    mtc0 s4, COP0_DMA_SPADDR
    b 3f
    mtc0 t0, COP0_DMA_WRITE
2:  mtc0 t0, COP0_DMA_READ
3:  andi t2, t2, 0xf
4:  mfc0 $1, COP0_SP_STATUS
    and $1, $1, t2
    bnez $1, 4b
    nop
    .set at
    jr ra
    nop

# fill: bring line t9 into its slot (writing back a dirty occupant), then
# continue at k0 with ra kept. Keeps a0, a1, t3-t7, v1.
fill:
    srl t0, t9, 5                   # a page whose reads need the DSP current
    lbu t0, %lo(ST + SPCST_SYNC)(t0)
    srl t1, t9, 2
    andi t1, t1, 7
    srlv t0, t0, t1
    andi t0, t0, 1
    bnez t0, OP_TRAP
    move t8, ra
    andi t1, t9, 31
    sll t1, t1, 1
    lhu t2, %lo(TAGS)(t1)
    andi t0, t2, 0x8000
    beqz t0, 1f
    andi t2, t2, 0x7fff
    sll s4, t1, 5
    jal wmark                       # a dirty line out: its page was written
    addiu s4, s4, %lo(LINES)
    sll t2, t2, 6
    lw s0, %lo(ST + SPCST_ARAM)
    addu s0, s0, t2
    jal SDmaOut
    li t0, DMA_SIZE(64, 1)
1:  andi s4, t9, 31
    sll s4, s4, 6
    addiu s4, s4, %lo(LINES)
    sll t2, t9, 6
    lw s0, %lo(ST + SPCST_ARAM)
    addu s0, s0, t2
    jal SDmaIn
    li t0, DMA_SIZE(64, 1)
    andi t1, t9, 31
    sll t1, t1, 1
    sh t9, %lo(TAGS)(t1)
    jr k0
    move ra, t8

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
    jal fetch
    move t5, v0
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
    jal m_ptrw
    or a3, a3, k1
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
    jal m_ptrw
    or a3, a3, k1
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
    move ra, v1
    j rd
    move a0, a2

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
OP_CMPY:
    jal rd
    move a0, a2
    b cmp_c
    move t3, s3
OP_CMPX:
    jal rd
    move a0, a2
    move t3, s2
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
    li ra, %lo(loop)
    j wr
    move a0, a2
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
OP_RORA:
    move v0, s1
    b ror_c
    li t5, 1

# Two operands: a2 destination, a3 source.
OP_CMPMM2:
    jal m2_rd
    nop
    move t3, v0
    b cmp_c
    move v0, t5
OP_MOVMI:                           # mov dp, #imm: read source, dummy-read dp, write
    jal m2_rd
    nop
    b wr_loop
    move a1, t5
# Bits, branches.
OP_CLR1:
    b bit1_c
    li t3, 0xff
OP_SET1:
    move t3, zero
bit1_c:
    jal rd
    move a0, a2
    srl t0, t7, 5
    li t1, 1
    sllv t1, t1, t0
    or a1, v0, t1                   # set the bit; clr1 then flips it back
    and t1, t1, t3
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
OP_CALL:
    jal push
    srl a1, s6, 8
    andi a1, s6, 0xff
    move s6, a2
    j push                          # returns to loop
    li ra, %lo(loop)
OP_RET:
    jal pull
    nop
    jal pull
    move t5, v0
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
    j push                          # returns to loop
    li ra, %lo(loop)
OP_POP:                             # popp popa popx popy, through REGS
    jal pull
    nop
    sb s7, %lo(REGS) + 0
    sb s1, %lo(REGS) + 1
    sb s2, %lo(REGS) + 2
    sb s3, %lo(REGS) + 3
    srl t0, t7, 5
    andi t0, t0, 3
    sb v0, %lo(REGS)(t0)
    lbu s7, %lo(REGS) + 0
    lbu s1, %lo(REGS) + 1
    lbu s2, %lo(REGS) + 2
    lbu s3, %lo(REGS) + 3
    andi k1, s7, 0x20
    j loop
    sll k1, k1, 3

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
# Register families through REGS (psw a x y sp), slots and deltas by
# opcode >> 5 (XFT, IDT): smaller than a branch per register.
OP_XFER:                            # movxa movax movxp movay movya
    srl t0, t7, 4
    andi t0, t0, 0xe
    lbu t1, %lo(XFT)(t0)            # source slot
    lbu t2, %lo(XFT) + 1(t0)        # destination slot
    sb s1, %lo(REGS) + 1
    sb s2, %lo(REGS) + 2
    sb s3, %lo(REGS) + 3
    sb s5, %lo(REGS) + 4
    lbu v0, %lo(REGS)(t1)
xr_st:                              # REGS[t2] = v0, back into a x y, Z/N from v0
    sb v0, %lo(REGS)(t2)
    lbu s1, %lo(REGS) + 1
    lbu s2, %lo(REGS) + 2
    j zn_loop
    lbu s3, %lo(REGS) + 3
OP_INCDEC:                          # decx incx deca inca decy incy
    srl t0, t7, 4
    andi t0, t0, 0xe
    lbu t2, %lo(IDT)(t0)            # register slot
    lb t1, %lo(IDT) + 1(t0)         # +1 or -1
    sb s1, %lo(REGS) + 1
    sb s2, %lo(REGS) + 2
    sb s3, %lo(REGS) + 3
    lbu v0, %lo(REGS)(t2)
    addu v0, v0, t1
    b xr_st
    andi v0, v0, 0xff
OP_DIV:                             # on the vector unit, in the borrowed IMEM
    j ext_go
    li t1, EXT_DIV
OP_XCN:
    srl t0, s1, 4
    sll s1, s1, 4
    or s1, s1, t0
    andi s1, s1, 0xff
    j zn_loop
    move v0, s1
OP_MUL:                             # on the vector unit, in the borrowed IMEM
    j ext_go
    li t1, EXT_MUL
# Words (direct page): a2 low address, a3 high.
OP_MOVW:
    jal rdw_dp
    nop
    andi s1, t5, 0xff
    srl s3, t5, 8
    j zn16
    li ra, %lo(loop)
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
    li ra, %lo(loop)
    j wr
    move a0, a3
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
    j zn16
    li ra, %lo(loop)

# Left to the CPU: stop before this opcode.
OP_TRAP:                            # also from an access the CPU must make
    lhu s6, %lo(PC0)
    lw fp, %lo(FP0)
    lbu s5, ST + SPCRUN_SP0
    lbu s7, ST + SPCRUN_PSW0
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
    blez fp, 1f                     # fp > 0 only when stopped before a trap
    subu t1, zero, fp
    subu t0, t0, fp
    move t1, zero
1:  sw t0, %lo(ST + SPCST_USED)
    sb t1, %lo(ST + SPCST_CCL)
    lw t2, %lo(ST + SPCST_CYCLES)
    addu t2, t2, t0
    jal tsync                       # timers current at the end, as apu_runCycles
    sw t2, %lo(ST + SPCST_CYCLES)
    lw s0, %lo(EXTBUF)              # rspq's IMEM back (nothing borrowed is
    addiu s0, s0, EXT_SAVE_OFS      # called after this), during the writebacks
    li s4, 0x1000
    jal SDmaInA
    li t0, DMA_SIZE(RSPQ_IMEM_SIZE, 1)
    lw t1, %lo(ST + SPCST_RAWN)     # the raw log's last entries
    andi t0, t1, 63
    beqz t0, 1f
    lw s0, %lo(ST + SPCST_RAWBUF)
    subu t1, t1, t0
    addu s0, s0, t1
    li s4, ST + SPCLOG_STAGE
    jal SDmaOut
    addiu t0, t0, -1                # DMA_SIZE(bytes, 1)
1:  li s5, 0                        # write back dirty lines
1:  lhu t2, %lo(TAGS)(s5)
    andi t0, t2, 0x8000
    beqz t0, 2f
    andi t2, t2, 0x7fff
    sll s4, s5, 5
    addiu s4, s4, %lo(LINES)
    sll t2, t2, 6
    lw s0, %lo(ST + SPCST_ARAM)
    addu s0, s0, t2
    jal SDmaOut
    li t0, DMA_SIZE(64, 1)
2:  addiu s5, s5, 2
    li t0, 64
    bne s5, t0, 1b
    nop
    lw s0, %lo(ST + SPCST_ARAM)
    li s4, %lo(ZP)
    jal SDmaOut
    li t0, DMA_SIZE(0x200, 1)
    move s0, s7
    li s4, %lo(ST)
    jal SDmaOutA                    # the block out (with the tags and page bits:
    li t0, DMA_SIZE(SPCRUN_IO, 1)   # which pages the run wrote), then rspq's DMEM back
    lw s0, %lo(EXTBUF)
    addiu s0, s0, EXT_DSAVE_OFS
    li s4, %lo(RSPQ_RESIDENT_END)
    jal SDmaIn                      # waits for every transfer: rspq whole again
    li t0, DMA_SIZE(BORROW_DMEM_SIZE, 1)
    lw ra, %lo(RET_RA)
    jr ra
    nop

