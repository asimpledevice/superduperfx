/* RSP S-DSP test: random states through the C reference and the RSP. */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>

#include "snesdsp_rsp.h"
#include "dsp.h"

static uint32_t s_rng = 0x1234567u;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}

/* Milestone 1 reference: dsp_handleNoise from dsp.c, verbatim. */
static void ref_noise(Dsp *dsp) {
  if(dsp->noiseRate != 0) {
    dsp->noiseCounter++;
  }
  if(dsp->noiseCounter >= dsp->noiseRate && dsp->noiseRate != 0) {
    int bit = (dsp->noiseSample & 1) ^ ((dsp->noiseSample >> 1) & 1);
    dsp->noiseSample = ((dsp->noiseSample >> 1) & 0x3fff) | (bit << 14);
    dsp->noiseSample = ((int16_t) ((dsp->noiseSample & 0x7fff) << 1)) >> 1;
    dsp->noiseCounter = 0;
  }
}

int main(void) {
  debug_init_isviewer();
  snesdsp_rsp_init();
  static Dsp a, b;
  int fails = 0, tests = 2000;
  for (int t = 0; t < tests; t++) {
    memset(&a, 0, sizeof a);
    a.noiseSample = (int16_t)rnd();
    a.noiseRate = (rnd() & 3) ? (uint16_t)(rnd() % 40) : 0;
    a.noiseCounter = (uint16_t)(a.noiseRate ? rnd() % (a.noiseRate + 2) : rnd());
    b = a;
    uint32_t n = 1 + rnd() % 300;
    for (uint32_t i = 0; i < n; i++) ref_noise(&a);
    snesdsp_rsp_run(&b, n);
    if (a.noiseSample != b.noiseSample || a.noiseCounter != b.noiseCounter) {
      if (fails++ < 5)
        debugf("FAIL test %d n=%lu: ref ns=%04x nc=%u  rsp ns=%04x nc=%u\n", t, (unsigned long)n,
               (uint16_t)a.noiseSample, a.noiseCounter, (uint16_t)b.noiseSample, b.noiseCounter);
    }
  }
  debugf("RSPDSP_TEST %s: %d/%d tests\n", fails ? "FAIL" : "PASS", tests - fails, tests);
  for (;;) {}
}
