/* Tile cache test ROM: random VRAM through the RSP's tile sync, compared
 * tile by tile with the C reference, then random changes (single bytes,
 * runs, whole blocks) and syncs again; random CGRAM and brightness through
 * every TLUT arrangement. Also times a full conversion and a sync with no
 * changes. Results on screen and in the debug log. */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>

#include "tile_cache.h"

#ifndef ROUNDS
#define ROUNDS 40
#endif

static uint32_t s_rng = 0x13579bdu;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* VRAM at 4 mod 8, like the PPU's array. */
static uint8_t s_vram_base[0x10000 + 32] __attribute__((aligned(16)));
static uint16_t *const s_vram = (uint16_t *)(s_vram_base + 4);
static uint16_t s_cgram_base[256 + 8] __attribute__((aligned(16)));
static uint16_t s_tlut[256] __attribute__((aligned(16)));
static uint16_t s_tlut_ref[256];

static char s_text[16][48];
static int s_ntext;
static void say(const char *t) {
  debugf("[tiletest] %s\n", t);
  if (s_ntext < 16) strncpy(s_text[s_ntext++], t, 47);
}

/* Compare every tile of every cache with the reference; returns bad tiles. */
static int check(int *first_bpp, unsigned *first_tile) {
  static uint8_t ref[TC_CI8_TILE];
  int bad = 0;
  const struct { int bpp; const uint8_t *c; unsigned n, sz; } k[3] = {
    {2, n64_tile_cache_c2(), TC_C2_TILES, TC_CI4_TILE},
    {4, n64_tile_cache_c4(), TC_C4_TILES, TC_CI4_TILE},
    {8, n64_tile_cache_c8(), TC_C8_TILES, TC_CI8_TILE},
  };
  for (int f = 0; f < 3; f++) {
    const uint8_t *c = UncachedAddr(k[f].c);
    for (unsigned t = 0; t < k[f].n; t++) {
      tc_ref_tile(s_vram, k[f].bpp, t, ref);
      if (memcmp(ref, c + (size_t)t * k[f].sz, k[f].sz)) {
        if (!bad) { *first_bpp = k[f].bpp; *first_tile = t; }
        bad++;
      }
    }
  }
  return bad;
}

static void mutate(void) {
  const unsigned kind = rnd() % 4, n = 1 + rnd() % 24;
  for (unsigned i = 0; i < n; i++) {
    const unsigned a = rnd() & 0x7fff;
    switch (kind) {
    case 0: s_vram[a] ^= (uint16_t)(1u << (rnd() % 16)); break;          /* one bit */
    case 1: for (unsigned j = 0; j < 16; j++) s_vram[(a + j) & 0x7fff] = (uint16_t)rnd(); break;
    case 2: for (unsigned j = 0; j < 128; j++) s_vram[((a & ~127u) + j) & 0x7fff] = (uint16_t)rnd(); break;
    default: s_vram[a] = (uint16_t)rnd(); break;
    }
  }
}

int main(void) {
  debug_init_isviewer();
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  rspq_init();
  n64_tile_cache_init(TC_FMT_2BPP | TC_FMT_4BPP | TC_FMT_8BPP);
  char t[64];
  int fails = 0;

  /* Full conversion. */
  for (unsigned i = 0; i < 0x8000; i++) s_vram[i] = (uint16_t)rnd();
  uint32_t t0 = TICKS_READ();
  volatile uint32_t *res = n64_tile_cache_sync(s_vram);
  rspq_wait();
  uint32_t full_us = TICKS_TO_US(TICKS_READ() - t0);
  int fb = 0; unsigned ft = 0;
  int bad = check(&fb, &ft);
  snprintf(t, sizeof t, "full: %lu groups, %lu us, %d bad", (unsigned long)*res, (unsigned long)full_us, bad);
  say(t);
  if (bad) { fails++; snprintf(t, sizeof t, "  first: %dbpp tile %u", fb, ft); say(t); }

  /* No change. */
  t0 = TICKS_READ();
  res = n64_tile_cache_sync(s_vram);
  rspq_wait();
  snprintf(t, sizeof t, "unchanged: %lu groups, %lu us", (unsigned long)*res,
           (unsigned long)TICKS_TO_US(TICKS_READ() - t0));
  say(t);

  /* Random changes. */
  int rbad = 0;
  uint32_t groups = 0, us = 0;
  for (int r = 0; r < ROUNDS; r++) {
    mutate();
    t0 = TICKS_READ();
    res = n64_tile_cache_sync(s_vram);
    rspq_wait();
    us += TICKS_TO_US(TICKS_READ() - t0);
    groups += *res;
    bad = check(&fb, &ft);
    if (bad) {
      if (!rbad) { snprintf(t, sizeof t, "  round %d: %d bad, %dbpp tile %u", r, bad, fb, ft); say(t); }
      rbad++;
    }
  }
  if (rbad) fails++;
  snprintf(t, sizeof t, "%d change rounds: %d bad, %lu groups, %lu us", ROUNDS, rbad,
           (unsigned long)groups, (unsigned long)us);
  say(t);

  /* TLUTs. */
  int tbad = 0;
  for (int r = 0; r < 300; r++) {
    uint16_t *cg = (uint16_t *)((uint8_t *)s_cgram_base + 2 * (rnd() % 4));   /* any even alignment */
    for (int i = 0; i < 256; i++) cg[i] = (uint16_t)rnd();
    const unsigned l = rnd() % 16, kind = rnd() % 3;
    const unsigned lo = 32 * (rnd() % 4), hi = rnd() & 1 ? 0x100 : 32 * (rnd() % 4);
    n64_tile_cache_tlut(cg, l, kind, lo, hi, s_tlut);
    rspq_wait();
    tc_ref_tlut(cg, l, kind, lo, hi, s_tlut_ref);
    const uint16_t *got = UncachedAddr(s_tlut);
    for (int i = 0; i < 256; i++)
      if (got[i] != s_tlut_ref[i]) {
        if (!tbad) {
          snprintf(t, sizeof t, "  tlut kind %u l%u entry %d: %04x want %04x", kind, l, i, got[i], s_tlut_ref[i]);
          say(t);
        }
        tbad++;
        break;
      }
  }
  if (tbad) fails++;
  snprintf(t, sizeof t, "TLUTs: 300 tested, %d bad", tbad);
  say(t);
  say(fails ? "FAIL" : "PASS");

  surface_t *fb_s = display_get();
  graphics_fill_screen(fb_s, 0);
  for (int i = 0; i < s_ntext; i++) graphics_draw_text(fb_s, 16, 24 + 12 * i, s_text[i]);
  display_show(fb_s);
  for (;;) {}
}
