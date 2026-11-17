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

#if N64PPU_SCENE
/* Scene builder, step 4 (docs/n64/PPU_DESIGN.md): main-screen BG passes
 * drawn as copy-mode texture rectangles from the tile cache (n64/rsptile)
 * with quads from n64_ppu_scene.c, instead of the expanded planes. The
 * frame description supplies each line's BG registers and CGRAM; the VRAM
 * snapshot (rspbg) supplies the tilemaps and the tile cache's source.
 * Passes with a colour-window clip keep the planes. */
#include "bg_rsp.h"
#include "n64_ppu_build.h"
#include "n64_ppu_desc.h"
#include "n64_ppu_scene.h"
#include "snes/ppu.h"
#include "tile_cache.h"
#include "scene_layout.h"
#if N64PPU_SCENE_RSP
#include "scene_rsp.h"
#endif

static bool s_scene_init;
static uint32_t s_scene_vram_count = ~0u;
static const uint16_t *s_scene_vram;
uint32_t g_scene_mode_passes[4];
uint32_t g_scene_sync_blocks;   /* VRAM blocks the tile cache revisited */   /* BG passes per N64PPU_SCENE_TL_* */
uint32_t g_scene_quads, g_scene_passes, g_scene_ticks, g_scene_tlut_ticks, g_scene_sync_ticks;
/* TLUTs for this frame: [cgram version][kind]: TC_TLUT_16 for 4bpp
 * layers, TC_TLUT_2BPP with the OBJ half for 2bpp (palette base in the low
 * half), keyed by brightness and base. */
enum { kSceneTluts = 64 };
static uint16_t s_scene_tlut[kSceneTluts][256] __attribute__((aligned(16)));
static struct { int cgram; uint8_t bright, kind, base, mode, mflags; } s_scene_tkey[kSceneTluts];
static int s_scene_ntlut;

static void scene_frame_begin(void) {
  if (!s_scene_init) {
    n64_tile_cache_init(TC_FMT_2BPP | TC_FMT_4BPP);
#if N64PPU_SCENE_RSP
    n64_scene_rsp_init();
#endif
    s_scene_init = true;
  }
  s_scene_ntlut = 0;
  uint32_t count;
  const uint16_t *snap = n64_bgrsp_snapshot(&count);
  s_scene_vram = snap;
  if (!snap || count == s_scene_vram_count) return;
  s_scene_vram_count = count;
  /* The RDP may still be drawing from the caches: let it finish first. */
  const uint32_t t0 = TICKS_READ();
  rdpq_fence();
  /* Only the blocks stored to since the last sync (the first sync takes
   * everything: the caches start from zero VRAM). */
  static bool s_synced;
  uint32_t dirty[8];
  n64_ppu_build_take_vram_dirty(dirty);
  if (!s_synced) memset(dirty, 0xff, sizeof dirty);
  s_synced = true;
  g_scene_sync_blocks += n64_tile_cache_sync_dirty(snap, dirty);
  data_cache_hit_invalidate((void *)((uintptr_t)snap & ~15u), 0x10000 + 16);
  g_scene_sync_ticks += TICKS_READ() - t0;
}

/* The TLUT for a band's CGRAM version and registers: tile cache
 * arrangement `kind` (palette base `base` for 2bpp) in colour mode `mode`
 * (N64PPU_SCENE_TL_*: plain, clipped to black, subscreen math, math with
 * the backdrop clipped). */
static const uint16_t *scene_tlut(const N64PpuBand *band, unsigned kind, unsigned base,
                                  unsigned mode) {
  const Ppu *regs = (const Ppu *)&band->regs;
  const int cgram = band->cgram;
  const unsigned bright = PPU_brightness(regs);
  const unsigned mflags = mode >= N64PPU_SCENE_TL_MATH
      ? (PPU_subtractColor(regs) ? 1u : 0u) | (PPU_halfColor(regs) ? 2u : 0u) : 0u;
  for (int i = 0; i < s_scene_ntlut; ++i)
    if (s_scene_tkey[i].cgram == cgram && s_scene_tkey[i].bright == bright &&
        s_scene_tkey[i].kind == kind && s_scene_tkey[i].base == base &&
        s_scene_tkey[i].mode == mode && s_scene_tkey[i].mflags == mflags)
      return s_scene_tlut[i];
  if (s_scene_ntlut == kSceneTluts) return NULL;
  const int i = s_scene_ntlut++;
  s_scene_tkey[i].cgram = cgram;
  s_scene_tkey[i].bright = (uint8_t)bright;
  s_scene_tkey[i].kind = (uint8_t)kind;
  s_scene_tkey[i].base = (uint8_t)base;
  s_scene_tkey[i].mode = (uint8_t)mode;
  s_scene_tkey[i].mflags = (uint8_t)mflags;
  /* On the CPU for now: the frame description's CGRAM copy is rewritten by
   * the next frame's replay before the RSP would get to it. */
  const uint32_t t0 = TICKS_READ();
  const uint16_t *cg = n64_ppu_desc()->cgram[cgram];
  if (mode == N64PPU_SCENE_TL_PLAIN) tc_ref_tlut(cg, bright, kind, base, 0x100, s_scene_tlut[i]);
  else n64_ppu_scene_tlut(cg, (const struct Ppu *)regs, kind, base, mode, s_scene_tlut[i]);
  g_scene_tlut_ticks += TICKS_READ() - t0;
  data_cache_hit_writeback(s_scene_tlut[i], 512);
  return s_scene_tlut[i];
}

/* Draws BG layer `layer`'s tiles of priority `hi` over rows [a, b) (within
 * the current scissor). Returns false when it cannot (the caller draws the
 * plane). */
static bool scene_draw_bg_(int layer, bool hi, int a, int b, int sx0, int sx1, unsigned mode);
static bool scene_draw_bg(int layer, bool hi, int a, int b, int sx0, int sx1, unsigned mode) {
  const uint32_t t0 = TICKS_READ();
  const bool r = scene_draw_bg_(layer, hi, a, b, sx0, sx1, mode);
  if (r) g_scene_mode_passes[mode & 3]++;
  else {
    static int n;
    if (n++ < 8) n64_platform_log("[scene] BG pass refused: layer %d mode %u rows %d-%d, tluts %d\n",
                                  layer, mode, a, b, s_scene_ntlut);
  }
  g_scene_ticks += TICKS_READ() - t0;
  return r;
}
static bool scene_draw_bg_(int layer, bool hi, int a, int b, int sx0, int sx1, unsigned tmode) {
  if (!s_scene_vram) return false;
  const N64PpuDesc *d = n64_ppu_desc();
  static N64SceneQuad q[33 * 29];
  bool copy_mode = false;
  for (int k = 0; k < d->nbands; ++k) {
    const N64PpuBand *band = &d->band[k];
    const int r0 = band->y0 - 1 > a ? band->y0 - 1 : a;
    const int r1 = band->y1 < b ? band->y1 : b;   /* y1 inclusive line = row y1 - 1 */
    if (r0 >= r1) continue;
    const Ppu *regs = (const Ppu *)&band->regs;
    const int mode = PPU_mode(regs);
    if (mode > 1 || PPU_bigTiles(regs, layer)) return false;
    const int bpp = (mode == 0 || layer == 2) ? 2 : 4;
    const unsigned pal_base = mode == 0 ? 32u * (unsigned)layer : 0;
    const N64SceneBg bg = {.hs = regs->hScroll[layer], .vs = regs->vScroll[layer],
                           .xsc = regs->bgXsc[layer], .bpp = (uint8_t)bpp,
                           .tileadr = (uint16_t)PPU_bgTileAdr(regs, layer)};
    const uint16_t *tl = scene_tlut(band, bpp == 4 ? TC_TLUT_16 : TC_TLUT_2BPP, pal_base, tmode);
    if (!tl) return false;
    if (!copy_mode) {
      rdpq_set_mode_copy(true);
      rdpq_mode_tlut(TLUT_RGBA16);
      copy_mode = true;
    }
    rdpq_tex_upload_tlut((uint16_t *)tl, 0, 256);
    s_loaded_tlut = -1;
    const uint8_t *cache = bpp == 4 ? n64_tile_cache_c4() : n64_tile_cache_c2();
#if N64PPU_SCENE_RSP
    /* The RSP reads the tilemaps and emits the rectangles, at most 32 rows
     * a command: at most five tile rows of 33 tiles at 56 bytes, ~9.3 KB of
     * RDP commands, under RSPQ_RDP_ROOM, so a command never waits for the
     * RDP once Red has started it (docs/n64/PPU_DESIGN.md). */
    for (int c0 = r0; c0 < r1; c0 += 32)
      n64_scene_rsp_bg(s_scene_vram, cache,
                       bpp == 4 ? n64_tile_cache_empty4() : n64_tile_cache_empty2(), bg.hs, bg.vs,
                       bg.xsc, bpp, bg.tileadr, c0, c0 + 32 < r1 ? c0 + 32 : r1, hi, sx0, sx1, kOx, kOy);
    continue;
#endif
    const int n = n64_scene_bg(&bg, s_scene_vram, r0, r1, q, (int)(sizeof q / sizeof q[0]));
    for (int i = 0; i < n; ++i) {
      if (!(q[i].flags & N64SCENE_PRIO) != !hi) continue;
      surface_t t = surface_make((void *)(cache + (size_t)q[i].tile * TC_CI4_TILE), FMT_CI4, 16, 8, 8);
      rdpq_tex_upload(TILE0, &t, &(rdpq_texparms_t){.palette = q[i].pal});
      const int h = q[i].flags & N64SCENE_HFLIP, v = q[i].flags & N64SCENE_VFLIP;
      const int row = v ? 7 - q[i].row : q[i].row;
      /* Clip to the span here: a copy-mode rectangle cut by the scissor's
       * left edge does not start its texture at the cut (ares shows the
       * wrong texels), so the rectangle starts at the span with s advanced. */
      const int x0 = q[i].x0 > sx0 ? q[i].x0 : sx0;
      const int x1 = q[i].x0 + 8 < sx1 ? q[i].x0 + 8 : sx1;
      if (x0 >= x1) continue;
      __rdpq_texture_rectangle_raw_fx(TILE0, (kOx + x0) * 4, (kOy + q[i].r0) * 4,
                                      (kOx + x1) * 4, (kOy + q[i].r1) * 4,
                                      ((h ? 8 : 0) + x0 - q[i].x0) * 32, row * 32, 1024,
                                      v ? -1024 : 1024);
      g_scene_quads++;
    }
  }
  g_scene_passes++;
  /* Back to the plane state draw_rdp_band set up. */
  if (copy_mode) {
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_mode_filter(FILTER_POINT);
  }
  return true;
}

/* Sprites of priority `prio` over rows [a, b) of lines with obj_scene, in
 * the current span: each strip (N64PpuObjStrip, n64_ppu_build.c) clipped to
 * the rows and the span, back to front in OAM rotation order from `first`
 * (lower rotation index on top), as copy-mode rectangles from the 4bpp tile
 * cache with the OBJ palettes (8-15 of the 4bpp TLUT). */
#define kObjRecs 4096    /* ring of tile records (32 KB): several frames of sprites */
static uint8_t s_obj_rec[kObjRecs * 8 + 512] __attribute__((aligned(16)));
static unsigned s_obj_next;
uint32_t g_scene_obj_rects;

static void scene_obj_flush(uint8_t *recs, int n) {
#if N64PPU_SCENE_RSP
  data_cache_hit_writeback(recs, (size_t)n * 8);
  n64_scene_rsp_tiles(recs, n, n64_tile_cache_c4(), n64_tile_cache_empty4(), kOx, kOy);
#else
  const uint8_t *cache = n64_tile_cache_c4();
  for (int i = 0; i < n; ++i) {
    const uint8_t *r = recs + 8 * i;
    const unsigned tile = (unsigned)r[0] << 8 | r[1];
    surface_t t = surface_make((void *)(cache + (size_t)tile * TC_CI4_TILE), FMT_CI4, 16, 8, 8);
    rdpq_tex_upload(TILE0, &t, &(rdpq_texparms_t){.palette = r[7] >> 4});
    const bool v = (r[6] & 0x80) != 0;
    __rdpq_texture_rectangle_raw_fx(TILE0, (kOx + r[2]) * 4, (kOy + r[4]) * 4,
                                    (kOx + r[3] + 1) * 4, (kOy + r[5] + 1) * 4,
                                    (r[6] & 0x7f) * 32, (r[7] & 7) * 32, 1024, v ? -1024 : 1024);
  }
#endif
}

static bool scene_draw_obj(const N64PpuFrame *f, int prio, unsigned first, int a, int b,
                           int sx0, int sx1, unsigned mode) {
  if (!s_scene_vram) return false;
  const uint32_t t0 = TICKS_READ();
  const N64PpuDesc *d = n64_ppu_desc();
  bool copy_mode = false, ok = true;
  for (int k = 0; k < d->nbands && ok; ++k) {
    const N64PpuBand *band = &d->band[k];
    const int r0 = band->y0 - 1 > a ? band->y0 - 1 : a;
    const int r1 = band->y1 < b ? band->y1 : b;
    if (r0 >= r1) continue;
    const uint16_t *tl = scene_tlut(band, TC_TLUT_16, 0, mode);
    if (!tl) { ok = false; break; }
    int n = 0;
    uint8_t *recs = NULL;
    for (int i = 127; i >= 0; --i) {
      const unsigned spr = (first + (unsigned)i) & 127;
      if (f->obj_prio[spr] != prio) continue;
      for (unsigned j = f->obj_start[spr]; j < f->obj_start[spr + 1]; ++j) {
        const N64PpuObjStrip *st = &f->obj_strip[j];
        const int y0 = st->r0 > r0 ? st->r0 : r0;
        const int y1 = st->r1 < r1 ? st->r1 : r1;
        if (y0 >= y1) continue;
        const int x0 = st->x > sx0 ? st->x : sx0;
        const int x1 = st->x + 8 < sx1 ? st->x + 8 : sx1;
        if (x0 >= x1) continue;
        if (!copy_mode) {
          rdpq_set_mode_copy(true);
          rdpq_mode_tlut(TLUT_RGBA16);
          copy_mode = true;
        }
        if (!recs) {
          rdpq_tex_upload_tlut((uint16_t *)tl, 0, 256);
          s_loaded_tlut = -1;
          if (s_obj_next + SCENE_TILES_MAX > kObjRecs) s_obj_next = 0;
          recs = s_obj_rec + (size_t)s_obj_next * 8;
        }
        const int dr = st->d0 + (y0 - st->r0);   /* row within the tile, before flips */
        const bool vflip = (st->flags & N64PPU_OBJ_VFLIP) != 0;
        uint8_t *r = recs + 8 * n;
        r[0] = (uint8_t)(st->tile >> 8);
        r[1] = (uint8_t)st->tile;
        r[2] = (uint8_t)x0;
        r[3] = (uint8_t)(x1 - 1);
        r[4] = (uint8_t)y0;
        r[5] = (uint8_t)(y1 - 1);
        r[6] = (uint8_t)(((st->flags & N64PPU_OBJ_HFLIP) ? 8 : 0) + x0 - st->x) | (vflip ? 0x80 : 0);
        r[7] = (uint8_t)((vflip ? 7 - dr : dr) | (8 + (st->flags & 7)) << 4);
        g_scene_obj_rects++;
        if (++n == SCENE_TILES_MAX) {
          scene_obj_flush(recs, n);
          s_obj_next += (unsigned)n;
          if (s_obj_next + SCENE_TILES_MAX > kObjRecs) s_obj_next = 0;
          recs = s_obj_rec + (size_t)s_obj_next * 8;
          n = 0;
        }
      }
    }
    if (n) {
      scene_obj_flush(recs, n);
      s_obj_next += (unsigned)n;
    }
  }
  if (copy_mode) {
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_mode_filter(FILTER_POINT);
  }
  g_scene_ticks += TICKS_READ() - t0;
  return ok;
}
#endif

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
#if N64PPU_SCENE
        /* No plane was built for these either (n64_ppu_build_line). */
        if ((L->scene_bg & 2) && scene_draw_bg(layer, (*o & N64PPU_HI) != 0, a, b, sp->x0, sp->x1,
                                               sp->clip ? N64PPU_SCENE_TL_MATH_CLIP : N64PPU_SCENE_TL_MATH))
          continue;
#endif
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
#if N64PPU_SCENE
      /* No plane was built for these (n64_ppu_build_line, same rule). */
      const unsigned mode = sp->clip ? N64PPU_SCENE_TL_BLACK : N64PPU_SCENE_TL_PLAIN;
      if (p <= N64PPU_BG4 && (L->scene_bg & 1) && scene_draw_bg(p, (*o & N64PPU_HI) != 0, a, b, sp->x0, sp->x1, mode)) continue;
      /* Sprites of lines with obj_scene: no plane rows either. */
      if (p >= N64PPU_OBJ0 && p <= N64PPU_OBJ3 && L->obj_scene) {
        if (!scene_draw_obj(f, p - N64PPU_OBJ0, L->obj_first, a, b, sp->x0, sp->x1, mode)) {
          static int warned;
          if (!warned++) n64_platform_log("[scene] sprite pass without a palette slot\n");
        }
        continue;
      }
#endif
      const uint32_t *rows = (*o & N64PPU_HI) ? f->rows_hi[p] : f->rows[p];
      if (!rows_any(rows, a, b)) continue;
      const int sel = n64ppu_entry_sel(L->order, *o);
      use_tlut(f, sp->clip ? N64PPU_TLUT_BLACK + sel : L->tl_main[sel]);
      draw_plane(f, p, rows, a, b);
    }
  }
}

/* Out of line: only RENDER=2 runs it, and n64_ppu_draw_and_show must fit its
 * slot in icache_order.ld (profile/check_placement.py). */
static __attribute__((noinline)) int compare(const N64PpuFrame *f, const surface_t *fb) {
  int mismatches = 0, logged = 0;
  static int s_cmp_frame;
  const bool map = ++s_cmp_frame == 300;
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
      if (bad) n64_platform_log("[map] %3d %s\n", r + 1, m);
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

#ifdef N64_PRESENT_PROF
/* Present's parts (ticks): 0 display_get, 1 issuing the frame's RDP
 * commands, 2 the status line, 3 waiting for the RDP (N64_PRESENT_RDPWAIT
 * builds only), 4 display_show. */
uint32_t g_present_prof[5];
#define PP_T() get_ticks()
/* RDP counters (RCP clocks, 62.5 MHz) over the frame's commands, read after
 * waiting for the RDP (N64_PRESENT_RDPWAIT): 0 pipeline busy, 1 TMEM busy,
 * 2 command buffer busy. And a second status line the host formats. */
uint32_t g_present_rdp[3];
char g_present_line[48];
#define DPC_STATUS_REG (*(volatile uint32_t *)0xA410000C)
#define DPC_COUNTER(off) ((*(volatile uint32_t *)(0xA4100000 + (off))) & 0xffffff)
#define PP_ADD(i, t) (g_present_prof[i] += get_ticks() - (t))
#else
#define PP_T() 0u
#define PP_ADD(i, t) ((void)(t))
#endif
#ifdef N64_HANG_PROBE
extern volatile uint32_t g_hang_where;
extern surface_t *volatile g_hang_fb;
#define DRAW_WHERE(v) (g_hang_where = (v))
#else
#define DRAW_WHERE(v) ((void)0)
#endif
int n64_ppu_draw_and_show(const N64PpuFrame *f, const char *status, int compare_with_software) {
  DRAW_WHERE(0x21);
  uint32_t pt = PP_T();
  surface_t *fb = display_get();
  DRAW_WHERE(0x22);
#if N64PPU_SCENE
  scene_frame_begin();
#endif
  PP_ADD(0, pt); pt = PP_T();
  data_cache_hit_writeback((void *)f->tlut, sizeof(f->tlut[0]) * (size_t)f->tlut_count);

  rdpq_attach(fb, NULL);
#if defined(N64_PRESENT_PROF) && defined(N64_PRESENT_RDPWAIT)
  DRAW_WHERE(0x23);
  rspq_wait();                     /* nothing else on the RDP while counting */
  DPC_STATUS_REG = 0x3c0;          /* clear the clock, command, pipe and TMEM counters */
#endif
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

  PP_ADD(1, pt); pt = PP_T();
#ifdef N64_PRESENT_RDPWAIT
  DRAW_WHERE(0x24);
  rspq_wait();   /* measurement only: the RDP's drawing time */
  PP_ADD(3, pt); pt = PP_T();
#ifdef N64_PRESENT_PROF
  g_present_rdp[0] += DPC_COUNTER(0x18);
  g_present_rdp[1] += DPC_COUNTER(0x1c);
  g_present_rdp[2] += DPC_COUNTER(0x14);
#endif
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
  pt = PP_T();
  if (status && status[0]) {
    graphics_draw_box(fb, 16, N64_STATUS_Y - 2, fb->width - 32, 12, graphics_make_color(0, 0, 0, 255));
    graphics_set_color(graphics_make_color(255, 255, 0, 255), 0);
    graphics_draw_text(fb, 24, N64_STATUS_Y, status);
  }
#ifdef N64_PRESENT_PROF
  if (g_present_line[0]) {
    graphics_draw_box(fb, 16, N64_STATUS_Y + 10, fb->width - 32, 12, graphics_make_color(0, 0, 0, 255));
    graphics_set_color(graphics_make_color(0, 255, 255, 255), 0);
    graphics_draw_text(fb, 24, N64_STATUS_Y + 12, g_present_line);
  }
#endif
  PP_ADD(2, pt); pt = PP_T();
  DRAW_WHERE(0x25);
#ifdef N64_HANG_PROBE
  g_hang_fb = fb;
#endif
  display_show(fb);
  DRAW_WHERE(0x26);
  PP_ADD(4, pt);
  return mismatches;
}
