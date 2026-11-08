/* S-DSP batches on the RSP (libdragon side). See rsp_snesdsp.S. */
#ifndef SNESDSP_RSP_H
#define SNESDSP_RSP_H

#include <stdbool.h>
#include <stdint.h>
#include "snesdsp_rsp_layout.h"

struct Dsp;

typedef struct __attribute__((aligned(16))) RspDspState {
  uint8_t bytes[RSPDSP_STATE_SIZE];
} RspDspState;

/* A logged write, in time order: before sample `tag` of the batch, either
 * a DSP register write (isReg) or an ARAM byte write. */
typedef struct SnesDspLogIn {
  uint16_t tag;
  uint8_t isReg;
  uint8_t val;
  uint16_t adr;
} SnesDspLogIn;

void snesdsp_rsp_init(void);

/* Run `samples` DSP samples on the RSP, with `log` applied at its tags,
 * exactly as the C code would (apply writes tagged k, run sample k, ...;
 * then writes tagged `samples`). `aram` is the DSP's view of ARAM: 64 KB
 * plus a 16-byte mirror of bytes 0-15, 16-byte aligned; the RSP reads and
 * writes it (echo, logged writes). Output samples are appended to the DSP's
 * output ring as dsp_cycle would. Synchronous.
 * Returns false, changing nothing, if the batch needs the C DSP (pitch
 * modulation, an echo buffer touching $0000-$000F or wrapping past $FFFF,
 * the host shadow mixer or output filter, or more than the buffers hold). */
bool snesdsp_rsp_batch(struct Dsp *dsp, uint8_t *aram, const SnesDspLogIn *log,
                       uint32_t n, uint32_t samples);

/* Conversion between the C DSP and the RSP block (exposed for tests). */
void snesdsp_rsp_pack(const struct Dsp *dsp, RspDspState *s);
void snesdsp_rsp_unpack(struct Dsp *dsp, const RspDspState *s);

#endif
