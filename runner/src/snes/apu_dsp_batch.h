/* Deferred S-DSP batches: the log entry type and the optional host hook
 * that runs a batch elsewhere (the N64 build runs it on the RSP). Plain C;
 * included by apu.h and by host code that cannot include the full runtime
 * headers. */
#ifndef APU_DSP_BATCH_H
#define APU_DSP_BATCH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Dsp Dsp;

typedef struct ApuDspLogEntry {
  uint16_t adr;   /* ARAM address, or DSP register for a register write */
  uint8_t val;
  uint8_t old;    /* ARAM value before the write (ARAM entries) */
  uint16_t tag;   /* samples owed when written */
  uint8_t isReg;  /* 1 register write, 0 ARAM write (2: verify builds) */
} ApuDspLogEntry;

/* SNESRECOMP_DSP_BATCH_HOOK names a host function that runs a batch:
 * `samples` DSP samples with `log` applied at its tags (writes tagged k
 * before sample k; then writes tagged `samples`), reading and writing `aram`
 * (64 KB, already returned to its state at the start of the batch), with the
 * same results as the C DSP, output samples included. It returns false,
 * having changed nothing, to leave the batch to the C DSP. aramEndReachable
 * says the DSP may read ARAM page $FF during the batch (a BRR block could
 * wrap past $FFFF). */
#ifdef SNESRECOMP_DSP_BATCH_HOOK
bool SNESRECOMP_DSP_BATCH_HOOK(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log,
                               uint32_t n, uint32_t samples, bool aramEndReachable);
#endif

/* Asynchronous variant: SNESRECOMP_DSP_LAUNCH_HOOK starts a batch and
 * returns at once (or returns false, having changed nothing, for the C DSP).
 * Unlike the synchronous hook it gets ARAM as it is now, with `log` holding
 * the values each logged write replaced; pageMarks (32 bytes, bit per page)
 * are the pages the batch can read. Until SNESRECOMP_DSP_JOIN_HOOK returns,
 * the Dsp still shows the batch's start state; the runtime joins before
 * anything can observe the DSP or the echo bytes it writes back. */
#ifdef SNESRECOMP_DSP_LAUNCH_HOOK
bool SNESRECOMP_DSP_LAUNCH_HOOK(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log,
                                uint32_t n, uint32_t samples, const uint8_t *pageMarks);
void SNESRECOMP_DSP_JOIN_HOOK(void);
#endif

#endif
