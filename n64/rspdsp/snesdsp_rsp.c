/* S-DSP batches on the RSP: libdragon glue. Only this file and the test
 * include <libdragon.h>; the runtime's DSP state comes from dsp.h.
 * The N64 CPU and the RSP are both big-endian, so the state block is read
 * and written natively. */
#include <libdragon.h>
#include <stddef.h>
#include <string.h>

#include "snesdsp_rsp.h"
#include "blue_thread.h"
#include "dsp.h"
#include "apu_dsp_batch.h"

DEFINE_RSP_UCODE(rsp_snesdsp);

enum { kMaxSamples = 1024, kMaxOps = 4096 };

static uint32_t s_overlay_id;
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
void snesdsp_rsp_init(void) {
  s_overlay_id = rspq_overlay_register(&rsp_snesdsp);
  view_init();
  /* Launches invalidate only the samples a batch writes: no line of the
   * output buffer may stay dirty from the startup BSS clear. */
  data_cache_hit_writeback_invalidate(&s_out, sizeof s_out);
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
  memset(s, 0, sizeof *s);
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
    if (pmod_used(&s_shadow) || (mirror && !echo_ok(&s_shadow))) return false;
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
  return !pmod_used(dsp) && (!mirror || echo_ok(dsp));
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
static void run(Dsp *dsp, uint8_t *aram, uint32_t aram_len, Op *o, uint32_t samples) {
  const uint32_t nops = (uint32_t)(o - s_ops.op);
  memset(o, 0, 32 * sizeof *o);  /* the RSP fetches whole chunks */
  snesdsp_rsp_pack(dsp, &s_block);
  put(&s_block, RSPDSP_P_SAMPLES, (int)samples);
  put(&s_block, RSPDSP_P_LOGN, (int)nops);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_ARAM) = PhysicalAddr(aram);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_LOG) = PhysicalAddr(s_ops.op);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_OUT) = PhysicalAddr(s_out.s);
  data_cache_hit_writeback_invalidate(&s_block, sizeof s_block);
  data_cache_hit_writeback_invalidate(s_ops.op, (nops + 32) * sizeof(Op));
  data_cache_hit_writeback_invalidate(aram, aram_len);
  data_cache_hit_writeback_invalidate(&s_out, sizeof s_out);
  const uint32_t t0 = get_ticks();
  rspq_write(s_overlay_id, 0, PhysicalAddr(&s_block), 0);
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
  shadow_from(dsp);
  Op *o = s_ops.op, *end = s_ops.op + kMaxOps;
  for (uint32_t i = 0; i < n; i++)
    if (!decode(&o, end, log[i].tag, log[i].isReg == 1, log[i].adr, log[i].val, false))
      return false;
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
static uint8_t s_view[RSPDSP_ARAM_SIZE] __attribute__((aligned(16)));  /* + the mirror of $0000-$000F */
/* The view is only ever accessed uncached; drop any lines the startup BSS
 * clear left dirty, so a later eviction cannot overwrite page copies. */
static void view_init(void) { data_cache_hit_writeback_invalidate(s_view, sizeof s_view); }
static struct {
  bool active, echoWrites;
  Dsp *dsp;
  uint8_t *aram;
  uint32_t samples, nops;
  uint16_t esa, index, remain, delay;
  rspq_syncpoint_t sync;
} s_flight;
uint32_t g_snesdsp_rsp_async, g_snesdsp_rsp_joins, g_snesdsp_rsp_join_wait;
uint32_t g_snesdsp_rsp_launch_ticks, g_snesdsp_rsp_join_ticks, g_snesdsp_rsp_pages;
uint32_t g_snesdsp_rsp_join_wait_blue;   /* the part of join_wait spent on the Blue thread */
uint32_t g_snesdsp_rsp_decode_ticks, g_snesdsp_rsp_log_entries;   /* launch: log decode, entries */
uint32_t g_snesdsp_rsp_view_ticks, g_snesdsp_rsp_pack_ticks, g_snesdsp_rsp_submit_ticks;
uint32_t g_snesdsp_rsp_reject[4];

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
  /* Why batches stay in C (g_snesdsp_rsp_reject: an echo window that wraps
   * past $FFFF, pitch modulation, other state, the log). The view mirrors
   * $0000-$000F after $FFFF for BRR blocks that run past the end. */
  if (!echo_ok(dsp)) { g_snesdsp_rsp_reject[0]++; return false; }
  if (pmod_used(dsp)) { g_snesdsp_rsp_reject[1]++; return false; }
  if (((uintptr_t)aram & 7) || !batch_ok(dsp, samples, true)) { g_snesdsp_rsp_reject[2]++; return false; }
  const uint32_t td = get_ticks();
  shadow_from(dsp);
  Op *o = s_ops.op, *end = s_ops.op + kMaxOps;
  for (uint32_t i = 0; i < n; i++)
    if (!decode(&o, end, log[i].tag, log[i].isReg == 1, log[i].adr, log[i].val, true))
      { g_snesdsp_rsp_reject[3]++; return false; }
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

  const uint32_t tp = get_ticks();
  g_snesdsp_rsp_view_ticks += tp - tv;
  const uint32_t nops = (uint32_t)(o - s_ops.op);
  memset(o, 0, 32 * sizeof *o);
  snesdsp_rsp_pack(dsp, &s_block);
  put(&s_block, RSPDSP_P_SAMPLES, (int)samples);
  put(&s_block, RSPDSP_P_LOGN, (int)nops);
  const uint32_t ts = get_ticks();
  g_snesdsp_rsp_pack_ticks += ts - tp;
  *(uint32_t *)(s_block.bytes + RSPDSP_P_ARAM) = PhysicalAddr(s_view);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_LOG) = PhysicalAddr(s_ops.op);
  *(uint32_t *)(s_block.bytes + RSPDSP_P_OUT) = PhysicalAddr(s_out.s);
  data_cache_hit_writeback_invalidate(&s_block, sizeof s_block);
  data_cache_hit_writeback_invalidate(s_ops.op, (nops + 32) * sizeof(Op));
  /* Only the samples this batch writes; the CPU never writes s_out. */
  data_cache_hit_invalidate(&s_out, (samples * 4 + 15) & ~15u);
  rspq_write(s_overlay_id, 0, PhysicalAddr(&s_block), 0);
  s_flight.sync = rspq_syncpoint_new();
  rspq_flush();
  g_snesdsp_rsp_submit_ticks += get_ticks() - ts;
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

void snesdsp_rsp_apu_join(void) {
  if (!s_flight.active) return;
  const uint32_t tj = get_ticks();
  const uint32_t t0 = get_ticks();
  rspq_syncpoint_wait(s_flight.sync);
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
  s_flight.active = false;
  g_snesdsp_rsp_join_ticks += get_ticks() - tj;
}
