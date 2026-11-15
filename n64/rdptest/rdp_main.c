/* RDP facts test ROM (docs/n64/PPU_DESIGN.md, work plan step 0).
 *
 * Page 1: 16x8 textures (RGBA16, CI8, CI4 with palette 0 and palette 3)
 * drawn with texture rectangles in copy mode and in 1-cycle mode, unflipped
 * and flipped horizontally (negative DsDx), vertically (negative DtDy) and
 * both. TLUT entries 16n are transparent (alpha 0, alpha compare). Each rectangle is read
 * back from the framebuffer and compared with the expected pixels; a
 * failing case prints the texel columns it actually showed in its first
 * row. Results on screen and in the debug log.
 * Page 2 (press A): a 256x240 display with a border and grid, to see how
 * the VI scales a 256-wide framebuffer on the TV. A again goes back. */
#include <libdragon.h>
#include <stdio.h>
#include <string.h>

enum { TW = 16, TH = 8 };
enum { K_RGBA, K_CI8, K_CI4P0, K_CI4P3, K_N };
static const char *const kFmtName[K_N] = {"RGBA16", "CI8   ", "CI4 p0", "CI4 p3"};
static const char *const kFlipName[4] = {"none", "H", "V", "HV"};

static uint16_t s_tlut[256] __attribute__((aligned(16)));
static uint16_t s_rgba[TW * TH] __attribute__((aligned(16)));
static uint8_t s_ci8[TW * TH] __attribute__((aligned(16)));
static uint8_t s_ci4[TW * TH / 2] __attribute__((aligned(16)));
static uint8_t s_idx[K_N][TH][TW];   /* the TLUT entry each texel shows (0: transparent) */
static const uint16_t kBg = (0 << 11) | (31 << 6) | (0 << 1) | 1;   /* green: no palette entry has g > 7 */

/* TLUT entry k. The RDP's transparency is the entry's alpha, not the index:
 * entries 0, 16, 32, ... (colour 0 of each 16-colour palette, the SNES's
 * transparent colour for 4bpp tiles) have alpha 0, and the CI8 texture
 * avoids them. */
static uint16_t entry(int k) { return (uint16_t)(((k & 31) << 11) | ((k >> 5) << 6) | (17 << 1) | (k % 16 != 0)); }
static int decode(uint16_t px) {   /* framebuffer pixel back to a TLUT entry; -1 background, -2 unknown */
  if ((px & 0xfffe) == (kBg & 0xfffe)) return -1;
  for (int k = 1; k < 256; k++) if ((entry(k) & 0xfffe) == (px & 0xfffe)) return k;
  return -2;
}

static void make_textures(void) {
  for (int k = 0; k < 256; k++) s_tlut[k] = entry(k);
  memset(s_ci4, 0, sizeof s_ci4);
  for (int y = 0; y < TH; y++)
    for (int x = 0; x < TW; x++) {
      int i8 = (x + 2 * y) % 7 == 0 ? 0 : 1 + (y * TW + x) % 255;
      if (i8 % 16 == 0 && i8) i8++;
      s_ci8[y * TW + x] = (uint8_t)i8;
      s_rgba[y * TW + x] = i8 ? entry(i8) : 0;
      s_idx[K_RGBA][y][x] = s_idx[K_CI8][y][x] = (uint8_t)i8;
      int i4 = (x * 3 + y * 5) & 15;
      s_ci4[(y * TW + x) / 2] |= (uint8_t)(x & 1 ? i4 : i4 << 4);
      s_idx[K_CI4P0][y][x] = (uint8_t)i4;
      s_idx[K_CI4P3][y][x] = (uint8_t)(i4 ? 48 + i4 : 0);
    }
  data_cache_hit_writeback(s_tlut, sizeof s_tlut);
  data_cache_hit_writeback(s_rgba, sizeof s_rgba);
  data_cache_hit_writeback(s_ci8, sizeof s_ci8);
  data_cache_hit_writeback(s_ci4, sizeof s_ci4);
}

static void upload(int f) {
  surface_t s;
  switch (f) {
  case K_RGBA: s = surface_make(s_rgba, FMT_RGBA16, TW, TH, TW * 2); break;
  case K_CI8: s = surface_make(s_ci8, FMT_CI8, TW, TH, TW); break;
  default: s = surface_make(s_ci4, FMT_CI4, TW, TH, TW / 2); break;
  }
  rdpq_tex_upload(TILE0, &s, &(rdpq_texparms_t){.palette = f == K_CI4P3 ? 3 : 0});
}

static void set_mode(int copy, int f) {
  if (copy) rdpq_set_mode_copy(true);
  else {
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_mode_alphacompare(1);
    rdpq_mode_dithering(DITHER_NONE_NONE);
  }
  rdpq_mode_tlut(f == K_RGBA ? TLUT_NONE : TLUT_RGBA16);
}

static void draw(int x, int y, int flip) {
  const int h = flip & 1, v = flip & 2;
  __rdpq_texture_rectangle_raw_fx(TILE0, x * 4, y * 4, (x + TW) * 4, (y + TH) * 4,
                                  h ? (TW - 1) * 32 : 0, v ? (TH - 1) * 32 : 0,
                                  h ? -1024 : 1024, v ? -1024 : 1024);
}

static int case_x(int copy, int flip) { return 8 + (copy * 4 + flip) * 24; }
static int case_y(int f) { return 182 + f * 10; }

static char s_lines[24][48];
static int s_nlines;
static void line(const char *t) {
  if (s_nlines < 24) snprintf(s_lines[s_nlines++], 48, "%s", t);
  debugf("[rdptest] %s\n", t);
}

static void page_flips(void) {
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  surface_t *fb = display_get();
  rdpq_attach(fb, NULL);
  rdpq_set_mode_fill(color_from_packed16(kBg));
  rdpq_fill_rectangle(0, 0, 320, 240);
  for (int f = 0; f < K_N; f++) {
    for (int copy = 0; copy < 2; copy++) {
      set_mode(copy, f);
      rdpq_tex_upload_tlut(s_tlut, 0, 256);
      upload(f);
      for (int flip = 0; flip < 4; flip++) draw(case_x(copy, flip), case_y(f), flip);
    }
  }
  rdpq_detach_wait();
  const uint16_t *px = UncachedAddr(fb->buffer);
  const int stride = fb->stride / 2;
  s_nlines = 0;
  int fails = 0, ndetail = 0;
  for (int copy = 1; copy >= 0; copy--)
    for (int f = 0; f < K_N; f++) {
      char t[48];
      int o = snprintf(t, sizeof t, "%s %s:", copy ? "copy" : "1cyc", kFmtName[f]);
      for (int flip = 0; flip < 4; flip++) {
        int bad = 0;
        for (int y = 0; y < TH; y++)
          for (int x = 0; x < TW; x++) {
            const int sx = flip & 1 ? TW - 1 - x : x, sy = flip & 2 ? TH - 1 - y : y;
            const int want = s_idx[f][sy][sx] ? s_idx[f][sy][sx] : -1;
            const int got = decode(px[(case_y(f) + y) * stride + case_x(copy, flip) + x]);
            if (got != want) bad++;
          }
        if (bad) fails++;
        o += snprintf(t + o, sizeof t - o, " %s%s", kFlipName[flip], bad ? "X" : "ok");
        if (bad) {   /* which source columns the first row showed */
          char d[64];
          int q = snprintf(d, sizeof d, "  %s %s %s:", copy ? "cpy" : "1cy", kFmtName[f], kFlipName[flip]);
          const int sy = flip & 2 ? TH - 1 : 0;
          for (int x = 0; x < TW && q < 60; x++) {
            const int got = decode(px[case_y(f) * stride + case_x(copy, flip) + x]);
            int col = -1;
            for (int c = 0; c < TW; c++)
              if (s_idx[f][sy][c] && s_idx[f][sy][c] == got) { col = c; break; }
            q += snprintf(d + q, sizeof d - q, col < 0 ? "." : "%x", col);
          }
          debugf("[rdptest] %s\n", d);
          if (ndetail++ < 8) snprintf(s_lines[s_nlines++], 48, "%s", d + 2);   /* the log has all */
        }
      }
      line(t);
    }
  {
    char t[48];
    snprintf(t, sizeof t, "%d of 32 differ. A: 256x240 page", fails);
    line(t);
  }
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  for (int i = 0; i < s_nlines; i++) graphics_draw_text(fb, 8, 6 + i * 10, s_lines[i]);
  graphics_draw_text(fb, 8, 224, "Left 4: 1-cycle. Right 4: copy");
  display_show(fb);
}

static void page_256(void) {
  display_init(RESOLUTION_256x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  surface_t *fb = display_get();
  graphics_fill_screen(fb, graphics_make_color(0, 0, 64, 255));
  const uint32_t white = graphics_make_color(255, 255, 255, 255);
  const uint32_t red = graphics_make_color(255, 0, 0, 255);
  for (int x = 0; x < 256; x += 32) graphics_draw_box(fb, x, 0, 1, 240, white);
  for (int y = 0; y < 240; y += 32) graphics_draw_box(fb, 0, y, 256, 1, white);
  graphics_draw_box(fb, 0, 0, 256, 2, red);
  graphics_draw_box(fb, 0, 238, 256, 2, red);
  graphics_draw_box(fb, 0, 0, 2, 240, red);
  graphics_draw_box(fb, 254, 0, 2, 240, red);
  graphics_draw_box(fb, 0, 8, 256, 1, red);     /* the SNES's 224 lines: 8..231 */
  graphics_draw_box(fb, 0, 231, 256, 1, red);
  graphics_set_color(white, 0);
  graphics_draw_text(fb, 40, 100, "256x240 framebuffer");
  graphics_draw_text(fb, 40, 112, "Red: the edges and lines 8/231");
  graphics_draw_text(fb, 40, 124, "Squares: 32x32 pixels");
  graphics_draw_text(fb, 40, 148, "A: back to the flip tests");
  display_show(fb);
}

int main(void) {
  debug_init_isviewer();
  joypad_init();
  rdpq_init();
  make_textures();
  int page = 0;
  page_flips();
  for (;;) {
    joypad_poll();
    if (joypad_get_buttons_pressed(JOYPAD_PORT_1).a) {
      rspq_wait();
      display_close();
      page ^= 1;
      if (page) page_256(); else page_flips();
    }
  }
}
