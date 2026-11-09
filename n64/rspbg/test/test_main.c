/* RSP BG test: random VRAM and random jobs through rsp_bg.S, each row
 * compared with a CPU expansion (the rules of flush_run in
 * n64/src/n64_ppu_build.c). Reports over the debug log. */
#include <libdragon.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "bg_rsp.h"

enum { kStride = 272, kPad = 8, kLines = 224 };

static uint32_t s_rng = 0x1234567u;
static uint32_t rnd(void) {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}

static void cpu_row(const uint16_t *vram, uint8_t *row, unsigned y, unsigned hs,
                    unsigned tileadr, unsigned xsc, int bpp, unsigned pal_base, uint8_t *flags) {
  const bool wider = xsc & 1, higher = xsc & 2;
  int sc = ((xsc & 0xfc) << 8) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && higher) sc += wider ? 0x800 : 0x400;
  *flags = 0;
  for (int t = 0; t < 33; ++t) {
    const unsigned mx = (hs >> 3) + (unsigned)t;
    const int map = ((mx >> 5) & 1) ? sc + (wider ? 0x400 : 0) : sc;
    const uint16_t entry = vram[(map & 0x7fff) + (mx & 31)];
    const unsigned yy = y & 7, r = (entry & 0x8000) ? 7 - yy : yy;
    const bool hi = entry & 0x2000;
    const int base = tileadr + (entry & 0x3ff) * (bpp == 4 ? 16 : 8);
    const uint16_t w0 = vram[(base + r) & 0x7fff], w1 = bpp == 4 ? vram[(base + r + 8) & 0x7fff] : 0;
    if (w0 | w1) *flags |= hi ? 2 : 1;
    const uint8_t pal = (uint8_t)((hi ? 128 : 0) + pal_base + ((entry >> 10) & 7) * (bpp == 4 ? 16 : 4));
    for (int j = 0; j < 8; ++j) {
      const int b = (entry & 0x4000) ? j : 7 - j;
      const unsigned c = ((w0 >> b) & 1) | ((w0 >> (b + 8)) & 1) << 1 |
                         ((w1 >> b) & 1) << 2 | ((w1 >> (b + 8)) & 1) << 3;
      const int pos = kPad - (int)(hs & 7) + t * 8 + j;
      if (pos >= kPad && pos < kPad + 256) row[pos] = (uint8_t)(c | pal);
    }
  }
}

int main(void) {
  debug_init_isviewer();
  debug_init_usblog();
  rspq_init();
  n64_bgrsp_init();
  /* Like the PPU's array: 2-aligned, 4 mod 8 (DMA drops the low 3 bits). */
  uint16_t *vram = (uint16_t *)((uint8_t *)memalign(16, 0x10000 + 16) + 4);
  uint8_t *plane = memalign(16, kStride * kLines);
  static uint8_t want[kStride];
  for (int i = 0; i < 0x8000; ++i) vram[i] = (uint16_t)rnd();
  int bad_rows = 0, rows = 0, bad_flags = 0;
  const int tests = 300;
  for (int t = 0; t < tests; ++t) {
    if (t % 50 == 0) for (int i = 0; i < 0x8000; ++i) vram[i] = (rnd() & 3) ? (uint16_t)rnd() : 0;
    memset(plane, 0x5a, kStride * kLines);
    n64_bgrsp_cpu_wrote(plane, kStride * kLines);
    n64_bgrsp_frame_begin();
    n64_bgrsp_sync_vram(vram, (uint32_t)t + 1);
    int r0 = (int)(rnd() % kLines);
    int n = 1 + (int)(rnd() % (kLines - r0));
    unsigned vs = rnd() & 0x3ff, hs = rnd() & 0x3ff, tileadr = (rnd() & 7) << 12;
    unsigned xsc = rnd() & 0xff;
    int bpp = (rnd() & 1) ? 4 : 2;
    unsigned pal_base = (bpp == 2 && (rnd() & 1)) ? 32u * (rnd() & 3) : 0;
    if (t < 10) {   /* SMW title, SUB_BG3: whole frame, no scroll */
      r0 = 0; n = 224; vs = 0; hs = 0; xsc = 0x53; bpp = 2; tileadr = 0x4000; pal_base = 0;
    }
    const uint16_t y0 = (uint16_t)(r0 + 1 + vs);
    const int job = n64_bgrsp_submit(vram, plane + (size_t)r0 * kStride + kPad, n, y0, (uint16_t)hs,
                                     (uint16_t)tileadr, (uint8_t)xsc, (uint8_t)bpp, (uint8_t)pal_base);
    n64_bgrsp_wait();
    const uint8_t *fl = n64_bgrsp_flags(job);
    const uint8_t *got = (const uint8_t *)UncachedAddr(plane);
    for (int i = 0; i < n; ++i) {
      uint8_t f;
      memset(want, 0, sizeof want);
      cpu_row(vram, want, (unsigned)(y0 + i), hs, tileadr, xsc, bpp, pal_base, &f);
      const uint8_t *g = got + (size_t)(r0 + i) * kStride;
      rows++;
      if (memcmp(g + kPad, want + kPad, 256)) {
        if (bad_rows++ < 10) {
          int x = 0;
          while (g[kPad + x] == want[kPad + x]) x++;
          debugf("BAD test %d row %d (i %d of %d) x %d: rsp %02x cpu %02x | bpp %d hs %u y %u xsc %02x tile %04x\n",
                 t, r0 + i, i, n, x, g[kPad + x], want[kPad + x], bpp, hs, (unsigned)(y0 + i), xsc, tileadr);
        }
      }
      if (fl[i] != f && bad_flags++ < 5)
        debugf("BADFLAG test %d row %d: rsp %d cpu %d\n", t, r0 + i, fl[i], f);
      if (g[0] != 0x5a || g[kStride - 1] != 0x5a) {
        if (bad_rows++ < 10) debugf("MARGIN test %d row %d: %02x %02x\n", t, r0 + i, g[0], g[kStride - 1]);
      }
    }
    if (r0 > 0 && got[(size_t)(r0 - 1) * kStride + kPad] != 0x5a && bad_rows++ < 10)
      debugf("OVERRUN before test %d\n", t);
    if (r0 + n < kLines && got[(size_t)(r0 + n) * kStride + kPad] != 0x5a && bad_rows++ < 10)
      debugf("OVERRUN after test %d\n", t);
  }
  debugf("RSPBG_TEST rows %d bad %d badflags %d\n", rows, bad_rows, bad_flags);
  /* Timing: whole-frame jobs (224 rows) per configuration. */
  static const struct { int bpp; unsigned hs; const char *name; } cfg[] = {
    {4, 0, "4bpp"}, {4, 3, "4bpp fine"}, {2, 0, "2bpp"}, {2, 5, "2bpp fine"}};
  for (int c = 0; c < 4; ++c) {
    uint32_t best = ~0u;
    for (int rep = 0; rep < 4; ++rep) {
      n64_bgrsp_frame_begin();
      n64_bgrsp_sync_vram(vram, 1000u + (uint32_t)c);
      const uint32_t t0 = get_ticks();
      n64_bgrsp_submit(vram, plane + kPad, 224, 1, (uint16_t)cfg[c].hs, 0x2000, 0x53,
                       (uint8_t)cfg[c].bpp, 0);
      n64_bgrsp_wait();
      const uint32_t dt = get_ticks() - t0;
      if (dt < best) best = dt;
    }
    debugf("RSPBG_TIME %-10s %lu us per 224 rows\n", cfg[c].name, (unsigned long)TICKS_TO_US(best));
  }
  for (;;) {}
}
