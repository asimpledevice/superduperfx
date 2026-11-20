/* Scene builder test ROM. Each round: random VRAM (tilemaps and characters,
 * some of them empty), synced into the tile cache; then random background
 * passes -- depth, 8x8 or 16x16 tiles, scroll, tilemap base and size,
 * character base, rows, span, priority -- drawn by the RSP's SceneCmd_Bg
 * through the RDP into a cleared framebuffer, and by the C reference
 * (n64_scene_bg's quads, texels from tc_ref_tile) on the CPU. The two
 * pictures must be equal, pixel for pixel. Results on screen and in the
 * debug log. The palette gives every colour index its own colour, so a
 * wrong tile, row, flip or palette shows. */
#include <libdragon.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "tile_cache.h"
#include "scene_rsp.h"
#include "n64_ppu_scene.h"

#ifndef ROUNDS
#define ROUNDS 12
#endif
#ifndef PASSES
#define PASSES 40
#endif

static uint32_t s_rng = 0x2468aceu;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* VRAM at 4 mod 8, like the PPU's array. */
static uint8_t s_vram_base[0x10000 + 32] __attribute__((aligned(16)));
static uint16_t *const s_vram = (uint16_t *)(s_vram_base + 4);
static uint16_t s_tlut[256] __attribute__((aligned(16)));
static uint16_t s_want[224][256];
enum { kOx = 32, kOy = 8 };
static const uint16_t kBg = (0 << 11) | (0 << 6) | (12 << 1) | 1;

static char s_text[16][48];
static int s_ntext;
static void say(const char *t) {
  debugf("[scenetest] %s\n", t);
  if (s_ntext < 16) strncpy(s_text[s_ntext++], t, 47);
}

/* Entry k's colour: all different, none the background; index 0 of each
 * 16 is transparent (alpha 0), as the tile cache's TLUTs have it. */
static uint16_t entry(int k) { return k % 16 ? (uint16_t)((k & 31) << 11 | (k >> 5) << 6 | 31 << 1 | 1) : 0; }

static void random_vram(void) {
  for (unsigned i = 0; i < 0x8000; i++) s_vram[i] = (uint16_t)rnd();
  /* A good share of empty characters, in runs. */
  for (int k = 0; k < 96; k++) {
    const unsigned a = (rnd() % 0x7e0) * 16, n = 16 * (1 + rnd() % 12);
    for (unsigned i = 0; i < n; i++) s_vram[(a + i) & 0x7fff] = 0;
  }
  data_cache_hit_writeback(s_vram_base, sizeof s_vram_base);
}

typedef struct { N64SceneBg bg; int r0, r1, prio, sx0, sx1; } Pass;

static void reference(const Pass *p) {
  static N64SceneQuad q[33 * 29];
  static uint8_t tile[TC_CI4_TILE];
  for (int y = 0; y < 224; y++) for (int x = 0; x < 256; x++) s_want[y][x] = kBg;
  const int n = n64_scene_bg(&p->bg, s_vram, p->r0, p->r1, q, (int)(sizeof q / sizeof q[0]));
  for (int i = 0; i < n; i++) {
    if (!(q[i].flags & N64SCENE_PRIO) != !p->prio) continue;
    tc_ref_tile(s_vram, p->bg.bpp, q[i].tile, tile);
    const int h = q[i].flags & N64SCENE_HFLIP, v = q[i].flags & N64SCENE_VFLIP;
    for (int r = q[i].r0; r < q[i].r1; r++) {
      const int row = q[i].row + (r - q[i].r0), t = v ? 7 - row : row;
      for (int x = q[i].x0; x < q[i].x0 + 8; x++) {
        if (x < p->sx0 || x >= p->sx1 || x < 0 || x > 255) continue;
#ifdef SCENETEST_BREAK   /* the check must be able to fail: a wrong rule for 16x16 tiles' flipped halves */
        const int s = (h && !p->bg.big ? 8 : 0) + x - q[i].x0;
#else
        const int s = (h ? 8 : 0) + x - q[i].x0;
#endif
        const unsigned b = tile[8 * t + s / 2], texel = s & 1 ? b & 15 : b >> 4;
        if (texel) s_want[r][x] = entry(q[i].pal * 16 + (int)texel);
      }
    }
  }
}

int main(void) {
  debug_init_isviewer();
  debug_init_usblog();
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  rdpq_init();
  n64_tile_cache_init(TC_FMT_2BPP | TC_FMT_4BPP);
  n64_scene_rsp_init();
  for (int k = 0; k < 256; k++) s_tlut[k] = entry(k);
  data_cache_hit_writeback(s_tlut, sizeof s_tlut);

  int bad_passes = 0, total = 0, big_passes = 0;
  char t[64];
  for (int round = 0; round < ROUNDS; round++) {
    random_vram();
    n64_tile_cache_sync(s_vram);
    rspq_wait();
    for (int k = 0; k < PASSES; k++) {
      Pass p;
      memset(&p, 0, sizeof p);
      p.bg.bpp = rnd() & 1 ? 4 : 2;
      p.bg.big = (rnd() & 3) == 0 ? 0 : (uint8_t)(rnd() & 1);
      p.bg.hs = (uint16_t)(rnd() & 0x3ff);
      p.bg.vs = (uint16_t)(rnd() & 0x3ff);
      p.bg.xsc = (uint8_t)rnd();
      p.bg.tileadr = (uint16_t)((rnd() & 7) << 12);
      p.r0 = (int)(rnd() % 224);
      p.r1 = p.r0 + 1 + (int)(rnd() % (unsigned)(224 - p.r0));
      if ((rnd() & 3) == 0) { p.r0 = 0; p.r1 = 224; }
      p.sx0 = (int)(rnd() % 256);
      p.sx1 = p.sx0 + 1 + (int)(rnd() % (unsigned)(256 - p.sx0));
      if (rnd() & 1) { p.sx0 = 0; p.sx1 = 256; }
      p.prio = (int)(rnd() & 1);
      big_passes += p.bg.big;

      surface_t *fb = display_get();
      rdpq_attach(fb, NULL);
      rdpq_set_mode_fill(color_from_packed16(kBg));
      rdpq_fill_rectangle(0, 0, 320, 240);
      rdpq_set_mode_copy(true);
      rdpq_mode_tlut(TLUT_RGBA16);
      rdpq_tex_upload_tlut(s_tlut, 0, 256);
      const uint8_t *cache = p.bg.bpp == 4 ? n64_tile_cache_c4() : n64_tile_cache_c2();
      const uint8_t *empty = p.bg.bpp == 4 ? n64_tile_cache_empty4() : n64_tile_cache_empty2();
      for (int c0 = p.r0; c0 < p.r1; c0 += 32)
        n64_scene_rsp_bg(s_vram, cache, empty, p.bg.hs, p.bg.vs, p.bg.xsc, p.bg.bpp, p.bg.big, p.bg.tileadr,
                         c0, c0 + 32 < p.r1 ? c0 + 32 : p.r1, p.prio, p.sx0, p.sx1, kOx, kOy);
      rdpq_detach_wait();

      reference(&p);
      const uint16_t *px = UncachedAddr(fb->buffer);
      const int stride = fb->stride / 2;
      int bad = 0, fx = 0, fy = 0;
      for (int y = 0; y < 224; y++)
        for (int x = 0; x < 256; x++)
          if ((px[(kOy + y) * stride + kOx + x] | 1) != (s_want[y][x] | 1)) {
            if (!bad) { fx = x; fy = y; }
            bad++;
          }
      total++;
      if (bad) {
        if (bad_passes < 4) {
          snprintf(t, sizeof t, "r%d p%d: %d px, first %d,%d", round, k, bad, fx, fy);
          say(t);
          snprintf(t, sizeof t, " %dbpp big%d hs%x vs%x sc%02x ta%x", p.bg.bpp, p.bg.big, p.bg.hs, p.bg.vs, p.bg.xsc, p.bg.tileadr);
          say(t);
          snprintf(t, sizeof t, " rows %d-%d span %d-%d prio %d", p.r0, p.r1, p.sx0, p.sx1, p.prio);
          say(t);
        }
        bad_passes++;
      }
      display_show(fb);
    }
  }
  snprintf(t, sizeof t, "%d passes (%d with 16x16 tiles)", total, big_passes);
  say(t);
  snprintf(t, sizeof t, "%d differ from the reference", bad_passes);
  say(t);
  say(bad_passes ? "FAIL" : "PASS");
  debugf("SCENETEST_DONE\n");

  rspq_wait();
  surface_t *fb = display_get();
  graphics_fill_screen(fb, 0);
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  for (int i = 0; i < s_ntext; i++) graphics_draw_text(fb, 8, 16 + 12 * i, s_text[i]);
  display_show(fb);
  for (;;) {}
}
