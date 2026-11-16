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
  uint8_t isReg;  /* 1 register write, 0 ARAM write (2: verify builds;
                     3: diverted, SNESRECOMP_ECHO_IN_PLACE) */
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
 * are the pages the batch can read. viewFresh (32 bytes) is the host's to
 * keep: it sets a page's bit when its own copy of the page equals ARAM, and
 * the runtime clears the bit whenever ARAM's page changes otherwise. Until SNESRECOMP_DSP_JOIN_HOOK returns,
 * the Dsp still shows the batch's start state; the runtime joins before
 * anything can observe the DSP or the echo bytes it writes back. */
/* SNESRECOMP_DSP_FRONTEND: the batch being launched also comes with the
 * records the S-DSP front end produced for it (runner/src/snes/
 * dsp_frontend.c): each decoded block (16 bytes: big-endian start address,
 * the 9 bytes as read at that sample, 5 pad) and each key on's start
 * address, in the order the S-DSP consumes them (sample by sample, voices
 * 0-7). `valid` is false when they cannot stand for ARAM: the front end was
 * not tracking every voice (pitch modulation), a read touched an echo
 * window while echo writes were on, or a buffer filled. The arrays stay
 * unchanged until the next launch's batch completes. */
typedef struct ApuDspRecords {
  const void *blocks;
  uint32_t nblocks;
  const uint16_t *keyons;
  uint32_t nkeyons;
  bool valid;
  /* SNESRECOMP_ECHO_IN_PLACE: the 16-byte lines of ARAM whose echo slots the
   * batch reads or writes, as runs of lines (wrapping at 4096). The host
   * writes them back and drops them from its data cache before the batch
   * starts; nruns > 16 means: the whole ARAM. Log entries with isReg 3 are
   * SPC writes diverted to the S-DSP: ARAM does not hold them; the batch
   * applies them at their tags. Other ARAM entries are already in ARAM. */
  uint16_t run_line[16], run_count[16];
  uint32_t nruns;
} ApuDspRecords;
#if SNESRECOMP_ECHO_IN_PLACE
/* Host cache hooks (the N64 build: n64/rspdsp/snesdsp_rsp.c): write back
 * and drop `len` bytes at `p` from the CPU's data cache; read one byte past
 * the cache. */
void apu_echo_cache_drop(const void *p, uint32_t len);
uint8_t apu_echo_read_uncached(const uint8_t *p);
#endif
extern ApuDspRecords g_apu_dsp_records;

#ifdef SNESRECOMP_DSP_LAUNCH_HOOK
bool SNESRECOMP_DSP_LAUNCH_HOOK(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log,
                                uint32_t n, uint32_t samples, const uint8_t *pageMarks,
                                uint8_t *viewFresh);
void SNESRECOMP_DSP_JOIN_HOOK(void);
#endif

#endif
