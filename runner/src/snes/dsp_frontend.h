/* S-DSP front end: the control half of the S-DSP (docs/n64/RED_DESIGN.md,
 * "ARAM between the SPC and the S-DSP: the agreed design").
 *
 * It runs beside the SPC, in the SPC's time, and tracks exactly what the C
 * S-DSP (dsp.c dsp_cycle) does with positions: each voice's pitch counter,
 * the BRR block it decodes when the counter passes $FFFF (with the previous
 * block's loop / end flags, the directory's loop address and ENDX), and key
 * on (polled on even samples, after the decode; KOF held keeps a key on
 * latched). At each block decode it reads the 9-byte block from ARAM at
 * that sample, the moment the S-DSP would, and reports it (on_block), so a
 * back end that only synthesises (Red) never needs the SPC's ARAM.
 *
 * Event-driven: it keeps the next sample at which anything can happen (the
 * earliest block decode over the voices, a pending key on) and does nothing
 * until then; a voice's decode sample follows from its counter and pitch.
 * Callers advance it to the current sample before every S-DSP register
 * write and ARAM write (dsp_fe_advance), then pass register writes on
 * (dsp_fe_write).
 *
 * Not predictable here: pitch modulation (a voice's pitch depends on the
 * previous voice's output). Such voices are dropped from tracking
 * (dsp_fe_tracked) and need the S-DSP itself.
 *
 * Envelopes are lazy: a voice's envelope (gain, stage, rate counter) is
 * stepped, exactly as dsp_cycleChannel does, only when something needs it:
 * an ENVX read (dsp_fe_envx), a block ending without a loop, a key on, a
 * write to the voice's ADSR/GAIN registers, KOF or FLG (key off and reset
 * act on every sample while set). */
#ifndef SNESRECOMP_DSP_FRONTEND_H
#define SNESRECOMP_DSP_FRONTEND_H

#include <stdbool.h>
#include <stdint.h>

struct Dsp;

typedef void DspFeBlockFn(void *ctx, int voice, uint32_t sample, uint16_t start,
                          const uint8_t bytes[9]);
/* A key on's start address (read from the directory at that sample). */
typedef void DspFeKeyOnFn(void *ctx, int voice, uint32_t sample, uint16_t start);
/* Every ARAM range the front end reads: directory entries and blocks. */
typedef void DspFeReadFn(void *ctx, uint16_t adr, unsigned len);

typedef struct DspFeVoice {
  uint32_t t;          /* the counter is as of the start of sample t */
  uint32_t next;       /* next sample with an event for this voice (UINT32_MAX: none) */
  uint16_t pitch, counter, offset;
  uint8_t srcn, prev_flags;
  bool key_on, key_off, pmod;
  /* Envelope, as of the start of sample te. */
  uint32_t te;
  uint16_t gain, rate_counter, sustain, gain_value;
  uint16_t rates[4];   /* attack, decay, sustain, gain (dsp.c adsrRates) */
  uint8_t state, gain_mode;
  bool use_gain, direct_gain;
} DspFeVoice;

typedef struct DspFrontEnd {
  DspFeVoice v[8];
  const uint8_t *aram;
  uint32_t now;        /* samples processed (the next sample's index) */
  uint32_t next;       /* earliest event over the voices */
  uint32_t even_base;  /* a sample index that was an even sample */
  uint16_t dir;
  uint8_t endx;
  uint8_t untracked;   /* voices with pitch modulation (bit per voice) */
  bool reset;          /* FLG bit 7 */
  DspFeBlockFn *on_block;
  DspFeKeyOnFn *on_keyon;   /* optional */
  DspFeReadFn *on_read;     /* optional */
  void *ctx;
  uint32_t events;     /* statistics: samples on which work was done */
} DspFrontEnd;

/* Start from the S-DSP's current state (at sample `now`). */
void dsp_fe_init(DspFrontEnd *fe, const struct Dsp *dsp, const uint8_t *aram, uint32_t now,
                 DspFeBlockFn *on_block, void *ctx);
/* Process every sample before `sample` (no-op unless an event is due). */
static inline void dsp_fe_advance(DspFrontEnd *fe, uint32_t sample) {
  extern void dsp_fe_run(DspFrontEnd *fe, uint32_t sample);
  if (fe->next < sample) dsp_fe_run(fe, sample);
  else fe->now = sample;
}
/* A register write at sample fe->now (call dsp_fe_advance first). */
void dsp_fe_write(DspFrontEnd *fe, uint8_t adr, uint8_t val);
/* State queries at sample fe->now. */
uint16_t dsp_fe_counter(const DspFrontEnd *fe, int voice);
static inline uint8_t dsp_fe_endx(const DspFrontEnd *fe) { return fe->endx; }
/* ENVX at sample fe->now (gain >> 4): steps the voice's envelope up to now. */
uint8_t dsp_fe_envx(DspFrontEnd *fe, int voice);
/* Step a voice's envelope up to fe->now (dsp_fe_envx does this). */
void dsp_fe_env_sync(DspFrontEnd *fe, int voice);
static inline bool dsp_fe_tracked(const DspFrontEnd *fe, int voice) {
  return !((fe->untracked >> voice) & 1);
}

#endif
