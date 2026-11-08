/* S-DSP batches on the RSP (libdragon side). See rsp_snesdsp.S. */
#ifndef SNESDSP_RSP_H
#define SNESDSP_RSP_H

#include <stdint.h>
#include "snesdsp_rsp_layout.h"

struct Dsp;

typedef struct __attribute__((aligned(16))) RspDspState {
  uint8_t bytes[RSPDSP_STATE_SIZE];
} RspDspState;

void snesdsp_rsp_init(void);
/* Run `samples` DSP samples on the RSP: state in, state out (synchronous). */
void snesdsp_rsp_run(struct Dsp *dsp, uint32_t samples);
/* Conversion between the C DSP and the RSP block (exposed for tests). */
void snesdsp_rsp_pack(const struct Dsp *dsp, RspDspState *s);
void snesdsp_rsp_unpack(struct Dsp *dsp, const RspDspState *s);

#endif
