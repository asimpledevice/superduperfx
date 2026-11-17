/* S-DSP batches on the RSP: libdragon glue. Only this file and the test
 * include <libdragon.h>; the runtime's DSP state comes from dsp.h.
 * The N64 CPU and the RSP are both big-endian, so the state block is read
 * and written natively. */
#include <libdragon.h>
#include <stddef.h>
#include <string.h>

#include "snesdsp_rsp.h"
#include "blue_thread.h"
#include "../src/red.h"
#include "dsp.h"
#include "apu_dsp_batch.h"

/* Launch batches through Red's DMEM command ring (no flush, no fetch DMA;
 * a batch depends on nothing queued in RDRAM). 0: through the RDRAM queue. */
#ifndef RSPDSP_RING
#define RSPDSP_RING 1
#endif

DEFINE_RSP_UCODE(rsp_snesdsp);
DEFINE_RSP_UCODE(rsp_snesdsp_ne);   /* no-echo variant (rsp_snesdsp_ne.S) */
#if RSPDSP_VM
DEFINE_RSP_UCODE(rsp_snesdsp_vm);   /* voice-major variant (rsp_snesdsp_vm.S) */
static uint32_t s_overlay_vm_id;
uint32_t g_snesdsp_rsp_vm_batches;  /* batches run on the voice-major variant */
#endif
/* RSPDSP_PREP=1 (step 2a): register writes are decoded on the RSP
 * (rsp_snesdsp_prep.S) instead of here; the C decode stays the reference. */
#ifndef RSPDSP_PREP
#define RSPDSP_PREP 0
#endif
#if RSPDSP_PREP
DEFINE_RSP_UCODE(rsp_snesdsp_prep);
static uint32_t s_prep_id;
static struct __attribute__((aligned(16))) { uint8_t b[RSPPREP_PARAMS_SIZE]; } s_prm;
#endif

enum { kMaxSamples = 1024, kMaxOps = 4096 };

static uint32_t s_overlay_id, s_overlay_ne_id;
static uint32_t s_run_overlay;           /* the variant the next batch runs on */
uint32_t g_snesdsp_rsp_ne_batches;       /* batches run on the no-echo variant */

/* The no-echo variant (rsp_snesdsp_ne.S) is exact for a batch in which echo
 * writes are off and both echo volumes are 0 from start to end: echo then
 * changes neither the output nor ARAM. Checked on the state at the batch's
 * start and on every logged register write (FLG bit 5 clear turns writes
 * on; EVOLL/EVOLR non-zero). Game-independent: nothing is assumed about the
 * driver. */
static inline bool echo_idle_write(bool reg, unsigned adr, unsigned val) {
  if (!reg) return true;
  adr &= 0x7f;
  if ((adr == 0x2c || adr == 0x3c) && val) return false;
  if (adr == 0x6c && !(val & 0x20)) return false;
  return true;
}
#define CHOOSE_OVERLAY(dsp, log, n, IS_REG)                                   \
  do {                                                                        \
    bool idle_ = !(dsp)->echoWrites && !(dsp)->echoVolumeL && !(dsp)->echoVolumeR; \
    for (uint32_t i_ = 0; idle_ && i_ < (n); i_++)                            \
      idle_ = echo_idle_write(IS_REG((log)[i_]), (log)[i_].adr, (log)[i_].val); \
    IDLE_FORCE(idle_);                                                        \
    s_run_overlay = idle_ ? s_overlay_ne_id : s_overlay_id;                   \
    if (idle_) g_snesdsp_rsp_ne_batches++;                                    \
  } while (0)
#ifdef RSPDSP_FORCE_NE   /* test only: the check must fail with this */
#define IDLE_FORCE(x) ((x) = true)
#else
#define IDLE_FORCE(x) ((void)0)
#endif
#define IS_REG_IN(e) ((e).isReg != 0)
#define IS_REG_APU(e) ((e).isReg == 1)
static RspDspState s_block;
static struct __attribute__((aligned(16))) { int16_t s[kMaxSamples * 2 + 8]; } s_out;
typedef struct { uint16_t tag; uint8_t kind, pad; uint16_t adr, val; } Op;
static struct __attribute__((aligned(16))) { Op op[kMaxOps + 32]; } s_ops;
static Dsp s_shadow;
/* The shadow decodes register writes; dsp_write touches nothing from the
 * output ring on, so only the state before it is copied. */
static inline void shadow_from(const Dsp *dsp) {
  memcpy(&s_shadow, dsp, offsetof(Dsp, sampleBuffer));
}
uint32_t g_snesdsp_rsp_ticks, g_snesdsp_rsp_samples;

/* The caller initialises rspq first (rspq_init, or rdpq_init). */
static void view_init(void);
#if SNESRECOMP_DSP_STREAM
static void stream_init(void);
#endif
void snesdsp_rsp_init(void) {
  s_overlay_id = rspq_overlay_register(&rsp_snesdsp);
  s_overlay_ne_id = rspq_overlay_register(&rsp_snesdsp_ne);
#if RSPDSP_VM
  s_overlay_vm_id = rspq_overlay_register(&rsp_snesdsp_vm);
  /* The voice-major variant's snapshot area is written by the RSP only: no
   * line of it may stay dirty in the cache from the startup BSS clear. */
  data_cache_hit_writeback_invalidate(s_block.vm_snap, sizeof s_block.vm_snap);
#endif
  s_run_overlay = s_overlay_id;
#if RSPDSP_PREP
  s_prep_id = rspq_overlay_register(&rsp_snesdsp_prep);
#endif
  view_init();
  /* Launches invalidate only the samples a batch writes: no line of the
   * output buffer may stay dirty from the startup BSS clear. */
  data_cache_hit_writeback_invalidate(&s_out, sizeof s_out);
#if SNESRECOMP_DSP_STREAM
  stream_init();
#endif
}

static inline void put(RspDspState *s, unsigned off, int v) {
  *(int16_t *)(s->bytes + off) = (int16_t)v;
}
static inline int16_t get(const RspDspState *s, unsigned off) {
  return *(const int16_t *)(s->bytes + off);
}
static inline uint16_t getu(const RspDspState *s, unsigned off) {
  return *(const uint16_t *)(s->bytes + off);
}

/* u/c split of coefficient k for a product shifted right by `shift`. */
static inline void put_uc(RspDspState *s, unsigned off_u, unsigned off_c, int k, int shift) {
  const int32_t K = (int32_t)k * (1 << (16 - shift));
  put(s, off_u, (int)(uint16_t)(K & 0xffff));
  put(s, off_c, (int)(K >> 16));
}

#define MASK(field) ({ unsigned m_ = 0; \
  for (int c_ = 0; c_ < 8; c_++) if (dsp->channel[c_].field) m_ |= 1u << c_; m_; })

static void pack_coefficients(const Dsp *dsp, RspDspState *s) {
  for (int c = 0; c < 8; c++) {
    put_uc(s, RSPDSP_V_VOLL_U + 2 * c, RSPDSP_V_VOLL_C + 2 * c, dsp->channel[c].volumeL, 6);
    put_uc(s, RSPDSP_V_VOLR_U + 2 * c, RSPDSP_V_VOLR_C + 2 * c, dsp->channel[c].volumeR, 6);
    put(s, RSPDSP_V_ECHOMASK + 2 * c, dsp->channel[c].echoEnable ? -1 : 0);
    put_uc(s, RSPDSP_FIR_U + 2 * c, RSPDSP_FIR_C + 2 * c, dsp->firValues[c], 6);
  }
  put_uc(s, RSPDSP_SC_U + 0, RSPDSP_SC_C + 0, dsp->masterVolumeL, 7);
  put_uc(s, RSPDSP_SC_U + 2, RSPDSP_SC_C + 2, dsp->masterVolumeR, 7);
  put_uc(s, RSPDSP_SC_U + 4, RSPDSP_SC_C + 4, dsp->echoVolumeL, 7);
  put_uc(s, RSPDSP_SC_U + 6, RSPDSP_SC_C + 6, dsp->echoVolumeR, 7);
  put_uc(s, RSPDSP_SC_U + 8, RSPDSP_SC_C + 8, dsp->feedbackVolume, 7);
  put_uc(s, RSPDSP_SC_U + 10, RSPDSP_SC_C + 10, dsp->feedbackVolume, 7);
}

void snesdsp_rsp_pack(const Dsp *dsp, RspDspState *s) {
  memset(s->bytes, 0, sizeof s->bytes);   /* (vm_snap is the RSP's: never in the cache) */
  for (int c = 0; c < 8; c++) {
    const DspChannel *ch = &dsp->channel[c];
    const unsigned v = 2u * c;
    put(s, RSPDSP_V_PITCH + v, ch->pitch);
    put(s, RSPDSP_V_COUNTER + v, ch->pitchCounter);
    put(s, RSPDSP_V_GAIN + v, ch->gain);
    put(s, RSPDSP_V_SAMPLEOUT + v, ch->sampleOut);
    put(s, RSPDSP_V_VOLL + v, ch->volumeL);
    put(s, RSPDSP_V_VOLR + v, ch->volumeR);
    put(s, RSPDSP_V_RATECTR + v, ch->rateCounter);
    put(s, RSPDSP_V_GAINVAL + v, ch->gainValue);
    put(s, RSPDSP_V_SUSTAIN + v, ch->sustainLevel);
    put(s, RSPDSP_V_DECOFF + v, ch->decodeOffset);
    put(s, RSPDSP_V_OLD + v, ch->old);
    put(s, RSPDSP_V_OLDER + v, ch->older);
    put(s, RSPDSP_V_RATE0 + v, ch->adsrRates[0]);
    put(s, RSPDSP_V_RATE1 + v, ch->adsrRates[1]);
    put(s, RSPDSP_V_RATE2 + v, ch->adsrRates[2]);
    put(s, RSPDSP_V_RATE3 + v, ch->adsrRates[3]);
    put(s, RSPDSP_V_SRCN + v, ch->srcn);
    put(s, RSPDSP_V_PREVFLAGS + v, ch->previousFlags);
    put(s, RSPDSP_V_ADSRSTATE + v, ch->adsrState);
    put(s, RSPDSP_V_GAINMODE + v, ch->gainMode);
    for (int i = 0; i < 19; i++)
      put(s, RSPDSP_DECODE + c * RSPDSP_DECODE_STRIDE + 2 * i, ch->decodeBuffer[i] * 2);
  }
  for (int i = 0; i < 8; i++) {
    put(s, RSPDSP_FIR_L + 2 * i, dsp->firBufferL[i]);
    put(s, RSPDSP_FIR_L + 16 + 2 * i, dsp->firBufferL[i]);
    put(s, RSPDSP_FIR_R + 2 * i, dsp->firBufferR[i]);
    put(s, RSPDSP_FIR_R + 16 + 2 * i, dsp->firBufferR[i]);
    put(s, RSPDSP_FIR_VALS + 2 * i, dsp->firValues[i]);
  }
  memcpy(s->bytes + RSPDSP_RAM, dsp->ram, 0x80);
  put(s, RSPDSP_DIRPAGE, dsp->dirPage);
  put(s, RSPDSP_EVENCYCLE, dsp->evenCycle);
  put(s, RSPDSP_MUTE, dsp->mute);
  put(s, RSPDSP_RESET, dsp->reset);
  put(s, RSPDSP_MVOLL, dsp->masterVolumeL);
  put(s, RSPDSP_MVOLR, dsp->masterVolumeR);
  put(s, RSPDSP_NOISE_SAMPLE, dsp->noiseSample);
  put(s, RSPDSP_NOISE_RATE, dsp->noiseRate);
  put(s, RSPDSP_NOISE_COUNTER, dsp->noiseCounter);
  put(s, RSPDSP_ECHOWRITES, dsp->echoWrites);
  put(s, RSPDSP_EVOLL, dsp->echoVolumeL);
  put(s, RSPDSP_EVOLR, dsp->echoVolumeR);
  put(s, RSPDSP_FEEDBACK, dsp->feedbackVolume);
  put(s, RSPDSP_ECHOADR, dsp->echoBufferAdr);
  put(s, RSPDSP_ECHODELAY, dsp->echoDelay);
  put(s, RSPDSP_ECHOREMAIN, dsp->echoRemain);
  put(s, RSPDSP_ECHOINDEX, dsp->echoBufferIndex);
  put(s, RSPDSP_FIRINDEX, dsp->firBufferIndex);
  put(s, RSPDSP_M_KEYON, MASK(keyOn));
  put(s, RSPDSP_M_KEYOFF, MASK(keyOff));
  put(s, RSPDSP_M_NOISE, MASK(useNoise));
  put(s, RSPDSP_M_PMOD, MASK(pitchModulation));
  put(s, RSPDSP_M_ECHO, MASK(echoEnable));
  put(s, RSPDSP_M_USEGAIN, MASK(useGain));
  put(s, RSPDSP_M_DIRECTGAIN, MASK(directGain));
  pack_coefficients(dsp, s);
}

void snesdsp_rsp_unpack(Dsp *dsp, const RspDspState *s) {
  const unsigned kon = getu(s, RSPDSP_M_KEYON), kof = getu(s, RSPDSP_M_KEYOFF);
  const unsigned non = getu(s, RSPDSP_M_NOISE), pmon = getu(s, RSPDSP_M_PMOD);
  const unsigned eon = getu(s, RSPDSP_M_ECHO), ug = getu(s, RSPDSP_M_USEGAIN);
  const unsigned dg = getu(s, RSPDSP_M_DIRECTGAIN);
  for (int c = 0; c < 8; c++) {
    DspChannel *ch = &dsp->channel[c];
    const unsigned v = 2u * c;
    ch->pitch = getu(s, RSPDSP_V_PITCH + v);
    ch->pitchCounter = getu(s, RSPDSP_V_COUNTER + v);
    ch->gain = getu(s, RSPDSP_V_GAIN + v);
    ch->sampleOut = get(s, RSPDSP_V_SAMPLEOUT + v);
    ch->volumeL = (int8_t)get(s, RSPDSP_V_VOLL + v);
    ch->volumeR = (int8_t)get(s, RSPDSP_V_VOLR + v);
    ch->rateCounter = getu(s, RSPDSP_V_RATECTR + v);
    ch->gainValue = getu(s, RSPDSP_V_GAINVAL + v);
    ch->sustainLevel = getu(s, RSPDSP_V_SUSTAIN + v);
    ch->decodeOffset = getu(s, RSPDSP_V_DECOFF + v);
    ch->old = get(s, RSPDSP_V_OLD + v);
    ch->older = get(s, RSPDSP_V_OLDER + v);
    ch->adsrRates[0] = getu(s, RSPDSP_V_RATE0 + v);
    ch->adsrRates[1] = getu(s, RSPDSP_V_RATE1 + v);
    ch->adsrRates[2] = getu(s, RSPDSP_V_RATE2 + v);
    ch->adsrRates[3] = getu(s, RSPDSP_V_RATE3 + v);
    ch->srcn = (uint8_t)getu(s, RSPDSP_V_SRCN + v);
    ch->previousFlags = (uint8_t)getu(s, RSPDSP_V_PREVFLAGS + v);
    ch->adsrState = (uint8_t)getu(s, RSPDSP_V_ADSRSTATE + v);
    ch->gainMode = (uint8_t)getu(s, RSPDSP_V_GAINMODE + v);
    for (int i = 0; i < 19; i++)
      ch->decodeBuffer[i] = get(s, RSPDSP_DECODE + c * RSPDSP_DECODE_STRIDE + 2 * i) / 2;
    ch->keyOn = (kon >> c) & 1;
    ch->keyOff = (kof >> c) & 1;
    ch->useNoise = (non >> c) & 1;
    ch->pitchModulation = (pmon >> c) & 1;
    ch->echoEnable = (eon >> c) & 1;
    ch->useGain = (ug >> c) & 1;
    ch->directGain = (dg >> c) & 1;
  }
  for (int i = 0; i < 8; i++) {
    dsp->firBufferL[i] = get(s, RSPDSP_FIR_L + 2 * i);
    dsp->firBufferR[i] = get(s, RSPDSP_FIR_R + 2 * i);
    dsp->firValues[i] = (int8_t)get(s, RSPDSP_FIR_VALS + 2 * i);
  }
  memcpy(dsp->ram, s->bytes + RSPDSP_RAM, 0x80);
  dsp->dirPage = getu(s, RSPDSP_DIRPAGE);
  dsp->evenCycle = getu(s, RSPDSP_EVENCYCLE) != 0;
  dsp->mute = getu(s, RSPDSP_MUTE) != 0;
  dsp->reset = getu(s, RSPDSP_RESET) != 0;
  dsp->masterVolumeL = (int8_t)get(s, RSPDSP_MVOLL);
  dsp->masterVolumeR = (int8_t)get(s, RSPDSP_MVOLR);
  dsp->noiseSample = get(s, RSPDSP_NOISE_SAMPLE);
  dsp->noiseRate = getu(s, RSPDSP_NOISE_RATE);
  dsp->noiseCounter = getu(s, RSPDSP_NOISE_COUNTER);
  dsp->echoWrites = getu(s, RSPDSP_ECHOWRITES) != 0;
  dsp->echoVolumeL = (int8_t)get(s, RSPDSP_EVOLL);
  dsp->echoVolumeR = (int8_t)get(s, RSPDSP_EVOLR);
  dsp->feedbackVolume = (int8_t)get(s, RSPDSP_FEEDBACK);
  dsp->echoBufferAdr = getu(s, RSPDSP_ECHOADR);
  dsp->echoDelay = getu(s, RSPDSP_ECHODELAY);
  dsp->echoRemain = getu(s, RSPDSP_ECHOREMAIN);
  dsp->echoBufferIndex = getu(s, RSPDSP_ECHOINDEX);
  dsp->firBufferIndex = (uint8_t)getu(s, RSPDSP_FIRINDEX);
}

/* The echo window must avoid $0000-$000F (mirrored after $FFFF for BRR
 * reads) and must not wrap past $FFFF. */
static bool echo_ok(const Dsp *d) {
  uint32_t len = d->echoDelay;
  const uint32_t current = (uint32_t)d->echoBufferIndex + d->echoRemain;
  if (current > len) len = current;
  if (!d->echoWrites) return true;
  return d->echoBufferAdr >= 16 && (uint32_t)d->echoBufferAdr + 4u * len <= 0x10000u;
}

static bool pmod_used(const Dsp *d) {
  for (int c = 1; c < 8; c++) if (d->channel[c].pitchModulation) return true;
  return false;
}
/* Pitch modulation at the batch's start or after a logged write: the batch
 * runs on the voice-major variant (RSPDSP_VM), else on the CPU. */
static bool s_batch_pmod;
static inline bool pmod_ok(const Dsp *d) {
  if (!pmod_used(d)) return true;
#if RSPDSP_VM
  s_batch_pmod = true;
  return true;
#else
  return false;
#endif
}
#if RSPDSP_VM
#ifdef RSPDSP_VM_ALL   /* test only: every batch voice-major */
#define VM_CHOOSE() (s_run_overlay = s_overlay_vm_id, g_snesdsp_rsp_vm_batches++)
#else
#define VM_CHOOSE() (s_batch_pmod ? (s_run_overlay = s_overlay_vm_id, g_snesdsp_rsp_vm_batches++) : 0)
#endif
#else
#define VM_CHOOSE() ((void)0)
#endif

/* Emit stores for every state field register `reg` assigns, taken from
 * the shadow after dsp_write ran on it. */
static Op *emit_reg(Op *o, uint16_t tag, uint8_t reg) {
  const Dsp *d = &s_shadow;
  const unsigned c = reg >> 4, lo = reg & 15, v = 2 * c;
#define HALF(off, value) do { o->tag = tag; o->kind = RSPDSP_LOG_HALF; o->pad = 0; \
    o->adr = (off); o->val = (uint16_t)(int16_t)(value); o++; } while (0)
  const DspChannel *ch = &d->channel[c];
  unsigned m;
  switch (lo) {
    case 0: HALF(RSPDSP_V_VOLL + v, ch->volumeL); break;
    case 1: HALF(RSPDSP_V_VOLR + v, ch->volumeR); break;
    case 2: case 3: HALF(RSPDSP_V_PITCH + v, ch->pitch); break;
    case 4: HALF(RSPDSP_V_SRCN + v, ch->srcn); break;
    case 5:
      HALF(RSPDSP_V_RATE0 + v, ch->adsrRates[0]);
      HALF(RSPDSP_V_RATE1 + v, ch->adsrRates[1]);
      m = 0; for (int i = 0; i < 8; i++) if (d->channel[i].useGain) m |= 1u << i;
      HALF(RSPDSP_M_USEGAIN, m);
      break;
    case 6:
      HALF(RSPDSP_V_RATE2 + v, ch->adsrRates[2]);
      HALF(RSPDSP_V_SUSTAIN + v, ch->sustainLevel);
      break;
    case 7:
      m = 0; for (int i = 0; i < 8; i++) if (d->channel[i].directGain) m |= 1u << i;
      HALF(RSPDSP_M_DIRECTGAIN, m);
      HALF(RSPDSP_V_GAINMODE + v, ch->gainMode);
      HALF(RSPDSP_V_RATE3 + v, ch->adsrRates[3]);
      HALF(RSPDSP_V_GAINVAL + v, ch->gainValue);
      break;
    case 0xc:
      switch (c) {
        case 0: HALF(RSPDSP_MVOLL, d->masterVolumeL); break;
        case 1: HALF(RSPDSP_MVOLR, d->masterVolumeR); break;
        case 2: HALF(RSPDSP_EVOLL, d->echoVolumeL); break;
        case 3: HALF(RSPDSP_EVOLR, d->echoVolumeR); break;
        case 4: m = 0; for (int i = 0; i < 8; i++) if (d->channel[i].keyOn) m |= 1u << i;
                HALF(RSPDSP_M_KEYON, m); break;
        case 5: m = 0; for (int i = 0; i < 8; i++) if (d->channel[i].keyOff) m |= 1u << i;
                HALF(RSPDSP_M_KEYOFF, m); break;
        case 6:
          HALF(RSPDSP_RESET, d->reset);
          HALF(RSPDSP_MUTE, d->mute);
          HALF(RSPDSP_ECHOWRITES, d->echoWrites);
          HALF(RSPDSP_NOISE_RATE, d->noiseRate);
          break;
        default: break;  /* $7C: register file only */
      }
      break;
    case 0xd:
      switch (c) {
        case 0: HALF(RSPDSP_FEEDBACK, d->feedbackVolume); break;
        case 2: m = 0; for (int i = 0; i < 8; i++) if (d->channel[i].pitchModulation) m |= 1u << i;
                HALF(RSPDSP_M_PMOD, m); break;
        case 3: m = 0; for (int i = 0; i < 8; i++) if (d->channel[i].useNoise) m |= 1u << i;
                HALF(RSPDSP_M_NOISE, m); break;
        case 4: m = 0; for (int i = 0; i < 8; i++) if (d->channel[i].echoEnable) m |= 1u << i;
                HALF(RSPDSP_M_ECHO, m); break;
        case 5: HALF(RSPDSP_DIRPAGE, d->dirPage); break;
        case 6: HALF(RSPDSP_ECHOADR, d->echoBufferAdr); break;
        case 7: HALF(RSPDSP_ECHODELAY, d->echoDelay); break;
        default: break;
      }
      break;
    case 0xf: HALF(RSPDSP_FIR_VALS + v, d->firValues[c]); break;
    default: break;
  }
  /* Split coefficients that follow the fields above (pack_coefficients
   * computes the same values for the whole block). */
#define UC(off_u, off_c, k, shift) do { \
    const int32_t K_ = (int32_t)(k) * (1 << (16 - (shift))); \
    HALF(off_u, (int)(uint16_t)(K_ & 0xffff)); HALF(off_c, (int)(K_ >> 16)); } while (0)
  if (lo == 0) UC(RSPDSP_V_VOLL_U + v, RSPDSP_V_VOLL_C + v, ch->volumeL, 6);
  if (lo == 1) UC(RSPDSP_V_VOLR_U + v, RSPDSP_V_VOLR_C + v, ch->volumeR, 6);
  if (lo == 0xf) UC(RSPDSP_FIR_U + v, RSPDSP_FIR_C + v, d->firValues[c], 6);
  if (lo == 0xc && c < 4) {
    const int k = c == 0 ? d->masterVolumeL : c == 1 ? d->masterVolumeR
                : c == 2 ? d->echoVolumeL : d->echoVolumeR;
    UC(RSPDSP_SC_U + 2 * c, RSPDSP_SC_C + 2 * c, k, 7);
  }
  if (reg == 0x0d) {
    UC(RSPDSP_SC_U + 8, RSPDSP_SC_C + 8, d->feedbackVolume, 7);
    UC(RSPDSP_SC_U + 10, RSPDSP_SC_C + 10, d->feedbackVolume, 7);
  }
  if (reg == 0x4d)
    for (int i = 0; i < 8; i++) HALF(RSPDSP_V_ECHOMASK + 2 * i, d->channel[i].echoEnable ? -1 : 0);
#undef UC
#undef HALF
  o->tag = tag; o->kind = RSPDSP_LOG_BYTE; o->pad = 0;
  o->adr = RSPDSP_RAM + reg; o->val = d->ram[reg]; o++;
  return o;
}

/* Decode one logged write into RSP operations. */
static bool decode(Op **po, Op *end, uint16_t tag, bool isReg, uint16_t adr, uint8_t val,
                   bool mirror) {
  Op *o = *po;
  if (o + 24 > end) return false;
  if (isReg) {
    dsp_write(&s_shadow, (uint8_t)adr, val);
    if (!pmod_ok(&s_shadow) || (mirror && !echo_ok(&s_shadow))) return false;
    o = emit_reg(o, tag, (uint8_t)adr);
  } else {
    o->tag = tag; o->kind = RSPDSP_LOG_ARAM; o->pad = 0;
    o->adr = adr; o->val = val; o++;
    if (mirror && adr < 16) {  /* and its mirror after $FFFF: address bit 16 in pad */
      o[0] = o[-1];
      o->pad = 1;
      o++;
    }
  }
  *po = o;
  return true;
}

static bool batch_ok(const Dsp *dsp, uint32_t samples, bool mirror) {
  if (samples > kMaxSamples || dsp->shadow || dsp->output_filter || dsp->suppressOutput)
    return false;
  s_batch_pmod = false;
  return pmod_ok(dsp) && (!mirror || echo_ok(dsp));
}

/* ENVX ($x8) and OUTX ($x9) are left to the CPU: only the last sample's
 * values can be observed (the runtime joins before the SPC reads them),
 * unless a write logged after the last sample replaced them. */
static void set_envx_outx(Dsp *dsp, const Op *ops, uint32_t nops, uint32_t samples) {
  if (!samples) return;
  uint32_t keep = 0;
  for (uint32_t i = nops; i-- > 0 && ops[i].tag == samples;)
    if (ops[i].kind == RSPDSP_LOG_BYTE) {
      const unsigned r = ops[i].adr - RSPDSP_RAM;
      if ((r & 0xe) == 8) keep |= 1u << ((r >> 4) * 2 + (r & 1));
    }
  for (int c = 0; c < 8; c++) {
    if (!(keep & (1u << (2 * c)))) dsp->ram[c * 16 + 8] = (uint8_t)(dsp->channel[c].gain >> 4);
    if (!(keep & (2u << (2 * c))))
      dsp->ram[c * 16 + 9] = (uint8_t)(dsp->channel[c].sampleOut >> 7);
  }
}

/* Run the decoded batch (s_ops, nops) on `aram` (aram_len bytes covered by
 * cache maintenance) and publish its output. */
#if RSPDSP_BLOCKS
/* ---- Block and key-on records (RSPDSP_BLOCKS) -----------------------------
 * The microcode takes each decoded block and each key on's start address
 * from records instead of reading ARAM (docs/n64/RED_DESIGN.md). Here the
 * records of a batch are produced by running the S-DSP front end over it
 * (dsp_frontend.c) with the logged ARAM writes applied at their samples.
 * A batch in which a block or directory read touches an echo window while
 * echo writes may be on falls back to C: those bytes are the S-DSP's own
 * echo output, which the front end does not see. */
#include "dsp_frontend.h"
enum { kMaxBlk = 4096, kMaxKon = 1024 };
typedef struct { uint16_t start; uint8_t b[9]; uint8_t pad[5]; } BlkRec;
_Static_assert(sizeof(BlkRec) == RSPDSP_BLK_REC, "block record");
static BlkRec s_blk[kMaxBlk + 16] __attribute__((aligned(16)));
static uint16_t s_kon[kMaxKon + 32] __attribute__((aligned(16)));
static uint32_t s_nblk, s_nkon;
static bool s_conflict, s_overflow, s_echo_on;
static struct { uint16_t start; uint32_t len; } s_win[8];
static int s_nwin;
uint32_t g_snesdsp_rsp_blocks, g_snesdsp_rsp_echo_conflicts, g_snesdsp_rsp_last_nblk, g_snesdsp_rsp_last_nkon;
static uint8_t s_scratch[0x10000];

static void st_block(void *ctx, int voice, uint32_t sample, uint16_t start, const uint8_t b[9]) {
  (void)ctx; (void)voice; (void)sample;
  if (s_nblk == kMaxBlk) { s_overflow = true; return; }
  BlkRec *r = &s_blk[s_nblk++];
  r->start = start;
  memcpy(r->b, b, 9);
#ifdef RSPDSP_BLOCKS_BREAK   /* test only: the check must fail with this */
  if (s_nblk == 1) r->b[4] ^= 0x11;
#endif
}
static void st_keyon(void *ctx, int voice, uint32_t sample, uint16_t start) {
  (void)ctx; (void)voice; (void)sample;
  if (s_nkon == kMaxKon) { s_overflow = true; return; }
  s_kon[s_nkon++] = start;
}
static void st_read(void *ctx, int voice, uint32_t sample, uint16_t adr, unsigned len) {
  (void)voice; (void)sample;
  (void)ctx;
  if (!s_echo_on) return;
  for (int w = 0; w < s_nwin; w++) {
    const uint32_t off = (uint16_t)(adr - s_win[w].start);   /* distance into the window, mod 64K */
    if (off < s_win[w].len || (uint16_t)(s_win[w].start - adr) < len) { s_conflict = true; return; }
  }
}
static void st_window(uint16_t esa, uint32_t len) {
  if (s_nwin == 8) { s_conflict = true; return; }
  s_win[s_nwin].start = esa;
  s_win[s_nwin].len = len;
  s_nwin++;
}

/* Records for a batch from the state at its start; false: run it in C. */
static bool streams_for_batch(const Dsp *dsp, const uint8_t *aram, const SnesDspLogIn *log,
                              uint32_t n, uint32_t samples) {
  memcpy(s_scratch, aram, 0x10000);
  s_nblk = s_nkon = 0;
  s_conflict = s_overflow = false;
  s_echo_on = dsp->echoWrites;
  s_nwin = 0;
  uint32_t len = dsp->echoDelay;
  if ((uint32_t)dsp->echoBufferIndex + dsp->echoRemain > len) len = dsp->echoBufferIndex + dsp->echoRemain;
  st_window(dsp->echoBufferAdr, 4u * len);
  DspFrontEnd fe;
  dsp_fe_init(&fe, dsp, s_scratch, 0, st_block, NULL);
  fe.on_keyon = st_keyon;
  fe.on_read = st_read;
  uint16_t esa = dsp->echoBufferAdr;
  for (uint32_t i = 0; i < n; i++) {
    dsp_fe_advance(&fe, log[i].tag);
    if (log[i].isReg) {
      const uint8_t r = (uint8_t)log[i].adr, v = log[i].val;
      if (r == 0x6c && !(v & 0x20)) s_echo_on = true;
      if (r == 0x6d) esa = (uint16_t)(v << 8);
      if (r == 0x6d || r == 0x7d) st_window(esa, 4u * 15u * 512u);   /* any length it may reach */
      dsp_fe_write(&fe, r, v);
    } else {
      s_scratch[log[i].adr] = log[i].val;
    }
  }
  dsp_fe_advance(&fe, samples);
  if (s_conflict) g_snesdsp_rsp_echo_conflicts++;
  if (fe.untracked || s_conflict || s_overflow) return false;
  g_snesdsp_rsp_blocks += s_nblk;
  g_snesdsp_rsp_last_nblk = s_nblk;
  g_snesdsp_rsp_last_nkon = s_nkon;
  return true;
}
#endif

static void run(Dsp *dsp, uint8_t *aram, uint32_t aram_len, Op *o, uint32_t samples) {
  const uint32_t nops = (uint32_t)(o - s_ops.op);
  memset(o, 0, 32 * sizeof *o);  /* the RSP fetches whole chunks */
  snesdsp_rsp_pack(dsp, &s_block);
  put(&s_block, RSPDSP_P_SAMPLES, (int)samples);
  put(&s_block, RSPDSP_P_LOGN, (int)nops);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_ARAM) = PhysicalAddr(aram);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_LOG) = PhysicalAddr(s_ops.op);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_OUT) = PhysicalAddr(s_out.s);
#if RSPDSP_BLOCKS
  *(uint32_t *)(s_block.bytes + RSPDSP_P_BLK) = PhysicalAddr(s_blk);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_KON) = PhysicalAddr(s_kon);
  data_cache_hit_writeback(s_blk, (s_nblk + 16) * sizeof s_blk[0]);
  data_cache_hit_writeback(s_kon, (s_nkon + 32) * sizeof s_kon[0]);
#endif
  data_cache_hit_writeback_invalidate(&s_block, RSPDSP_STATE_SIZE);
  data_cache_hit_writeback_invalidate(s_ops.op, (nops + 32) * sizeof(Op));
  data_cache_hit_writeback_invalidate(aram, aram_len);
  data_cache_hit_writeback_invalidate(&s_out, sizeof s_out);
  const uint32_t t0 = get_ticks();
  rspq_write(s_run_overlay, 0, PhysicalAddr(&s_block), 0);
  rspq_wait();
  g_snesdsp_rsp_ticks += get_ticks() - t0;
  g_snesdsp_rsp_samples += samples;
  snesdsp_rsp_unpack(dsp, &s_block);
  set_envx_outx(dsp, s_ops.op, nops, samples);

  /* Output ring, as dsp_cycle publishes each sample. */
  for (uint32_t i = 0; i < samples; i++) {
    const uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
    if (fill >= DSP_SAMPLE_RING) continue;
    const uint32_t w = dsp->sampleWrite & (DSP_SAMPLE_RING - 1);
    dsp->sampleBuffer[w * 2] = s_out.s[i * 2];
    dsp->sampleBuffer[w * 2 + 1] = s_out.s[i * 2 + 1];
    dsp->sampleWrite++;
  }
}

bool snesdsp_rsp_batch(Dsp *dsp, uint8_t *aram, const SnesDspLogIn *log, uint32_t n,
                       uint32_t samples) {
  if (!batch_ok(dsp, samples, true)) return false;
  shadow_from(dsp);  /* decodes register writes; sees only those */
  Op *o = s_ops.op, *end = s_ops.op + kMaxOps;
  for (uint32_t i = 0; i < n; i++)
    if (!decode(&o, end, log[i].tag, log[i].isReg, log[i].adr, log[i].val, true)) return false;
  CHOOSE_OVERLAY(dsp, log, n, IS_REG_IN);
  VM_CHOOSE();
#if RSPDSP_BLOCKS
  if (!streams_for_batch(dsp, aram, log, n, samples)) return false;
#endif
  run(dsp, aram, RSPDSP_ARAM_SIZE, o, samples);
  return true;
}

/* The runtime's deferred-DSP hook (SNESRECOMP_DSP_BATCH_HOOK): `aram` is the
 * emulator's own ARAM, already at its batch-start state, with no mirror
 * after $FFFF, so batches that may read page $FF stay in C. */
bool snesdsp_rsp_apu_batch(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log, uint32_t n,
                           uint32_t samples, bool aramEndReachable) {
  if (aramEndReachable || ((uintptr_t)aram & 7) || !batch_ok(dsp, samples, false))
    return false;
#if RSPDSP_BLOCKS
  return false;   /* records come from the inline front end (not yet wired here) */
#endif
  shadow_from(dsp);
  Op *o = s_ops.op, *end = s_ops.op + kMaxOps;
  for (uint32_t i = 0; i < n; i++)
    if (!decode(&o, end, log[i].tag, log[i].isReg == 1, log[i].adr, log[i].val, false))
      return false;
  CHOOSE_OVERLAY(dsp, log, n, IS_REG_APU);
  VM_CHOOSE();
  run(dsp, aram, 0x10000, o, samples);
  return true;
}

/* ---- Asynchronous batches (SNESRECOMP_DSP_LAUNCH_HOOK) ----------------
 * The RSP works on a private view of ARAM: the pages the batch can read,
 * copied at launch and returned to their batch-start values by undoing the
 * logged writes, so the SPC keeps running on the live ARAM meanwhile. At the
 * join the DSP state and output are taken back and the echo bytes the batch
 * wrote are copied into the live ARAM (the runtime joins before the SPC can
 * touch the echo window). */
#if SNESRECOMP_ECHO_IN_PLACE
/* Echo in place (docs/n64/RED_DESIGN.md): the RSP works on the live ARAM.
 * Its blocks and key-on starts come as records, its register writes and the
 * SPC writes diverted to it (isReg 3) in the log; the echo ring is its own
 * in ARAM, and the runtime keeps the SPC off the lines it has still to
 * touch. No view, no undo, no copy-back. */
static void view_init(void) {}
void apu_echo_cache_drop(const void *p, uint32_t len) { data_cache_hit_writeback_invalidate((void *)p, len); }
uint8_t apu_echo_read_uncached(const uint8_t *p) { return *(const volatile uint8_t *)UncachedAddr(p); }
#if RSPDSP_PREP || !RSPDSP_BLOCKS
#error "SNESRECOMP_ECHO_IN_PLACE needs RSPDSP_BLOCKS and not RSPDSP_PREP"
#endif
#else
static uint8_t s_view[RSPDSP_ARAM_SIZE] __attribute__((aligned(16)));  /* + the mirror of $0000-$000F */
/* The view is only ever accessed uncached; drop any lines the startup BSS
 * clear left dirty, so a later eviction cannot overwrite page copies. */
static void view_init(void) { data_cache_hit_writeback_invalidate(s_view, sizeof s_view); }
#endif
static struct {
  bool active, echoWrites;
  Dsp *dsp;
  uint8_t *aram;
  uint32_t samples, nops;
  uint16_t esa, index, remain, delay;
  uint32_t seq;   /* this batch's number: Red posts RED_MSG_DSP_DONE with it */
} s_flight;
static uint32_t s_seq;
uint32_t g_snesdsp_rsp_async, g_snesdsp_rsp_joins, g_snesdsp_rsp_join_wait;
uint32_t g_snesdsp_rsp_launch_ticks, g_snesdsp_rsp_join_ticks, g_snesdsp_rsp_pages;
uint32_t g_snesdsp_rsp_join_wait_blue;   /* the part of join_wait spent on the Blue thread */
uint32_t g_snesdsp_rsp_decode_ticks, g_snesdsp_rsp_log_entries;   /* launch: log decode, entries */
uint32_t g_snesdsp_rsp_view_ticks, g_snesdsp_rsp_pack_ticks, g_snesdsp_rsp_submit_ticks;
uint32_t g_snesdsp_rsp_reject[5];   /* [4]: no valid front-end records (RSPDSP_BLOCKS) */
uint32_t g_snesdsp_rsp_blue_yields, g_snesdsp_rsp_fallbacks;

#if RSPDSP_PREP
/* The operations emit_reg writes for register `reg`, its byte included. */
static uint32_t reg_ops(uint8_t reg) {
  const unsigned c = reg >> 4;
  switch (reg & 15) {
    case 0: case 1: case 5: case 0xf: return 4;
    case 2: case 3: case 4: return 2;
    case 6: return 3;
    case 7: return 5;
    case 0xc: return c < 4 ? 4 : c == 6 ? 5 : c == 7 ? 1 : 2;
    case 0xd: return c == 0 ? 6 : c == 1 ? 1 : c == 4 ? 10 : 2;
    default: return 1;
  }
}

/* What decode() would decide for this log, without decoding: the number of
 * operations, or -1 where it rejects the batch (no room for the next entry,
 * pitch modulation, an echo window outside ARAM: the same tests at the same
 * entries, on the few registers they read). */
static int prep_count(const Dsp *d, const ApuDspLogEntry *log, uint32_t n) {
  unsigned pmod = 0;
  for (int c = 0; c < 8; c++) pmod |= (unsigned)d->channel[c].pitchModulation << c;
  bool ew = d->echoWrites;
  uint32_t esa = d->echoBufferAdr, delay = d->echoDelay;
  const uint32_t cur = (uint32_t)d->echoBufferIndex + d->echoRemain;
  uint32_t o = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (o + 24 > kMaxOps) return -1;
    if (log[i].isReg != 1) { o += log[i].adr < 16 ? 2u : 1u; continue; }
    const uint8_t reg = (uint8_t)log[i].adr, v = log[i].val;
    if (reg == 0x2d) pmod = v;
    else if (reg == 0x6c) ew = (v & 0x20) == 0;
    else if (reg == 0x6d) esa = (uint32_t)v << 8;
    else if (reg == 0x7d) { delay = (v & 0xfu) * 512u; if (!delay) delay = 1; }
    if (pmod & 0xfe) return -1;
    const uint32_t len = cur > delay ? cur : delay;
    if (ew && !(esa >= 16 && esa + 4u * len <= 0x10000u)) return -1;
    o += reg_ops(reg);
  }
  return (int)o;
}

/* The decode's input for the RSP: the log and the state at the batch's
 * start that is not a function of the written byte. */
static void prep_params(const Dsp *d, const ApuDspLogEntry *log, uint32_t n) {
  data_cache_hit_writeback(log, n * sizeof *log);
  uint8_t *b = s_prm.b;
  *(uint32_t *)(b + RSPPREP_LOG) = PhysicalAddr(log);
  *(uint32_t *)(b + RSPPREP_N) = n;
  *(uint32_t *)(b + RSPPREP_OPS) = PhysicalAddr(s_ops.op);
  unsigned ug = 0, dg = 0;
  for (int c = 0; c < 8; c++) {
    const DspChannel *ch = &d->channel[c];
    ug |= (unsigned)ch->useGain << c;
    dg |= (unsigned)ch->directGain << c;
    *(uint16_t *)(b + RSPPREP_PITCH + 2 * c) = ch->pitch;
    *(uint16_t *)(b + RSPPREP_GAINVAL + 2 * c) = ch->gainValue;
    *(uint16_t *)(b + RSPPREP_RATE3 + 2 * c) = ch->adsrRates[3];
    *(uint16_t *)(b + RSPPREP_GAINMODE + 2 * c) = ch->gainMode;
  }
  *(uint16_t *)(b + RSPPREP_USEGAIN) = (uint16_t)ug;
  *(uint16_t *)(b + RSPPREP_DIRECTGAIN) = (uint16_t)dg;
  data_cache_hit_writeback_invalidate(&s_prm, sizeof s_prm);
}
#endif

#if RSPDSP_PREP && defined(RSPDSP_PREP_SELFTEST)
/* preptest: decode `log` on the CPU (the reference) and on the RSP. Returns
 * 0 when both reject the batch, 1 when the operations are identical, -1
 * when only one rejects, -2 when the counts differ, else -(3 + index of the
 * first differing operation). */
static Op s_ref[kMaxOps + 32];
int snesdsp_rsp_prep_selftest(const Dsp *dsp, const ApuDspLogEntry *log, uint32_t n,
                              uint32_t *nops_out) {
  shadow_from(dsp);
  Op *o = s_ref, *end = s_ref + kMaxOps;
  bool ok_c = true;
  for (uint32_t i = 0; i < n && ok_c; i++)
    ok_c = decode(&o, end, log[i].tag, log[i].isReg == 1, log[i].adr, log[i].val, true);
  const int cnt = prep_count(dsp, log, n);
  if (ok_c != (cnt >= 0)) return -1;
  if (!ok_c) return 0;
  const uint32_t nc = (uint32_t)(o - s_ref);
  *nops_out = nc;
  if ((uint32_t)cnt != nc) return -2;
  memset(o, 0, 32 * sizeof *o);
  memset(s_ops.op, 0xa5, (nc + 32) * sizeof(Op));   /* stale bytes would show */
  data_cache_hit_writeback_invalidate(s_ops.op, (nc + 32) * sizeof(Op));
  prep_params(dsp, log, n);
  rspq_write(s_prep_id, 0, PhysicalAddr(&s_prm), 0);
  rspq_wait();
  for (uint32_t i = 0; i < nc + 32; i++)
    if (memcmp(&s_ops.op[i], &s_ref[i], sizeof(Op))) return -(3 + (int)i);
  return 1;
}
const void *snesdsp_rsp_prep_selftest_ops(int rsp) { return rsp ? (const void *)s_ops.op : (const void *)s_ref; }
#endif

static bool launch(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log, uint32_t n,
                   uint32_t samples, const uint8_t *marks, uint8_t *fresh);
bool snesdsp_rsp_apu_launch(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log, uint32_t n,
                            uint32_t samples, const uint8_t *marks, uint8_t *fresh) {
  const uint32_t t0 = get_ticks();
  const bool ok = launch(dsp, aram, log, n, samples, marks, fresh);
  g_snesdsp_rsp_launch_ticks += get_ticks() - t0;
  return ok;
}
static bool launch(Dsp *dsp, uint8_t *aram, const ApuDspLogEntry *log, uint32_t n,
                   uint32_t samples, const uint8_t *marks, uint8_t *fresh) {
#if RSPDSP_BLOCKS
  /* The blocks and key-on starts come from the inline front end. */
  if (!g_apu_dsp_records.valid) { g_snesdsp_rsp_reject[4]++; return false; }
#endif
  /* Why batches stay in C (g_snesdsp_rsp_reject: an echo window that wraps
   * past $FFFF, pitch modulation, other state, the log). The view mirrors
   * $0000-$000F after $FFFF for BRR blocks that run past the end. */
  if (!echo_ok(dsp)) { g_snesdsp_rsp_reject[0]++; return false; }
  if (pmod_used(dsp)) { g_snesdsp_rsp_reject[1]++; return false; }
  if (((uintptr_t)aram & 7) || !batch_ok(dsp, samples, true)) { g_snesdsp_rsp_reject[2]++; return false; }
  const uint32_t td = get_ticks();
#if RSPDSP_PREP
  if ((uintptr_t)log & 7) { g_snesdsp_rsp_reject[2]++; return false; }   /* DMA alignment */
  const int cnt = prep_count(dsp, log, n);
  if (cnt < 0) { g_snesdsp_rsp_reject[3]++; return false; }
  const uint32_t nops = (uint32_t)cnt;
#else
  shadow_from(dsp);
  Op *o = s_ops.op, *end = s_ops.op + kMaxOps;
  for (uint32_t i = 0; i < n; i++) {
#if SNESRECOMP_ECHO_IN_PLACE
    /* ARAM holds the SPC's other writes already. */
    if (log[i].isReg == 3) {
      if (!decode(&o, end, log[i].tag, false, log[i].adr, log[i].val, false))
        { g_snesdsp_rsp_reject[3]++; return false; }
      continue;
    }
    if (log[i].isReg != 1) continue;
#endif
    if (!decode(&o, end, log[i].tag, log[i].isReg == 1, log[i].adr, log[i].val, true))
      { g_snesdsp_rsp_reject[3]++; return false; }
  }
#endif
  g_snesdsp_rsp_decode_ticks += get_ticks() - td;
  g_snesdsp_rsp_log_entries += n;
  /* The view keeps its pages between batches: a marked page is copied only
   * when ARAM's page changed since (fresh bit clear). Pages go to the view
   * through the uncached segment: no write-allocate misses, and nothing to
   * write back. The view is never read cached outside the echo copy-back,
   * which invalidates first. Pages the undo below touches differ from ARAM
   * afterwards, so they lose their bit; echo pages the batch writes match
   * ARAM again after the join's copy-back. */
  const uint32_t tv = get_ticks();
#if SNESRECOMP_ECHO_IN_PLACE
  (void)marks; (void)fresh;
  {
    /* The lines the batch's echo touches: the SPC's writes there to RDRAM
     * first, and none of them left in the cache (the RSP writes them). */
    const ApuDspRecords *r = &g_apu_dsp_records;
    if (r->nruns > 16) {
      data_cache_hit_writeback_invalidate(aram, 0x10000);
    } else {
      for (uint32_t k = 0; k < r->nruns; k++) {
        const uint32_t l = r->run_line[k], c = r->run_count[k];
        const uint32_t c1 = l + c > 0x1000 ? 0x1000 - l : c;
        data_cache_hit_writeback_invalidate(aram + l * 16, c1 * 16);
        if (c1 < c) data_cache_hit_writeback_invalidate(aram, (c - c1) * 16);
      }
    }
  }
#else
  uint8_t *uview = UncachedAddr(s_view);
  for (unsigned p = 0; p < 256; p++) {
    const uint8_t bit = (uint8_t)(1u << (p & 7));
    if ((marks[p >> 3] & bit) && !(fresh[p >> 3] & bit)) {
      const uint64_t *src = (const uint64_t *)(aram + p * 256);
      uint64_t *dst = (uint64_t *)(uview + p * 256);
      for (int i = 0; i < 32; i++) dst[i] = src[i];
      if (p == 0) {  /* and its mirror */
        uint64_t *m = (uint64_t *)(uview + 0x10000);
        m[0] = src[0]; m[1] = src[1];
      }
      fresh[p >> 3] |= bit;
      g_snesdsp_rsp_pages++;
    }
  }
  for (uint32_t i = n; i-- > 0;)
    if (!log[i].isReg) {
      uview[log[i].adr] = log[i].old;
      if (log[i].adr < 16) uview[0x10000 + log[i].adr] = log[i].old;
      fresh[log[i].adr >> 11] &= (uint8_t)~(1u << ((log[i].adr >> 8) & 7));
    }
#endif

  const uint32_t tp = get_ticks();
  g_snesdsp_rsp_view_ticks += tp - tv;
#if !RSPDSP_PREP
  const uint32_t nops = (uint32_t)(o - s_ops.op);
  memset(o, 0, 32 * sizeof *o);
#endif
  CHOOSE_OVERLAY(dsp, log, n, IS_REG_APU);
  snesdsp_rsp_pack(dsp, &s_block);
  put(&s_block, RSPDSP_P_SAMPLES, (int)samples);
  put(&s_block, RSPDSP_P_LOGN, (int)nops);
#if RSPDSP_PREP
  prep_params(dsp, log, n);
#endif
  const uint32_t ts = get_ticks();
  g_snesdsp_rsp_pack_ticks += ts - tp;
#if SNESRECOMP_ECHO_IN_PLACE
  *(uint32_t *)(s_block.bytes + RSPDSP_P_ARAM) = PhysicalAddr(aram);
#else
  *(uint32_t *)(s_block.bytes + RSPDSP_P_ARAM) = PhysicalAddr(s_view);
#endif
  *(uint32_t *)(s_block.bytes + RSPDSP_P_LOG) = PhysicalAddr(s_ops.op);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_OUT) = PhysicalAddr(s_out.s);
#if RSPDSP_BLOCKS
  {
    const ApuDspRecords *r = &g_apu_dsp_records;
    *(uint32_t *)(s_block.bytes + RSPDSP_P_BLK) = PhysicalAddr(r->blocks);
    *(uint32_t *)(s_block.bytes + RSPDSP_P_KON) = PhysicalAddr(r->keyons);
    data_cache_hit_writeback(r->blocks, (r->nblocks + 16) * RSPDSP_BLK_REC);
    data_cache_hit_writeback(r->keyons, (r->nkeyons + 32) * RSPDSP_KON_REC);
    g_snesdsp_rsp_blocks += r->nblocks;
  }
#endif
  s_seq = (s_seq + 1) & 0xffffffu;
  *(uint32_t *)(s_block.bytes + RSPDSP_P_DONE_MSG) = RED_MSG(RED_MSG_DSP_DONE, s_seq);
  data_cache_hit_writeback_invalidate(&s_block, RSPDSP_STATE_SIZE);
  /* (RSPDSP_PREP: the RSP writes them; no line may stay in the cache.) */
  data_cache_hit_writeback_invalidate(s_ops.op, (nops + 32) * sizeof(Op));
  /* Only the samples this batch writes; the CPU never writes s_out. */
  data_cache_hit_invalidate(&s_out, (samples * 4 + 15) & ~15u);
#if RSPDSP_RING
#if RSPDSP_PREP   /* the decode first: Red runs the ring in order */
  const uint32_t pcmd[2] = {s_prep_id | PhysicalAddr(&s_prm), 0};
  rspq_ring_write(pcmd, 2);
#endif
  const uint32_t cmd[2] = {s_run_overlay | PhysicalAddr(&s_block), 0};
  rspq_ring_write(cmd, 2);
#else
#if RSPDSP_PREP
  rspq_write(s_prep_id, 0, PhysicalAddr(&s_prm), 0);
#endif
  rspq_write(s_run_overlay, 0, PhysicalAddr(&s_block), 0);
  rspq_flush();
#endif
  g_snesdsp_rsp_submit_ticks += get_ticks() - ts;
  s_flight.seq = s_seq;
  s_flight.active = true;
  s_flight.dsp = dsp;
  s_flight.aram = aram;
  s_flight.samples = samples;
  s_flight.nops = nops;
  s_flight.echoWrites = dsp->echoWrites;
  s_flight.esa = dsp->echoBufferAdr;
  s_flight.index = dsp->echoBufferIndex;
  s_flight.remain = dsp->echoRemain;
  s_flight.delay = dsp->echoDelay;
  g_snesdsp_rsp_async++;
  return true;
}

#ifdef N64_HANG_PROBE
/* Hang probe (n64_platform.c): the batch in flight, and where the join is. */
extern volatile uint32_t g_hang_where;
void snesdsp_rsp_probe(uint32_t out[8]) {
  out[0] = s_flight.active;
  out[1] = s_flight.seq;
  out[2] = s_seq;
  out[3] = s_flight.samples;
  out[4] = s_flight.nops;
  /* What RDRAM holds for the RSP (uncached): the block's sample and op counts. */
  const RspDspState *u = UncachedAddr(&s_block);
  out[5] = *(const uint16_t *)(u->bytes + RSPDSP_P_SAMPLES);
  out[6] = *(const uint16_t *)(u->bytes + RSPDSP_P_LOGN);
  out[7] = *(const uint32_t *)(u->bytes + RSPDSP_P_DONE_MSG);
}
#define HANG_WHERE(v) (g_hang_where = (v))
#else
#define HANG_WHERE(v) ((void)0)
#endif

void snesdsp_rsp_apu_join(void) {
  if (!s_flight.active) return;
#ifdef N64_HANG_PROBE
  const uint32_t where_was = g_hang_where;
#endif
  const uint32_t tj = get_ticks();
  const uint32_t t0 = get_ticks();
  /* Red rings when the batch is done (RED_MSG_DSP_DONE). Blue hands the CPU
   * to Green meanwhile; Green's service returns once the doorbell asks for
   * Blue (RUN_FLAG_WANT_BLUE). A doorbell that never comes (50 ms) falls
   * back to waiting for the RSP and is counted. */
  if (!red_dsp_done(s_flight.seq)) {
#if SNESRECOMP_BLUE_THREAD
    if (blue_is_current()) {
      g_red_blue_waits = s_flight.seq | 0x80000000u;
      HANG_WHERE(0x11);
      while (!red_dsp_done(s_flight.seq) && (uint32_t)get_ticks() - t0 < TICKS_FROM_MS(50)) {
        blue_yield();
        g_snesdsp_rsp_blue_yields++;
      }
      g_red_blue_waits = 0;
    }
#endif
    HANG_WHERE(0x12);
    while (!red_dsp_done(s_flight.seq) && (uint32_t)get_ticks() - t0 < TICKS_FROM_MS(50)) {}
    if (!red_dsp_done(s_flight.seq)) {
      g_snesdsp_rsp_fallbacks++;
      HANG_WHERE(0x13);
#if RSPDSP_RING
      rspq_ring_wait();
#else
      rspq_wait();
#endif
    }
  }
#ifdef N64_HANG_PROBE
  g_hang_where = where_was;
#endif
  g_snesdsp_rsp_join_wait += get_ticks() - t0;   /* time the CPU waited */
  if (blue_is_current()) g_snesdsp_rsp_join_wait_blue += get_ticks() - t0;
  g_snesdsp_rsp_joins++;
  g_snesdsp_rsp_samples += s_flight.samples;
  Dsp *dsp = s_flight.dsp;
  snesdsp_rsp_unpack(dsp, &s_block);
  set_envx_outx(dsp, s_ops.op, s_flight.nops, s_flight.samples);
  for (uint32_t i = 0; i < s_flight.samples; i++) {
    const uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
    if (fill >= DSP_SAMPLE_RING) continue;
    const uint32_t w = dsp->sampleWrite & (DSP_SAMPLE_RING - 1);
    dsp->sampleBuffer[w * 2] = s_out.s[i * 2];
    dsp->sampleBuffer[w * 2 + 1] = s_out.s[i * 2 + 1];
    dsp->sampleWrite++;
  }
#if !SNESRECOMP_ECHO_IN_PLACE
  /* The echo bytes the batch wrote, sample by sample as dsp.c writes them:
   * FLG, ESA and EDL writes logged in the batch (ops tagged k apply before
   * sample k) move the window and switch the writes. */
  {
    uint32_t index = s_flight.index, remain = s_flight.remain, k = 0;
    uint16_t esa = s_flight.esa, delay = s_flight.delay;
    bool writes = s_flight.echoWrites;
    const Op *op = s_ops.op;
    for (uint32_t i = 0; i < s_flight.samples; i++) {
      for (; k < s_flight.nops && op[k].tag <= i; k++) {
        if (op[k].kind != RSPDSP_LOG_HALF) continue;
        if (op[k].adr == RSPDSP_ECHOADR) esa = op[k].val;
        else if (op[k].adr == RSPDSP_ECHODELAY) delay = op[k].val;
        else if (op[k].adr == RSPDSP_ECHOWRITES) writes = op[k].val != 0;
      }
      if (writes) {
        const uint16_t adr = (uint16_t)(esa + index * 4);
        memcpy(s_flight.aram + adr, (const uint8_t *)UncachedAddr(s_view) + adr, 4);
      }
      index = (index + 1) & 0xffff;
      remain = (remain - 1) & 0xffff;
      if (!remain) { remain = delay; index = 0; }
    }
  }
#endif
  s_flight.active = false;
  g_snesdsp_rsp_join_ticks += get_ticks() - tj;
}

#if SNESRECOMP_DSP_STREAM
/* ---- The audio split: the microcode as the S-DSP back end ---------------
 * (apu_dsp_batch.h, docs/n64/RED_DESIGN.md "The audio split".) Yellow's
 * state block (s_block) is packed once when it takes over and stays in
 * RDRAM: each batch DMAs it in, runs, and DMAs it out. Blue posts a batch to
 * one of kSlots slots (its parameters, operations and records) and returns;
 * it waits only for a free slot, for Yellow's progress when the SPC reads
 * OUTX (or ENVX/ENDX of an untracked voice) or an echo line Yellow has still
 * to write, and when it takes the state back (saves). Red posts each batch's
 * sequence number when done (RED_MSG_DSP_DONE); progress is the end sample
 * of the newest batch done. Output goes straight to s_ring, 8 bytes a
 * sample; Green reads it once published. */
enum { kSlots = 4, kSlotBlocks = 512, kSlotKeyons = 1024 };
typedef struct {
  struct __attribute__((aligned(16))) { uint8_t b[RSPDSP_PARAMS_SIZE]; } prm;
  struct __attribute__((aligned(16))) { Op op[kMaxOps + 32]; } ops;
  uint8_t blocks[(kSlotBlocks + 16) * RSPDSP_BLK_REC] __attribute__((aligned(16)));
  uint16_t keyons[kSlotKeyons + 32] __attribute__((aligned(16)));
  uint32_t seq, end;
} Slot;
static Slot s_slot[kSlots];
static uint32_t s_head, s_tail;        /* slots posted (tail) and found done (head) */
static uint32_t s_posted, s_done_end, s_published;
static bool s_running;
static int16_t s_ring[APU_STREAM_RING * 4 + 8] __attribute__((aligned(16)));
/* Untracked voices (apu_dsp_batch.h). Those untracked by pitch modulation
 * run exactly on the voice-major variant (RSPDSP_VM): a batch with any of
 * them goes there, reading every voice's directory entries and blocks from
 * ARAM through the ledger (its records unused). Those untracked by an echo
 * conflict get the stopgap (documented in RED_DESIGN.md), on either
 * variant: such a voice is silenced, harmlessly for the others: its pitch is
 * held at 0 (its counter never wraps, so it takes no block records), its
 * envelope is put in release at gain 0, and KON writes (and the handover)
 * leave its key-on latch clear. A latch already set when it is untracked
 * may still key it on: harmless, its decode buffer is cleared and it never
 * decodes, so it stays silent. Without RSPDSP_VM every untracked voice gets
 * the stopgap. */
static uint8_t s_untracked, s_silenced;
static bool s_vm_mode;              /* the last batch posted was voice-major */
static uint32_t s_ledger_seen;      /* ledger entries whose lines were written back */
uint32_t g_snesdsp_rsp_untracked;   /* voices silenced so far */
#define HALF_OP(o, t, off, value) do { (o)->tag = (t); (o)->kind = RSPDSP_LOG_HALF; (o)->pad = 0; \
    (o)->adr = (off); (o)->val = (uint16_t)(int16_t)(value); (o)++; } while (0)
static Op *untrack(Op *o, uint16_t tag, uint8_t newly) {
  s_untracked |= newly;
  s_silenced |= newly;
  for (int v = 0; v < 8; v++)
    if ((newly >> v) & 1) {
      HALF_OP(o, tag, RSPDSP_V_PITCH + 2 * v, 0);
      HALF_OP(o, tag, RSPDSP_V_GAIN + 2 * v, 0);
      HALF_OP(o, tag, RSPDSP_V_ADSRSTATE + 2 * v, 4);
      g_snesdsp_rsp_untracked++;
    }
  return o;
}
static ApuYellowRegs s_regs;
/* The ring is only ever read after its lines are dropped: none may stay
 * dirty from the startup BSS clear. */
static void stream_init(void) {
  data_cache_hit_writeback_invalidate(s_ring, sizeof s_ring);
  /* The ledger is written through the uncached alias only: no line of it
   * may stay in the cache from the startup BSS clear. */
  data_cache_hit_writeback_invalidate(g_apu_ledger, sizeof g_apu_ledger);
  data_cache_hit_writeback_invalidate(g_apu_ledger_tail_pub, sizeof g_apu_ledger_tail_pub);
  data_cache_hit_writeback_invalidate(g_apu_ledger_page, sizeof g_apu_ledger_page);
}
void *apu_host_uncached(void *p) { return UncachedAddr(p); }
uint32_t g_snesdsp_rsp_stream_posts, g_snesdsp_rsp_stream_slot_waits, g_snesdsp_rsp_stream_overflow;

static inline void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void put16(void *p, uint16_t v) { memcpy(p, &v, 2); }

/* Wait for batch `seq` (the wait of the old join, without the join). */
static void wait_seq(uint32_t seq) {
  if (red_dsp_done(seq)) return;
  const uint32_t t0 = get_ticks();
#if SNESRECOMP_BLUE_THREAD
  if (blue_is_current()) {
    g_red_blue_waits = seq | 0x80000000u;
    while (!red_dsp_done(seq) && (uint32_t)get_ticks() - t0 < TICKS_FROM_MS(50)) {
      blue_yield();
      g_snesdsp_rsp_blue_yields++;
    }
    g_red_blue_waits = 0;
  }
#endif
  while (!red_dsp_done(seq) && (uint32_t)get_ticks() - t0 < TICKS_FROM_MS(50)) {}
  if (!red_dsp_done(seq)) {
    g_snesdsp_rsp_fallbacks++;
#if RSPDSP_RING
    rspq_ring_wait();
#else
    rspq_wait();
#endif
    /* Red has run everything posted: every batch is done, whether or not
     * its message arrived. */
    if (!red_dsp_done(seq)) g_red_dsp_done = s_seq;
  }
  g_snesdsp_rsp_join_wait += get_ticks() - t0;
  if (blue_is_current()) g_snesdsp_rsp_join_wait_blue += get_ticks() - t0;
}

static void reap(void) {
  while (s_head != s_tail && red_dsp_done(s_slot[s_head % kSlots].seq)) {
    s_done_end = s_slot[s_head % kSlots].end;
    s_head++;
  }
}

static void wait_all(void) {
  while (s_head != s_tail) { wait_seq(s_slot[s_head % kSlots].seq); reap(); }
}

void apu_yellow_start(const Dsp *dsp, uint8_t *aram, uint32_t at, uint8_t untracked,
                      uint8_t conflict) {
  wait_all();
  snesdsp_rsp_pack(dsp, &s_block);
#if !RSPDSP_VM
  conflict = untracked;
#endif
  s_untracked = untracked;
  s_silenced = conflict;   /* (see untrack) */
  s_vm_mode = false;
  for (int v = 0; v < 8; v++)
    if ((conflict >> v) & 1) {
      put(&s_block, RSPDSP_V_PITCH + 2 * v, 0);
      put(&s_block, RSPDSP_V_GAIN + 2 * v, 0);
      put(&s_block, RSPDSP_V_ADSRSTATE + 2 * v, 4);
    }
  put(&s_block, RSPDSP_M_KEYON, getu(&s_block, RSPDSP_M_KEYON) & ~conflict);
  put32(s_block.bytes + RSPDSP_P_ARAM, PhysicalAddr(aram));
  data_cache_hit_writeback_invalidate(&s_block, RSPDSP_STATE_SIZE);
  memcpy(&s_shadow, dsp, offsetof(Dsp, sampleBuffer));   /* decodes register writes from here on */
  s_posted = s_done_end = s_published = at;
  s_running = true;
}

void apu_yellow_post(const ApuDspLogEntry *log, uint32_t n, uint32_t samples) {
  if (s_tail - s_head == kSlots) {   /* no free slot: wait for the oldest */
    g_snesdsp_rsp_stream_slot_waits++;
    wait_seq(s_slot[s_head % kSlots].seq);
  }
  reap();
  Slot *sl = &s_slot[s_tail % kSlots];
  CHOOSE_OVERLAY(&s_shadow, log, n, IS_REG_APU);   /* on the state at the batch's start */
  /* Register writes (decoded on the shadow, which follows every write) and
   * SPC writes diverted to the S-DSP; other ARAM writes are in ARAM. */
  bool vm = (s_untracked & ~s_silenced) != 0;
  Op *o = sl->ops.op, *end = sl->ops.op + kMaxOps;
  for (uint32_t i = 0; i < n && o + 24 <= end; i++) {
    if (log[i].isReg == 1) {
      const uint8_t r = (uint8_t)log[i].adr;
      dsp_write(&s_shadow, r, log[i].val);
      o = emit_reg(o, log[i].tag, r);
      if (r == 0x2d && (log[i].val & 0xfe & ~s_untracked)) {
#if RSPDSP_VM   /* the voice-major variant (Blue starts a batch at such a write) */
        s_untracked |= (uint8_t)(log[i].val & 0xfe);
        vm = true;
#else
        o = untrack(o, log[i].tag, (uint8_t)(log[i].val & 0xfe & ~s_untracked));
#endif
      }
      /* The stopgap for silenced voices (see untrack). */
      else if (r == 0x4c && s_silenced)   /* key ons skip them */
        HALF_OP(o, log[i].tag, RSPDSP_M_KEYON, log[i].val & ~s_silenced);
      else if (((r & 0xf) == 2 || (r & 0xf) == 3) && ((s_silenced >> (r >> 4)) & 1))
        HALF_OP(o, log[i].tag, RSPDSP_V_PITCH + 2 * (r >> 4), 0);
    } else if (log[i].isReg == 4) {
      if ((uint8_t)log[i].adr & ~s_silenced) o = untrack(o, log[i].tag, (uint8_t)(log[i].adr & ~s_silenced));
    } else if (log[i].isReg == 3) {
      o->tag = log[i].tag; o->kind = RSPDSP_LOG_ARAM; o->pad = 0;
      o->adr = log[i].adr; o->val = log[i].val; o++;
    }
  }
  if (o + 24 > end) g_snesdsp_rsp_stream_overflow++;   /* (Blue caps a post's log) */
  const uint32_t nops = (uint32_t)(o - sl->ops.op);
  memset(o, 0, 32 * sizeof *o);
  /* Records. */
  const ApuDspRecords *r = &g_apu_dsp_records;
  uint32_t nb = r->nblocks, nk = r->nkeyons;
  if (nb > kSlotBlocks || nk > kSlotKeyons) { g_snesdsp_rsp_stream_overflow++; nb = nb > kSlotBlocks ? kSlotBlocks : nb; nk = nk > kSlotKeyons ? kSlotKeyons : nk; }
  memcpy(sl->blocks, r->blocks, nb * RSPDSP_BLK_REC);
  memset(sl->blocks + nb * RSPDSP_BLK_REC, 0, 16 * RSPDSP_BLK_REC);
  memcpy(sl->keyons, r->keyons, nk * RSPDSP_KON_REC);
  memset(sl->keyons + nk, 0, 32 * RSPDSP_KON_REC);
  g_snesdsp_rsp_blocks += nb;
  uint8_t *aram = s_shadow.apu_ram;
#if RSPDSP_VM
  /* A voice-major batch reads ARAM at its own samples: every SPC write
   * before the batch's end must be in RDRAM (later ones are undone through
   * the ledger). Entering the mode, all of ARAM; then the lines of the
   * ledger's new entries (SPC writes are ledgered while a voice is
   * untracked, which every voice-major batch has). */
  if (vm) {
    s_run_overlay = s_overlay_vm_id;
    g_snesdsp_rsp_vm_batches++;
    if (!s_vm_mode) data_cache_hit_writeback(aram, 0x10000);
    else
      for (uint32_t i = s_ledger_seen; i != g_apu_ledger_tail; i++) {
        const volatile ApuLedgerEntry *e = UncachedAddr(&g_apu_ledger[i & (APU_LEDGER_LEN - 1)]);
        data_cache_hit_writeback(aram + (e->adr & ~15u), 16);
      }
  }
  s_vm_mode = vm;
  s_ledger_seen = g_apu_ledger_tail;
#endif
  /* The batch's echo lines: the SPC's writes there to RDRAM, none left in
   * the cache (the RSP writes them). */
  if (r->nruns > 16) {
    data_cache_hit_writeback_invalidate(aram, 0x10000);
  } else {
    for (uint32_t k = 0; k < r->nruns; k++) {
      const uint32_t l = r->run_line[k], c = r->run_count[k];
      const uint32_t c1 = l + c > 0x1000 ? 0x1000 - l : c;
      data_cache_hit_writeback_invalidate(aram + l * 16, c1 * 16);
      if (c1 < c) data_cache_hit_writeback_invalidate(aram, (c - c1) * 16);
    }
  }
  /* Parameters. */
  s_seq = (s_seq + 1) & 0xffffffu;
  uint8_t *p = sl->prm.b;
  put32(p + RSPDSP_PP_LOG, PhysicalAddr(sl->ops.op));
  put32(p + RSPDSP_PP_OUT, PhysicalAddr(s_ring) + (s_posted & (APU_STREAM_RING - 1)) * RSPDSP_OUT_SLOT);
  put32(p + RSPDSP_PP_DONE_MSG, RED_MSG(RED_MSG_DSP_DONE, s_seq));
  put32(p + RSPDSP_PP_BLK, PhysicalAddr(sl->blocks));
  put32(p + RSPDSP_PP_KON, PhysicalAddr(sl->keyons));
  put16(p + RSPDSP_PP_SAMPLES, (uint16_t)samples);
  put16(p + RSPDSP_PP_LOGN, (uint16_t)nops);
  put32(p + RSPDSP_PP_START, s_posted);
  put32(p + RSPDSP_PP_LEDGER, PhysicalAddr(g_apu_ledger));
  put32(p + RSPDSP_PP_LTAIL, PhysicalAddr(g_apu_ledger_tail_pub));
  put32(p + RSPDSP_PP_LPAGE, PhysicalAddr(g_apu_ledger_page));
  data_cache_hit_writeback(&sl->prm, sizeof sl->prm);
  data_cache_hit_writeback(sl->ops.op, (nops + 32) * sizeof(Op));
  data_cache_hit_writeback(sl->blocks, (nb + 16) * RSPDSP_BLK_REC);
  data_cache_hit_writeback(sl->keyons, (nk + 32) * RSPDSP_KON_REC);
  sl->seq = s_seq;
  s_posted += samples;
  sl->end = s_posted;
  s_tail++;
  const uint32_t cmd[2] = {s_run_overlay | PhysicalAddr(&s_block), PhysicalAddr(&sl->prm)};
#if RSPDSP_RING
  rspq_ring_write(cmd, 2);
#else
  rspq_write(s_run_overlay, 0, PhysicalAddr(&s_block), PhysicalAddr(&sl->prm));
  rspq_flush();
#endif
  g_snesdsp_rsp_stream_posts++;
  g_snesdsp_rsp_samples += samples;
}

uint32_t apu_yellow_progress(void) { reap(); return s_done_end; }

void apu_yellow_wait(uint32_t sample) {
  reap();
  while (s_head != s_tail && (int32_t)(sample - s_done_end) > 0) {
    wait_seq(s_slot[s_head % kSlots].seq);
    reap();
  }
  g_snesdsp_rsp_joins++;
}

/* The readable registers from Yellow's own state block (valid when nothing
 * later is in flight: Blue asks after waiting for the SPC's time). */
const ApuYellowRegs *apu_yellow_regs(void) {
  const RspDspState *u = UncachedAddr(&s_block);
  for (int v = 0; v < 8; v++) {
    s_regs.envx[v] = (uint8_t)(getu(u, RSPDSP_V_GAIN + 2 * v) >> 4);
    s_regs.outx[v] = (uint8_t)(get(u, RSPDSP_V_SAMPLEOUT + 2 * v) >> 7);
  }
  s_regs.endx = u->bytes[RSPDSP_RAM + 0x7c];
  s_regs.at = s_done_end;
  return &s_regs;
}

void apu_yellow_stop(Dsp *dsp) {
  if (!s_running) return;
  wait_all();
  data_cache_hit_invalidate(&s_block, RSPDSP_STATE_SIZE);
  snesdsp_rsp_unpack(dsp, &s_block);
  set_envx_outx(dsp, s_slot[0].ops.op, 0, 1);   /* ENVX/OUTX from the state */
  s_running = false;
}

void apu_yellow_discard(void) { wait_all(); s_running = false; }

bool apu_yellow_running(void) { return s_running; }

const int16_t *apu_yellow_ring(uint32_t *mask) {
  *mask = APU_STREAM_RING - 1;
  return s_ring;
}

/* Green: the finished samples, with their lines dropped from the cache. */
uint32_t apu_yellow_publish(void) {
  const uint32_t p = apu_yellow_progress();
  uint32_t from = s_published;
  if ((uint32_t)(p - from) > APU_STREAM_RING) from = p - APU_STREAM_RING;
  while (from != p) {
    const uint32_t i = from & (APU_STREAM_RING - 1);
    uint32_t n = APU_STREAM_RING - i;
    if (n > p - from) n = p - from;
    /* Whole lines (Green never writes the ring, so none is dirty). */
    const uintptr_t a = (uintptr_t)&s_ring[i * 4] & ~(uintptr_t)15;
    const uintptr_t e = ((uintptr_t)&s_ring[(i + n) * 4] + 15) & ~(uintptr_t)15;
    data_cache_hit_invalidate((void *)a, (unsigned long)(e - a));
    from += n;
  }
  s_published = p;
  return p;
}
#endif
