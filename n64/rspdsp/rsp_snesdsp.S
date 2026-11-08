# S-DSP on the RSP: runs batches of samples for the deferred DSP.
#
# The CPU converts the canonical C DSP state (runner/src/snes/dsp.c) into an
# RspDspState block (snesdsp_rsp_layout.h), queues SnesDspCmd_Run, and
# converts the block back when the batch is done. Arithmetic matches dsp.c
# bit for bit (n64/rspdsp/test compares the two on random states):
#
# - Integer shifts of products truncate (floor), as in C on this target.
# - Products the C code shifts are formed exactly on the vector unit
#   (VMUDH gives the full 32-bit product, VMUDM gives floor(a*b/65536)) and
#   finished on the scalar unit where C shifts or saturates sequentially.
# - The mixing order (voice 0..7, saturating after each voice) is kept.
#
# Not handled here (the CPU runs such batches in C): pitch modulation, the
# host-only shadow mixer and output filter.
#
# Per batch: apply logged writes due before sample k, run sample k, ...;
# after the last sample, apply writes logged after it. Output samples go to
# P_OUT as int16 L/R pairs.

#include <rsp_queue.inc>
#include "snesdsp_rsp_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand SnesDspCmd_Run, 8
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8               # rspq needs a non-empty saved state
RSPQ_EndSavedState

    .align 4
DSP:        .ds.b RSPDSP_STATE_SIZE

#include "rsp_snesdsp_tables.inc"

    .align 4
STG_T0:     .ds.b 16        # interpolation taps (doubled samples), per voice
STG_T1:     .ds.b 16
STG_T2:     .ds.b 16
STG_T3:     .ds.b 16
STG_G0:     .ds.b 16        # gauss * 32, per voice
STG_G1:     .ds.b 16
STG_G2:     .ds.b 16
STG_G3:     .ds.b 16
STG_RAW:    .ds.b 16        # sample before gain
STG_G32:    .ds.b 16        # gain << 5
STG_A:      .ds.b 16        # generic multiply operands / results
STG_B:      .ds.b 16
STG_HI:     .ds.b 16
STG_MD:     .ds.b 16
STG_HI2:    .ds.b 16
STG_MD2:    .ds.b 16
SCR_BRR:    .ds.b 24        # BRR block window (8-byte aligned DMA)
    .align 3
SCR_DIR:    .ds.b 16
SCR_ECHO:   .ds.b 16
SCR_RMW:    .ds.b 16
    .align 3
LOGBUF:     .ds.b 32 * RSPDSP_LOG_ENTRY
OUTBUF:     .ds.b 32 * 4 + 8

    .text

#define LOG_CHUNK 32

# Persistent registers during a run:
#   s7 state RDRAM address   s3 return address into rspq
#   s6 samples in the batch  s5 current sample index
#   s2 log entries left      s1 RDRAM address of the next log chunk
#   fp samples in OUTBUF
# The DMEM log position (LOG_PTR, LOG_LEFT) lives in DMEM.
# DMA helpers clobber t0, t2, at, s0, s4.

# a0: RDRAM address of the RspDspState block (8-byte aligned)
SnesDspCmd_Run:
    move s3, ra
    li t0, 0xffffff
    and s0, a0, t0                  # top byte of a0 is the command id
    move s7, s0
    li s4, %lo(DSP)
    jal DMAIn
    li t0, DMA_SIZE(RSPDSP_STATE_SIZE, 1)

    lhu s6, %lo(DSP + RSPDSP_P_SAMPLES)
    lhu s2, %lo(DSP + RSPDSP_P_LOGN)
    lw s1, %lo(DSP + RSPDSP_P_LOG)
    sw zero, %lo(LOG_LEFT)
    move fp, zero
    move s5, zero

Run_loop:
    jal Log_ApplyDue                # writes logged before sample s5
    nop
    beq s5, s6, Run_done
    nop
    jal Dsp_Sample
    nop
    b Run_loop
    addiu s5, s5, 1

Run_done:
    jal Out_Flush
    nop
    move ra, s3                     # DMAOut returns straight to rspq
    move s0, s7
    li s4, %lo(DSP)
    j DMAOut
    li t0, DMA_SIZE(RSPDSP_STATE_SIZE, 1)

########################################################################
# Logged writes
########################################################################

# Apply every log entry whose tag equals s5.
Log_ApplyDue:
    move v1, ra
    lw t8, %lo(LOG_PTR)
    lw t9, %lo(LOG_LEFT)
Log_next:
    beqz s2, Log_ret
    nop
    bnez t9, Log_have
    nop
    # Fetch the next chunk of entries.
    move s0, s1
    li s4, %lo(LOGBUF)
    jal DMAIn
    li t0, DMA_SIZE(LOG_CHUNK * RSPDSP_LOG_ENTRY, 1)
    addiu s1, s1, LOG_CHUNK * RSPDSP_LOG_ENTRY
    li t8, %lo(LOGBUF)
    li t9, LOG_CHUNK
Log_have:
    lhu t1, 0(t8)                   # tag
    bne t1, s5, Log_ret
    nop
    lbu t3, 2(t8)                   # kind
    lbu t1, 3(t8)                   # ARAM address bit 16 (mirror writes)
    lhu a0, 4(t8)                   # ARAM address / state offset
    sll t1, t1, 16
    or a0, a0, t1
    lhu a1, 6(t8)                   # value
    addiu t8, t8, RSPDSP_LOG_ENTRY
    addiu t9, t9, -1
    beqz t3, Log_aram
    addiu s2, s2, -1
    addiu t3, t3, -1
    bnez t3, Log_byte
    nop
    b Log_next
    sh a1, %lo(DSP)(a0)             # state halfword (decoded register write)
Log_byte:
    b Log_next
    sb a1, %lo(DSP)(a0)             # state byte (register file)
Log_aram:
    sw t8, %lo(LOG_PTR)
    jal Aram_Rmw
    sw t9, %lo(LOG_LEFT)
    lw t8, %lo(LOG_PTR)
    b Log_next
    lw t9, %lo(LOG_LEFT)
Log_ret:
    sw t8, %lo(LOG_PTR)
    jr v1
    sw t9, %lo(LOG_LEFT)

# Write byte a1 to ARAM address a0 (read-modify-write of 8 bytes). Writes
# to $0000-$000F arrive twice from the CPU: once more for the mirror.
Aram_Rmw:                           # a0: ARAM offset (may be >= $10000), a1: value
    move t7, ra
    lw t1, %lo(DSP + RSPDSP_P_ARAM)
    li t3, ~7
    and t3, a0, t3
    addu s0, t1, t3
    li s4, %lo(SCR_RMW)
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
    andi t3, a0, 7
    sb a1, %lo(SCR_RMW)(t3)
    lw t1, %lo(DSP + RSPDSP_P_ARAM)
    li t3, ~7
    and t3, a0, t3
    addu s0, t1, t3
    li s4, %lo(SCR_RMW)
    jal DMAOut
    li t0, DMA_SIZE(8, 1)
    jr t7
    nop

# Read 8 bytes of ARAM containing offset a0 into SCR_DIR; returns the DMEM
# address of the requested byte in v0. (Used for directory entries.)
Aram_Read8:
    move t7, ra
    lw t1, %lo(DSP + RSPDSP_P_ARAM)
    li t3, ~7
    and t3, a0, t3
    addu s0, t1, t3
    li s4, %lo(SCR_DIR)
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
    andi t3, a0, 7
    jr t7
    addiu v0, t3, %lo(SCR_DIR)

# Directory entry for SRCN a2: v0 = start address, v1 = loop address.
Dir_Entry:
    move t6, ra
    lhu a0, %lo(DSP + RSPDSP_DIRPAGE)
    sll t1, a2, 2
    addu a0, a0, t1
    jal Aram_Read8
    andi a0, a0, 0xffff
    lbu t1, 0(v0)
    lbu t3, 1(v0)
    sll t3, t3, 8
    or a3, t1, t3                   # start
    lbu t1, 2(v0)
    lbu t3, 3(v0)
    sll t3, t3, 8
    or v1, t1, t3                   # loop
    jr t6
    move v0, a3

########################################################################
# One sample (dsp_cycle)
########################################################################

# Saturate register r to int16 using temporaries t, u.
.macro CLAMP16 r, t, u
    sra \t, \r, 15
    sra \u, \r, 31
    beq \t, \u, 1f
    nop
    li \r, 32767
    xor \r, \r, \u
1:
.endm

Dsp_Sample:
    move v1, ra
    sw v1, %lo(SAVE_RA)

    # --- A: pitch counters; decode a BRR block on wrap -----------------
    move a2, zero                   # voice
A_voice:
    sll t4, a2, 1
    lhu t1, %lo(DSP + RSPDSP_V_PITCH)(t4)
    lhu t3, %lo(DSP + RSPDSP_V_COUNTER)(t4)
    addu t1, t1, t3
    sh t1, %lo(DSP + RSPDSP_V_COUNTER)(t4)
    srl t1, t1, 16
    beqz t1, A_next
    nop
    jal Brr_Decode
    nop
A_next:
    addiu a2, a2, 1
    sltiu t1, a2, 8
    bnez t1, A_voice
    nop

    # --- B: Gaussian interpolation for all voices -----------------------
    move a2, zero
B_voice:
    sll t4, a2, 1
    lhu t1, %lo(DSP + RSPDSP_V_COUNTER)(t4)
    srl t3, t1, 12                  # sample index
    sll t3, t3, 1
    li t5, RSPDSP_DECODE_STRIDE
    # t6 = voice * 40 (= voice * 32 + voice * 8)
    sll t6, a2, 5
    sll t7, a2, 3
    addu t6, t6, t7
    addu t6, t6, t3
    lh t5, %lo(DSP + RSPDSP_DECODE + 0)(t6)
    sh t5, %lo(STG_T0)(t4)
    lh t5, %lo(DSP + RSPDSP_DECODE + 2)(t6)
    sh t5, %lo(STG_T1)(t4)
    lh t5, %lo(DSP + RSPDSP_DECODE + 4)(t6)
    sh t5, %lo(STG_T2)(t4)
    lh t5, %lo(DSP + RSPDSP_DECODE + 6)(t6)
    sh t5, %lo(STG_T3)(t4)
    srl t1, t1, 4
    andi t1, t1, 0xff               # offset
    sll t1, t1, 1
    li t5, 0x1fe
    subu t6, t5, t1                 # (0xff - offset) * 2
    lhu t5, %lo(GAUSS32)(t6)
    sh t5, %lo(STG_G0)(t4)
    lhu t5, %lo(GAUSS32 + 0x200)(t6) # 0x1ff - offset
    sh t5, %lo(STG_G1)(t4)
    lhu t5, %lo(GAUSS32 + 0x200)(t1) # 0x100 + offset
    sh t5, %lo(STG_G2)(t4)
    lhu t5, %lo(GAUSS32)(t1)        # offset
    sh t5, %lo(STG_G3)(t4)
    addiu a2, a2, 1
    sltiu t1, a2, 8
    bnez t1, B_voice
    nop
    li t1, %lo(STG_T0)
    lqv $v01, 0x00, t1
    lqv $v02, 0x10, t1
    lqv $v03, 0x20, t1
    lqv $v04, 0x30, t1
    lqv $v05, 0x40, t1
    lqv $v06, 0x50, t1
    lqv $v07, 0x60, t1
    lqv $v08, 0x70, t1
    vmudm $v09, $v01, $v05          # floor(2s * 32g / 65536) = (g*s) >> 10
    vmudm $v10, $v02, $v06
    vmudm $v11, $v03, $v07
    vmudm $v12, $v04, $v08
    vaddc $v13, $v09, $v10          # 16-bit wrap: the (int16) cast in C
    vaddc $v13, $v13, $v11
    ctc2 zero, COP2_CTRL_VCO
    vadd $v13, $v13, $v12           # saturating add: the clamp
    vmudm $v13, $v13, $v31,e(0)     # * 0x8000 >> 16: >> 1 (arithmetic)
    sqv $v13, 0x00, t1              # STG_T0 doubles as the raw sample
    # Noise voices take the noise sample instead.
    lhu t3, %lo(DSP + RSPDSP_M_NOISE)
    lh t5, %lo(DSP + RSPDSP_NOISE_SAMPLE)
    move a2, zero
B_noise:
    andi t6, t3, 1
    beqz t6, 1f
    sll t4, a2, 1
    sh t5, %lo(STG_T0)(t4)
1:  srl t3, t3, 1
    addiu a2, a2, 1
    sltiu t1, a2, 8
    bnez t1, B_noise
    nop

    # --- C: key on/off, reset, envelope ------------------------------
    move a2, zero
C_voice:
    jal Envelope
    nop
    addiu a2, a2, 1
    sltiu t1, a2, 8
    bnez t1, C_voice
    nop

    # --- D: gain, ENVX/OUTX --------------------------------------------
    li t1, %lo(STG_T0)
    lqv $v01, 0x00, t1
    li t1, %lo(STG_G32)
    lqv $v02, 0x00, t1
    vmudm $v03, $v01, $v02          # (sample * gain) >> 11
    li t1, %lo(DSP + RSPDSP_V_SAMPLEOUT)
    sqv $v03, 0x00, t1
    move a2, zero
D_voice:
    sll t4, a2, 1
    sll t6, a2, 4                   # register base $x0
    lhu t1, %lo(DSP + RSPDSP_V_GAIN)(t4)
    srl t1, t1, 4
    sb t1, %lo(DSP + RSPDSP_RAM + 8)(t6)
    lh t1, %lo(DSP + RSPDSP_V_SAMPLEOUT)(t4)
    sra t1, t1, 7
    sb t1, %lo(DSP + RSPDSP_RAM + 9)(t6)
    addiu a2, a2, 1
    sltiu t1, a2, 8
    bnez t1, D_voice
    nop

    # --- E: voice volumes, dry mix and echo input ----------------------
    li t1, %lo(DSP + RSPDSP_V_VOLL)
    lqv $v02, 0x00, t1
    lqv $v04, 0x10, t1              # V_VOLR follows V_VOLL
    vmudh $v05, $v03, $v02          # 32-bit products sample * volL
    vsar $v06, COP2_ACC_HI
    vsar $v07, COP2_ACC_MD
    li t1, %lo(STG_HI)
    sqv $v06, 0x00, t1
    sqv $v07, 0x10, t1              # STG_MD
    vmudh $v05, $v03, $v04
    vsar $v06, COP2_ACC_HI
    vsar $v07, COP2_ACC_MD
    sqv $v06, 0x20, t1              # STG_HI2
    sqv $v07, 0x30, t1              # STG_MD2
    move a0, zero                   # totalL
    move a1, zero                   # totalR
    move a3, zero                   # echo inL
    move v0, zero                   # echo inR
    move a2, zero
E_voice:
    sll t4, a2, 1
    lh t1, %lo(STG_HI)(t4)
    lhu t3, %lo(STG_MD)(t4)
    sll t1, t1, 16
    or t1, t1, t3
    sra t8, t1, 6                   # (sample * volL) >> 6
    lh t5, %lo(STG_HI2)(t4)
    lhu t3, %lo(STG_MD2)(t4)
    sll t5, t5, 16
    or t5, t5, t3
    sra t5, t5, 6                   # (sample * volR) >> 6
    jal Clamp_t1
    addu t1, a0, t8
    move a0, t1
    jal Clamp_t1
    addu t1, a1, t5
    move a1, t1
    lhu t3, %lo(DSP + RSPDSP_M_ECHO)
    srlv t3, t3, a2
    andi t3, t3, 1
    beqz t3, E_next
    nop
    jal Clamp_t1
    addu t1, a3, t8
    move a3, t1
    jal Clamp_t1
    addu t1, v0, t5
    move v0, t1
E_next:
    addiu a2, a2, 1
    sltiu t1, a2, 8
    bnez t1, E_voice
    nop

    # --- F: master volume ---------------------------------------------
    sh a0, %lo(STG_A + 0)
    sh a1, %lo(STG_A + 2)
    lh t1, %lo(DSP + RSPDSP_MVOLL)
    sh t1, %lo(STG_B + 0)
    lh t1, %lo(DSP + RSPDSP_MVOLR)
    jal Mul_AB
    sh t1, %lo(STG_B + 2)
    jal Prod
    li t4, 0
    jal Clamp_t1
    sra t1, t1, 7
    move a0, t1
    jal Prod
    li t4, 2
    jal Clamp_t1
    sra t1, t1, 7
    move a1, t1

    # --- G: echo -------------------------------------------------------
    jal Echo
    nop

    # --- H: mute, noise, output, even/odd ------------------------------
    lhu t1, %lo(DSP + RSPDSP_MUTE)
    beqz t1, 1f
    nop
    move a0, zero
    move a1, zero
1:  jal Noise
    nop
    sll t1, fp, 2
    sh a0, %lo(OUTBUF + 0)(t1)
    sh a1, %lo(OUTBUF + 2)(t1)
    addiu fp, fp, 1
    li t1, 32
    bne fp, t1, 1f
    nop
    jal Out_Flush
    nop
1:  lhu t1, %lo(DSP + RSPDSP_EVENCYCLE)
    xori t1, t1, 1
    sh t1, %lo(DSP + RSPDSP_EVENCYCLE)
    lw ra, %lo(SAVE_RA)
    jr ra
    nop

    .data
    .align 2
SAVE_RA:    .word 0
SAVE_RA2:   .word 0
SAVE_RA3:   .word 0
LOG_PTR:    .word 0
LOG_LEFT:   .word 0
    .text

# STG_HI/STG_MD = 32-bit products of STG_A and STG_B (lanes).
Mul_AB:
    li t1, %lo(STG_A)
    lqv $v01, 0x00, t1
    lqv $v02, 0x10, t1              # STG_B
    vmudh $v03, $v01, $v02
    vsar $v04, COP2_ACC_HI
    vsar $v05, COP2_ACC_MD
    sqv $v04, 0x20, t1              # STG_HI
    jr ra
    sqv $v05, 0x30, t1              # STG_MD

# t1 = 32-bit product in lane at byte offset t4 of STG_HI/STG_MD.
Prod:
    lh t1, %lo(STG_HI)(t4)
    lhu t3, %lo(STG_MD)(t4)
    sll t1, t1, 16
    jr ra
    or t1, t1, t3

# Echo helper: t1 = clamp16(base + (product(t4) >> 7)); base is a0, a1 or
# a2 for lanes 0, 2 and 4/6.
Prod_Clamp7:
    lh t1, %lo(STG_HI)(t4)
    lhu t3, %lo(STG_MD)(t4)
    sll t1, t1, 16
    or t1, t1, t3
    sra t1, t1, 7
    move t3, a2
    bnez t4, 1f
    nop
    move t3, a0
1:  li t5, 2
    bne t4, t5, 2f
    nop
    move t3, a1
2:  addu t1, t1, t3
    # fall through

# Saturate t1 to int16 (uses t6, t7).
Clamp_t1:
    sra t6, t1, 15
    sra t7, t1, 31
    beq t6, t7, 1f
    nop
    li t1, 32767
    xor t1, t1, t7
1:  jr ra
    nop

# Write OUTBUF (fp samples) to P_OUT and advance it.
Out_Flush:
    beqz fp, 1f
    move t7, ra
    lw s0, %lo(DSP + RSPDSP_P_OUT)
    sll t1, fp, 2
    addu t3, s0, t1
    sw t3, %lo(DSP + RSPDSP_P_OUT)
    addiu t1, t1, 7                 # DMA in whole 8-byte units
    srl t1, t1, 3
    sll t1, t1, 3
    addiu t0, t1, -1                # DMA_SIZE(len, 1) = len - 1
    li s4, %lo(OUTBUF)
    jal DMAOut
    nop
    move fp, zero
1:  jr t7
    nop

########################################################################
# Noise (dsp_handleNoise)
########################################################################
Noise:
    lhu t0, %lo(DSP + RSPDSP_NOISE_RATE)
    beqz t0, 2f
    lhu t1, %lo(DSP + RSPDSP_NOISE_COUNTER)
    addiu t1, t1, 1
    andi t1, t1, 0xffff
    sltu t3, t1, t0
    bnez t3, 1f
    lh t3, %lo(DSP + RSPDSP_NOISE_SAMPLE)
    srl t4, t3, 1
    xor t5, t3, t4
    andi t5, t5, 1
    andi t4, t4, 0x3fff
    sll t5, t5, 14
    or t4, t4, t5
    sll t4, t4, 17
    sra t4, t4, 17
    sh t4, %lo(DSP + RSPDSP_NOISE_SAMPLE)
    move t1, zero
1:  sh t1, %lo(DSP + RSPDSP_NOISE_COUNTER)
2:  jr ra
    nop

########################################################################
# BRR decode (dsp_decodeBrr) for voice a2
########################################################################
Brr_Decode:
    sw ra, %lo(SAVE_RA2)
    sll t4, a2, 1
    sll t6, a2, 5
    sll t7, a2, 3
    addu t6, t6, t7
    sw t6, %lo(BRR_DEC)
    # copy samples 16..18 to 0..2
    lh t1, %lo(DSP + RSPDSP_DECODE + 32)(t6)
    sh t1, %lo(DSP + RSPDSP_DECODE + 0)(t6)
    lh t1, %lo(DSP + RSPDSP_DECODE + 34)(t6)
    sh t1, %lo(DSP + RSPDSP_DECODE + 2)(t6)
    lh t1, %lo(DSP + RSPDSP_DECODE + 36)(t6)
    sh t1, %lo(DSP + RSPDSP_DECODE + 4)(t6)
    lhu t1, %lo(DSP + RSPDSP_V_PREVFLAGS)(t4)
    andi t3, t1, 1                  # flags 1 or 3: end of sample, loop
    beqz t3, Brr_block
    nop
    sw t1, %lo(BRR_PF)
    lhu a2, %lo(DSP + RSPDSP_V_SRCN)(t4)
    sw t4, %lo(BRR_T4)
    jal Dir_Entry
    nop
    lw t4, %lo(BRR_T4)
    srl a2, t4, 1
    sh v1, %lo(DSP + RSPDSP_V_DECOFF)(t4)
    lw t1, %lo(BRR_PF)
    li t3, 1
    bne t1, t3, 1f
    li t3, 4
    sh t3, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
    sh zero, %lo(DSP + RSPDSP_V_GAIN)(t4)
1:  lbu t1, %lo(DSP + RSPDSP_RAM + 0x7c)
    li t3, 1
    sllv t3, t3, a2
    or t1, t1, t3
    sb t1, %lo(DSP + RSPDSP_RAM + 0x7c)
Brr_block:
    # Fetch 16 bytes containing the 9-byte block (the ARAM view mirrors
    # its first 16 bytes after $FFFF, so a wrapping block is contiguous).
    lhu a0, %lo(DSP + RSPDSP_V_DECOFF)(t4)
    lw t1, %lo(DSP + RSPDSP_P_ARAM)
    li t3, ~7
    and t3, a0, t3
    addu s0, t1, t3
    li s4, %lo(SCR_BRR)
    sw t4, %lo(BRR_T4)
    jal DMAIn
    li t0, DMA_SIZE(16, 1)
    lw t4, %lo(BRR_T4)
    andi t3, a0, 7
    addiu v0, t3, %lo(SCR_BRR)      # v0 -> block in DMEM
    addiu a0, a0, 9
    andi a0, a0, 0xffff
    sh a0, %lo(DSP + RSPDSP_V_DECOFF)(t4)
    lbu t1, 0(v0)                   # header
    srl a1, t1, 4                   # shift
    srl a3, t1, 2
    andi a3, a3, 3                  # filter
    andi t1, t1, 3
    sh t1, %lo(DSP + RSPDSP_V_PREVFLAGS)(t4)
    lh t8, %lo(DSP + RSPDSP_V_OLD)(t4)
    lh t9, %lo(DSP + RSPDSP_V_OLDER)(t4)
    lw t6, %lo(BRR_DEC)
    addiu t6, t6, %lo(DSP + RSPDSP_DECODE + 6)   # output: sample 3
    li t7, 16                        # nibbles left
Brr_byte:
    lbu t1, 1(v0)
    addiu v0, v0, 1
    srl s0, t1, 4                    # high nibble first
    jal Brr_Nibble
    nop
    andi s0, t1, 0x0f
    jal Brr_Nibble
    nop
    addiu t7, t7, -2
    bnez t7, Brr_byte
    nop
    lw t4, %lo(BRR_T4)
    sh t8, %lo(DSP + RSPDSP_V_OLD)(t4)
    sh t9, %lo(DSP + RSPDSP_V_OLDER)(t4)
    lw ra, %lo(SAVE_RA2)
    srl a2, t4, 1
    jr ra
    nop

# One nibble s0 (0..15): shift a1, filter a3, old t8, older t9; stores the
# doubled result at t6 and advances it. Keeps t1.
Brr_Nibble:
    sll s0, s0, 28
    sra s0, s0, 28                   # sign-extend 4 bits
    sltiu t3, a1, 13
    beqz t3, 1f
    nop
    sllv s0, s0, a1
    b 2f
    sra s0, s0, 1
1:  li t3, ~0x7ff
    and s0, s0, t3
2:  beqz a3, 5f
    li t3, 1
    beq a3, t3, 3f
    li t3, 2
    beq a3, t3, 4f
    nop
    # filter 3: s += 2*old + ((13 * -old) >> 6) - older + ((3 * older) >> 4)
    subu t3, zero, t8                # -old
    sll t5, t3, 3
    sll s4, t3, 2
    addu t5, t5, s4
    addu t5, t5, t3                  # 13 * -old
    sra t5, t5, 6
    sll s4, t8, 1
    addu t5, t5, s4
    subu t5, t5, t9
    sll s4, t9, 1
    addu s4, s4, t9                  # 3 * older
    sra s4, s4, 4
    addu t5, t5, s4
    b 5f
    addu s0, s0, t5
3:  # filter 1: s += old + (-old >> 4)
    subu t3, zero, t8
    sra t3, t3, 4
    addu t3, t3, t8
    b 5f
    addu s0, s0, t3
4:  # filter 2: s += 2*old + ((3 * -old) >> 5) - older + (older >> 4)
    subu t3, zero, t8
    sll t5, t3, 1
    addu t5, t5, t3                  # 3 * -old
    sra t5, t5, 5
    sll s4, t8, 1
    addu t5, t5, s4
    subu t5, t5, t9
    sra s4, t9, 4
    addu t5, t5, s4
    addu s0, s0, t5
5:  CLAMP16 s0, t3, t5
    sll s0, s0, 17
    sra s0, s0, 17                   # clip to 15 bits
    move t9, t8
    move t8, s0
    sll t3, s0, 1                    # stored doubled
    sh t3, 0(t6)
    jr ra
    addiu t6, t6, 2

    .data
    .align 2
BRR_DEC:    .word 0
BRR_PF:     .word 0
BRR_T4:     .word 0
    .text

########################################################################
# Key on/off, reset and envelope for voice a2 (dsp_cycleChannel part 2)
########################################################################
Envelope:
    sw ra, %lo(SAVE_RA3)
    sll t4, a2, 1
    li t5, 1
    sllv t5, t5, a2                  # voice bit
    lhu t1, %lo(DSP + RSPDSP_EVENCYCLE)
    beqz t1, Env_reset
    nop
    lhu t1, %lo(DSP + RSPDSP_M_KEYOFF)
    and t1, t1, t5
    beqz t1, 1f
    li t3, 4
    b Env_reset
    sh t3, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
1:  lhu t1, %lo(DSP + RSPDSP_M_KEYON)
    and t3, t1, t5
    beqz t3, Env_reset
    nop
    xor t1, t1, t5                   # keyOn = false
    sh t1, %lo(DSP + RSPDSP_M_KEYON)
    sh zero, %lo(DSP + RSPDSP_V_PREVFLAGS)(t4)
    sw t4, %lo(BRR_T4)
    lhu a2, %lo(DSP + RSPDSP_V_SRCN)(t4)
    jal Dir_Entry
    nop
    lw t4, %lo(BRR_T4)
    srl a2, t4, 1
    sh v0, %lo(DSP + RSPDSP_V_DECOFF)(t4)
    # clear the decode buffer (20 int16)
    sll t6, a2, 5
    sll t7, a2, 3
    addu t6, t6, t7
    addiu t6, t6, %lo(DSP + RSPDSP_DECODE)
    li t7, 10
2:  sw zero, 0(t6)
    addiu t7, t7, -1
    bnez t7, 2b
    addiu t6, t6, 4
    sh zero, %lo(DSP + RSPDSP_V_GAIN)(t4)
    sh zero, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
    li t5, 1
    sllv t5, t5, a2
Env_reset:
    lhu t1, %lo(DSP + RSPDSP_RESET)
    beqz t1, 1f
    li t3, 4
    sh t3, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
    sh zero, %lo(DSP + RSPDSP_V_GAIN)(t4)
1:  # envelope
    lhu a0, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
    lhu a1, %lo(DSP + RSPDSP_V_GAIN)(t4)
    li t3, 4
    beq a0, t3, Env_release
    nop
    lhu t1, %lo(DSP + RSPDSP_M_USEGAIN)
    and t1, t1, t5
    sltu a3, zero, t1                # a3 = liveGain
    beqz a3, 2f
    nop
    lhu t1, %lo(DSP + RSPDSP_M_DIRECTGAIN)
    and t1, t1, t5
    beqz t1, 3f
    nop
    lhu a1, %lo(DSP + RSPDSP_V_GAINVAL)(t4)   # direct gain
    b Env_store
    nop
3:  lhu t1, %lo(DSP + RSPDSP_V_RATE3)(t4)
    b 4f
    nop
2:  sltiu t3, a0, 3
    bnez t3, 5f
    move t3, a0
    li t3, 2
5:  sll t3, t3, 4                    # stage * 16: rate vectors are 16 bytes apart
    addu t3, t3, t4
    lhu t1, %lo(DSP + RSPDSP_V_RATE0)(t3)
4:  beqz t1, Env_store               # rate 0: frozen
    nop
    lhu t3, %lo(DSP + RSPDSP_V_RATECTR)(t4)
    addiu t3, t3, 1
    andi t3, t3, 0xffff
    sltu t6, t3, t1
    beqz t6, 6f
    nop
    b Env_store
    sh t3, %lo(DSP + RSPDSP_V_RATECTR)(t4)
6:  sh zero, %lo(DSP + RSPDSP_V_RATECTR)(t4)
    # handleGain (not releasing)
    beqz a3, Env_adsr
    nop
    lhu t1, %lo(DSP + RSPDSP_V_GAINMODE)(t4)
    beqz t1, Env_lindec
    li t3, 1
    beq t1, t3, Env_expdec
    li t3, 2
    beq t1, t3, Env_lininc
    nop
    # bent increase
    sltiu t3, a1, 0x600
    li t1, 8
    beqz t3, 7f
    nop
    li t1, 32
7:  b Env_inc
    addu a1, a1, t1
Env_lininc:
    addiu a1, a1, 32
Env_inc:
    sltiu t3, a1, 0x800
    bnez t3, Env_stage
    nop
    b Env_stage
    li a1, 0x7ff
Env_lindec:
    addiu a1, a1, -32
    andi a1, a1, 0xffff
    sltiu t3, a1, 0x800
    bnez t3, Env_stage
    nop
    b Env_stage
    move a1, zero
Env_expdec:
    addiu t3, a1, -1
    sra t3, t3, 8
    addiu t3, t3, 1
    subu a1, a1, t3
    b Env_stage
    andi a1, a1, 0xffff
Env_adsr:
    bnez a0, Env_expdec              # decay and sustain
    nop
    lhu t1, %lo(DSP + RSPDSP_V_RATE0)(t4)   # attack
    li t3, 1
    li t6, 32
    bne t1, t3, 8f
    nop
    li t6, 1024
8:  b Env_inc
    addu a1, a1, t6
Env_stage:
    bnez a0, 9f
    nop
    sltiu t3, a1, 0x7e0              # attack -> decay
    bnez t3, Env_store
    nop
    li t3, 1
    sh t3, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
    sltiu t3, a1, 0x800
    bnez t3, Env_store
    nop
    b Env_store
    li a1, 0x7ff
9:  li t3, 1
    bne a0, t3, Env_store
    nop
    lhu t1, %lo(DSP + RSPDSP_V_SUSTAIN)(t4)
    sltu t3, a1, t1
    beqz t3, Env_store
    li t3, 2
    b Env_store
    sh t3, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
Env_release:
    addiu a1, a1, -8
    andi a1, a1, 0xffff
    sltiu t3, a1, 0x800
    bnez t3, Env_store
    nop
    move a1, zero
Env_store:
    sh a1, %lo(DSP + RSPDSP_V_GAIN)(t4)
    sll t1, a1, 5
    sh t1, %lo(STG_G32)(t4)
    lw ra, %lo(SAVE_RA3)
    jr ra
    nop

########################################################################
# Echo (dsp_handleEcho). In: a0/a1 totals, a3/v0 echo input.
# Out: a0/a1 totals with echo.
########################################################################
Echo:
    sw ra, %lo(SAVE_RA2)
    sw a3, %lo(ECHO_INL)
    sw v0, %lo(ECHO_INR)
    lhu t1, %lo(DSP + RSPDSP_ECHOINDEX)
    lhu t3, %lo(DSP + RSPDSP_ECHOADR)
    sll t1, t1, 2
    addu t1, t1, t3
    andi t1, t1, 0xffff
    sw t1, %lo(ECHO_ADR)
    lw t3, %lo(DSP + RSPDSP_P_ARAM)
    li t5, ~7
    and t5, t1, t5
    addu s0, t3, t5
    li s4, %lo(SCR_ECHO)
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
    lw t1, %lo(ECHO_ADR)
    andi t1, t1, 4
    addiu v1, t1, %lo(SCR_ECHO)      # v1 -> 4 echo bytes
    lhu t6, %lo(DSP + RSPDSP_FIRINDEX)
    sll t6, t6, 1
    # firL[fi] = (int16)(b0 | b1 << 8) >> 1, kept twice for rotation
    lbu t1, 0(v1)
    lbu t3, 1(v1)
    sll t3, t3, 8
    or t1, t1, t3
    sll t1, t1, 16
    sra t1, t1, 17
    sh t1, %lo(DSP + RSPDSP_FIR_L)(t6)
    sh t1, %lo(DSP + RSPDSP_FIR_L + 16)(t6)
    lbu t1, 2(v1)
    lbu t3, 3(v1)
    sll t3, t3, 8
    or t1, t1, t3
    sll t1, t1, 16
    sra t1, t1, 17
    sh t1, %lo(DSP + RSPDSP_FIR_R)(t6)
    sh t1, %lo(DSP + RSPDSP_FIR_R + 16)(t6)
    # rotated history: lane i = fir[(fi + i + 1) & 7]
    addiu t6, t6, 2
    li t7, 0
1:  lh t1, %lo(DSP + RSPDSP_FIR_L)(t6)
    sh t1, %lo(STG_A)(t7)
    lh t1, %lo(DSP + RSPDSP_FIR_R)(t6)
    sh t1, %lo(STG_T1)(t7)
    addiu t6, t6, 2
    addiu t7, t7, 2
    sltiu t1, t7, 16
    bnez t1, 1b
    nop
    li t1, %lo(DSP + RSPDSP_FIR_VALS)
    lqv $v02, 0x00, t1
    li t1, %lo(STG_A)
    lqv $v01, 0x00, t1
    vmudh $v03, $v01, $v02
    vsar $v04, COP2_ACC_HI
    vsar $v05, COP2_ACC_MD
    li t1, %lo(STG_HI)
    sqv $v04, 0x00, t1
    sqv $v05, 0x10, t1               # STG_MD
    li t1, %lo(STG_T1)
    lqv $v01, 0x00, t1
    vmudh $v03, $v01, $v02
    vsar $v04, COP2_ACC_HI
    vsar $v05, COP2_ACC_MD
    li t1, %lo(STG_HI2)
    sqv $v04, 0x00, t1
    sqv $v05, 0x10, t1               # STG_MD2
    jal Fir_Sum
    li t9, %lo(STG_HI)
    move a2, t1                      # sumL
    jal Fir_Sum
    li t9, %lo(STG_HI2)
    move t8, t1                      # sumR
    # echo volumes and feedback: [sumL*evolL, sumR*evolR, sumL*fb, sumR*fb]
    sh a2, %lo(STG_A + 0)
    sh t8, %lo(STG_A + 2)
    sh a2, %lo(STG_A + 4)
    sh t8, %lo(STG_A + 6)
    lh t1, %lo(DSP + RSPDSP_EVOLL)
    sh t1, %lo(STG_B + 0)
    lh t1, %lo(DSP + RSPDSP_EVOLR)
    sh t1, %lo(STG_B + 2)
    lh t1, %lo(DSP + RSPDSP_FEEDBACK)
    sh t1, %lo(STG_B + 4)
    jal Mul_AB
    sh t1, %lo(STG_B + 6)
    jal Prod_Clamp7
    li t4, 0
    move a0, t1
    jal Prod_Clamp7
    li t4, 2
    move a1, t1
    lw a2, %lo(ECHO_INL)
    jal Prod_Clamp7
    li t4, 4
    move a3, t1
    lw a2, %lo(ECHO_INR)
    jal Prod_Clamp7
    li t4, 6
    move v0, t1
    lhu t1, %lo(DSP + RSPDSP_ECHOWRITES)
    beqz t1, 2f
    nop
    lw t1, %lo(ECHO_ADR)
    andi t1, t1, 4
    addiu v1, t1, %lo(SCR_ECHO)
    andi a3, a3, 0xfffe
    andi v0, v0, 0xfffe
    sb a3, 0(v1)
    srl t1, a3, 8
    sb t1, 1(v1)
    sb v0, 2(v1)
    srl t1, v0, 8
    sb t1, 3(v1)
    lw t1, %lo(ECHO_ADR)
    lw t3, %lo(DSP + RSPDSP_P_ARAM)
    li t5, ~7
    and t5, t1, t5
    addu s0, t3, t5
    li s4, %lo(SCR_ECHO)
    jal DMAOut
    li t0, DMA_SIZE(8, 1)
    # (Echo buffers at $0000 would also need the mirror after $FFFF; the
    # CPU runs such batches in C.)
2:  # indices
    lhu t1, %lo(DSP + RSPDSP_FIRINDEX)
    addiu t1, t1, 1
    andi t1, t1, 7
    sh t1, %lo(DSP + RSPDSP_FIRINDEX)
    lhu t1, %lo(DSP + RSPDSP_ECHOINDEX)
    addiu t1, t1, 1
    sh t1, %lo(DSP + RSPDSP_ECHOINDEX)
    lhu t1, %lo(DSP + RSPDSP_ECHOREMAIN)
    addiu t1, t1, -1
    andi t1, t1, 0xffff
    bnez t1, 3f
    sh t1, %lo(DSP + RSPDSP_ECHOREMAIN)
    lhu t1, %lo(DSP + RSPDSP_ECHODELAY)
    sh t1, %lo(DSP + RSPDSP_ECHOREMAIN)
    sh zero, %lo(DSP + RSPDSP_ECHOINDEX)
3:  lw ra, %lo(SAVE_RA2)
    jr ra
    nop

# FIR sum from products at t9 (HI) / t9+16 (MD): taps 0-6 summed, cast to
# int16, tap 7 cast to int16 and added, clamped, LSB cleared. Result t1.
Fir_Sum:
    move t3, zero                    # sum
    move t4, zero
1:  addu t5, t9, t4
    lh t6, 0(t5)
    lhu t7, 16(t5)
    sll t6, t6, 16
    or t6, t6, t7
    sra t6, t6, 6
    addiu t4, t4, 2
    sltiu t7, t4, 14
    bnez t7, 1b
    addu t3, t3, t6
    sll t3, t3, 16
    sra t3, t3, 16                   # (int16) sum of taps 0-6
    lh t6, 14(t9)
    lhu t7, 30(t9)
    sll t6, t6, 16
    or t6, t6, t7
    sra t6, t6, 6
    sll t6, t6, 16
    sra t6, t6, 16                   # (int16) tap 7
    addu t1, t3, t6
    CLAMP16 t1, t6, t7
    li t3, ~1
    jr ra
    and t1, t1, t3

    .data
    .align 2
ECHO_ADR:   .word 0
ECHO_INL:   .word 0
ECHO_INR:   .word 0
    .text
