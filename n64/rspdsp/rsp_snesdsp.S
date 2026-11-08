# S-DSP on the RSP: runs batches of samples for the deferred DSP.
#
# The CPU converts the canonical C DSP state (runner/src/snes/dsp.c) into an
# RspDspState block (snesdsp_rsp.h), queues SnesDspCmd_Run, and converts the
# block back when the batch is done. The arithmetic must match dsp.c bit for
# bit; n64/rspdsp/test compares the two on random states.
#
# Milestone 1: the noise generator only (dsp_handleNoise).

#include <rsp_queue.inc>
#include "snesdsp_rsp_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand SnesDspCmd_Run, 8
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 4
DSP_STATE: .ds.b RSPDSP_STATE_SIZE
RSPQ_EndSavedState

    .text

# a0: RDRAM address of the RspDspState block (8-byte aligned)
# a1: number of samples to run
SnesDspCmd_Run:
    move s3, ra                     # jal below clobbers the return into rspq
    li t0, 0xffffff
    and s0, a0, t0                  # top byte of a0 is the command id
    move s7, s0                     # keep the RDRAM address for write-back
    move s6, a1                     # samples left
    li s4, %lo(DSP_STATE)
    jal DMAIn
    li t0, DMA_SIZE(RSPDSP_STATE_SIZE, 1)

    beqz s6, Run_done
    nop
Run_sample:
    jal Dsp_Noise
    nop
    addiu s6, s6, -1
    bnez s6, Run_sample
    nop

Run_done:
    move ra, s3                     # DMAOut returns straight to rspq
    move s0, s7
    li s4, %lo(DSP_STATE)
    li t0, DMA_SIZE(RSPDSP_STATE_SIZE, 1)
    j DMAOut
    nop

/* dsp_handleNoise:
 *   if (noiseRate) noiseCounter++;
 *   if (noiseRate && noiseCounter >= noiseRate) {
 *     bit = (ns & 1) ^ ((ns >> 1) & 1);
 *     ns = ((ns >> 1) & 0x3fff) | (bit << 14);
 *     ns = (int16_t)((ns & 0x7fff) << 1) >> 1;
 *     noiseCounter = 0;
 *   }
 * (Assembler '#' comments go through the C preprocessor: keep them away
 * from words like "if" at the start of a line.) */
Dsp_Noise:
    lhu t0, %lo(DSP_STATE) + RSPDSP_NOISE_RATE
    beqz t0, Noise_done
    lhu t1, %lo(DSP_STATE) + RSPDSP_NOISE_COUNTER
    addiu t1, t1, 1
    andi t1, t1, 0xffff
    sltu t2, t1, t0
    bnez t2, Noise_store
    lh t3, %lo(DSP_STATE) + RSPDSP_NOISE_SAMPLE
    srl t4, t3, 1                   # ns >> 1 (arithmetic not needed: masked to 14 bits)
    xor t5, t3, t4
    andi t5, t5, 1                  # bit
    andi t4, t4, 0x3fff
    sll t5, t5, 14
    or t4, t4, t5
    andi t4, t4, 0x7fff
    sll t4, t4, 17                  # (x << 1) as int16, sign at bit 31
    sra t4, t4, 17                  # >> 1 arithmetic, back to an int16 value
    sh t4, %lo(DSP_STATE) + RSPDSP_NOISE_SAMPLE
    move t1, zero
Noise_store:
    sh t1, %lo(DSP_STATE) + RSPDSP_NOISE_COUNTER
Noise_done:
    jr ra
    nop
