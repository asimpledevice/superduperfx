/* Yellow in C: the S-DSP back end of the audio stream (SNESRECOMP_DSP_STREAM,
 * docs/n64/RED_DESIGN.md "The audio split") as a C model, for host builds and
 * tests. The N64 build's back end is the microcode on Red
 * (n64/rspdsp/snesdsp_rsp.c); both implement apu_dsp_batch.h's
 * apu_yellow_* interface with the same results.
 *
 * Yellow owns its S-DSP state from apu_yellow_start until apu_yellow_stop:
 * Blue never touches it in between. Blue posts batches (register writes,
 * SPC writes diverted to the S-DSP, and the front end's block and key-on
 * records); Yellow runs them in order on the live ARAM (its echo ring is
 * in place there), writes its output samples and, after each batch, the
 * readable registers (OUTX, ENVX, ENDX) as of its progress.
 *
 * This model runs a batch as late as it can: only when Blue waits for a
 * sample, or when more than kSlots batches are outstanding (the microcode's
 * slots). Running late is the hardest case for Blue's rules, so tests see
 * every hazard the RSP could. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apu.h"
#include "dsp.h"
#include "apu_dsp_batch.h"

#if SNESRECOMP_DSP_STREAM

enum { kSlots = 4 };

typedef struct {
  ApuDspLogEntry log[APU_DSP_LOG_LEN];
  uint32_t n, samples;
  uint8_t blocks[16 * 2048];
  uint32_t nblocks;
  uint16_t keyons[1024];
  uint32_t nkeyons;
} YellowBatch;

static struct {
  bool running;
  Dsp dsp;              /* Yellow's own S-DSP state */
  uint8_t *aram;
  uint32_t progress;    /* samples run */
  uint32_t posted;      /* samples posted */
  YellowBatch batch[kSlots];
  uint32_t head, tail;  /* batches posted (tail) and run (head) */
  ApuYellowRegs regs;
  uint32_t missing;     /* records that ran out (must stay 0) */
  int16_t ring[APU_STREAM_RING * 4];   /* output: 8 bytes a sample */
  uint8_t untracked;    /* voices read from ARAM through the ledger */
  uint32_t sample;      /* the sample being run */
} y;

/* ARAM at Yellow's sample: the undo ledger applied. */
static uint8_t yellow_read(void *ctx, uint16_t adr) {
  (void)ctx;
  for (uint32_t i = g_apu_ledger_head; i != g_apu_ledger_tail; i++) {
    const ApuLedgerEntry *e = &g_apu_ledger[i & (APU_LEDGER_LEN - 1)];
    if (e->adr == adr && (int32_t)(e->time - y.sample) > 0) return e->old;
  }
  return y.aram[adr];
}

uint32_t g_dsp_yellow_c_missing;

static void yellow_regs(void) {
  for (int v = 0; v < 8; v++) {
    y.regs.envx[v] = y.dsp.ram[v * 16 + 8];
    y.regs.outx[v] = y.dsp.ram[v * 16 + 9];
  }
  y.regs.endx = y.dsp.ram[0x7c];
  y.regs.at = y.progress;
}

static void yellow_run_one(void) {
  YellowBatch *b = &y.batch[y.head % kSlots];
  DspRecords rec = {b->blocks, b->nblocks, 0, b->keyons, b->nkeyons, 0, false,
                    y.untracked, yellow_read, NULL};
  y.dsp.records = &rec;
  uint32_t i = 0;
  for (uint32_t k = 0; k <= b->samples; k++) {
    for (; i < b->n && b->log[i].tag == k; i++) {
      const ApuDspLogEntry *e = &b->log[i];
      if (e->isReg == 1) {
        dsp_write(&y.dsp, (uint8_t)e->adr, e->val);
        if (e->adr == 0x2d) y.untracked |= e->val & 0xfe;   /* as the front end */
      } else if (e->isReg == 3) {
        y.aram[e->adr] = e->val;
      } else if (e->isReg == 4) {
        y.untracked |= (uint8_t)e->adr;
      }
    }
    rec.untracked = y.untracked;
    if (k < b->samples) {
      y.sample = y.progress + k;
      dsp_cycle(&y.dsp);
      /* The sample to the output ring; Yellow's own Dsp ring stays drained. */
      const uint32_t w = (y.dsp.sampleWrite - 1) & (DSP_SAMPLE_RING - 1);
      int16_t *o = &y.ring[4u * ((y.progress + k) & (APU_STREAM_RING - 1))];
      o[0] = y.dsp.sampleBuffer[w * 2];
      o[1] = y.dsp.sampleBuffer[w * 2 + 1];
      y.dsp.sampleRead = y.dsp.sampleWrite;
    }
  }
  y.dsp.records = NULL;
  if (rec.missing || rec.iblock != rec.nblocks || rec.ikeyon != rec.nkeyons) {
    y.missing++;
    g_dsp_yellow_c_missing++;
  }
  y.progress += b->samples;
  y.head++;
  yellow_regs();
}

void apu_yellow_start(const Dsp *dsp, uint8_t *aram, uint32_t at, uint8_t untracked) {
  y.dsp = *dsp;
  y.dsp.apu_ram = aram;
  y.dsp.shadow = NULL;
  y.dsp.output_filter = NULL;
  y.dsp.records = NULL;
  y.dsp.outRing = NULL;
  y.dsp.sampleRead = y.dsp.sampleWrite;   /* drained: every sample is written */
  y.aram = aram;
  y.progress = y.posted = at;
  y.head = y.tail = 0;
  y.untracked = untracked;   /* the front end's */
  y.running = true;
  yellow_regs();
}

void apu_yellow_post(const ApuDspLogEntry *log, uint32_t n, uint32_t samples) {
  if (y.tail - y.head == kSlots) yellow_run_one();   /* no free slot: the oldest runs */
  YellowBatch *b = &y.batch[y.tail % kSlots];
  b->n = 0;
  for (uint32_t i = 0; i < n; i++)
    if (log[i].isReg == 1 || log[i].isReg == 3 || log[i].isReg == 4) b->log[b->n++] = log[i];
  b->samples = samples;
  const ApuDspRecords *r = &g_apu_dsp_records;
  b->nblocks = r->nblocks < 2048 ? r->nblocks : 2048;
  b->nkeyons = r->nkeyons < 1024 ? r->nkeyons : 1024;
  memcpy(b->blocks, r->blocks, 16u * b->nblocks);
  memcpy(b->keyons, r->keyons, 2u * b->nkeyons);
  y.tail++;
  y.posted += samples;
  /* Test knob: run every batch at once (the eager extreme). */
  if (getenv("YELLOW_EAGER")) while (y.head != y.tail) yellow_run_one();
}

uint32_t apu_yellow_progress(void) { return y.progress; }

void apu_yellow_wait(uint32_t sample) {
  while ((int32_t)(sample - y.progress) > 0 && y.head != y.tail) yellow_run_one();
}

const ApuYellowRegs *apu_yellow_regs(void) { return &y.regs; }

void apu_yellow_stop(Dsp *dsp) {
  if (!y.running) return;
  while (y.head != y.tail) yellow_run_one();
  /* The simulation state back to Blue's Dsp (the output stays in the ring). */
  memcpy(dsp->ram, y.dsp.ram, offsetof(Dsp, sampleBuffer) - offsetof(Dsp, ram));
  y.running = false;
}

void apu_yellow_discard(void) { y.running = false; y.head = y.tail = 0; }

bool apu_yellow_running(void) { return y.running; }

const Dsp *apu_yellow_dsp(void) { return &y.dsp; }

const int16_t *apu_yellow_ring(uint32_t *mask) { *mask = APU_STREAM_RING - 1; return y.ring; }

uint32_t apu_yellow_publish(void) { return y.progress; }

#endif
