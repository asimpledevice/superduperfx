/* Inline SPC-side checks for the deferred S-DSP (see apu.h). Included by
 * spc.c and apu.c once both Apu and Spc are complete types. */
#ifndef APU_DSP_WATCH_H
#define APU_DSP_WATCH_H

#include "apu.h"
#include "spc.h"

#if SNESRECOMP_ECHO_IN_PLACE && !(SNESRECOMP_APU_DEFER_DSP && SNESRECOMP_DSP_FRONTEND)
#error "SNESRECOMP_ECHO_IN_PLACE needs SNESRECOMP_APU_DEFER_DSP and SNESRECOMP_DSP_FRONTEND"
#endif
#if SNESRECOMP_DSP_STREAM
/* The audio split: the SPC's ARAM accesses leave the fast path only on
 * pages where the S-DSP may still touch an echo line (its ring in use, or a
 * line stamped past its progress). apu.c decides exactly per line. */
#define APU_DSP_PAGE_BIT(marks, adr) ((marks)[(adr) >> 11] & (1u << (((adr) >> 8) & 7)))
void apu_dspWriteSlow(Apu* apu, uint16_t adr, uint8_t val);
uint8_t apu_dspReadSlow(Apu* apu, uint16_t adr);
static inline bool apu_streamLate(const Apu* apu, uint32_t stamp) {
  return (int32_t)(stamp - apu->dspAt) > 0;
}
static inline void apu_spcWriteRam(Apu* apu, uint16_t adr, uint8_t val) {
  APU_FE_BEFORE_ARAM_WRITE(apu);
  APU_FE_ECHO_LOG(apu, adr, 1);
  const uint32_t p = adr >> 8;
  if (__builtin_expect(((apu->echoRegion[p >> 3] >> (p & 7)) & 1) ||
                       apu_streamLate(apu, apu->echoPageTouch[p]) ||
                       apu_streamLate(apu, apu->echoPageDirty[p]), 0))
    apu_dspWriteSlow(apu, adr, val);
  else
    apu->ram[adr] = val;
}
static inline uint8_t apu_spcReadRam(Apu* apu, uint16_t adr) {
  APU_FE_ECHO_LOG(apu, adr, 0);
  const uint32_t p = adr >> 8;
  if (__builtin_expect((apu->fe.echo_writes && ((apu->echoRegion[p >> 3] >> (p & 7)) & 1)) ||
                       apu_streamLate(apu, apu->echoPageDirty[p]), 0))
    return apu_dspReadSlow(apu, adr);
  return apu->ram[adr];
}
#elif SNESRECOMP_APU_DEFER_DSP
/* Page marks during a batch (bits in Spc.dspWatchLog/dspWatchSync): LOG =
 * the DSP may read the page, so SPC writes there are logged; SYNC = the DSP
 * may write it (echo), so SPC reads there sync first. All clear between
 * batches. */
#define APU_DSP_PAGE_BIT(marks, adr) ((marks)[(adr) >> 11] & (1u << (((adr) >> 8) & 7)))
/* Writes that leave the inline path: to a page the batch may read, or one
 * whose copy in the batch host's view is current. */
#ifdef SNESRECOMP_DSP_LAUNCH_HOOK
#define APU_DSP_WRITE_SLOW(spc, adr) \
  (((spc)->dspWatchLog[(adr) >> 11] | (spc)->dspViewFresh[(adr) >> 11]) & (1u << (((adr) >> 8) & 7)))
#else
#define APU_DSP_WRITE_SLOW(spc, adr) APU_DSP_PAGE_BIT((spc)->dspWatchLog, adr)
#endif
/* An SPC write to plain ARAM. The check stays small because it is inlined
 * into every SPC store; logging is out of line. */
static inline void apu_spcWriteRam(Apu* apu, uint16_t adr, uint8_t val) {
  APU_FE_BEFORE_ARAM_WRITE(apu);
  APU_FE_ECHO_LOG(apu, adr, 1);
#if SNESRECOMP_APU_DEFER_DSP_VERIFY
  if (apu->dspPending) { apu_dspLogRam(apu, adr, val); return; }
#endif
  if (__builtin_expect(APU_DSP_WRITE_SLOW(apu->spc, adr), 0))
    apu_dspWriteSlow(apu, adr, val);
  else
    apu->ram[adr] = val;
}
/* An SPC read of plain ARAM: pages the DSP writes (echo) may hold owed data. */
#if SNESRECOMP_ECHO_IN_PLACE
uint8_t apu_dspReadSlow(Apu* apu, uint16_t adr);
#endif
static inline uint8_t apu_spcReadRam(Apu* apu, uint16_t adr) {
  APU_FE_ECHO_LOG(apu, adr, 0);
  if (__builtin_expect(APU_DSP_PAGE_BIT(apu->spc->dspWatchSync, adr), 0)) {
#if SNESRECOMP_ECHO_IN_PLACE
    return apu_dspReadSlow(apu, adr);
#else
    apu_dspEchoRead(apu);
#endif
  }
  return apu->ram[adr];
}
#else
static inline void apu_spcWriteRam(Apu* apu, uint16_t adr, uint8_t val) {
  APU_FE_BEFORE_ARAM_WRITE(apu);
  APU_FE_ECHO_LOG(apu, adr, 1);
  apu->ram[adr] = val;
}
static inline uint8_t apu_spcReadRam(Apu* apu, uint16_t adr) {
  APU_FE_ECHO_LOG(apu, adr, 0);
  return apu->ram[adr];
}
#endif

#endif
