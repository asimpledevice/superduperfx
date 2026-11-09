/* S-DSP batches on the RSP: libdragon glue. Only this file and the test
 * include <libdragon.h>; the runtime's DSP state comes from dsp.h.
 * The N64 CPU and the RSP are both big-endian, so the state block is read
 * and written natively. */
#include <libdragon.h>
#include <stddef.h>
#include <string.h>

#include "snesdsp_rsp.h"
#include "dsp.h"

DEFINE_RSP_UCODE(rsp_snesdsp);

enum { kMaxSamples = 1024, kMaxOps = 4096 };

static uint32_t s_overlay_id;
static RspDspState s_block;
static struct __attribute__((aligned(16))) { int16_t s[kMaxSamples * 2 + 8]; } s_out;
typedef struct { uint16_t tag; uint8_t kind, pad; uint16_t adr, val; } Op;
static struct __attribute__((aligned(16))) { Op op[kMaxOps + 32]; } s_ops;
static Dsp s_shadow;
uint32_t g_snesdsp_rsp_ticks, g_snesdsp_rsp_samples;

void snesdsp_rsp_init(void) {
  rspq_init();
  s_overlay_id = rspq_overlay_register(&rsp_snesdsp);
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
  /* Split coefficients that follow the fields above. */
  {
    RspDspState tmp;
    pack_coefficients(d, &tmp);
#define COPY(off) HALF(off, *(int16_t *)(tmp.bytes + (off)))
    if (lo == 0) { COPY(RSPDSP_V_VOLL_U + v); COPY(RSPDSP_V_VOLL_C + v); }
    if (lo == 1) { COPY(RSPDSP_V_VOLR_U + v); COPY(RSPDSP_V_VOLR_C + v); }
    if (lo == 0xf) { COPY(RSPDSP_FIR_U + v); COPY(RSPDSP_FIR_C + v); }
    if (lo == 0xc && c < 4) { COPY(RSPDSP_SC_U + 2 * c); COPY(RSPDSP_SC_C + 2 * c); }
    if (reg == 0x0d) { COPY(RSPDSP_SC_U + 8); COPY(RSPDSP_SC_C + 8);
                       COPY(RSPDSP_SC_U + 10); COPY(RSPDSP_SC_C + 10); }
    if (reg == 0x4d) for (int i = 0; i < 8; i++) COPY(RSPDSP_V_ECHOMASK + 2 * i);
#undef COPY
  }
#undef HALF
  o->tag = tag; o->kind = RSPDSP_LOG_BYTE; o->pad = 0;
  o->adr = RSPDSP_RAM + reg; o->val = d->ram[reg]; o++;
  return o;
}

bool snesdsp_rsp_batch(Dsp *dsp, uint8_t *aram, const SnesDspLogIn *log, uint32_t n,
                       uint32_t samples) {
  if (samples > kMaxSamples || dsp->shadow || dsp->output_filter || dsp->suppressOutput)
    return false;
  if (pmod_used(dsp) || !echo_ok(dsp)) return false;
  /* Decode the log against a shadow that sees only register writes. */
  s_shadow = *dsp;
  Op *o = s_ops.op, *end = s_ops.op + kMaxOps;
  for (uint32_t i = 0; i < n; i++) {
    if (o + 24 > end) return false;
    if (log[i].isReg) {
      dsp_write(&s_shadow, (uint8_t)log[i].adr, log[i].val);
      if (pmod_used(&s_shadow) || !echo_ok(&s_shadow)) return false;
      o = emit_reg(o, log[i].tag, (uint8_t)log[i].adr);
    } else {
      o->tag = log[i].tag; o->kind = RSPDSP_LOG_ARAM; o->pad = 0;
      o->adr = log[i].adr; o->val = log[i].val; o++;
      if (log[i].adr < 16) {  /* and its mirror after $FFFF: address bit 16 in pad */
        o[0] = o[-1];
        o->pad = 1;
        o++;
      }
    }
  }
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
  data_cache_hit_writeback_invalidate(aram, RSPDSP_ARAM_SIZE);
  data_cache_hit_writeback_invalidate(&s_out, sizeof s_out);
  const uint32_t t0 = get_ticks();
  rspq_write(s_overlay_id, 0, PhysicalAddr(&s_block), 0);
  rspq_wait();
  g_snesdsp_rsp_ticks += get_ticks() - t0;
  g_snesdsp_rsp_samples += samples;
  snesdsp_rsp_unpack(dsp, &s_block);

  /* Output ring, as dsp_cycle publishes each sample. */
  for (uint32_t i = 0; i < samples; i++) {
    const uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
    if (fill >= DSP_SAMPLE_RING) continue;
    const uint32_t w = dsp->sampleWrite & (DSP_SAMPLE_RING - 1);
    dsp->sampleBuffer[w * 2] = s_out.s[i * 2];
    dsp->sampleBuffer[w * 2 + 1] = s_out.s[i * 2 + 1];
    dsp->sampleWrite++;
  }
  return true;
}
