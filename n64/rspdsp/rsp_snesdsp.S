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
SCR_RMW:    .ds.b 16
    .align 5
SCR_ECHO:   .ds.b 32        # two echo buffers (current, prefetch), 16 apart
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
    li t1, -1
    sw t1, %lo(PREF_ADR)            # no echo prefetch yet
    li t1, %lo(SCR_ECHO)
    sw t1, %lo(PREF_BUF)
    move fp, zero
    move s5, zero

Run_loop:
#ifndef PROF_NO_LOG
    jal Log_ApplyDue                # writes logged before sample s5
    nop
#endif
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
    li t1, -1
    sw t1, %lo(PREF_ADR)            # an echo prefetch may be stale
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

# One BRR nibble at bits 31..28 of s0: shift (a1, a0), filter (a3), clamp
# to 16 bits, clip to 15, store doubled at t6. old t8, older t9.
.macro BRR_NIBBLE off
    srav s0, s0, a1
    jalr a3
    sllv s0, s0, a0                 # (delay slot)
    sra t3, s0, 15                  # CLAMP16, with the delay slot used
    sra t5, s0, 31
    beq t3, t5, 1f
    move t9, t8
    li s0, 32767
    xor s0, s0, t5
1:  sll s0, s0, 17
    sra s0, s0, 17
    sll t3, s0, 1
    move t8, s0
    sh t3, \off(t6)
.endm

Dsp_Sample:
    move v1, ra
    sw v1, %lo(SAVE_RA)

#ifndef PROF_NO_A
    # --- A: pitch counters; decode a BRR block on wrap -----------------
    # counter += pitch for all voices; the add's carries are the wraps.
    li t1, %lo(DSP + RSPDSP_V_PITCH)
    lqv $v01, 0x00, t1
    lqv $v02, 0x10, t1              # V_COUNTER
    vaddc $v02, $v02, $v01
    sqv $v02, 0x10, t1
    cfc2 t3, COP2_CTRL_VCO          # bit v: voice v wrapped
    ctc2 zero, COP2_CTRL_VCO
    andi t3, t3, 0xff
    beqz t3, A_done                 # no voice needs a block (most samples)
    move a2, zero
A_voice:
    andi t1, t3, 1
    beqz t1, A_next
    srl t3, t3, 1
    sw t3, %lo(A_MASK)
    jal Brr_Decode
    nop
    lw t3, %lo(A_MASK)
A_next:
    bnez t3, A_voice
    addiu a2, a2, 1
A_done:

#endif
#ifndef PROF_NO_B
    # --- B: Gaussian interpolation for all voices -----------------------
    # Two voices per vector, voice-major: one ldv takes a voice's four
    # doubled samples into lanes 0-3 (or 4-7); the coefficients g0,g1 come
    # by llv from GAUSSP[0xff - offset] and g2,g3 by two lsv from
    # GAUSSP[offset]. The C sum (p0 + p1 + p2 with 16-bit wrap, then + p3
    # with saturation) is formed across lanes: lane 0 (and 4) holds it.
    # Addresses for all voices: sample index * 2 + the voice's decode
    # buffer, GAUSSP + offset * 4 and GAUSSP + (0xff - offset) * 4.
    li t1, %lo(DSP)
    li t3, %lo(ENV_K)
    li t9, %lo(STG_T1)
    li t2, %lo(GAUSSP)
    lqv $v20, RSPDSP_V_COUNTER, t1
    lqv $v21, 0x00, t3
    lqv $v23, 0x20, t3              # B_DECODE (offsets in the state)
    mtc2 t1, $v19.e0
    mtc2 t2, $v19.e1
    vmudl $v16, $v20, $v30, e(2)    # counter >> 11
    vmudl $v17, $v20, $v31, e(1)    # counter >> 2
    vand $v16, $v16, $v21, e(4)     # & 0x1e
    vand $v17, $v17, $v21, e(5)     # & 0x3fc
    vaddc $v23, $v23, $v19, e(0)    # + DSP
    vxor $v18, $v17, $v21, e(5)
    vaddc $v17, $v17, $v19, e(1)    # + GAUSSP
    vaddc $v18, $v18, $v19, e(1)
    vaddc $v16, $v16, $v23
    sqv $v16, 0x00, t9              # STG_T1: sample addresses
    sqv $v17, 0x10, t9              # STG_T2: GAUSSP[offset]
    sqv $v18, 0x20, t9              # STG_T3: GAUSSP[0xff - offset]
    li t8, %lo(STG_T0)
    li t7, 2
    lhu t1, 0x00(t9)
    lhu t2, 0x10(t9)
    lhu t3, 0x20(t9)
B_quad:
    lhu t4, 0x02(t9)
    lhu t5, 0x12(t9)
    lhu t6, 0x22(t9)
    ldv $v01, 0, 0, t1
    llv $v05, 0, 0, t3
    lsv $v05, 4, 2, t2
    lsv $v05, 6, 0, t2
    lhu t1, 0x04(t9)
    ldv $v01, 8, 0, t4
    lhu t2, 0x14(t9)
    llv $v05, 8, 0, t6
    lhu t3, 0x24(t9)
    lsv $v05, 12, 2, t5
    lsv $v05, 14, 0, t5
    lhu t4, 0x06(t9)
    lhu t5, 0x16(t9)
    vmudm $v09, $v01, $v05          # floor(2s * 32g / 65536) = (g*s) >> 10
    lhu t6, 0x26(t9)
    ldv $v02, 0, 0, t1
    llv $v06, 0, 0, t3
    vaddc $v10, $v09, $v09, e(1q)   # p0 + p1 (wrapping)
    lsv $v06, 4, 2, t2
    lsv $v06, 6, 0, t2
    ldv $v02, 8, 0, t4
    llv $v06, 8, 0, t6
    vaddc $v10, $v10, $v09, e(2h)   # + p2
    lsv $v06, 12, 2, t5
    lsv $v06, 14, 0, t5
    ctc2 zero, COP2_CTRL_VCO
    vadd $v10, $v10, $v09, e(3h)    # + p3, saturating
    addiu t9, t9, 8
    vmudm $v11, $v02, $v06
    lhu t1, 0x00(t9)
    lhu t2, 0x10(t9)
    lhu t3, 0x20(t9)
    ssv $v10, 0, 0, t8
    vaddc $v12, $v11, $v11, e(1q)
    ssv $v10, 8, 2, t8
    addiu t7, t7, -1
    vaddc $v12, $v12, $v11, e(2h)
    ctc2 zero, COP2_CTRL_VCO
    vadd $v12, $v12, $v11, e(3h)
    ssv $v12, 0, 4, t8
    ssv $v12, 8, 6, t8
    bnez t7, B_quad
    addiu t8, t8, 8
    # >> 1, and noise voices take the noise sample.
    li t1, %lo(STG_T0)
    lhu t3, %lo(DSP + RSPDSP_M_NOISE)
    lh t5, %lo(DSP + RSPDSP_NOISE_SAMPLE)
    lqv $v13, 0x00, t1
    li t2, %lo(ENV_BITS)
    lqv $v22, 0x00, t2
    mtc2 t3, $v14.e0
    mtc2 t5, $v15.e0
    vmudm $v13, $v13, $v31, e(0)    # * 0x8000 >> 16: >> 1 (arithmetic)
    vand $v14, $v22, $v14, e(0)
    veq $v25, $v14, vzero
    vmrg $v13, $v13, $v15, e(0)
    sqv $v13, 0x00, t1              # STG_T0 doubles as the raw sample
#endif
#ifndef PROF_NO_C
    # --- C: key on/off, reset, envelope ------------------------------
    # Key on (even samples, without key off): per voice, rare.
    lhu t1, %lo(DSP + RSPDSP_EVENCYCLE)
    lhu t3, %lo(DSP + RSPDSP_M_KEYOFF)
    beqz t1, C_reset
    lhu t5, %lo(DSP + RSPDSP_M_KEYON)
    nor t3, t3, zero
    and t5, t5, t3
    beqz t5, C_reset
    move a2, zero
C_voice:
    andi t1, t5, 1
    beqz t1, 1f
    srl t5, t5, 1
    sw t5, %lo(KON_LEFT)
    jal Env_KeyOn
    nop
    lw t5, %lo(KON_LEFT)
1:  bnez t5, C_voice
    addiu a2, a2, 1
C_reset:
    lhu t1, %lo(DSP + RSPDSP_RESET)
    beqz t1, C_env
    li t3, %lo(DSP + RSPDSP_V_ADSRSTATE)
    vor $v01, vzero, $v30, e(5)     # 4: release
    sqv $v01, 0x00, t3
    li t3, %lo(DSP + RSPDSP_V_GAIN)
    sqv vzero, 0x00, t3
C_env:
    # The envelope for all voices: each case is computed in every lane and
    # merges pick the lane's own ($v30 lanes 0x80..1, $v31 0x8000..0x100).
    # Compares only set VCC for the merge after them. Ordered so a vector
    # result is read no sooner than four instructions later.
    li t1, %lo(DSP)
    li t3, %lo(ENV_K)
    lhu t5, %lo(DSP + RSPDSP_M_USEGAIN)
    lhu t6, %lo(DSP + RSPDSP_M_DIRECTGAIN)
    lhu t7, %lo(DSP + RSPDSP_EVENCYCLE)
    lhu t8, %lo(DSP + RSPDSP_M_KEYOFF)
    lqv $v22, 0x10, t3                  # 1 << lane
    subu t7, zero, t7
    and t8, t8, t7                      # key off acts on even samples
    mtc2 t8, $v05.e0
    lqv $v02, RSPDSP_V_GAIN, t1         # G
    mtc2 t5, $v09.e0
    mtc2 t6, $v10.e0
    lqv $v01, RSPDSP_V_ADSRSTATE, t1    # S
    lqv $v21, 0x00, t3                  # 0x600 0x7e0 0x7ff 0x7fff
    lqv $v06, RSPDSP_V_RATE0, t1
    lqv $v04, RSPDSP_V_RATE2, t1
    vor $v29, vzero, $v30, e(2)         # 32
    lqv $v07, RSPDSP_V_RATE1, t1
    vand $v09, $v22, $v09, e(0)         # useGain bits
    lqv $v03, RSPDSP_V_RATECTR, t1
    vand $v10, $v22, $v10, e(0)         # directGain bits
    lqv $v08, RSPDSP_V_RATE3, t1
    vand $v05, $v22, $v05, e(0)         # key off bits
    vsub $v12, $v02, $v30, e(7)         # g - 1
    lqv $v28, RSPDSP_V_GAINMODE, t1
    vsub $v13, $v02, $v30, e(2)         # g - 32
    lqv $v27, RSPDSP_V_SUSTAIN, t1
    vadd $v14, $v02, $v30, e(2)         # g + 32
    lqv $v26, RSPDSP_V_GAINVAL, t1
    vsub $v24, $v02, $v30, e(4)         # g - 8
    veq $v25, $v05, vzero
    vmrg $v01, $v01, $v30, e(5)         # key off: release (4)
    vand $v11, $v09, $v10               # direct gain (with useGain)
    vmudm $v12, $v12, $v31, e(7)        # (g - 1) >> 8
    vge $v13, $v13, vzero               # linear decrease, floor 0
    vlt $v14, $v14, $v21, e(2)          # linear increase, ceiling 0x7ff
    vge $v24, $v24, vzero               # release step
    veq $v25, $v01, vzero
    vmrg $v04, $v06, $v04               # rate: attack
    vlt $v25, $v02, $v21, e(0)
    vmrg $v15, $v29, $v30, e(4)         # bent: 32 below 0x600, else 8
    vadd $v12, $v12, $v30, e(7)
    vne $v25, $v06, $v30, e(7)
    vmrg $v16, $v29, $v31, e(5)         # attack: 32, or 1024 at rate 1
    veq $v25, $v01, $v30, e(7)
    vmrg $v04, $v07, $v04               # rate: decay
    vadd $v15, $v02, $v15
    vsub $v12, $v02, $v12               # exponential decrease
    vadd $v16, $v02, $v16
    vne $v25, $v09, vzero
    vmrg $v04, $v08, $v04               # rate: gain mode (rate3)
    vlt $v15, $v15, $v21, e(2)          # bent increase
    vlt $v16, $v16, $v21, e(2)          # attack
    vadd $v05, $v03, $v30, e(7)         # counter + 1
    veq $v25, $v28, vzero
    vmrg $v18, $v13, $v15               # gain mode 0 / 3
    veq $v25, $v01, vzero
    vmrg $v17, $v16, $v12               # ADSR step: attack / decrease
    vlt $v25, $v05, $v04
    vmrg $v23, $v05, vzero              # counter: +1, or 0 on a step
    veq $v25, $v28, $v30, e(7)
    vmrg $v18, $v12, $v18               # gain mode 1
    veq $v25, $v04, vzero
    vmrg $v23, $v03, $v23               # rate 0: frozen
    veq $v25, $v28, $v30, e(6)
    vmrg $v18, $v14, $v18               # gain mode 2
    vne $v25, $v11, vzero
    vmrg $v23, $v03, $v23               # direct gain: counter kept
    vne $v25, $v09, vzero
    vmrg $v17, $v18, $v17               # stepped gain
    veq $v25, $v01, $v30, e(5)
    vmrg $v23, $v03, $v23               # release: counter kept
    veq $v25, $v01, vzero
    vmrg $v06, $v17, vzero              # attack lanes: stepped gain, else 0
    veq $v25, $v01, $v30, e(7)
    vmrg $v07, $v17, $v21, e(3)         # decay lanes: stepped gain, else 0x7fff
    sqv $v23, RSPDSP_V_RATECTR, t1
    vlt $v25, $v06, $v21, e(1)
    vmrg $v20, $v01, $v30, e(7)         # attack -> decay at 0x7e0
    vlt $v25, $v05, $v04
    vmrg $v19, $v02, $v17               # no step: gain kept
    vge $v25, $v07, $v27
    vmrg $v20, $v20, $v30, e(6)         # decay -> sustain below the level
    veq $v25, $v04, vzero
    vmrg $v19, $v02, $v19
    vlt $v25, $v05, $v04
    vmrg $v20, $v01, $v20               # no step: state kept
    vne $v25, $v11, vzero
    vmrg $v19, $v26, $v19               # direct gain
    veq $v25, $v04, vzero
    vmrg $v20, $v01, $v20
    veq $v25, $v01, $v30, e(5)
    vmrg $v19, $v24, $v19               # release
    vne $v25, $v11, vzero
    vmrg $v20, $v01, $v20
    li t3, %lo(STG_G32)
    vmudn $v18, $v19, $v29, e(0)        # gain << 5 (low product, unclamped)
    veq $v25, $v01, $v30, e(5)
    vmrg $v20, $v01, $v20
    sqv $v19, RSPDSP_V_GAIN, t1
    sqv $v18, 0x00, t3
    sqv $v20, RSPDSP_V_ADSRSTATE, t1

#endif
#ifndef PROF_NO_D
    # --- D: gain, ENVX/OUTX --------------------------------------------
    li t1, %lo(STG_T0)
    lqv $v01, 0x00, t1
    li t1, %lo(STG_G32)
    lqv $v02, 0x00, t1
    vmudm $v03, $v01, $v02          # (sample * gain) >> 11
    li t1, %lo(DSP + RSPDSP_V_GAIN)
    lqv $v09, 0x00, t1
    li t1, %lo(DSP + RSPDSP_V_SAMPLEOUT)
    sqv $v03, 0x00, t1
    vmudm $v09, $v09, $v31, e(3)    # gain >> 4 ($v31 lane 3 = 0x1000)
    vmudm $v10, $v03, $v31, e(6)    # sample >> 7 ($v31 lane 6 = 0x0200)
    li t1, %lo(DSP + RSPDSP_RAM)    # ENVX ($x8) and OUTX ($x9): low bytes
    li t3, %lo(DSP + RSPDSP_RAM + 0x40)
    sbv $v09, 1, 0x08, t1
    sbv $v10, 1, 0x09, t1
    sbv $v09, 3, 0x18, t1
    sbv $v10, 3, 0x19, t1
    sbv $v09, 5, 0x28, t1
    sbv $v10, 5, 0x29, t1
    sbv $v09, 7, 0x38, t1
    sbv $v10, 7, 0x39, t1
    sbv $v09, 9, 0x08, t3
    sbv $v10, 9, 0x09, t3
    sbv $v09, 11, 0x18, t3
    sbv $v10, 11, 0x19, t3
    sbv $v09, 13, 0x28, t3
    sbv $v10, 13, 0x29, t3
    sbv $v09, 15, 0x38, t3
    sbv $v10, 15, 0x39, t3

#endif
#ifndef PROF_NO_E
    # --- E: voice volumes; dry mix and echo input -----------------------
    # $v03 = sample out. Terms (s * vol) >> 6 for all voices at once (exact:
    # see RSPDSP_V_VOLL_U), then four accumulators (dry L/R, echo L/R) add
    # the voices in order with saturation, as C clamps after each voice.
    li t1, %lo(DSP + RSPDSP_V_VOLL_U)
    lqv $v04, 0x00, t1
    lqv $v05, 0x10, t1
    lqv $v06, 0x20, t1
    lqv $v07, 0x30, t1
    lqv $v08, 0x40, t1              # echo mask
    vmudm $v11, $v03, $v04
    vmadh $v11, $v03, $v05          # TL
    vmudm $v12, $v03, $v06
    vmadh $v12, $v03, $v07          # TR
    vand $v13, $v11, $v08
    vand $v14, $v12, $v08
    vxor $v15, $v15, $v15
    vxor $v16, $v16, $v16
    vxor $v17, $v17, $v17
    vxor $v18, $v18, $v18
    ctc2 zero, COP2_CTRL_VCO
    vadd $v15, $v15, $v11, e(0)
    vadd $v16, $v16, $v12, e(0)
    vadd $v17, $v17, $v13, e(0)
    vadd $v18, $v18, $v14, e(0)
    vadd $v15, $v15, $v11, e(1)
    vadd $v16, $v16, $v12, e(1)
    vadd $v17, $v17, $v13, e(1)
    vadd $v18, $v18, $v14, e(1)
    vadd $v15, $v15, $v11, e(2)
    vadd $v16, $v16, $v12, e(2)
    vadd $v17, $v17, $v13, e(2)
    vadd $v18, $v18, $v14, e(2)
    vadd $v15, $v15, $v11, e(3)
    vadd $v16, $v16, $v12, e(3)
    vadd $v17, $v17, $v13, e(3)
    vadd $v18, $v18, $v14, e(3)
    vadd $v15, $v15, $v11, e(4)
    vadd $v16, $v16, $v12, e(4)
    vadd $v17, $v17, $v13, e(4)
    vadd $v18, $v18, $v14, e(4)
    vadd $v15, $v15, $v11, e(5)
    vadd $v16, $v16, $v12, e(5)
    vadd $v17, $v17, $v13, e(5)
    vadd $v18, $v18, $v14, e(5)
    vadd $v15, $v15, $v11, e(6)
    vadd $v16, $v16, $v12, e(6)
    vadd $v17, $v17, $v13, e(6)
    vadd $v18, $v18, $v14, e(6)
    vadd $v15, $v15, $v11, e(7)
    vadd $v16, $v16, $v12, e(7)
    vadd $v17, $v17, $v13, e(7)
    vadd $v18, $v18, $v14, e(7)

#endif
    # --- F: master volume (clamped by VMADH) -----------------------------
    li t1, %lo(DSP + RSPDSP_SC_U)
    lqv $v20, 0x00, t1
    lqv $v21, 0x10, t1              # SC_C
    vmudm $v22, $v15, $v20, e(0)
    vmadh $v22, $v15, $v21, e(0)    # total L (all lanes)
    vmudm $v23, $v16, $v20, e(1)
    vmadh $v23, $v16, $v21, e(1)    # total R

#ifndef PROF_NO_G
    # --- G: echo -------------------------------------------------------
    jal Echo
    nop

#endif
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
KON_LEFT:   .word 0
    .text

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
    # Shift as one arithmetic right shift of the nibble placed at bits
    # 31..28: (n << shift) >> 1 == (n << 28) >> (29 - shift). Invalid
    # shifts (13-15) keep only the sign: (n << 28) >> 31 << 11.
    srl a1, t1, 4
    li t3, 29
    subu a1, t3, a1
    sltiu t3, t1, 0xd0
    bnez t3, 1f
    move a0, zero                   # post-shift
    li a1, 31
    li a0, 11
1:  srl t3, t1, 2
    andi t3, t3, 3
    sll t3, t3, 2
    lw a3, %lo(BRR_FILTERS)(t3)     # filter routine for this block
    andi t1, t1, 3
    sh t1, %lo(DSP + RSPDSP_V_PREVFLAGS)(t4)
    lh t8, %lo(DSP + RSPDSP_V_OLD)(t4)
    lh t9, %lo(DSP + RSPDSP_V_OLDER)(t4)
    lw t6, %lo(BRR_DEC)
    addiu t6, t6, %lo(DSP + RSPDSP_DECODE + 6)   # output: sample 3
    li t7, 8                         # bytes left
Brr_byte:
    lbu t1, 1(v0)
    addiu v0, v0, 1
    andi s0, t1, 0xf0
    sll s0, s0, 24                   # high nibble first
    BRR_NIBBLE 0
    sll s0, t1, 28
    BRR_NIBBLE 2
    addiu t7, t7, -1
    bnez t7, Brr_byte
    addiu t6, t6, 4
    lw t4, %lo(BRR_T4)
    sh t8, %lo(DSP + RSPDSP_V_OLD)(t4)
    sh t9, %lo(DSP + RSPDSP_V_OLDER)(t4)
    lw ra, %lo(SAVE_RA2)
    srl a2, t4, 1
    jr ra
    nop

# Filters (s0 += prediction from old t8, older t9); called with jalr.
Brr_F0:
    jr ra
    nop
Brr_F1:                              # s += old + (-old >> 4)
    subu t3, zero, t8
    sra t3, t3, 4
    addu t3, t3, t8
    jr ra
    addu s0, s0, t3
Brr_F2:                              # s += 2*old + ((3 * -old) >> 5) - older + (older >> 4)
    subu t3, zero, t8
    sll t5, t3, 1
    addu t5, t5, t3
    sra t5, t5, 5
    sll s4, t8, 1
    addu t5, t5, s4
    subu t5, t5, t9
    sra s4, t9, 4
    addu t5, t5, s4
    jr ra
    addu s0, s0, t5
Brr_F3:                              # s += 2*old + ((13 * -old) >> 6) - older + ((3 * older) >> 4)
    subu t3, zero, t8
    sll t5, t3, 3
    sll s4, t3, 2
    addu t5, t5, s4
    addu t5, t5, t3
    sra t5, t5, 6
    sll s4, t8, 1
    addu t5, t5, s4
    subu t5, t5, t9
    sll s4, t9, 1
    addu s4, s4, t9
    sra s4, s4, 4
    addu t5, t5, s4
    jr ra
    addu s0, s0, t5

    .data
    .align 2
BRR_DEC:    .word 0
BRR_FILTERS: .word Brr_F0, Brr_F1, Brr_F2, Brr_F3
BRR_PF:     .word 0
BRR_T4:     .word 0
    .text

########################################################################
# Key on for voice a2 (even samples, key off clear; dsp_cycleChannel)
########################################################################
Env_KeyOn:
    sw ra, %lo(SAVE_RA3)
    sll t4, a2, 1
    li t5, 1
    sllv t5, t5, a2                  # voice bit
    lhu t1, %lo(DSP + RSPDSP_M_KEYON)
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
Env_key_ret:
    lw ra, %lo(SAVE_RA3)
    jr ra
    nop

    .data
    .align 4
ENV_K:      .half 0x600, 0x7e0, 0x7ff, 0x7fff, 0x1e, 0x3fc, 0, 0
ENV_BITS:   .half 1, 2, 4, 8, 16, 32, 64, 128
B_DECODE:   .half RSPDSP_DECODE + 0 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 1 * RSPDSP_DECODE_STRIDE
            .half RSPDSP_DECODE + 2 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 3 * RSPDSP_DECODE_STRIDE
            .half RSPDSP_DECODE + 4 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 5 * RSPDSP_DECODE_STRIDE
            .half RSPDSP_DECODE + 6 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 7 * RSPDSP_DECODE_STRIDE
    .text

########################################################################
# Echo (dsp_handleEcho). In: $v22/$v23 totals after master volume, $v17/
# $v18 echo input (all lanes). Out: a0/a1 totals with echo.
########################################################################
Echo:
    sw ra, %lo(SAVE_RA2)
    lhu t1, %lo(DSP + RSPDSP_ECHOINDEX)
    lhu t3, %lo(DSP + RSPDSP_ECHOADR)
    lw t5, %lo(PREF_ADR)
    sll t1, t1, 2
    addu t1, t1, t3
    andi t1, t1, 0xffff
    sw t1, %lo(ECHO_ADR)
    bne t5, t1, Echo_sync           # no prefetch of this address
    lw t4, %lo(PREF_BUF)
    jal DMAWaitIdle
    nop
    b Echo_have
    nop
Echo_sync:
    lw t3, %lo(DSP + RSPDSP_P_ARAM)
    li t5, ~7
    and t5, t1, t5
    addu s0, t3, t5
    move s4, t4
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
Echo_have:
    lw t1, %lo(ECHO_ADR)
    lhu t6, %lo(DSP + RSPDSP_FIRINDEX)
    andi t1, t1, 4
    addu v1, t1, t4                  # v1 -> 4 echo bytes (t4: buffer)
    sll t6, t6, 1
    # fir[fi] = (int16)(b0 | b1 << 8) >> 1, kept twice for rotation
    lbu t1, 0(v1)
    lbu t3, 1(v1)
    lbu t5, 2(v1)
    lbu t7, 3(v1)
    sll t3, t3, 8
    or t1, t1, t3
    sll t7, t7, 8
    or t5, t5, t7
    sll t1, t1, 16
    sra t1, t1, 17
    sll t5, t5, 16
    sra t5, t5, 17
    sh t1, %lo(DSP + RSPDSP_FIR_L)(t6)
    sh t1, %lo(DSP + RSPDSP_FIR_L + 16)(t6)
    sh t5, %lo(DSP + RSPDSP_FIR_R)(t6)
    sh t5, %lo(DSP + RSPDSP_FIR_R + 16)(t6)
    # Lane i = fir[(fi + i + 1) & 7]: a misaligned load of the doubled history.
    addiu t1, t6, %lo(DSP + RSPDSP_FIR_L + 2)
    addiu t3, t6, %lo(DSP + RSPDSP_FIR_R + 2)
    li t5, %lo(DSP + RSPDSP_FIR_U)
    lqv $v24, 0x00, t1
    lrv $v24, 0x10, t1
    lqv $v25, 0x00, t3
    lrv $v25, 0x10, t3
    lqv $v26, 0x00, t5
    lqv $v27, 0x10, t5               # FIR_C
    vnor $v12, vzero, $v30, e(7)     # 0xfffe
    vmudm $v28, $v24, $v26
    vmadh $v28, $v24, $v27
    vsar $v28, COP2_ACC_MD           # exact low 16 bits of (fir * coef) >> 6
    vmudm $v29, $v25, $v26
    vmadh $v29, $v25, $v27
    vsar $v29, COP2_ACC_MD
    # Sum in lane 0: taps 0-6 with 16-bit wrap, then tap 7 saturating,
    # then the LSB cleared.
    vaddc $v10, $v28, $v28, e(1q)    # pairs
    vaddc $v11, $v29, $v29, e(1q)
    vaddc $v13, $v10, $v10, e(2h)    # taps 0-3
    vaddc $v14, $v11, $v11, e(2h)
    vaddc $v13, $v13, $v10, e(4)     # + 4, 5
    vaddc $v14, $v14, $v11, e(4)
    vaddc $v13, $v13, $v28, e(6)     # + 6
    vaddc $v14, $v14, $v29, e(6)
    ctc2 zero, COP2_CTRL_VCO
    vadd $v13, $v13, $v28, e(7)      # + 7, clamped
    vadd $v14, $v14, $v29, e(7)
    vand $v10, $v13, $v12
    vand $v11, $v14, $v12
    # clamp(total + (sum * evol) >> 7) in lanes 2/3,
    # clamp(echo in + (sum * feedback) >> 7) in lanes 4/5.
    vmudn $v04, $v20, $v10, e(0)
    vmadh $v04, $v21, $v10, e(0)
    vmadh $v04, $v22, $v30, e(7)     # $v30 lane 7 = 1
    vmudn $v05, $v20, $v10, e(0)
    vmadh $v05, $v21, $v10, e(0)
    vmadh $v05, $v17, $v30, e(7)
    vmudn $v06, $v20, $v11, e(0)
    vmadh $v06, $v21, $v11, e(0)
    vmadh $v06, $v23, $v30, e(7)
    vmudn $v07, $v20, $v11, e(0)
    vmadh $v07, $v21, $v11, e(0)
    vmadh $v07, $v18, $v30, e(7)
    mfc2 a0, $v04.e2
    mfc2 a1, $v06.e3
    mfc2 a3, $v05.e4
    mfc2 v0, $v07.e5
    lhu t1, %lo(DSP + RSPDSP_ECHOWRITES)
    beqz t1, 2f
    nop
    andi a3, a3, 0xfffe
    andi v0, v0, 0xfffe
    sb a3, 0(v1)
    srl t1, a3, 8
    sb t1, 1(v1)
    sb v0, 2(v1)
    srl t1, v0, 8
    sb t1, 3(v1)
    # Written back in the background: DMA runs in order, so later reads
    # of ARAM (BRR, directory, the next prefetch) see it.
    lw t1, %lo(ECHO_ADR)
    lw t3, %lo(DSP + RSPDSP_P_ARAM)
    li t5, ~7
    and t5, t1, t5
    addu s0, t3, t5
    move s4, t4
    jal DMAOutAsync
    li t0, DMA_SIZE(8, 1)
    # (Echo buffers at $0000 would also need the mirror after $FFFF; the
    # CPU runs such batches in C.)
2:  # indices
    lhu t1, %lo(DSP + RSPDSP_FIRINDEX)
    lhu t3, %lo(DSP + RSPDSP_ECHOINDEX)
    lhu t5, %lo(DSP + RSPDSP_ECHOREMAIN)
    addiu t1, t1, 1
    andi t1, t1, 7
    sh t1, %lo(DSP + RSPDSP_FIRINDEX)
    addiu t3, t3, 1
    addiu t5, t5, -1
    andi t5, t5, 0xffff
    bnez t5, 3f
    sh t5, %lo(DSP + RSPDSP_ECHOREMAIN)
    lhu t5, %lo(DSP + RSPDSP_ECHODELAY)
    move t3, zero
    sh t5, %lo(DSP + RSPDSP_ECHOREMAIN)
3:  sh t3, %lo(DSP + RSPDSP_ECHOINDEX)
    # Prefetch the next sample's echo bytes into the other buffer. Used
    # only if the address still matches and no logged ARAM write came first.
    lhu t1, %lo(DSP + RSPDSP_ECHOADR)
    sll t3, t3, 2
    addu t1, t1, t3
    andi t1, t1, 0xffff
    sw t1, %lo(PREF_ADR)
    xori t4, t4, 16
    sw t4, %lo(PREF_BUF)
    lw t3, %lo(DSP + RSPDSP_P_ARAM)
    li t5, ~7
    and t5, t1, t5
    addu s0, t3, t5
    move s4, t4
    jal DMAInAsync
    li t0, DMA_SIZE(8, 1)
    lw ra, %lo(SAVE_RA2)
    jr ra
    nop

    .data
    .align 2
ECHO_ADR:   .word 0
PREF_ADR:   .word 0
PREF_BUF:   .word 0
A_MASK:     .word 0
    .text
