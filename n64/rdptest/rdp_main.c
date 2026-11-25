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
 * the VI scales a 256-wide framebuffer on the TV.
 * Page 3: the scissor's left edge in copy mode. Page 4 and 5: colour maths
 * in the blender and the combiner against the SNES's results. Page 6: one
 * layer drawn three ways, timed by the RDP's counters. Page 7: what a CPU
 * cache miss costs, alone and while the RDP draws, the RSP copies by DMA or
 * the cartridge is read (the RCP shares one channel to RDRAM between them).
 * A goes to the next. */
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
    snprintf(t, sizeof t, "%d of 32 differ. A: next page", fails);
    line(t);
  }
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  for (int i = 0; i < s_nlines; i++) graphics_draw_text(fb, 8, 6 + i * 10, s_lines[i]);
  graphics_draw_text(fb, 8, 224, "Left 4: 1-cycle. Right 4: copy");
  display_show(fb);
}

static void page_256(void) {
  display_init(RESOLUTION_256x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);   /* (libdragon refuses FILTERS_DISABLED below 320 wide: "not supported by the hardware") */
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
  graphics_draw_text(fb, 40, 100, "256x240 framebuffer");   /* (8-pixel font: 27 characters fit from x = 40) */
  graphics_draw_text(fb, 40, 112, "Red: edges, lines 8/231");
  graphics_draw_text(fb, 40, 124, "Squares: 32x32 pixels");
  graphics_draw_text(fb, 40, 148, "A: next page (scissor)");
  display_show(fb);
}

/* ---- Page 3: the scissor's left edge, and a rectangle clipped by hand ----
 * A 32x8 CI8 texture whose columns are all different, drawn at X0 in copy
 * and 1-cycle mode: (S) with the scissor's left edge d pixels inside the
 * rectangle, and (C) with the scissor wide open and the rectangle itself
 * started d pixels in, its s advanced by d (what the scene builder does). */
static uint8_t s_cols[32 * 8] __attribute__((aligned(16)));
static int col_entry(int x) { return 1 + x + x / 15; }   /* never a multiple of 16 (transparent) */

static void page_scissor(void) {
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  surface_t *fb = display_get();
  for (int y = 0; y < 8; y++) for (int x = 0; x < 32; x++) s_cols[y * 32 + x] = (uint8_t)col_entry(x);
  data_cache_hit_writeback(s_cols, sizeof s_cols);
  surface_t tex = surface_make(s_cols, FMT_CI8, 32, 8, 32);
  rdpq_attach(fb, NULL);
  rdpq_set_mode_fill(color_from_packed16(kBg));
  rdpq_fill_rectangle(0, 0, 320, 240);
  enum { X0 = 40, ND = 6 };
  for (int copy = 0; copy < 2; copy++)
    for (int byhand = 0; byhand < 2; byhand++)
      for (int d = 0; d < ND; d++) {
        const int x = X0 + d * 44, y = 120 + (copy * 2 + byhand) * 12;
        set_mode(copy, K_CI8);
        rdpq_tex_upload_tlut(s_tlut, 0, 256);
        rdpq_tex_upload(TILE0, &tex, NULL);
        if (byhand) {
          rdpq_set_scissor(0, 0, 320, 240);
          __rdpq_texture_rectangle_raw_fx(TILE0, (x + d) * 4, y * 4, (x + 32) * 4, (y + 8) * 4, d * 32, 0, 1024, 1024);
        } else {
          rdpq_set_scissor(x + d, y, x + 32, y + 8);
          __rdpq_texture_rectangle_raw_fx(TILE0, x * 4, y * 4, (x + 32) * 4, (y + 8) * 4, 0, 0, 1024, 1024);
        }
      }
  rdpq_set_scissor(0, 0, 320, 240);
  rdpq_detach_wait();
  const uint16_t *px = UncachedAddr(fb->buffer);
  const int stride = fb->stride / 2;
  s_nlines = 0;
  line("Left edge d pixels in, d = 0..5:");
  for (int copy = 1; copy >= 0; copy--)
    for (int byhand = 0; byhand < 2; byhand++) {
      char t[48];
      int o = snprintf(t, sizeof t, "%s %s:", copy ? "copy" : "1cyc", byhand ? "by hand" : "scissor");
      for (int d = 0; d < ND; d++) {
        const int x = X0 + d * 44, y = 120 + (copy * 2 + byhand) * 12;
        int bad = 0;
        for (int r = 0; r < 8; r++)
          for (int c = -2; c < 34; c++) {
            const int want = (c >= d && c < 32) ? col_entry(c) : -1;
            if (decode(px[(y + r) * stride + x + c]) != want) bad++;
          }
        o += snprintf(t + o, sizeof t - o, " %s", bad ? "X" : "ok");
        if (bad) {
          char dl[80];
          int q = snprintf(dl, sizeof dl, "  %s %s d=%d:", copy ? "cpy" : "1cy", byhand ? "hand" : "scis", d);
          for (int c = -2; c < 14 && q < 76; c++) {
            const int got = decode(px[y * stride + x + c]);
            int col = -1;
            for (int k = 0; k < 32; k++) if (col_entry(k) == got) col = k;
            q += snprintf(dl + q, sizeof dl - q, got == -1 ? "." : col < 0 ? "?" : "%x", col & 15);
          }
          debugf("[rdptest] %s\n", dl);
        }
      }
      line(t);
    }
  line("A: next page (colour maths)");
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  for (int i = 0; i < s_nlines; i++) graphics_draw_text(fb, 8, 6 + i * 10, s_lines[i]);
  display_show(fb);
}

/* ---- Page 4: colour maths ------------------------------------------------
 * Every pair of 5-bit values p (rows) and m (columns), one grey each, put
 * through the blender (m in the framebuffer) and through the colour combiner
 * (p and m in two textures), and read back. For each: how many of the 1,024
 * results equal the SNES's (half-add floor((p+m)/2), add min(31,p+m),
 * subtract max(0,p-m)), and the largest error. */
static uint16_t s_tp[32 * 32] __attribute__((aligned(16))), s_tm[32 * 32] __attribute__((aligned(16)));
static uint16_t grey16(int v) { return (uint16_t)(v << 11 | v << 6 | v << 1 | 1); }
static color_t grey32(int v, int a) { const int e = v << 3 | v >> 2; return RGBA32(e, e, e, a); }
enum { CM_B50, CM_BADD, CM_CADD, CM_CSUB1, CM_CSUB2, CM_CHALF, CM_N };
static const char *const kCmName[CM_N] = {"blend 50%", "blend add", "comb add", "comb sub", "comb sub2", "comb half"};
static int cm_want(int t, int p, int m) {
  switch (t) {
  case CM_B50: case CM_CHALF: return (p + m) >> 1;
  case CM_BADD: case CM_CADD: return p + m > 31 ? 31 : p + m;
  default: return p > m ? p - m : 0;
  }
}

static void page_maths(void) {
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  surface_t *fb = display_get();
  for (int r = 0; r < 32; r++) for (int c = 0; c < 32; c++) { s_tp[r * 32 + c] = grey16(r); s_tm[r * 32 + c] = grey16(c); }
  data_cache_hit_writeback(s_tp, sizeof s_tp);
  data_cache_hit_writeback(s_tm, sizeof s_tm);
  surface_t tp = surface_make(s_tp, FMT_RGBA16, 32, 32, 64), tm = surface_make(s_tm, FMT_RGBA16, 32, 32, 64);
  rdpq_attach(fb, NULL);
  rdpq_set_mode_fill(RGBA32(0, 0, 80, 255));
  rdpq_fill_rectangle(0, 0, 320, 240);
  for (int t = 0; t < CM_N; t++) {
    const int x0 = 8 + (t % 3) * 104, y0 = 4 + (t / 3) * 100;
    if (t == CM_B50 || t == CM_BADD) {
      for (int m = 0; m < 32; m++) {          /* the framebuffer side */
        rdpq_set_mode_fill(color_from_packed16(grey16(m)));
        rdpq_fill_rectangle(x0 + m * 3, y0, x0 + m * 3 + 3, y0 + 96);
      }
      rdpq_set_mode_standard();
      rdpq_mode_dithering(DITHER_NONE_NONE);
      rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
      rdpq_mode_blender(t == CM_B50 ? RDPQ_BLENDER_MULTIPLY : RDPQ_BLENDER_ADDITIVE);
      for (int p = 0; p < 32; p++) {
        rdpq_set_prim_color(grey32(p, t == CM_B50 ? 128 : 255));
        rdpq_fill_rectangle(x0, y0 + p * 3, x0 + 96, y0 + p * 3 + 3);
      }
    } else {
      rdpq_set_mode_standard();
      rdpq_mode_dithering(DITHER_NONE_NONE);
      rdpq_mode_blender(0);
      rdpq_set_env_color(RGBA32(255, 255, 255, 255));
      rdpq_set_prim_color(RGBA32(128, 128, 128, 128));
      switch (t) {
      case CM_CADD: rdpq_mode_combiner(RDPQ_COMBINER2((ONE, ZERO, TEX0, TEX1), (ZERO, ZERO, ZERO, ONE), (ZERO, ZERO, ZERO, COMBINED), (ZERO, ZERO, ZERO, COMBINED))); break;
      case CM_CSUB1: rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, TEX1, ENV, ZERO), (ZERO, ZERO, ZERO, ONE), (ZERO, ZERO, ZERO, COMBINED), (ZERO, ZERO, ZERO, COMBINED))); break;
      case CM_CSUB2: rdpq_mode_combiner(RDPQ_COMBINER2((ZERO, TEX1, ENV, TEX0), (ZERO, ZERO, ZERO, ONE), (ZERO, ZERO, ZERO, COMBINED), (ZERO, ZERO, ZERO, COMBINED))); break;
      default: rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, TEX1, PRIM, TEX1), (ZERO, ZERO, ZERO, ONE), (ZERO, ZERO, ZERO, COMBINED), (ZERO, ZERO, ZERO, COMBINED))); break;
      }
      rdpq_tex_multi_begin();
      rdpq_tex_upload(TILE0, &tp, NULL);
      rdpq_tex_upload(TILE1, &tm, NULL);
      rdpq_tex_multi_end();
      rdpq_texture_rectangle_scaled(TILE0, x0, y0, x0 + 96, y0 + 96, 0, 0, 32, 32);
    }
  }
  rdpq_detach_wait();
  const uint16_t *px = UncachedAddr(fb->buffer);
  const int stride = fb->stride / 2;
  s_nlines = 0;
  for (int t = 0; t < CM_N; t++) {
    const int x0 = 8 + (t % 3) * 104, y0 = 4 + (t / 3) * 100;
    int exact = 0, worst = 0, wp = 0, wm = 0, wgot = 0;
    for (int p = 0; p < 32; p++)
      for (int m = 0; m < 32; m++) {
        const int got = (px[(y0 + p * 3 + 1) * stride + x0 + m * 3 + 1] >> 11) & 31;
        const int e = got - cm_want(t, p, m);
        if (!e) exact++;
        if ((e < 0 ? -e : e) > (worst < 0 ? -worst : worst)) { worst = e; wp = p; wm = m; wgot = got; }
      }
    char l[48];
    if (worst) snprintf(l, sizeof l, "%s %d/1024 w%+d p%d m%d=%d", kCmName[t], exact, worst, wp, wm, wgot);
    else snprintf(l, sizeof l, "%s 1024/1024 exact", kCmName[t]);
    debugf("[rdptest] %s\n", l);
    if (s_nlines < 24) snprintf(s_lines[s_nlines++], 48, "%s", l);
  }
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  graphics_draw_text(fb, 8, 206, "rows p, columns m. A: results");
  display_show(fb);
  /* The results as text on their own screen (the grids above stay visible until A). */
}

static void page_maths_text(void) {
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  surface_t *fb = display_get();
  graphics_fill_screen(fb, graphics_make_color(0, 0, 64, 255));
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  graphics_draw_text(fb, 8, 6, "Colour maths, all 32x32 pairs:");
  for (int i = 0; i < s_nlines; i++) graphics_draw_text(fb, 8, 24 + i * 12, s_lines[i]);
  graphics_draw_text(fb, 8, 112, "exact = the SNES's result:");
  graphics_draw_text(fb, 8, 124, "half (p+m)/2 rounded down");
  graphics_draw_text(fb, 8, 136, "add min(31,p+m), sub max(0,p-m)");
  graphics_draw_text(fb, 8, 148, "w = worst error, at p and m");
  graphics_draw_text(fb, 8, 172, "A: next page (timing)");
  display_show(fb);
}

/* ---- Page 5: one 256x224 layer drawn three ways, timed by the RDP's own
 * counters ------------------------------------------------------------------
 * A: 28 strips of 256x8 CI8 in 1-cycle mode (the plane path).
 * B: 896 tiles, one 8x8 CI4 load and one copy-mode rectangle each.
 * C: the same tiles, 32 per load (a 16x256 CI4 texture), copy mode.
 * D: as C in 1-cycle mode.
 * Each is recorded once as a block, then run with nothing else on the RDP. */
#define DPC_STATUS_REG (*(volatile uint32_t *)0xA410000C)
#define DPC_COUNTER(off) ((*(volatile uint32_t *)(0xA4100000 + (off))) & 0xffffff)
static uint8_t s_plane[256 * 224] __attribute__((aligned(16)));
static uint8_t s_tiles[32 * 8 * 256] __attribute__((aligned(16)));   /* 32 groups of 32 tiles, 16 texels wide (8 bytes a row) */

static void time_block(rspq_block_t *b, const char *name) {
  rspq_wait();
  DPC_STATUS_REG = 0x3c0;   /* clear the clock, buffer, pipe and TMEM counters */
  const uint32_t t0 = TICKS_READ();
  rspq_block_run(b);
  rspq_wait();
  const uint32_t us = TICKS_TO_US(TICKS_READ() - t0);
  const uint32_t clk = DPC_COUNTER(0x10), buf = DPC_COUNTER(0x14), pipe = DPC_COUNTER(0x18), tmem = DPC_COUNTER(0x1c);
  char l[48];
  snprintf(l, sizeof l, "%s %lu.%02lu ms", name, (unsigned long)(us / 1000), (unsigned long)(us % 1000 / 10));
  line(l);
  /* counters in RDP clocks (62.5 MHz): 62.5 per microsecond */
  snprintf(l, sizeof l, " us: pipe %lu tmem %lu buf %lu", (unsigned long)(pipe * 2 / 125), (unsigned long)(tmem * 2 / 125),
           (unsigned long)(buf * 2 / 125));
  line(l);
  debugf("[rdptest]   clock %lu us\n", (unsigned long)(clk * 2 / 125));
}

static void page_timing(void) {
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  surface_t *fb = display_get();
  for (unsigned i = 0; i < sizeof s_plane; i++) s_plane[i] = (uint8_t)(1 + (i * 7 + (i >> 8) * 3) % 15);
  for (unsigned i = 0; i < sizeof s_tiles; i++) s_tiles[i] = (uint8_t)(0x11 * (1 + (i * 5 + (i >> 5)) % 15));
  data_cache_hit_writeback(s_plane, sizeof s_plane);
  data_cache_hit_writeback(s_tiles, sizeof s_tiles);
  rdpq_attach(fb, NULL);
  rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
  rdpq_fill_rectangle(0, 0, 320, 240);
  rdpq_tex_upload_tlut(s_tlut, 0, 256);
  rspq_block_t *blk[4];
  for (int k = 0; k < 4; k++) {
    rspq_block_begin();
    if (k == 0) {
      set_mode(0, K_CI8);
      for (int r = 0; r < 28; r++) {
        surface_t s = surface_make(s_plane + r * 8 * 256, FMT_CI8, 256, 8, 256);
        rdpq_tex_upload(TILE0, &s, NULL);
        rdpq_texture_rectangle(TILE0, 32, 8 + r * 8, 288, 16 + r * 8, 0, 0);
      }
    } else if (k == 1) {
      set_mode(1, K_CI4P0);
      for (int r = 0; r < 28; r++)
        for (int c = 0; c < 32; c++) {
          surface_t s = surface_make(s_tiles + ((r * 32 + c) & 1023) * 64, FMT_CI4, 16, 8, 8);
          rdpq_tex_upload(TILE0, &s, NULL);
          __rdpq_texture_rectangle_raw_fx(TILE0, (32 + c * 8) * 4, (8 + r * 8) * 4, (40 + c * 8) * 4, (16 + r * 8) * 4, 0, 0, 1024, 1024);
        }
    } else {
      set_mode(k == 2, K_CI4P0);
      for (int r = 0; r < 28; r++) {
        surface_t s = surface_make(s_tiles + (r & 31) * 2048, FMT_CI4, 16, 256, 8);
        rdpq_tex_upload(TILE0, &s, NULL);
        for (int c = 0; c < 32; c++)
          __rdpq_texture_rectangle_raw_fx(TILE0, (32 + c * 8) * 4, (8 + r * 8) * 4, (40 + c * 8) * 4, (16 + r * 8) * 4, 0, c * 8 * 32, 1024, 1024);
      }
    }
    blk[k] = rspq_block_end();
  }
  s_nlines = 0;
  line("One 256x224 layer, RDP alone:");
  time_block(blk[0], "A 28 strips, 1-cycle:");
  time_block(blk[1], "B 896 loads, copy:   ");
  time_block(blk[2], "C 28 loads, copy:    ");
  time_block(blk[3], "D 28 loads, 1-cycle: ");
  rdpq_detach_wait();
  for (int k = 0; k < 4; k++) rspq_block_free(blk[k]);
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  graphics_draw_box(fb, 0, 0, 320, 96, graphics_make_color(0, 0, 64, 255));
  for (int i = 0; i < s_nlines; i++) graphics_draw_text(fb, 8, 4 + i * 10, s_lines[i]);
  graphics_draw_text(fb, 8, 228, "A: next page (cache misses)");
  display_show(fb);
}

/* ---- Page 7: the cost of a cache miss ------------------------------------
 * I-cache: 16 KB direct-mapped, 32-byte lines. Eight stubs ("jr ra") 16 KB
 * apart share one line index, so calling them in turn misses every time;
 * the same stubs on eight neighbouring lines never miss. The difference a
 * call is a miss. D-cache (8 KB, 16-byte lines): the same with eight words
 * 8 KB apart against eight on neighbouring lines. Uncached: a word read
 * through KSEG1 from RDRAM and from the RSP's DMEM, against a cached one.
 * Interrupts are off while a loop runs. Times in CPU cycles (93.75 MHz;
 * Count ticks every other one). */
enum { kStubs = 8, kCalls = 1000 };
static uint32_t s_icode[kStubs * 4096 + 8] __attribute__((aligned(16384)));
static uint32_t s_ddata[kStubs * 2048 + 4] __attribute__((aligned(8192)));
static uint8_t s_pi_buf[256 * 1024] __attribute__((aligned(16)));
static uint8_t s_dma_buf[4096] __attribute__((aligned(16)));
typedef void (*StubFn)(void);
static StubFn s_fn[2][kStubs];                 /* [0] neighbouring lines, [1] one line index */
static volatile uint32_t *s_dp[2][kStubs];

static __attribute__((noinline)) uint32_t call_loop(StubFn *fn) {
  const uint32_t t0 = TICKS_READ();
  for (int i = 0; i < kCalls; i++) fn[i & (kStubs - 1)]();
  return TICKS_READ() - t0;
}
static __attribute__((noinline)) uint32_t read_loop(volatile uint32_t **p, uint32_t *sum) {
  uint32_t a = 0;
  const uint32_t t0 = TICKS_READ();
  for (int i = 0; i < kCalls; i++) a += *p[i & (kStubs - 1)];
  const uint32_t t = TICKS_READ() - t0;
  *sum = a;
  return t;
}

static void miss_setup(void) {
  /* Lines half a cache away from the loops' own code and from the pointer tables. */
  const uint32_t iline = ((((uint32_t)(uintptr_t)call_loop) >> 5) + 256) & 511;
  const uint32_t dline = ((((uint32_t)(uintptr_t)s_dp) >> 4) + 256) & 511;
  for (int k = 0; k < kStubs; k++) {
    uint32_t *far = s_icode + k * 4096 + iline * 8;                 /* + k * 16 KB */
    uint32_t *near = s_icode + ((iline + (uint32_t)k) & 511) * 8;   /* (k = 0: the same stub) */
    far[0] = near[0] = 0x03E00008; far[1] = near[1] = 0;            /* jr ra; nop */
    s_fn[1][k] = (StubFn)far; s_fn[0][k] = (StubFn)near;
    s_dp[1][k] = s_ddata + k * 2048 + dline * 4;                    /* + k * 8 KB */
    s_dp[0][k] = s_ddata + ((dline + (uint32_t)k) & 511) * 4;
  }
  data_cache_hit_writeback_invalidate(s_icode, sizeof s_icode);
  inst_cache_hit_invalidate(s_icode, sizeof s_icode);
  data_cache_hit_writeback_invalidate(s_ddata, sizeof s_ddata);
}

/* Cycles a miss, in tenths: I-cache and D-cache. base: the loops without misses. */
static uint32_t s_base_i, s_base_d;
static void miss_measure(uint32_t *ic, uint32_t *dc) {
  uint32_t sum;
  disable_interrupts();
  call_loop(s_fn[1]);                                   /* warm the loop itself */
  const uint32_t ti = call_loop(s_fn[1]);
  read_loop(s_dp[1], &sum);
  const uint32_t td = read_loop(s_dp[1], &sum);
  enable_interrupts();
  *ic = ti > s_base_i ? (ti - s_base_i) * 20 / kCalls : 0;
  *dc = td > s_base_d ? (td - s_base_d) * 20 / kCalls : 0;
}

static void miss_line(const char *name, uint32_t ic, uint32_t dc, uint32_t job_ms10) {
  char l[64];
  if (job_ms10)
    snprintf(l, sizeof l, "%s I %lu.%lu D %lu.%lu job %lums", name, (unsigned long)(ic / 10), (unsigned long)(ic % 10),
             (unsigned long)(dc / 10), (unsigned long)(dc % 10), (unsigned long)(job_ms10 / 10));
  else
    snprintf(l, sizeof l, "%s I %lu.%lu D %lu.%lu", name, (unsigned long)(ic / 10), (unsigned long)(ic % 10),
             (unsigned long)(dc / 10), (unsigned long)(dc % 10));
  line(l);
}

static void page_miss(void) {
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
  surface_t *fb = display_get();
  miss_setup();
  s_nlines = 0;
  line("Cache miss, CPU cycles each:");
  uint32_t sum, ic, dc;
  /* The loops with no miss, and the time the measurement takes. */
  disable_interrupts();
  call_loop(s_fn[0]); s_base_i = call_loop(s_fn[0]);
  read_loop(s_dp[0], &sum); s_base_d = read_loop(s_dp[0], &sum);
  enable_interrupts();
  const uint32_t w0 = TICKS_READ();
  miss_measure(&ic, &dc);
  const uint32_t window = TICKS_READ() - w0;
  miss_line("idle     ", ic, dc, 0);

  /* The RDP drawing: full-screen rectangles that read the framebuffer back. */
  rdpq_attach(fb, NULL);
  rdpq_set_mode_standard();
  rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
  rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
  rdpq_set_prim_color(RGBA32(40, 80, 160, 128));
  for (int i = 0; i < 64; i++) rdpq_fill_rectangle(0, 0, 320, 240);
  rspq_flush();
  uint32_t j0 = TICKS_READ();
  miss_measure(&ic, &dc);
  const bool rdp_busy = (*DP_STATUS & DP_STATUS_BUSY) != 0;
  rdpq_detach_wait();
  miss_line(rdp_busy ? "RDP busy " : "RDP done!", ic, dc, (uint32_t)(TICKS_TO_US(TICKS_READ() - j0) / 100));

  /* The RSP copying its 4 KB of DMEM to RDRAM, over and over. */
  data_cache_hit_writeback_invalidate(s_dma_buf, sizeof s_dma_buf);
  rspq_block_begin();
  for (int i = 0; i < 256; i++) rspq_dma_to_rdram(s_dma_buf, 0, 4096, false);
  rspq_block_t *dmas = rspq_block_end();
  for (int i = 0; i < 16; i++) rspq_block_run(dmas);   /* (a block: the queue itself would make the CPU wait) */
  const rspq_syncpoint_t sp = rspq_syncpoint_new();
  rspq_flush();
  j0 = TICKS_READ();
  miss_measure(&ic, &dc);
  const bool rsp_busy = !rspq_syncpoint_check(sp);
  rspq_wait();
  rspq_block_free(dmas);
  miss_line(rsp_busy ? "RSP DMA  " : "RSP done!", ic, dc, (uint32_t)(TICKS_TO_US(TICKS_READ() - j0) / 100));

  /* The cartridge being read into RDRAM. */
  data_cache_hit_writeback_invalidate(s_pi_buf, sizeof s_pi_buf);
  j0 = TICKS_READ();
  const uint64_t ticket = dma_read_async(s_pi_buf, 0x10001000, sizeof s_pi_buf);
  miss_measure(&ic, &dc);
  const bool pi_busy = !dma_finished(ticket);
  dma_wait_finished(ticket);
  miss_line(pi_busy ? "cart DMA " : "cart done", ic, dc, (uint32_t)(TICKS_TO_US(TICKS_READ() - j0) / 100));

  /* One word, uncached: RDRAM and the RSP's DMEM, against a cached word. */
  { static volatile uint32_t *up[2][kStubs];
    for (int k = 0; k < kStubs; k++) {
      up[0][k] = (volatile uint32_t *)(((uint32_t)(uintptr_t)s_dp[0][k]) | 0xA0000000u);
      up[1][k] = (volatile uint32_t *)(0xA4000000u + 0x800 + 4 * k);
    }
    disable_interrupts();
    const uint32_t tr = read_loop(up[0], &sum), ts = read_loop(up[1], &sum);
    enable_interrupts();
    char l[64];
    snprintf(l, sizeof l, "uncached: RDRAM %lu.%lu DMEM %lu.%lu",
             (unsigned long)((tr - s_base_d) * 20 / kCalls / 10), (unsigned long)((tr - s_base_d) * 20 / kCalls % 10),
             (unsigned long)((ts - s_base_d) * 20 / kCalls / 10), (unsigned long)((ts - s_base_d) * 20 / kCalls % 10));
    line(l);
  }
  { char l[64];
    snprintf(l, sizeof l, "(a measurement: %lu us; no-miss loop", (unsigned long)TICKS_TO_US(window)); line(l);
    snprintf(l, sizeof l, " %lu.%lu and %lu.%lu cycles a turn)", (unsigned long)(s_base_i * 20 / kCalls / 10), (unsigned long)(s_base_i * 20 / kCalls % 10),
             (unsigned long)(s_base_d * 20 / kCalls / 10), (unsigned long)(s_base_d * 20 / kCalls % 10)); line(l); }
  graphics_fill_screen(fb, graphics_make_color(0, 0, 64, 255));
  graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
  for (int i = 0; i < s_nlines; i++) graphics_draw_text(fb, 8, 8 + i * 12, s_lines[i]);
  graphics_draw_text(fb, 8, 228, "A: back to the flip tests");
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
    bool next = joypad_get_buttons_pressed(JOYPAD_PORT_1).a;
#ifdef RDPTEST_AUTO   /* for the emulator: go through the pages unattended */
    { static uint32_t t0; if (!t0) t0 = TICKS_READ();
      if (TICKS_TO_MS(TICKS_READ() - t0) > 1500) { t0 = TICKS_READ(); next = true; } }
#endif
    if (next) {
      rspq_wait();
      display_close();
      page = (page + 1) % 7;
      switch (page) {
      case 0: page_flips(); break;
      case 1: page_256(); break;
      case 2: page_scissor(); break;
      case 3: page_maths(); break;
      case 4: page_maths_text(); break;
      case 5: page_timing(); break;
      default: page_miss(); break;
      }
    }
  }
}
