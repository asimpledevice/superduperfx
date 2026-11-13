/* DMEM layout shared by rsp_hwtest.S and hwtest_main.c (absolute DMEM
 * offsets; the CPU sees them at SP_DMEM + offset). */
#ifndef HWTEST_LAYOUT_H
#define HWTEST_LAYOUT_H

#define HT_MODE    0x00   /* test the RSP runs (written while it is halted) */
#define HT_PARAM   0x04
#define HT_PARAM2  0x08
#define HT_COUNT   0x10   /* RSP loop counter */
#define HT_REC     0x20   /* 4-word record: k, ~k, 3k, k ^ 0x5a5a5a5a */
#define HT_SEQ     0x30   /* sequence counter around HT_REC (mode SEQ) */
#define HT_IN      0x40   /* written by the CPU */
#define HT_OUT     0x44   /* written by the RSP */
#define HT_SHARED  0x48   /* counter both sides increment under the semaphore */
#define HT_RSPN    0x4c   /* RSP increments done */
#define HT_RES     0x50   /* 8 result words */
#define HT_SCRATCH 0x70   /* a word the CPU hammers */
/* Mailboxes (v3): one-word boxes hold seq << 24 | payload; the block
 * boxes hold 8 words written under the semaphore. Down = CPU to RSP. */
#define HT_DOWNW   0x74
#define HT_UPW     0x78
#define HT_DOWNB   0x80   /* 8 words */
#define HT_UPB     0xA0   /* 8 words */
#define HT_GUARD   0x100  /* 2 KB pattern nobody writes during a test */
#define HT_GUARD_END 0x900
#define HT_DMABUF  0xA00  /* 1 KB DMA target */
#define HT_DMABUF_SIZE 0x400

/* Modes */
#define HT_M_COUNT  1   /* COUNT++ and an unprotected record, until SIG7 */
#define HT_M_SEQ    2   /* the record under a sequence counter, PARAM idle passes between updates, until SIG7 */
#define HT_M_ECHO   3   /* OUT = IN, COUNT++, until SIG7; then GUARD sum -> RES0 */
#define HT_M_SEM    4   /* PARAM increments of SHARED under the semaphore; retries -> RES1 */
#define HT_M_SIGIN  5   /* for each IN = seq: OUT = seq << 8 | SIG byte; until SIG7 */
#define HT_M_SIGOUT 6   /* for v in 0..255: SIG byte = v, COUNT = v + 1, wait IN == v + 1 */
#define HT_M_BELL   7   /* on SIG3: clear SIG3, set SIG0 (+ interrupt if PARAM), COUNT++; until SIG7 */
#define HT_M_DMA    8   /* DMA 1 KB from PARAM to DMABUF, sum it, mismatches vs PARAM2 -> RES2; COUNT++; until SIG7 */
#define HT_M_SPIN   9   /* COUNT++ until SIG7, holding the semaphore throughout if PARAM */
#define HT_M_WORD  10   /* COUNT++; on a new DOWNW (changed, or SIG3 set with PARAM bit 2): UPW = top byte | (payload + 1), then SIG0 (PARAM bit 0) and the interrupt (bit 1); until SIG7 */
#define HT_M_BLOCK 11   /* COUNT++; every 64 passes try the semaphore once: post UPB, check a new DOWNB; skips, bad words, DOWNBs seen, UPBs posted -> RES0-3; until SIG7 */

#endif
