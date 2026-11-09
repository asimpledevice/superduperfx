/* RDP backend, libdragon half: composite a built frame. See n64_ppu_rdp.h. */
#include <libdragon.h>
#include <string.h>

#include "n64_ppu_rdp.h"
#include "n64_platform.h"

enum { kOx = 32, kOy = 8 };   /* SNES picture position in the 320x240 display */

static bool rows_any(const uint32_t *bits, int a, int b) {
  for (int r = a; r < b; ++r)
    if (bits[r >> 5] & (1u << (r & 31))) return true;
  return false;
}

static uint16_t s_sw16[N64PPU_LINES * 256] __attribute__((aligned(16)));

static void draw_software_lines(const N64PpuFrame *f, int a, int b) {
  for (int r = a; r < b; ++r) {
    const uint32_t *src = f->sw_rows + (size_t)r * 256;
    uint16_t *dst = s_sw16 + (size_t)r * 256;
    for (int x = 0; x < 256; ++x) {
      const uint32_t p = src[x];
      dst[x] = (uint16_t)((((p >> 19) & 31) << 11) | (((p >> 11) & 31) << 6) |
                          (((p >> 3) & 31) << 1) | 1);
    }
  }
  data_cache_hit_writeback(s_sw16 + (size_t)a * 256, (size_t)(b - a) * 512);
  surface_t s = surface_make(s_sw16, FMT_RGBA16, 256, N64PPU_LINES, 512);
  rdpq_set_scissor(0, 0, 320, 240);
  rdpq_set_mode_copy(false);
  for (int y = a; y < b; y += 8) {
    const int y1 = y + 8 < b ? y + 8 : b;
    rdpq_tex_upload_sub(TILE0, &s, NULL, 0, y, 256, y1);
    rdpq_texture_rectangle(TILE0, kOx, kOy + y, kOx + 256, kOy + y1, 0, y);
  }
}

/* Draws one plane's rows [a, b) with palette tl (TLUT already loaded). */
int g_n64ppu_strips[N64PPU_PLANES];
/* Draws rows [a, b) of a plane in 8-row strips; rows selects which strips
 * hold pixels for this pass. */
static void draw_plane(const N64PpuFrame *f, int p, const uint32_t *rows, int a, int b) {
  surface_t s = surface_make(f->plane[p], FMT_CI8, N64PPU_STRIDE, N64PPU_LINES, N64PPU_STRIDE);
  for (int y = a & ~7; y < b; y += 8) {
    const int y0 = y < a ? a : y;
    const int y1 = y + 8 < b ? y + 8 : b;
    if (!rows_any(rows, y0, y1)) continue;
    g_n64ppu_strips[p]++;
    /* An 8-row window of the plane, uploaded whole and sampled from (0,0).
     * Write back exactly the rows the RDP is about to read. */
    data_cache_hit_writeback(f->plane[p] + (size_t)y * N64PPU_STRIDE, 8 * N64PPU_STRIDE);
    surface_t strip = surface_make_sub(&s, N64PPU_PAD, y, 256, 8);
    rdpq_tex_upload(TILE0, &strip, NULL);
    rdpq_texture_rectangle(TILE0, kOx, kOy + y0, kOx + 256, kOy + y1, 0, y0 - y);
  }
}

static int s_loaded_tlut = -1;

static void use_tlut(const N64PpuFrame *f, int t) {
  if (t == s_loaded_tlut) return;
  rdpq_tex_upload_tlut((uint16_t *)f->tlut[t], 0, 256);
  s_loaded_tlut = t;
}

static void draw_rdp_band(const N64PpuFrame *f, int a, int b) {
  const N64PpuLine *L = &f->line[a];
  for (int k = 0; k < L->nspans; ++k) {
    const N64PpuSpan *sp = &L->spans[k];
    rdpq_set_scissor(kOx + sp->x0, kOy + a, kOx + sp->x1, kOy + b);

    /* Backdrop, with fixed-colour math where it applies. A flat-colour
     * rectangle in standard mode: FILL mode needs 4-pixel-aligned scissors
     * and window spans start anywhere. */
    const uint16_t fill = sp->math ? (sp->clip ? L->fill_math_clip : L->fill_math)
                                   : (sp->clip ? L->fill_clip : L->fill);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_set_prim_color(color_from_packed16(fill));
    rdpq_fill_rectangle(kOx + sp->x0, kOy + a, kOx + sp->x1, kOy + b);

    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_mode_filter(FILTER_POINT);
    s_loaded_tlut = -1;

    /* Subscreen under the main screen, through the backdrop-math palette. */
    if (sp->math && L->sub_layers) {
      for (const int8_t *o = kN64PpuOrder[L->order]; *o >= 0; ++o) {
        if ((*o & 0xf) >= N64PPU_OBJ0) continue;
        const int p = (*o & 0xf) - N64PPU_BG1 + N64PPU_SUB_BG1;
        const int layer = n64ppu_plane_layer(p);
        if (!(L->sub_layers & (1 << layer)) || (sp->hide_sub & (1 << layer))) continue;
        const uint32_t *rows = (*o & N64PPU_HI) ? f->rows_hi[p] : f->rows[p];
        if (!rows_any(rows, a, b)) continue;
        const int sel = n64ppu_entry_sel(L->order, *o);
        use_tlut(f, sp->clip ? L->tl_math_clip[sel] : L->tl_math[sel]);
        draw_plane(f, p, rows, a, b);
      }
    }
    /* Main screen. */
    for (const int8_t *o = kN64PpuOrder[L->order]; *o >= 0; ++o) {
      const int p = *o & 0xf;
      const int layer = n64ppu_plane_layer(p);
      if (!(L->main_layers & (1 << layer)) || (sp->hide & (1 << layer))) continue;
      const uint32_t *rows = (*o & N64PPU_HI) ? f->rows_hi[p] : f->rows[p];
      if (!rows_any(rows, a, b)) continue;
      const int sel = n64ppu_entry_sel(L->order, *o);
      use_tlut(f, sp->clip ? N64PPU_TLUT_BLACK + sel : L->tl_main[sel]);
      draw_plane(f, p, rows, a, b);
    }
  }
}

static int compare(const N64PpuFrame *f, const surface_t *fb) {
  int mismatches = 0, logged = 0;
  static int s_cmp_frame;
  const bool map = ++s_cmp_frame == 90;
  for (int r = 0; r < N64PPU_LINES; ++r) {
    if (f->line[r].kind != N64PPU_LINE_RDP) continue;
    const uint16_t *row = (const uint16_t *)((const uint8_t *)fb->buffer + (size_t)(r + kOy) * fb->stride) + kOx;
    if (map) {
      char m[257]; int bad = 0;
      for (int x = 0; x < 256; ++x) {
        const uint32_t p = f->sw_rows[(size_t)r * 256 + x];
        const uint16_t want = (uint16_t)((((p >> 19) & 31) << 10) | (((p >> 11) & 31) << 5) | ((p >> 3) & 31));
        const uint16_t got = row[x] >> 1;
        m[x] = got == want ? (want ? 'o' : '.') : (got ? 'X' : '#');
        bad |= got != want || want;
      }
      m[256] = 0;
      if (bad) n64_platform_log("[map] %3d %.*s\n", r + 1, 140, m + 60);
    }
    const uint32_t *ref = f->sw_rows + (size_t)r * 256;
    for (int x = 0; x < 256; ++x) {
      const uint16_t got = row[x] >> 1;
      const uint32_t p = ref[x];
      const uint16_t want = (uint16_t)((((p >> 19) & 31) << 10) | (((p >> 11) & 31) << 5) | ((p >> 3) & 31));
      if (got != want) {
        if (logged++ < 4)
          n64_platform_log("[rdp] diff line %d x %d: rdp %04x sw %04x\n", r + 1, x, got, want);
        mismatches++;
      }
    }
  }
  return mismatches;
}

int n64_ppu_draw_and_show(const N64PpuFrame *f, const char *status, int compare_with_software) {
  surface_t *fb = display_get();
  data_cache_hit_writeback((void *)f->tlut, sizeof(f->tlut[0]) * (size_t)f->tlut_count);

  rdpq_attach(fb, NULL);
  rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
  rdpq_fill_rectangle(0, 0, fb->width, fb->height);
  for (int a = 0; a < N64PPU_LINES;) {
    int b = a + 1;
    const N64PpuLine *L = &f->line[a];
    while (b < N64PPU_LINES && !memcmp(&f->line[b], L, sizeof *L)) ++b;
    if (L->kind == N64PPU_LINE_SOFTWARE) draw_software_lines(f, a, b);
    else if (L->kind == N64PPU_LINE_RDP) draw_rdp_band(f, a, b);
    /* N64PPU_LINE_BLACK: already black. */
    a = b;
  }
#ifdef N64PPU_DBG_PROBE
  {
    static uint8_t ci8[64 * 8] __attribute__((aligned(16)));
    static uint16_t rgba[64 * 8] __attribute__((aligned(16)));
    memset(ci8, 0x81, sizeof ci8);
    for (int i = 0; i < 64 * 8; ++i) rgba[i] = 0xF801;
    data_cache_hit_writeback(ci8, sizeof ci8);
    data_cache_hit_writeback(rgba, sizeof rgba);
    rdpq_set_scissor(0, 0, 320, 240);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_tex_upload_tlut((uint16_t *)f->tlut[2], 0, 256);
    surface_t a = surface_make(ci8, FMT_CI8, 64, 8, 64);
    rdpq_tex_upload(TILE0, &a, NULL);
    rdpq_texture_rectangle(TILE0, 0, 0, 64, 8, 0, 0);
    rdpq_mode_tlut(TLUT_NONE);
    surface_t pl = surface_make(f->plane[N64PPU_OBJ3], FMT_CI8, N64PPU_STRIDE, N64PPU_LINES, N64PPU_STRIDE);
    surface_t sb = surface_make_sub(&pl, N64PPU_PAD, 112, 256, 8);
    rdpq_tex_upload(TILE0, &sb, NULL);
    rdpq_texture_rectangle(TILE0, 0, 10, 256, 18, 0, 0);
    surface_t c = surface_make(rgba, FMT_RGBA16, 64, 8, 128);
    rdpq_tex_upload(TILE0, &c, NULL);
    rdpq_texture_rectangle(TILE0, 0, 20, 64, 28, 0, 0);
  }
#endif
  rdpq_detach_wait();
#ifdef N64PPU_DBG_PROBE
  {
    static int n;
    if (++n == 90) {
      const uint16_t *px = (const uint16_t *)fb->buffer;
      const int st = fb->stride / 2;
      n64_platform_log("[probe] A ci8+tlut %04x  B plane-noTLUT x103 %04x x0 %04x  C rgba %04x  tlut2[81]=%04x plane[81?]=%02x\n",
                       px[4 * st + 10], px[14 * st + 103], px[14 * st + 0], px[24 * st + 10],
                       f->tlut[2][0x81], f->plane[N64PPU_OBJ3][(size_t)112 * N64PPU_STRIDE + N64PPU_PAD + 103]);
    }
  }
#endif

  int mismatches = -1;
  if (compare_with_software) mismatches = compare(f, fb);
  {
    static int n;
    if (++n == 90 || n == 600) {
      char buf[160]; int o = 0;
      for (int p = 0; p < N64PPU_PLANES; ++p) o += snprintf(buf + o, sizeof buf - o, " %d", g_n64ppu_strips[p]);
      n64_platform_log("[rdp] strips per plane:%s\n", buf);
    }
    memset(g_n64ppu_strips, 0, sizeof g_n64ppu_strips);
  }
  if (status && status[0]) {
    graphics_draw_box(fb, 16, N64_STATUS_Y - 2, fb->width - 32, 12, graphics_make_color(0, 0, 0, 255));
    graphics_set_color(graphics_make_color(255, 255, 0, 255), 0);
    graphics_draw_text(fb, 24, N64_STATUS_Y, status);
  }
  display_show(fb);
  return mismatches;
}
