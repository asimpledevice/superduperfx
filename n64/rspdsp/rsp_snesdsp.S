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

/* RSPDSP_PM (rsp_snesdsp_pm.S): the variant for batches with pitch
 * modulation (docs/n64/RED_DESIGN.md, "The pitch-modulation variant"). It
 * is this sample-major microcode plus a repair pass for the modulated
 * voices, and it reads every voice's directory entries and blocks from
 * ARAM itself (no records), through the ledger. */
#ifdef RSPDSP_PM
#undef RSPDSP_BLOCKS
#ifndef RSPDSP_COMPACT_BRR
#define RSPDSP_COMPACT_BRR 1
#endif
#if defined(RSPDSP_PM_ECHO_SYNC)
#define ECHO_SYNC 1   /* echo bytes read when needed, no prefetch (an A/B for hardware) */
#endif
#endif

/* Per-batch parameters: in the state block, or (RSPDSP_STREAM) in their own
 * block, loaded from the command's second word. */
#ifdef RSPDSP_STREAM
#define PAR(f) (PARAMS + RSPDSP_PP_##f)
#define OUT_SHIFT 3
#define OUT_FLUSH 16
#else
#define PAR(f) (DSP + RSPDSP_P_##f)
#define OUT_SHIFT 2
#define OUT_FLUSH 32
#endif

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
#ifdef RSPDSP_PM
PM_ENV:     .ds.b 48        # gain, rate counter, state, as C_env found them
#endif
SCR_BRR:    .ds.b 24        # BRR block window (8-byte aligned DMA)
    .align 3
SCR_DIR:    .ds.b 16                # (VM: 16 bytes are read)
SCR_RMW:    .ds.b 16
    .align 5
SCR_ECHO:   .ds.b 32        # two echo buffers (current, prefetch), 16 apart
    .align 3
LOGBUF:     .ds.b 32 * RSPDSP_LOG_ENTRY
#ifdef RSPDSP_BLOCKS
    .align 3
BLKBUF:     .ds.b 16 * RSPDSP_BLK_REC   # block records (16 at a time)
KONBUF:     .ds.b 64                    # key-on records (32 at a time)
#endif
#ifdef RSPDSP_STREAM
    .align 3
PARAMS:     .ds.b RSPDSP_PARAMS_SIZE
OUTBUF:     .ds.b OUT_FLUSH * RSPDSP_OUT_SLOT
#else
OUTBUF:     .ds.b 32 * 4 + 8
#endif

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
#ifdef RSPDSP_STREAM
    move s0, a1                     # this batch's parameters
    li s4, %lo(PARAMS)
    jal DMAIn
    li t0, DMA_SIZE(RSPDSP_PARAMS_SIZE, 1)
#endif

    lhu s6, %lo(PAR(SAMPLES))
    lhu s2, %lo(PAR(LOGN))
    lw s1, %lo(PAR(LOG))
    sw zero, %lo(LOG_LEFT)
#ifndef ECHO_SYNC
    li t1, -1
    sw t1, %lo(PREF_ADR)            # no echo prefetch yet
    li t1, %lo(SCR_ECHO)
    sw t1, %lo(PREF_BUF)
#endif
#ifdef RSPDSP_BLOCKS
    lw t1, %lo(PAR(BLK))
    sw t1, %lo(BLK_RD)
    lw t1, %lo(PAR(KON))
    sw t1, %lo(KON_RD)
    sw zero, %lo(BLK_LEFT)
    sw zero, %lo(KON_LEFT2)
#endif
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
    move s0, s7
    li s4, %lo(DSP)
    jal DMAOut                      # synchronous: the block is in RDRAM after it
    li t0, DMA_SIZE(RSPDSP_STATE_SIZE, 1)
    # Tell the CPU (Red's mailbox doorbell): the word the CPU chose.
    lw t1, %lo(PAR(DONE_MSG))
    RSPQ_MailboxPost t1, t0
    jr s3
    nop

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
#ifndef ECHO_SYNC
    li t1, -1
    sw t1, %lo(PREF_ADR)            # an echo prefetch may be stale
#endif
    lw t1, %lo(DSP + RSPDSP_P_ARAM)
    li t3, ~7
    and t3, a0, t3
    addu t8, t1, t3
    move s0, t8
    li s4, %lo(SCR_RMW)
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
    andi t3, a0, 7
    sb a1, %lo(SCR_RMW)(t3)
    move s0, t8
    li s4, %lo(SCR_RMW)
    jal DMAOut
    li t0, DMA_SIZE(8, 1)
    jr t7
    nop

#ifndef RSPDSP_BLOCKS   /* the front end reads the directory */
# Read 8 bytes of ARAM containing offset a0 into SCR_DIR; returns the DMEM
# address of the requested byte in v0. (Used for directory entries.)
#ifndef RSPDSP_PM
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
#endif

# Directory entry for SRCN a2: v0 = start address, v1 = loop address.
Dir_Entry:
    move t6, ra
    lhu a0, %lo(DSP + RSPDSP_DIRPAGE)
    sll t1, a2, 2
    addu a0, a0, t1
#ifdef RSPDSP_PM
    li s4, %lo(SCR_DIR)
    li a1, 4
    jal PM_Fetch
#else
    jal Aram_Read8
#endif
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

#endif

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

#ifndef PROF_NO_A
    # --- A: pitch counters; decode a BRR block on wrap -----------------
    # counter += pitch for all voices; the add's carries are the wraps.
    li t1, %lo(DSP + RSPDSP_V_PITCH)
    lqv $v01, 0x00, t1
    lqv $v02, 0x10, t1              # V_COUNTER
#ifdef RSPDSP_PM
    # (Modulated voices: their counters move in the repair pass below.)
    lhu t3, %lo(DSP + RSPDSP_M_PMOD)
    li t5, %lo(ENV_BITS)
    andi t3, t3, 0xfe               # voice 0 is never modulated
    lqv $v03, 0x00, t5
    mtc2 t3, $v04.e0
    vand $v04, $v03, $v04, e(0)
    veq $v04, $v04, vzero           # VCC: not modulated
    vmrg $v01, $v01, vzero
#endif
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
#ifdef RSPDSP_PM                      /* (modulated voices: in the repair pass) */
    lhu t1, %lo(DSP + RSPDSP_M_PMOD)
    andi t1, t1, 0xfe
    nor t1, t1, zero
    and t5, t5, t1
#endif
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
#ifdef RSPDSP_PM
    # The envelope's inputs, for the repair pass to run it again.
    li t1, %lo(DSP)
    li t3, %lo(PM_ENV)
    lqv $v01, RSPDSP_V_GAIN, t1
    lqv $v02, RSPDSP_V_RATECTR, t1
    lqv $v03, RSPDSP_V_ADSRSTATE, t1
    sqv $v01, 0x00, t3
    sqv $v02, 0x10, t3
    sqv $v03, 0x20, t3
    jal C_envsub
    nop
    b C_envend
    nop
C_envsub:
#endif
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
#ifdef RSPDSP_PM
    jr ra
    nop
C_envend:
#endif

#endif
#ifndef PROF_NO_D
    # --- D: gain (ENVX/OUTX are set by the CPU from the final state) -----
    li t1, %lo(STG_T0)
    lqv $v01, 0x00, t1
    li t1, %lo(STG_G32)
    lqv $v02, 0x00, t1
    vmudm $v03, $v01, $v02          # (sample * gain) >> 11
    li t1, %lo(DSP + RSPDSP_V_SAMPLEOUT)
    sqv $v03, 0x00, t1

#endif
#ifdef RSPDSP_PM
    jal PM_Repair                   # the modulated voices
    nop
    li t1, %lo(DSP + RSPDSP_V_SAMPLEOUT)
    lqv $v03, 0x00, t1
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
    sll t1, fp, OUT_SHIFT
    sh a0, %lo(OUTBUF + 0)(t1)
    sh a1, %lo(OUTBUF + 2)(t1)
    addiu fp, fp, 1
    li t1, OUT_FLUSH
    bne fp, t1, 1f
    nop
    jal Out_Flush
    nop
1:
    lhu t1, %lo(DSP + RSPDSP_EVENCYCLE)
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
#ifdef RSPDSP_BLOCKS
BLK_RD:     .word 0                 # RDRAM: the next unread block records
BLK_PTR:    .word 0
BLK_LEFT:   .word 0
KON_RD:     .word 0
KON_PTR:    .word 0
KON_LEFT2:  .word 0
#endif
    .text

# Write OUTBUF (fp samples) to P_OUT and advance it.
Out_Flush:
    beqz fp, 1f
    move t7, ra
    lw s0, %lo(PAR(OUT))
    sll t1, fp, OUT_SHIFT
    addu t3, s0, t1
    sw t3, %lo(PAR(OUT))
#ifndef RSPDSP_STREAM                 /* (8-byte slots are whole DMA units) */
    addiu t1, t1, 7                 # DMA in whole 8-byte units
    srl t1, t1, 3
    sll t1, t1, 3
#endif
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
    sll t5, t5, 31                  # the new bit 14, sign-extended
    sra t5, t5, 17
    andi t4, t4, 0x3fff
    or t4, t4, t5
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
    # copy samples 16..18 to 0..2 (and 19 to 3, decoded over below)
    addiu t1, t6, %lo(DSP + RSPDSP_DECODE)
    ldv $v03, 0, 32, t1
    sdv $v03, 0, 0, t1
    lhu t1, %lo(DSP + RSPDSP_V_PREVFLAGS)(t4)
    andi t3, t1, 1                  # flags 1 or 3: end of sample, loop
    beqz t3, Brr_block
    nop
    sw t1, %lo(BRR_PF)
#ifndef RSPDSP_BLOCKS   /* with blocks, the record carries the loop address */
    lhu a2, %lo(DSP + RSPDSP_V_SRCN)(t4)
    sw t4, %lo(BRR_T4)
    jal Dir_Entry
    nop
    lw t4, %lo(BRR_T4)
    srl a2, t4, 1
    sh v1, %lo(DSP + RSPDSP_V_DECOFF)(t4)
#endif
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
#ifdef RSPDSP_BLOCKS
    # The next block record (start address, 9 bytes), as the front end
    # read it from ARAM at this sample.
    sw t4, %lo(BRR_T4)              # the decode below reloads the voice from it
    lw t1, %lo(BLK_LEFT)
    bnez t1, 2f
    lw v0, %lo(BLK_PTR)
    lw s0, %lo(BLK_RD)
    li s4, %lo(BLKBUF)
    jal DMAIn
    li t0, DMA_SIZE(16 * RSPDSP_BLK_REC, 1)
    lw s0, %lo(BLK_RD)
    addiu s0, s0, 16 * RSPDSP_BLK_REC
    sw s0, %lo(BLK_RD)
    li t1, 16
    li v0, %lo(BLKBUF)
2:  addiu t1, t1, -1
    sw t1, %lo(BLK_LEFT)
    addiu t1, v0, RSPDSP_BLK_REC
    sw t1, %lo(BLK_PTR)
    lhu a0, 0(v0)                   # start
    # The decode below uses the 32 bytes at the block as scratch (as it
    # did in SCR_BRR): copy the record there, not decode in the buffer.
    lqv $v03, 0x00, v0
    li v0, %lo(SCR_BRR)
    sqv $v03, 0x00, v0
    addiu v0, v0, 2                 # v0 -> block in DMEM
#else
    # Fetch 16 bytes containing the 9-byte block (the ARAM view mirrors
    # its first 16 bytes after $FFFF, so a wrapping block is contiguous).
    lhu a0, %lo(DSP + RSPDSP_V_DECOFF)(t4)
    sw t4, %lo(BRR_T4)
#ifdef RSPDSP_PM
    li s4, %lo(SCR_BRR)
    jal PM_Fetch                    # v0 -> block in DMEM
    li a1, 9
    lw t4, %lo(BRR_T4)
#else
    lw t1, %lo(DSP + RSPDSP_P_ARAM)
    li t3, ~7
    and t3, a0, t3
    addu s0, t1, t3
    li s4, %lo(SCR_BRR)
    jal DMAIn
    li t0, DMA_SIZE(16, 1)
    lw t4, %lo(BRR_T4)
    andi t3, a0, 7
    addiu v0, t3, %lo(SCR_BRR)      # v0 -> block in DMEM
#endif
#endif
    addiu a0, a0, 9
    andi a0, a0, 0xffff
    sh a0, %lo(DSP + RSPDSP_V_DECOFF)(t4)
    lbu t1, 0(v0)                   # header
    addiu t5, v0, 1
    li t3, %lo(ENV_K)
    lpv $v03, 0, 0, t5              # data bytes << 8
    lqv $v04, 0x00, t3
    # All 16 nibbles shifted at once: n * 4096 (high nibbles masked, low
    # ones moved up), then (n << shift) >> 1 = VMUDM(n * 4096,
    # 2^(shift + 3)); invalid shifts (13-15) keep the sign, times 2048.
    srl a1, t1, 4
    addiu a1, a1, 3
    li t3, 1
    sllv a1, t3, a1
    sltiu t5, t1, 0xd0
    bnez t5, 1f
    li a0, 1
    li a1, 1
    li a0, 2048
1:  mtc2 a1, $v05.e0
    mtc2 a0, $v05.e1
    vand $v06, $v03, $v04, e(6)     # high nibbles * 4096 (& 0xf000)
    vmudn $v07, $v03, $v30, e(3)    # low nibbles * 4096 (byte << 12, wrapped)
    srl t3, t1, 2
    andi t3, t3, 3
    sll t3, t3, 2
    vmudm $v06, $v06, $v05, e(0)
    vmudm $v07, $v07, $v05, e(0)
#ifdef RSPDSP_COMPACT_BRR
    srl a3, t3, 2                   # the filter (one loop for all four)
#else
    lw a3, %lo(BRR_FILTERS)(t3)     # decode loop for this block's filter
#endif
    andi t1, t1, 3
    vmudn $v06, $v06, $v05, e(1)    # (x 1, or x 2048 for invalid shifts)
    vmudn $v07, $v07, $v05, e(1)
    sh t1, %lo(DSP + RSPDSP_V_PREVFLAGS)(t4)
    lh t8, %lo(DSP + RSPDSP_V_OLD)(t4)
    lh t9, %lo(DSP + RSPDSP_V_OLDER)(t4)
    li v0, %lo(STG_A)
    sqv $v06, 0x00, v0              # samples 0, 2, 4, ...
    sqv $v07, 0x10, v0              # samples 1, 3, 5, ...
    lw t6, %lo(BRR_DEC)
    addiu t6, t6, %lo(DSP + RSPDSP_DECODE + 6)   # output: sample 3
    addiu t7, t6, 32                # end
    li a1, 16                       # input steps: +16, -14, +16, ...
#ifdef RSPDSP_COMPACT_BRR
    j Brr_Lg
#else
    jr a3
#endif
    li a0, 2

# One loop per filter: s = nibble + prediction from old t8, older t9;
# clamp to 16 bits, clip to 15, store doubled.
.macro BRR_TAIL loop
    sra t3, s0, 15
    sra t5, s0, 31
    beq t3, t5, 1f
    move t9, t8
    li s0, 32767
    xor s0, s0, t5
1:  sll s0, s0, 17
    sra s0, s0, 17
    sll t3, s0, 1
    move t8, s0
    sh t3, 0(t6)
    addiu t6, t6, 2
    addu v0, v0, a1
    bne t6, t7, \loop
    subu a1, a0, a1
.endm
#ifdef RSPDSP_COMPACT_BRR
# One loop, the filter in a3 (smaller, a few cycles slower per sample).
Brr_Lg:
    lh s0, 0(v0)
    beqz a3, Brr_gt                 # filter 0: no prediction
    subu t3, zero, t8               # -old
    li t5, 1
    bne a3, t5, 2f
    nop
    sra t3, t3, 4                   # 1: old + (-old >> 4)
    b Brr_ga
    addu t3, t3, t8
2:  sll s4, t8, 1                   # 2, 3: 2 * old - older + ...
    li t5, 2
    bne a3, t5, 3f
    subu s4, s4, t9
    sll t5, t3, 1                   # 2: ((3 * -old) >> 5) + (older >> 4)
    addu t5, t5, t3
    sra t5, t5, 5
    sra a2, t9, 4
    b 4f
    addu t5, t5, a2
3:  sll t5, t3, 3                   # 3: ((13 * -old) >> 6) + ((3 * older) >> 4)
    sll a2, t3, 2
    addu t5, t5, a2
    addu t5, t5, t3
    sra t5, t5, 6
    sll a2, t9, 1
    addu a2, a2, t9
    sra a2, a2, 4
    addu t5, t5, a2
4:  addu t3, t5, s4
Brr_ga:
    addu s0, s0, t3
Brr_gt:
    BRR_TAIL Brr_Lg
    b Brr_end
    lw t4, %lo(BRR_T4)
#else
Brr_L0:
    lh s0, 0(v0)
    BRR_TAIL Brr_L0
    b Brr_end
    lw t4, %lo(BRR_T4)
Brr_L1:                             # s += old + (-old >> 4)
    lh s0, 0(v0)
    subu t3, zero, t8
    sra t3, t3, 4
    addu t3, t3, t8
    addu s0, s0, t3
    BRR_TAIL Brr_L1
    b Brr_end
    lw t4, %lo(BRR_T4)
Brr_L2:                             # s += 2*old + ((3 * -old) >> 5) - older + (older >> 4)
    lh s0, 0(v0)
    subu t3, zero, t8
    sll t5, t3, 1
    addu t5, t5, t3
    sra t5, t5, 5
    sll s4, t8, 1
    addu t5, t5, s4
    subu t5, t5, t9
    sra s4, t9, 4
    addu t5, t5, s4
    addu s0, s0, t5
    BRR_TAIL Brr_L2
    b Brr_end
    lw t4, %lo(BRR_T4)
Brr_L3:                             # s += 2*old + ((13 * -old) >> 6) - older + ((3 * older) >> 4)
    lh s0, 0(v0)
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
    addu s0, s0, t5
    BRR_TAIL Brr_L3
    lw t4, %lo(BRR_T4)
#endif
Brr_end:
    sh t8, %lo(DSP + RSPDSP_V_OLD)(t4)
    sh t9, %lo(DSP + RSPDSP_V_OLDER)(t4)
    lw ra, %lo(SAVE_RA2)
    srl a2, t4, 1
    jr ra
    nop

    .data
    .align 2
BRR_DEC:    .word 0
#ifndef RSPDSP_COMPACT_BRR
BRR_FILTERS: .word Brr_L0, Brr_L1, Brr_L2, Brr_L3
#endif
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
#ifdef RSPDSP_BLOCKS
    # The next key-on record: the start address the front end read from
    # the directory at this sample.
    lw t1, %lo(KON_LEFT2)
    bnez t1, 2f
    lw v0, %lo(KON_PTR)
    lw s0, %lo(KON_RD)
    li s4, %lo(KONBUF)
    jal DMAIn
    li t0, DMA_SIZE(64, 1)
    lw s0, %lo(KON_RD)
    addiu s0, s0, 64
    sw s0, %lo(KON_RD)
    li t1, 64 / RSPDSP_KON_REC
    li v0, %lo(KONBUF)
2:  addiu t1, t1, -1
    sw t1, %lo(KON_LEFT2)
    addiu t1, v0, RSPDSP_KON_REC
    sw t1, %lo(KON_PTR)
    lhu v0, 0(v0)
#else
    lhu a2, %lo(DSP + RSPDSP_V_SRCN)(t4)
    jal Dir_Entry
    nop
#endif
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
ENV_K:      .half 0x600, 0x7e0, 0x7ff, 0x7fff, 0x1e, 0x3fc, 0xf000, 0
ENV_BITS:   .half 1, 2, 4, 8, 16, 32, 64, 128
B_DECODE:   .half RSPDSP_DECODE + 0 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 1 * RSPDSP_DECODE_STRIDE
            .half RSPDSP_DECODE + 2 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 3 * RSPDSP_DECODE_STRIDE
            .half RSPDSP_DECODE + 4 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 5 * RSPDSP_DECODE_STRIDE
            .half RSPDSP_DECODE + 6 * RSPDSP_DECODE_STRIDE, RSPDSP_DECODE + 7 * RSPDSP_DECODE_STRIDE
    .text

#ifdef RSPDSP_PM
########################################################################
# Pitch modulation (RSPDSP_PM). Phases A-D ran for every voice, but the
# modulated voices' counters did not move and their key ons waited. Now,
# per modulated voice in order (each needs the previous voice's output of
# this sample, final by then): pitch (pitch * ((out >> 4) + 0x400)) >> 10,
# at most 0x3fff; counter; the block on a wrap; interpolation; output with
# the gain C_env found. Two events change the voice's envelope inputs: the
# block's release (flags 1) and a key on (even sample, KON and not KOFF),
# which also clears the decode buffer after the sample is read from it.
# Then the inputs C_env saw (PM_ENV, with earlier events kept) come back,
# the event is applied (key on, then reset, as dsp_cycleChannel orders
# them), and C_env runs again for all voices: the others get the same
# results, this voice its own.
# k0: the modulated voices; k1: this voice's events (1 release, 2 key on).
########################################################################
PM_Repair:
    sw ra, %lo(PM_RA)
    lhu k0, %lo(DSP + RSPDSP_M_PMOD)
    andi k0, k0, 0xfe
    move a2, zero
PM_next:
    addiu a2, a2, 1
    srlv t1, k0, a2
    beqz t1, PM_done                # no modulated voice above
    andi t1, t1, 1
    beqz t1, PM_next
    sll t4, a2, 1
    # Pitch, from the previous voice's output: (pitch * 4) *
    # ((out & 0xfff0) + 0x4000) >> 16 is the formula, formed exactly.
    lhu t1, %lo(DSP + RSPDSP_V_PITCH)(t4)
    lh t3, %lo(DSP + RSPDSP_V_SAMPLEOUT - 2)(t4)
    sll t1, t1, 2
    andi t3, t3, 0xfff0
    addiu t3, t3, 0x4000
    andi t3, t3, 0xffff
    mtc2 t1, $v13.e0
    mtc2 t3, $v14.e0
    move k1, zero
    vmudl $v13, $v14, $v13, e(0)
    lhu t3, %lo(DSP + RSPDSP_V_COUNTER)(t4)
    lhu t5, %lo(DSP + RSPDSP_V_PREVFLAGS)(t4)
    mfc2 t1, $v13.e0
    sltiu t6, t1, 0x4000
    bnez t6, 1f
    nop
    li t1, 0x3fff
1:  addu t3, t3, t1
    sh t3, %lo(DSP + RSPDSP_V_COUNTER)(t4)
    srl t3, t3, 16
    beqz t3, PM_interp
    xori t5, t5, 1
    sltiu k1, t5, 1                 # a wrap: release if the flags were 1
    jal Brr_Decode
    nop
    sll t4, a2, 1
PM_interp:
    # Interpolation (as phase B, one voice: taps in lanes 0-3), >> 1, or
    # the noise sample.
    lhu t1, %lo(DSP + RSPDSP_V_COUNTER)(t4)
    lhu t3, %lo(B_DECODE)(t4)
    srl t5, t1, 11
    andi t5, t5, 0x1e               # sample index * 2
    addu t5, t5, t3
    addiu t5, t5, %lo(DSP)
    srl t1, t1, 2
    andi t1, t1, 0x3fc              # offset * 4
    xori t3, t1, 0x3fc              # (0xff - offset) * 4
    addiu t1, t1, %lo(GAUSSP)
    addiu t3, t3, %lo(GAUSSP)
    ldv $v08, 0, 0, t5
    llv $v09, 0, 0, t3
    lsv $v09, 4, 2, t1
    lsv $v09, 6, 0, t1
    lhu t1, %lo(DSP + RSPDSP_M_NOISE)
    lhu t3, %lo(DSP + RSPDSP_EVENCYCLE)
    vmudm $v10, $v08, $v09
    lhu t5, %lo(DSP + RSPDSP_M_KEYON)
    lhu t6, %lo(DSP + RSPDSP_M_KEYOFF)
    srlv t1, t1, a2
    andi t1, t1, 1
    vaddc $v11, $v10, $v10, e(1q)
    nor t6, t6, zero
    and t5, t5, t6
    vaddc $v11, $v11, $v10, e(2h)
    srlv t5, t5, a2
    and t5, t5, t3                  # (even sample)
    ctc2 zero, COP2_CTRL_VCO
    andi t5, t5, 1
    sll t5, t5, 1
    vadd $v11, $v11, $v10, e(3h)
    or k1, k1, t5
    mfc2 t6, $v11.e0
    beqz t1, 1f
    sra t6, t6, 1
    lh t6, %lo(DSP + RSPDSP_NOISE_SAMPLE)
1:  bnez k1, PM_event
    sw t6, %lo(PM_S)
PM_out:
    # (sample * gain) >> 11, as phase D, with the gain << 5 from C_env.
    lhu t1, %lo(STG_G32)(t4)
    lw t6, %lo(PM_S)
    mtc2 t1, $v14.e0
    mtc2 t6, $v13.e0
    vmudm $v13, $v13, $v14, e(0)
    mfc2 t1, $v13.e0
    b PM_next
    sh t1, %lo(DSP + RSPDSP_V_SAMPLEOUT)(t4)
PM_done:
    lw ra, %lo(PM_RA)
    jr ra
    nop

PM_event:
    li t1, %lo(DSP)
    li t3, %lo(PM_ENV)
    lqv $v01, 0x00, t3
    lqv $v02, 0x10, t3
    lqv $v03, 0x20, t3
    andi t5, k1, 1
    sqv $v01, RSPDSP_V_GAIN, t1
    sqv $v02, RSPDSP_V_RATECTR, t1
    beqz t5, 1f
    sqv $v03, RSPDSP_V_ADSRSTATE, t1
    li t3, 4                        # the block's release
    sh t3, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
    sh zero, %lo(DSP + RSPDSP_V_GAIN)(t4)
1:  andi t5, k1, 2
    beqz t5, 2f
    nop
    jal Env_KeyOn
    nop
    lhu t1, %lo(DSP + RSPDSP_RESET)
    sll t4, a2, 1
    beqz t1, 2f
    li t3, 4
    sh t3, %lo(DSP + RSPDSP_V_ADSRSTATE)(t4)
    sh zero, %lo(DSP + RSPDSP_V_GAIN)(t4)
2:  li t1, %lo(DSP)                 # (kept for a later voice's event)
    li t3, %lo(PM_ENV)
    lqv $v01, RSPDSP_V_GAIN, t1
    lqv $v02, RSPDSP_V_RATECTR, t1
    lqv $v03, RSPDSP_V_ADSRSTATE, t1
    sqv $v01, 0x00, t3
    sqv $v02, 0x10, t3
    jal C_envsub
    sqv $v03, 0x20, t3
    b PM_out
    sll t4, a2, 1

# The voices' ARAM reads (directory entries, blocks): bytes [a0, a0 + a1)
# (a1 <= 9) as of the sample being run, into DMEM at s4 (16 bytes from
# a0 & ~7; past $FFFF ARAM wraps). Returns v0 = the DMEM address of byte
# a0. Keeps t4, t6, k0, k1.
PM_Fetch:
    move t7, ra
    sw s4, %lo(PM_DST)
    lw t1, %lo(DSP + RSPDSP_P_ARAM)
    li t3, ~7
    and t3, a0, t3
    addu s0, t1, t3
    jal DMAIn
    li t0, DMA_SIZE(16, 1)
    andi t3, a0, 0xfff8
    li t1, 0xfff8
    bne t3, t1, 1f
    lw s0, %lo(DSP + RSPDSP_P_ARAM)
    lw s4, %lo(PM_DST)
    addiu s4, s4, 8
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
1:
#ifdef RSPDSP_STREAM
    jal PM_Ledger
    nop
#endif
    lw v0, %lo(PM_DST)
    andi t3, a0, 7
    jr t7
    addu v0, v0, t3

#ifdef RSPDSP_STREAM
# Undo, in the bytes PM_Fetch read, the SPC's writes after the sample
# being run (t: the batch's first sample + s5): each address takes the old
# value of its oldest ledger entry with time > t (apu_dsp_batch.h). Blue
# writes an entry, the tail, the page's time, then the data reaches RDRAM;
# read here in the other order (data, page time, tail, entries), a write
# whose data was read is always found. Keeps a0, a1, t4, t6, t7, k0, k1.
PM_Ledger:
    sw ra, %lo(PM_LRA)
    lw t8, %lo(PAR(START))
    andi t1, a0, 0xff
    addu t1, t1, a1
    addu t8, t8, s5                 # t
    sltiu t1, t1, 0x101
    beqz t1, PM_Lscan               # (bytes in two pages: scan)
    srl t1, a0, 6
    lw s0, %lo(PAR(LPAGE))
    li t3, ~7
    and t3, t1, t3
    addu s0, s0, t3
    li s4, %lo(PM_LBUF)
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
    andi t1, t1, 4
    lw t3, %lo(PM_LBUF)(t1)         # the page's newest write
    subu t3, t3, t8
    blez t3, PM_Lret
    nop
PM_Lscan:
    lw s0, %lo(PAR(LTAIL))
    li s4, %lo(PM_LBUF)
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
    lw t9, %lo(PM_LBUF)             # newest first, from the tail
PM_Lent:
    addiu t9, t9, -1
    andi t1, t9, APU_LEDGER_LEN_RSP - 1
    lw s0, %lo(PAR(LEDGER))
    sll t1, t1, 3
    addu s0, s0, t1
    li s4, %lo(PM_LBUF)
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
    lw t3, %lo(PM_LBUF)             # time: at or before t ends the scan
    subu t3, t3, t8
    blez t3, PM_Lret
    lhu t3, %lo(PM_LBUF + 4)
    subu t3, t3, a0
    andi t3, t3, 0xffff
    sltu t5, t3, a1
    beqz t5, PM_Lent
    lbu t5, %lo(PM_LBUF + 6)        # (older entries come later: the oldest stays)
    lw v1, %lo(PM_DST)
    andi t0, a0, 7
    addu v1, v1, t0
    addu v1, v1, t3
    b PM_Lent
    sb t5, 0(v1)
PM_Lret:
    lw ra, %lo(PM_LRA)
    jr ra
    nop
#endif

    .data
    .align 2
PM_RA:      .word 0
PM_S:       .word 0                 # the voice's sample (interpolated or noise)
PM_DST:     .word 0
#ifdef RSPDSP_STREAM
PM_LRA:     .word 0
    .align 3
PM_LBUF:    .ds.b 8
#endif
    .text
#endif /* RSPDSP_PM */

#ifndef RSPDSP_NO_ECHO
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
#ifdef ECHO_SYNC
    li t4, %lo(SCR_ECHO)            # (read here)
#else
    sw t1, %lo(ECHO_ADR)
    bne t5, t1, Echo_sync           # no prefetch of this address
    lw t4, %lo(PREF_BUF)
    jal DMAWaitIdle
    nop
    b Echo_have
    nop
#endif
Echo_sync:
    lw t3, %lo(DSP + RSPDSP_P_ARAM)
    li t5, ~7
    and t5, t1, t5
    addu s0, t3, t5
    move s4, t4
    jal DMAIn
    li t0, DMA_SIZE(8, 1)
Echo_have:
#ifndef ECHO_SYNC                     /* (else t1, and s0 for the write, kept) */
    lw t1, %lo(ECHO_ADR)
#endif
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
#ifndef ECHO_SYNC
    lw t1, %lo(ECHO_ADR)
    lw t3, %lo(DSP + RSPDSP_P_ARAM)
    li t5, ~7
    and t5, t1, t5
    addu s0, t3, t5
#endif
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
#ifndef ECHO_SYNC
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
#endif
    lw ra, %lo(SAVE_RA2)
    jr ra
    nop

#else
########################################################################
# Echo, no-echo variant (RSPDSP_NO_ECHO, rsp_snesdsp_ne.S): used only for
# batches where echo writes are off and both echo volumes are 0 from start
# to end (the CPU checks the state and every logged write: choose_overlay
# in snesdsp_rsp.c). Echo then changes neither the output nor ARAM; what it
# leaves behind is its position (indices, as below) and the FIR history,
# which holds the bytes read at the batch's last eight samples, so only
# those are read. In: $v22/$v23 totals. Out: a0/a1 = the totals.
########################################################################
Echo:
    sw ra, %lo(SAVE_RA2)
    mfc2 a0, $v22.e0
    mfc2 a1, $v23.e0
    addiu t1, s5, 8
    slt t1, t1, s6                  # more than eight samples left: no read
    bnez t1, 2f
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
    lhu t6, %lo(DSP + RSPDSP_FIRINDEX)
    andi t1, t1, 4
    addiu v1, t1, %lo(SCR_ECHO)
    sll t6, t6, 1
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
2:  # indices (as the full Echo)
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
    lw ra, %lo(SAVE_RA2)
    jr ra
    nop

#endif
    .data
    .align 2
ECHO_ADR:   .word 0
PREF_ADR:   .word 0
PREF_BUF:   .word 0
A_MASK:     .word 0
    .text
