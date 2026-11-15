# S-DSP prep on the RSP (audio ownership step 2a, docs/n64/RED_DESIGN.md):
# decodes a batch's write log into the operations the S-DSP overlay applies
# (rsp_snesdsp.S), as decode()/emit_reg() in snesdsp_rsp.c do on the CPU.
# The output must be byte-identical to the C decode (the C path stays as the
# reference: RSPDSP_PREP=0, and n64/rspdsp/preptest compares the two).
#
# Input (PrepParams, snesdsp_rsp_layout.h): the log (ApuDspLogEntry: u16 adr,
# u8 val, u8 old, u16 tag, u8 isReg, u8 pad), its length, where the
# operations go, and the decode state that is not a function of the written
# byte: per-voice pitch, GAIN value (direct mode), gain mode and rate 3, and
# the USEGAIN/DIRECTGAIN masks, all at the batch's start. The CPU has
# checked the batch (pitch modulation, the echo window, operation count),
# so every entry decodes. After the operations come 32 zero operations, as
# the C decode leaves them (the S-DSP reads a whole chunk past the end).
# Red runs this command just before the S-DSP command on its ring.

#include <rsp_queue.inc>
#include "snesdsp_rsp_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand PrepCmd_Run, 8
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8
RSPQ_EndSavedState

    .align 4
PRM:        .ds.b RSPPREP_PARAMS_SIZE

RATES:      # rateValues[32] (dsp.c)
    .half 0, 2048, 1536, 1280, 1024, 768, 640, 512
    .half 384, 320, 256, 192, 160, 128, 96, 80
    .half 64, 48, 40, 32, 24, 20, 16, 12
    .half 10, 8, 6, 5, 4, 3, 2, 1

#define IN_CHUNK 32
#define OUT_OPS  96
    .align 3
IBUF:       .ds.b IN_CHUNK * 8
    .align 3
OBUF:       .ds.b OUT_OPS * 8

    .text

# Registers:
#   s3 return into rspq     s1 RDRAM address of the next input chunk
#   s2 entries left         t8 DMEM input pointer, t9 entries left in IBUF
#   s5 DMEM output pointer  s6 RDRAM address of the next output
#   k0 tag << 16            k1 (tag << 16) | HALF kind
#   a0 address, a1 value (u8), a2 voice, a3 low nibble
#   t1 value as int8, t6 (2 * voice) << 16
# DMA helpers clobber t0, t2, at, s0, s4.

# HALF op at a fixed state offset; \vr holds the value (low 16 bits used).
.macro HALFI off, vr
    andi t5, \vr, 0xffff
    lui t7, \off
    or t7, t7, t5
    sw k1, 0(s5)
    sw t7, 4(s5)
    addiu s5, s5, 8
.endm

# HALF op at a per-voice offset (\off + 2 * voice).
.macro HALFV off, vr
    andi t5, \vr, 0xffff
    lui t7, \off
    addu t7, t7, t6
    or t7, t7, t5
    sw k1, 0(s5)
    sw t7, 4(s5)
    addiu s5, s5, 8
.endm

# The u/c split of the int8 coefficient in t1 for a product shifted right
# by \sh: K = k << (16 - sh), u = K & 0xffff, c = K >> 16.
.macro UCI offu, offc, sh
    sll t3, t1, 16 - \sh
    HALFI \offu, t3
    sra t3, t3, 16
    HALFI \offc, t3
.endm
.macro UCV offu, offc, sh
    sll t3, t1, 16 - \sh
    HALFV \offu, t3
    sra t3, t3, 16
    HALFV \offc, t3
.endm

# t3 = rateValues[\idx]
.macro RATE idx
    sll t3, \idx, 1
    lhu t3, %lo(RATES)(t3)
.endm

# a0: RDRAM address of PrepParams (top byte: command id)
PrepCmd_Run:
    move s3, ra
    li t0, 0xffffff
    and s0, a0, t0
    li s4, %lo(PRM)
    jal DMAIn
    li t0, DMA_SIZE(RSPPREP_PARAMS_SIZE, 1)
    lw s1, %lo(PRM + RSPPREP_LOG)
    lw s2, %lo(PRM + RSPPREP_N)
    lw s6, %lo(PRM + RSPPREP_OPS)
    li s5, %lo(OBUF)
    move t9, zero

Prep_loop:
    # Room for the largest entry (10 operations) in OBUF.
    li t3, %lo(OBUF) + (OUT_OPS - 12) * 8
    sltu t3, s5, t3
    bnez t3, 1f
    nop
    jal Out_Flush
    nop
1:  beqz s2, Prep_done
    nop
    bnez t9, Prep_have
    nop
    move s0, s1
    li s4, %lo(IBUF)
    jal DMAIn
    li t0, DMA_SIZE(IN_CHUNK * 8, 1)
    addiu s1, s1, IN_CHUNK * 8
    li t8, %lo(IBUF)
    li t9, IN_CHUNK
Prep_have:
    lhu a0, 0(t8)                   # address / register
    lbu a1, 2(t8)                   # value
    lhu t1, 4(t8)                   # tag
    lbu t2, 6(t8)                   # 1: register write
    addiu t8, t8, 8
    addiu t9, t9, -1
    addiu s2, s2, -1
    sll k0, t1, 16
    addiu t2, t2, -1
    beqz t2, Prep_reg
    ori k1, k0, (RSPDSP_LOG_HALF << 8)

    # ARAM write: one operation, and its mirror after $FFFF below $0010.
    sll t1, a0, 16
    or t1, t1, a1
    sw k0, 0(s5)
    sw t1, 4(s5)
    sltiu t3, a0, 16
    beqz t3, Prep_loop
    addiu s5, s5, 8
    ori t3, k0, 1
    sw t3, 0(s5)
    sw t1, 4(s5)
    b Prep_loop
    addiu s5, s5, 8

Prep_done:
    # 32 zero operations after the last (OBUF emptied first: room).
    jal Out_Flush
    nop
    li t3, 32
2:  sw zero, 0(s5)
    sw zero, 4(s5)
    addiu t3, t3, -1
    bnez t3, 2b
    addiu s5, s5, 8
    jal Out_Flush
    nop
    jr s3
    nop

# OBUF[0 .. s5) to RDRAM at s6 (synchronous: the S-DSP reads it next).
Out_Flush:
    move v1, ra
    li s4, %lo(OBUF)
    subu t1, s5, s4
    beqz t1, 3f
    move s0, s6
    addu s6, s6, t1
    jal DMAOut
    addiu t0, t1, -1                # DMA_SIZE(len, 1) = len - 1
3:  jr v1
    li s5, %lo(OBUF)

########################################################################
# Register writes (emit_reg in snesdsp_rsp.c, operation for operation)
########################################################################

Prep_reg:
    andi a0, a0, 0xff
    srl a2, a0, 4
    andi a2, a2, 7
    andi a3, a0, 15
    sll t6, a2, 17                  # (2 * voice) << 16
    sll t1, a1, 24
    sra t1, t1, 24                  # the value as int8
    sll t4, a2, 1                   # 2 * voice: index into the u16 arrays

    beqz a3, R_volL
    addiu t3, a3, -1
    beqz t3, R_volR
    addiu t3, a3, -2
    beqz t3, R_pitchL
    addiu t3, a3, -3
    beqz t3, R_pitchH
    addiu t3, a3, -4
    beqz t3, R_srcn
    addiu t3, a3, -5
    beqz t3, R_adsr1
    addiu t3, a3, -6
    beqz t3, R_adsr2
    addiu t3, a3, -7
    beqz t3, R_gain
    addiu t3, a3, -0xc
    beqz t3, R_xC
    addiu t3, a3, -0xd
    beqz t3, R_xD
    addiu t3, a3, -0xf
    beqz t3, R_fir
    nop
    b R_byte                        # $x8-$xB, $xE: the register file only
    nop

R_volL:
    HALFV RSPDSP_V_VOLL, t1
    UCV RSPDSP_V_VOLL_U, RSPDSP_V_VOLL_C, 6
    b R_byte
    nop
R_volR:
    HALFV RSPDSP_V_VOLR, t1
    UCV RSPDSP_V_VOLR_U, RSPDSP_V_VOLR_C, 6
    b R_byte
    nop

R_pitchL:                           # pitch = (pitch & 0x3f00) | val
    lhu t2, %lo(PRM + RSPPREP_PITCH)(t4)
    andi t2, t2, 0x3f00
    b R_pitch
    or t2, t2, a1
R_pitchH:                           # pitch = ((pitch & 0xff) | val << 8) & 0x3fff
    lhu t2, %lo(PRM + RSPPREP_PITCH)(t4)
    andi t2, t2, 0xff
    sll t3, a1, 8
    or t2, t2, t3
    andi t2, t2, 0x3fff
R_pitch:
    sh t2, %lo(PRM + RSPPREP_PITCH)(t4)
    HALFV RSPDSP_V_PITCH, t2
    b R_byte
    nop

R_srcn:
    HALFV RSPDSP_V_SRCN, a1
    b R_byte
    nop

R_adsr1:                            # rates 0 and 1, useGain = !(val & 0x80)
    andi t2, a1, 0xf
    sll t2, t2, 1
    addiu t2, t2, 1
    RATE t2
    HALFV RSPDSP_V_RATE0, t3
    andi t2, a1, 0x70
    srl t2, t2, 3                   # ((val & 0x70) >> 4) * 2
    addiu t2, t2, 16
    RATE t2
    HALFV RSPDSP_V_RATE1, t3
    lhu t2, %lo(PRM + RSPPREP_USEGAIN)
    li t3, 1
    sllv t3, t3, a2
    or t2, t2, t3                   # set, then clear if val & 0x80
    andi t0, a1, 0x80
    beqz t0, 4f
    nop
    xor t2, t2, t3
4:  sh t2, %lo(PRM + RSPPREP_USEGAIN)
    HALFI RSPDSP_M_USEGAIN, t2
    b R_byte
    nop

R_adsr2:                            # rate 2, sustain = ((val >> 5) + 1) * 0x100
    andi t2, a1, 0x1f
    RATE t2
    HALFV RSPDSP_V_RATE2, t3
    srl t2, a1, 5
    addiu t2, t2, 1
    sll t2, t2, 8
    HALFV RSPDSP_V_SUSTAIN, t2
    b R_byte
    nop

R_gain:                             # directGain = !(val & 0x80)
    lhu t2, %lo(PRM + RSPPREP_DIRECTGAIN)
    li t3, 1
    sllv t3, t3, a2
    or t2, t2, t3
    andi t0, a1, 0x80
    beqz t0, 5f                     # direct: gainValue = (val & 0x7f) * 16
    nop
    xor t2, t2, t3                  # else gainMode = (val & 0x60) >> 5,
    andi t3, a1, 0x60               #      rate 3 = rateValues[val & 0x1f]
    srl t3, t3, 5
    sh t3, %lo(PRM + RSPPREP_GAINMODE)(t4)
    andi t0, a1, 0x1f
    RATE t0
    b 6f
    sh t3, %lo(PRM + RSPPREP_RATE3)(t4)
5:  andi t3, a1, 0x7f
    sll t3, t3, 4
    sh t3, %lo(PRM + RSPPREP_GAINVAL)(t4)
6:  sh t2, %lo(PRM + RSPPREP_DIRECTGAIN)
    HALFI RSPDSP_M_DIRECTGAIN, t2
    lhu t2, %lo(PRM + RSPPREP_GAINMODE)(t4)
    HALFV RSPDSP_V_GAINMODE, t2
    lhu t2, %lo(PRM + RSPPREP_RATE3)(t4)
    HALFV RSPDSP_V_RATE3, t2
    lhu t2, %lo(PRM + RSPPREP_GAINVAL)(t4)
    HALFV RSPDSP_V_GAINVAL, t2
    b R_byte
    nop

R_fir:
    HALFV RSPDSP_FIR_VALS, t1
    UCV RSPDSP_FIR_U, RSPDSP_FIR_C, 6
    b R_byte
    nop

R_xC:                               # $0C-$7C by voice field
    beqz a2, C_mvolL
    addiu t3, a2, -1
    beqz t3, C_mvolR
    addiu t3, a2, -2
    beqz t3, C_evolL
    addiu t3, a2, -3
    beqz t3, C_evolR
    addiu t3, a2, -4
    beqz t3, C_kon
    addiu t3, a2, -5
    beqz t3, C_kof
    addiu t3, a2, -6
    beqz t3, C_flg
    nop
    b R_byte                        # $7C: ENDX, the byte is cleared
    move a1, zero
C_mvolL:
    HALFI RSPDSP_MVOLL, t1
    UCI RSPDSP_SC_U + 0, RSPDSP_SC_C + 0, 7
    b R_byte
    nop
C_mvolR:
    HALFI RSPDSP_MVOLR, t1
    UCI RSPDSP_SC_U + 2, RSPDSP_SC_C + 2, 7
    b R_byte
    nop
C_evolL:
    HALFI RSPDSP_EVOLL, t1
    UCI RSPDSP_SC_U + 4, RSPDSP_SC_C + 4, 7
    b R_byte
    nop
C_evolR:
    HALFI RSPDSP_EVOLR, t1
    UCI RSPDSP_SC_U + 6, RSPDSP_SC_C + 6, 7
    b R_byte
    nop
C_kon:
    HALFI RSPDSP_M_KEYON, a1
    b R_byte
    nop
C_kof:
    HALFI RSPDSP_M_KEYOFF, a1
    b R_byte
    nop
C_flg:                              # reset, mute, echo writes, noise rate
    srl t2, a1, 7
    HALFI RSPDSP_RESET, t2
    srl t2, a1, 6
    andi t2, t2, 1
    HALFI RSPDSP_MUTE, t2
    srl t2, a1, 5
    andi t2, t2, 1
    xori t2, t2, 1
    HALFI RSPDSP_ECHOWRITES, t2
    andi t2, a1, 0x1f
    RATE t2
    HALFI RSPDSP_NOISE_RATE, t3
    b R_byte
    nop

R_xD:                               # $0D-$7D by voice field
    beqz a2, D_efb
    addiu t3, a2, -2
    beqz t3, D_pmon
    addiu t3, a2, -3
    beqz t3, D_non
    addiu t3, a2, -4
    beqz t3, D_eon
    addiu t3, a2, -5
    beqz t3, D_dir
    addiu t3, a2, -6
    beqz t3, D_esa
    addiu t3, a2, -7
    beqz t3, D_edl
    nop
    b R_byte                        # $1D: the register file only
    nop
D_efb:
    HALFI RSPDSP_FEEDBACK, t1
    UCI RSPDSP_SC_U + 8, RSPDSP_SC_C + 8, 7
    UCI RSPDSP_SC_U + 10, RSPDSP_SC_C + 10, 7
    b R_byte
    nop
D_pmon:
    HALFI RSPDSP_M_PMOD, a1
    b R_byte
    nop
D_non:
    HALFI RSPDSP_M_NOISE, a1
    b R_byte
    nop
D_eon:                              # the mask, then 0xffff per enabled voice
    HALFI RSPDSP_M_ECHO, a1
    move t6, zero
    move t4, a1
7:  andi t2, t4, 1
    subu t2, zero, t2
    HALFV RSPDSP_V_ECHOMASK, t2
    srl t4, t4, 1
    lui t3, 2
    addu t6, t6, t3
    lui t3, 16
    bne t6, t3, 7b
    nop
    b R_byte
    nop
D_dir:
    sll t2, a1, 8
    HALFI RSPDSP_DIRPAGE, t2
    b R_byte
    nop
D_esa:
    sll t2, a1, 8
    HALFI RSPDSP_ECHOADR, t2
    b R_byte
    nop
D_edl:                              # (val & 15) * 512, or 1 for 0
    andi t2, a1, 15
    sll t2, t2, 9
    bnez t2, 8f
    nop
    li t2, 1
8:  HALFI RSPDSP_ECHODELAY, t2
    b R_byte
    nop

R_byte:                             # the register file byte
    ori t2, k0, (RSPDSP_LOG_BYTE << 8)
    addiu t3, a0, RSPDSP_RAM
    sll t3, t3, 16
    or t3, t3, a1
    sw t2, 0(s5)
    sw t3, 4(s5)
    b Prep_loop
    addiu s5, s5, 8
