/* Inline SPC-side checks for the deferred S-DSP (see apu.h). Included by
 * spc.c and apu.c once both Apu and Spc are complete types. */
#ifndef APU_DSP_WATCH_H
#define APU_DSP_WATCH_H

#include "apu.h"
#include "spc.h"

#if SNESRECOMP_APU_DEFER_DSP
/* Page marks during a batch (bits in Spc.dspWatchLog/dspWatchSync): LOG =
 * the DSP may read the page, so SPC writes there are logged; SYNC = the DSP
 * may write it (echo), so SPC reads there sync first. All clear between
 * batches. */
#define APU_DSP_PAGE_BIT(marks, adr) ((marks)[(adr) >> 11] & (1u << (((adr) >> 8) & 7)))
/* An SPC write to plain ARAM. The check stays small because it is inlined
 * into every SPC store; logging is out of line. */
static inline void apu_spcWriteRam(Apu* apu, uint16_t adr, uint8_t val) {
#if SNESRECOMP_APU_DEFER_DSP_VERIFY
  if (apu->dspPending) { apu_dspLogRam(apu, adr, val); return; }
#endif
  if (__builtin_expect(APU_DSP_PAGE_BIT(apu->spc->dspWatchLog, adr), 0))
    apu_dspLogRam(apu, adr, val);
  else
    apu->ram[adr] = val;
}
/* An SPC read of plain ARAM: pages the DSP writes (echo) may hold owed data. */
static inline void apu_spcReadCheck(Apu* apu, uint16_t adr) {
  if (__builtin_expect(APU_DSP_PAGE_BIT(apu->spc->dspWatchSync, adr), 0))
    apu_dspEchoRead(apu);
}
#else
static inline void apu_spcWriteRam(Apu* apu, uint16_t adr, uint8_t val) {
  apu->ram[adr] = val;
}
static inline void apu_spcReadCheck(Apu* apu, uint16_t adr) { (void)apu; (void)adr; }
#endif

#endif
