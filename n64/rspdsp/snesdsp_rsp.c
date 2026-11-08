/* S-DSP batches on the RSP: libdragon glue. Only this file and the test
 * include <libdragon.h>; the runtime's DSP state comes from dsp.h. */
#include <libdragon.h>
#include <string.h>

#include "snesdsp_rsp.h"
#include "dsp.h"

DEFINE_RSP_UCODE(rsp_snesdsp);

static uint32_t s_overlay_id;
static RspDspState s_block;  /* 16-byte aligned; the RSP DMAs it */

void snesdsp_rsp_init(void) {
  rspq_init();
  s_overlay_id = rspq_overlay_register(&rsp_snesdsp);
}

static inline void put16(RspDspState *s, unsigned off, uint16_t v) {
  s->bytes[off] = (uint8_t)(v >> 8);  /* RSP is big-endian, like the CPU */
  s->bytes[off + 1] = (uint8_t)v;
}
static inline uint16_t get16(const RspDspState *s, unsigned off) {
  return (uint16_t)(s->bytes[off] << 8 | s->bytes[off + 1]);
}

void snesdsp_rsp_pack(const struct Dsp *dsp, RspDspState *s) {
  memset(s, 0, sizeof *s);
  put16(s, RSPDSP_NOISE_SAMPLE, (uint16_t)dsp->noiseSample);
  put16(s, RSPDSP_NOISE_RATE, dsp->noiseRate);
  put16(s, RSPDSP_NOISE_COUNTER, dsp->noiseCounter);
}

void snesdsp_rsp_unpack(struct Dsp *dsp, const RspDspState *s) {
  dsp->noiseSample = (int16_t)get16(s, RSPDSP_NOISE_SAMPLE);
  dsp->noiseRate = get16(s, RSPDSP_NOISE_RATE);
  dsp->noiseCounter = get16(s, RSPDSP_NOISE_COUNTER);
}

void snesdsp_rsp_run(struct Dsp *dsp, uint32_t samples) {
  snesdsp_rsp_pack(dsp, &s_block);
  data_cache_hit_writeback_invalidate(&s_block, sizeof s_block);
  rspq_write(s_overlay_id, 0, PhysicalAddr(&s_block), samples);
  rspq_wait();
  snesdsp_rsp_unpack(dsp, &s_block);
}
