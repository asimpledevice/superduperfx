/* S-DSP front end (runner/src/snes/dsp_frontend.c) against the C S-DSP
 * (dsp_cycle): random states and random logs of register and ARAM writes,
 * stepped together. After every sample, for every tracked voice: pitch
 * counter, BRR offset, previous flags, key-on latch; ENDX; and every block
 * the front end reports must be the one dsp_cycle decoded (its start, and
 * the 9 bytes in ARAM at that sample). Exits non-zero on any difference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "dsp_frontend.h"

int snes_frame_counter;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static uint8_t s_aram[0x10000];
static struct { int voice; uint32_t sample; uint16_t start; uint8_t b[9]; } s_blk[64];
static int s_nblk;
static void on_block(void *ctx, int voice, uint32_t sample, uint16_t start, const uint8_t b[9]) {
  (void)ctx;
  if (s_nblk < 64) {
    s_blk[s_nblk].voice = voice; s_blk[s_nblk].sample = sample; s_blk[s_nblk].start = start;
    memcpy(s_blk[s_nblk].b, b, 9);
  }
  s_nblk++;
}

static void random_state(Dsp *d) {
  memset(d, 0, sizeof *d);
  d->apu_ram = s_aram;
  dsp_reset(d);
  d->apu_ram = s_aram;
  for (int r = 0; r < 0x80; r++) {
    uint8_t v = (uint8_t)rnd();
    if (r == 0x2d) v = (rnd() % 8 == 0) ? v : 0;       /* pitch modulation sometimes */
    if (r == 0x4c || r == 0x5c) v &= (uint8_t)rnd();
    dsp_write(d, (uint8_t)r, v);
  }
  for (int c = 0; c < 8; c++) {
    DspChannel *ch = &d->channel[c];
    ch->pitchCounter = (uint16_t)rnd();
    ch->decodeOffset = (uint16_t)rnd();
    ch->previousFlags = rnd() & 3;
    if (rnd() % 4 == 0) ch->pitch = (uint16_t)(rnd() & 0x3fff);        /* high pitches too */
    if (rnd() % 16 == 0) ch->pitch = 0;
  }
  d->evenCycle = rnd() & 1;
}

int main(void) {
  static Dsp d;
  static DspFrontEnd fe;
  int fails = 0;
  uint32_t samples_total = 0, blocks_total = 0, events_total = 0, untracked_voices = 0;
  for (int t = 0; t < 3000 && fails < 5; t++) {
    for (int i = 0; i < 0x10000; i++) s_aram[i] = (uint8_t)rnd();
    random_state(&d);
    const uint32_t base = rnd() % 1000;
    dsp_fe_init(&fe, &d, s_aram, base, on_block, NULL);
    const uint32_t n = 64 + rnd() % 1024;
    for (uint32_t k = 0; k < n && fails < 5; k++) {
      const uint32_t s = base + k;
      /* Writes before sample s: registers (often the timing ones) and ARAM. */
      const int writes = rnd() % 8 == 0 ? 1 + rnd() % 4 : 0;
      for (int w = 0; w < writes; w++) {
        dsp_fe_advance(&fe, s);
        if (rnd() & 1) {
          static const uint8_t timing[] = {0x02, 0x03, 0x04, 0x4c, 0x5c, 0x7c, 0x5d, 0x2d};
          uint8_t r = timing[rnd() % sizeof timing];
          if (r < 0x0c) r = (uint8_t)(r | (rnd() % 8) << 4);
          uint8_t v = (uint8_t)rnd();
          if (r == 0x2d) v &= (rnd() % 4 == 0) ? 0xff : 0;
          if (r == 0x4c) v &= (uint8_t)rnd();
          dsp_write(&d, r, v);
          dsp_fe_write(&fe, r, v);
        } else {
          const uint16_t a = (uint16_t)rnd();
          s_aram[a] = (uint8_t)rnd();
        }
      }
      /* Expected decodes this sample (dsp_cycleChannel's rule). */
      bool dec[8];
      for (int c = 0; c < 8; c++) dec[c] = (uint32_t)d.channel[c].pitchCounter + d.channel[c].pitch > 0xffff;
      uint8_t kon_before[8];
      for (int c = 0; c < 8; c++) kon_before[c] = d.channel[c].keyOn;
      s_nblk = 0;
      dsp_fe_advance(&fe, s + 1);
      /* The bytes as dsp_cycle will read them: its decodes precede its echo
       * write within the sample, so compare before it runs. */
      for (int b = 0; b < s_nblk && b < 64; b++)
        for (int i = 0; i < 9; i++)
          if (s_blk[b].b[i] != s_aram[(uint16_t)(s_blk[b].start + i)]) {
            if (fails < 5) printf("test %d sample %u voice %d: block byte %d %02x, ARAM %02x\n", t, k,
                                  s_blk[b].voice, i, s_blk[b].b[i], s_aram[(uint16_t)(s_blk[b].start + i)]);
            fails++;
            break;
          }
      dsp_cycle(&d);
      samples_total++;
      /* Compare. */
      for (int c = 0; c < 8; c++) {
        if (!dsp_fe_tracked(&fe, c)) continue;
        const DspChannel *ch = &d.channel[c];
        const DspFeVoice *v = &fe.v[c];
        const uint16_t cnt = dsp_fe_counter(&fe, c);
        if (cnt != ch->pitchCounter || v->offset != ch->decodeOffset ||
            v->prev_flags != ch->previousFlags || v->key_on != ch->keyOn) {
          if (fails < 5)
            printf("test %d sample %u voice %d: counter %04x/%04x offset %04x/%04x flags %u/%u kon %d/%d\n",
                   t, k, c, cnt, ch->pitchCounter, v->offset, ch->decodeOffset, v->prev_flags,
                   ch->previousFlags, v->key_on, ch->keyOn);
          fails++;
        }
      }
      /* ENDX bits of untracked (PMON) voices are the S-DSP's to report. */
      if ((fe.endx ^ d.ram[0x7c]) & (uint8_t)~fe.untracked) {
        if (fails < 5) printf("test %d sample %u: ENDX %02x/%02x\n", t, k, fe.endx, d.ram[0x7c]);
        fails++;
      }
      /* Blocks: one per tracked decoding voice, at the start dsp_cycle used. */
      int want = 0;
      for (int c = 0; c < 8; c++) want += dec[c] && dsp_fe_tracked(&fe, c);
      int got = 0;
      for (int b = 0; b < s_nblk && b < 64; b++) {
        if (!dsp_fe_tracked(&fe, s_blk[b].voice)) continue;
        got++;
        const int c = s_blk[b].voice;
        /* dsp_cycle's start: the offset after its decode, less 9, unless a
         * key on replaced it in the same sample. */
        if (s_blk[b].sample != s || !dec[c]) {
          if (fails < 5) printf("test %d sample %u voice %d: block at sample %u, dsp decoded %d\n", t, k, c,
                                s_blk[b].sample - base, dec[c]);
          fails++;
          continue;
        }
        if (!(kon_before[c] && !d.channel[c].keyOn) &&
            (uint16_t)(d.channel[c].decodeOffset - 9) != s_blk[b].start) {
          if (fails < 5) printf("test %d sample %u voice %d: block start %04x, dsp %04x\n", t, k, c,
                                s_blk[b].start, (uint16_t)(d.channel[c].decodeOffset - 9));
          fails++;
        }

      }
      if (got != want) {
        if (fails < 5) printf("test %d sample %u: %d blocks, dsp decoded %d\n", t, k, got, want);
        fails++;
      }
      blocks_total += (uint32_t)got;
    }
    events_total += fe.events;
    for (int c = 0; c < 8; c++) untracked_voices += !dsp_fe_tracked(&fe, c);
  }
  printf("dsp front end: %u samples, %u blocks, work on %u samples (%.1f%%), %u voice-runs untracked (PMON)\n",
         samples_total, blocks_total, events_total, 100.0 * events_total / samples_total, untracked_voices);
  printf("%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
