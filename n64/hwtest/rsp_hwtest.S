# Hardware test microcode: the RSP side of hwtest_main.c's experiments on
# CPU/RSP signalling (DMEM access during a run, the semaphore, the SIG bits
# as a byte, doorbell latency, DMA under CPU traffic). Standalone (no rspq),
# so the test owns every SIG bit. The CPU writes HT_MODE and parameters
# while the RSP is halted; each mode ends with BREAK.

#include <rsp.inc>
#include "hwtest_layout.h"

    .set noreorder
    .set at

    .data
    .ds.b 16

    .text
    .globl _start
_start:
    lw t0, HT_MODE(zero)
    li t1, HT_M_COUNT
    beq t0, t1, m_count
    li t1, HT_M_SEQ
    beq t0, t1, m_seq
    li t1, HT_M_ECHO
    beq t0, t1, m_echo
    li t1, HT_M_SEM
    beq t0, t1, m_sem
    li t1, HT_M_SIGIN
    beq t0, t1, m_sigin
    li t1, HT_M_SIGOUT
    beq t0, t1, m_sigout
    li t1, HT_M_BELL
    beq t0, t1, m_bell
    li t1, HT_M_DMA
    beq t0, t1, m_dma
    li t1, HT_M_SPIN
    beq t0, t1, m_spin
    li t1, HT_M_WORD
    beq t0, t1, m_word
    li t1, HT_M_BLOCK
    beq t0, t1, m_block
    nop
finish:
    break
    nop
1:  b 1b
    nop

# COUNT++ and an unprotected record (k, ~k, 3k, k ^ 0x5a5a5a5a), until SIG7.
m_count:
    lw s0, HT_COUNT(zero)
    li s7, 0x5a5a5a5a
1:  addiu s0, s0, 1
    sw s0, HT_COUNT(zero)
    sw s0, HT_REC + 0(zero)
    nor t0, s0, zero
    sw t0, HT_REC + 4(zero)
    sll t1, s0, 1
    addu t1, t1, s0
    sw t1, HT_REC + 8(zero)
    xor t2, s0, s7
    sw t2, HT_REC + 12(zero)
    mfc0 t9, COP0_SP_STATUS
    andi t9, t9, SP_STATUS_SIG7
    beqz t9, 1b
    nop
    j finish
    nop

# The same record between two increments of SEQ (odd while writing), then
# PARAM idle loop passes (2 cycles each) before the next update: 0 rewrites
# back to back, faster than the CPU can read the record.
m_seq:
    move s0, zero
    move s1, zero
    li s7, 0x5a5a5a5a
    lw s6, HT_PARAM(zero)
1:  addiu s0, s0, 1
    addiu s1, s1, 1                 # odd: writing
    sw s1, HT_SEQ(zero)
    sw s0, HT_REC + 0(zero)
    nor t0, s0, zero
    sw t0, HT_REC + 4(zero)
    sll t1, s0, 1
    addu t1, t1, s0
    sw t1, HT_REC + 8(zero)
    xor t2, s0, s7
    sw t2, HT_REC + 12(zero)
    addiu s1, s1, 1                 # even: stable
    sw s1, HT_SEQ(zero)
    sw s0, HT_COUNT(zero)
    move t8, s6
2:  bnez t8, 2b
    addiu t8, t8, -1
    mfc0 t9, COP0_SP_STATUS
    andi t9, t9, SP_STATUS_SIG7
    beqz t9, 1b
    nop
    j finish
    nop

# OUT = IN, COUNT++, until SIG7; then the sum of GUARD into RES0.
m_echo:
    lw s0, HT_COUNT(zero)
1:  lw t0, HT_IN(zero)
    sw t0, HT_OUT(zero)
    addiu s0, s0, 1
    sw s0, HT_COUNT(zero)
    mfc0 t9, COP0_SP_STATUS
    andi t9, t9, SP_STATUS_SIG7
    beqz t9, 1b
    nop
    li t1, HT_GUARD
    li t2, HT_GUARD_END
    move v0, zero
2:  lw t0, 0(t1)
    addiu t1, t1, 4
    bne t1, t2, 2b
    addu v0, v0, t0
    sw v0, HT_RES + 0(zero)
    j finish
    nop

# PARAM increments of SHARED, each under the semaphore; retries into RES1.
m_sem:
    lw s1, HT_PARAM(zero)
    move s2, zero
1:  beqz s1, 3f
    nop
2:  mfc0 t0, COP0_SEMAPHORE         # reading takes it if it was free (0)
    beqz t0, 4f
    nop
    b 2b
    addiu s2, s2, 1
4:  lw t1, HT_SHARED(zero)
    addiu t1, t1, 1
    sw t1, HT_SHARED(zero)
    mtc0 zero, COP0_SEMAPHORE       # release
    lw t2, HT_RSPN(zero)
    addiu t2, t2, 1
    sw t2, HT_RSPN(zero)
    b 1b
    addiu s1, s1, -1
3:  sw s2, HT_RES + 4(zero)
    j finish
    nop

# For each new IN (a sequence number from the CPU, which set the SIG byte
# first): OUT = IN << 8 | SIG byte as the RSP reads it. IN = -1 stops.
m_sigin:
    move s0, zero
    li s6, -1
1:  lw t0, HT_IN(zero)
    beq t0, s6, 2f
    nop
    beq t0, s0, 1b
    nop
    move s0, t0
    mfc0 t1, COP0_SP_STATUS
    srl t1, t1, 7
    andi t1, t1, 0xff
    sll t2, t0, 8
    or t2, t2, t1
    b 1b
    sw t2, HT_OUT(zero)
2:  j finish
    nop

# For v in 0..255: set the SIG byte to v (one write of set/clear masks),
# COUNT = v + 1, wait for IN == v + 1. A wait that times out stores v + 1
# in RES3 and ends. Clears every SIG bit at the end.
m_sigout:
    move s0, zero                   # v
    sw zero, HT_RES + 12(zero)
1:  move t3, zero                   # mask
    move t4, zero                   # bit i
2:  srlv t5, s0, t4
    andi t5, t5, 1
    sll t6, t4, 1                   # 2i
    addiu t6, t6, 9                 # clear bit 9 + 2i
    addu t6, t6, t5                 # set bit 10 + 2i when the bit is 1
    li t7, 1
    sllv t7, t7, t6
    or t3, t3, t7
    addiu t4, t4, 1
    li t8, 8
    bne t4, t8, 2b
    nop
    mtc0 t3, COP0_SP_STATUS
    addiu s1, s0, 1
    sw s1, HT_COUNT(zero)
    li s2, 0x2000000                # wait budget (loop passes)
3:  lw t0, HT_IN(zero)
    beq t0, s1, 4f
    addiu s2, s2, -1
    bnez s2, 3b
    nop
    b 5f
    sw s1, HT_RES + 12(zero)        # timed out at v + 1
4:  li t8, 256
    bne s1, t8, 1b
    move s0, s1
5:  li t0, 0xAAAA00                 # clear SIG0-SIG7
    mtc0 t0, COP0_SP_STATUS
    j finish
    nop

# Doorbell: on SIG3, clear SIG3 and set SIG0 (and raise the SP interrupt
# when PARAM is set) in one write, COUNT++. Until SIG7.
m_bell:
    lw s0, HT_COUNT(zero)
    li s1, SP_WSTATUS_CLEAR_SIG3 | SP_WSTATUS_SET_SIG0
    lw t0, HT_PARAM(zero)
    beqz t0, 1f
    nop
    ori s1, s1, SP_WSTATUS_SET_INTR
1:  mfc0 t0, COP0_SP_STATUS
    andi t1, t0, SP_STATUS_SIG7
    bnez t1, 2f
    andi t1, t0, SP_STATUS_SIG3
    beqz t1, 1b
    nop
    mtc0 s1, COP0_SP_STATUS
    addiu s0, s0, 1
    b 1b
    sw s0, HT_COUNT(zero)
2:  j finish
    nop

# DMA 1 KB from RDRAM PARAM into DMABUF, sum it; a sum other than PARAM2
# counts in RES2. COUNT++ per pass, until SIG7.
m_dma:
    lw s3, HT_PARAM(zero)
    lw s4, HT_PARAM2(zero)
    move s5, zero
    lw s0, HT_COUNT(zero)
1:  li t0, HT_DMABUF
    mtc0 t0, COP0_DMA_SPADDR
    mtc0 s3, COP0_DMA_RAMADDR
    li t0, HT_DMABUF_SIZE - 1
    mtc0 t0, COP0_DMA_READ
2:  mfc0 t0, COP0_DMA_BUSY
    bnez t0, 2b
    nop
    li t1, HT_DMABUF
    li t2, HT_DMABUF + HT_DMABUF_SIZE
    move v0, zero
3:  lw t0, 0(t1)
    addiu t1, t1, 4
    bne t1, t2, 3b
    addu v0, v0, t0
    beq v0, s4, 4f
    nop
    addiu s5, s5, 1
    sw s5, HT_RES + 8(zero)
4:  addiu s0, s0, 1
    sw s0, HT_COUNT(zero)
    mfc0 t9, COP0_SP_STATUS
    andi t9, t9, SP_STATUS_SIG7
    beqz t9, 1b
    nop
    j finish
    nop

# v3. COUNT++ until SIG7; with PARAM set, take the semaphore first and hold
# it throughout (so CPU reads of it all return 1).
m_spin:
    lw s6, HT_PARAM(zero)
    beqz s6, 2f
    nop
1:  mfc0 t1, COP0_SEMAPHORE
    bnez t1, 1b
    nop
2:  lw s0, HT_COUNT(zero)
3:  addiu s0, s0, 1
    sw s0, HT_COUNT(zero)
    mfc0 t9, COP0_SP_STATUS
    andi t9, t9, SP_STATUS_SIG7
    beqz t9, 3b
    nop
    beqz s6, 4f
    nop
    mtc0 zero, COP0_SEMAPHORE
4:  j finish
    nop

# v3. One-word boxes. COUNT++ each pass (the background work); a new DOWNW
# is answered in UPW with the same top byte and payload + 1 (low 24 bits),
# then SIG0 if PARAM bit 0, plus the SP interrupt if bit 1. New means: it
# differs from the last one, or with PARAM bit 2, SIG3 is set (the RSP
# clears SIG3 once it has read the word). Until SIG7.
m_word:
    lw s3, HT_PARAM(zero)
    li s4, SP_WSTATUS_SET_SIG0
    andi t0, s3, 2
    beqz t0, 0f
    nop
    ori s4, s4, SP_WSTATUS_SET_INTR
0:  lw s0, HT_COUNT(zero)
    move s1, zero                   # the last DOWNW
    li s5, 0x00FFFFFF
1:  addiu s0, s0, 1
    sw s0, HT_COUNT(zero)
    andi t3, s3, 4
    beqz t3, 8f
    nop
    mfc0 t3, COP0_SP_STATUS         # SIG3 says a word is waiting
    andi t3, t3, SP_STATUS_SIG3
    beqz t3, 2f
    nop
    lw t0, HT_DOWNW(zero)
    li t3, SP_WSTATUS_CLEAR_SIG3
    b 9f
    mtc0 t3, COP0_SP_STATUS
8:  lw t0, HT_DOWNW(zero)
    beq t0, s1, 2f
    nop
9:  move s1, t0
    srl t1, t0, 24
    sll t1, t1, 24                  # seq << 24
    addiu t2, t0, 1
    and t2, t2, s5
    or t2, t2, t1
    sw t2, HT_UPW(zero)
    andi t3, s3, 3                  # a doorbell to ring?
    beqz t3, 2f
    nop
    mtc0 s4, COP0_SP_STATUS
2:  mfc0 t9, COP0_SP_STATUS
    andi t9, t9, SP_STATUS_SIG7
    beqz t9, 1b
    nop
    j finish
    nop

# v3. Block boxes. COUNT++ each pass; every 64 passes one try of the
# semaphore, never a wait: busy counts a skip. Holding it: post a new UPB
# (word i = seq + i * 0x01000001 for i < 7, word 7 = ~seq) and, if DOWNB
# word 0 changed, check the CPU's record the same way (bad words counted).
# Results: RES0 skips, RES1 bad words, RES2 DOWNBs seen, RES3 UPBs posted.
m_block:
    li s7, 0x01000001
    move s0, zero
    move s1, zero                   # UPB sequence
    move s2, zero                   # the last DOWNB word 0
    move s3, zero
    move s4, zero
    move s5, zero
1:  addiu s0, s0, 1
    sw s0, HT_COUNT(zero)
    andi t0, s0, 63
    bnez t0, 5f
    nop
    mfc0 t0, COP0_SEMAPHORE         # reading takes it if it was free
    beqz t0, 2f
    nop
    b 5f
    addiu s3, s3, 1
2:  addiu s1, s1, 1
    move t1, s1
    li t2, HT_UPB
    li t3, 7
3:  sw t1, 0(t2)
    addu t1, t1, s7
    addiu t3, t3, -1
    bnez t3, 3b
    addiu t2, t2, 4
    nor t1, s1, zero
    sw t1, 0(t2)                    # word 7
    lw t4, HT_DOWNB(zero)
    beq t4, s2, 4f
    nop
    move s2, t4
    addiu s5, s5, 1
    move t1, t4
    li t2, HT_DOWNB
    li t3, 7
6:  lw t5, 0(t2)
    beq t5, t1, 7f
    nop
    addiu s4, s4, 1
7:  addu t1, t1, s7
    addiu t3, t3, -1
    bnez t3, 6b
    addiu t2, t2, 4
    lw t5, 0(t2)
    nor t1, t4, zero
    beq t5, t1, 4f
    nop
    addiu s4, s4, 1
4:  mtc0 zero, COP0_SEMAPHORE       # release
5:  mfc0 t9, COP0_SP_STATUS
    andi t9, t9, SP_STATUS_SIG7
    beqz t9, 1b
    nop
    sw s3, HT_RES + 0(zero)
    sw s4, HT_RES + 4(zero)
    sw s5, HT_RES + 8(zero)
    sw s1, HT_RES + 12(zero)
    j finish
    nop
