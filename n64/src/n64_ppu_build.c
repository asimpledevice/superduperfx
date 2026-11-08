/* RDP backend, runtime half: expand each scanline into palette-indexed planes.
 * See n64_ppu_rdp.h. The tile, sprite, window and colour-math rules mirror
 * runner/src/snes/ppu.c (PpuDrawBackground_4bpp/_2bpp, ppu_evaluateSprites,
 * PpuWindows_CalcWithExtra, PpuDrawWholeLine) for the cases handled here;
 * anything else is reported so the caller renders that line in software. */
#include "n64_ppu_build.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/ppu_brightness.h"

/* ---- Bitplane expansion ------------------------------------------------ */

/* Bitplane byte -> 8 chunky pixels, one per byte, in memory order. */
static uint64_t s_expand[256], s_expand_rev[256];

static void init_tables(void) {
  static bool done;
  if (done) return;
  done = true;
  for (int b = 0; b < 256; ++b) {
    uint8_t fwd[8], rev[8];
    for (int i = 0; i < 8; ++i) {
      fwd[i] = (uint8_t)((b >> (7 - i)) & 1);   /* leftmost pixel is bit 7 */
      rev[i] = (uint8_t)((b >> i) & 1);         /* horizontally flipped */
    }
    memcpy(&s_expand[b], fwd, 8);
    memcpy(&s_expand_rev[b], rev, 8);
  }
}

static inline uint64_t splat8(uint8_t v) { return 0x0101010101010101ull * v; }

static inline void store8(uint8_t *dst, uint64_t v) { memcpy(dst, &v, 8); }

/* ---- Frame state ------------------------------------------------------- */

struct TlutKey {
  uint32_t cgram_count;
  uint16_t backdrop;
  uint8_t kind, brightness, transparency, flags, pad[2];
};

typedef struct {
  N64PpuFrame *frame;
  /* Rows that may hold non-zero bytes (persistent across frames). A row that
   * is not rewritten on a line is cleared before it can be drawn again. */
  uint32_t dirty[N64PPU_PLANES][(N64PPU_LINES + 31) / 32];
  struct TlutKey keys[N64PPU_MAX_TLUTS];   /* parallel to frame->tlut */
} BuildState;

static BuildState s_state;

N64PpuFrame *n64_ppu_build_frame_alloc(void) {
  init_tables();
  N64PpuFrame *f = calloc(1, sizeof(*f));
  if (!f) return NULL;
  for (int p = 0; p < N64PPU_PLANES; ++p) {
    f->plane[p] = memalign(16, (size_t)N64PPU_STRIDE * N64PPU_LINES);
    if (!f->plane[p]) return NULL;
    memset(f->plane[p], 0, (size_t)N64PPU_STRIDE * N64PPU_LINES);
  }
  return f;
}

static inline uint8_t *plane_row(N64PpuFrame *f, int plane, int r) {
  return f->plane[plane] + (size_t)r * N64PPU_STRIDE;
}

static inline void row_mark(N64PpuFrame *f, int plane, int r) {
  f->rows[plane][r >> 5] |= 1u << (r & 31);
}

static inline bool row_marked(const N64PpuFrame *f, int plane, int r) {
  return (f->rows[plane][r >> 5] >> (r & 31)) & 1;
}

/* A row not written this line but drawn as part of an 8-line strip must not
 * show last frame's pixels. */
static inline void clear_if_dirty(N64PpuFrame *f, int plane, int r) {
  if (s_state.dirty[plane][r >> 5] & (1u << (r & 31))) {
    memset(plane_row(f, plane, r), 0, N64PPU_STRIDE);
    s_state.dirty[plane][r >> 5] &= ~(1u << (r & 31));
  }
}

/* ---- Palettes ------------------------------------------------------------ */

enum { kTlutNormal = 1, kTlutMath = 2 };
enum { kMathSubtract = 1, kMathHalf = 2 };

/* Brightness applied to a 0..62 colour-math sum, as the software PPU's
 * brightnessMult / brightnessMultHalf tables do. Returns 5 bits. */
static inline unsigned bright(unsigned v, unsigned brightness, bool half) {
  if (half) v >>= 1;
  if (v > 31) v = 31;
  return ppu_brightness_level(v, brightness) >> 3;
}

static inline uint16_t pack5551(unsigned r, unsigned g, unsigned b) {
  return (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
}

/* Colour math on one 15-bit pair, then brightness. */
static uint16_t math_color(uint16_t a, uint16_t b, unsigned flags,
                           unsigned brightness, bool half) {
  unsigned out[3];
  for (int k = 0; k < 3; ++k) {
    const unsigned x = (a >> (5 * k)) & 31, y = (b >> (5 * k)) & 31;
    const unsigned v = (flags & kMathSubtract) ? (x >= y ? x - y : 0) : x + y;
    out[k] = bright(v, brightness, half);
  }
  return pack5551(out[0], out[1], out[2]);
}

/* Palette kind normal (CGRAM colour) or math (backdrop (+/-) CGRAM colour,
 * halved if flags say so), brightness applied; entries index % transparency
 * == 0 are transparent. Pooled per frame by their parameters. */
static uint8_t get_tlut_slow(N64PpuFrame *f, const Ppu *ppu, uint8_t kind, uint8_t transparency,
                             uint16_t backdrop, uint8_t flags, bool *full);

/* Most lines reuse the previous line's palettes: remember the last result per
 * (kind, transparency, flags) and skip the pool search when nothing moved. */
static struct { uint32_t count; uint16_t backdrop; uint8_t brightness, valid, slot; } s_tlut_memo[2][2][4];

static uint8_t get_tlut(N64PpuFrame *f, const Ppu *ppu, uint8_t kind, uint8_t transparency,
                        uint16_t backdrop, uint8_t flags, bool *full) {
  typeof(s_tlut_memo[0][0][0]) *m = &s_tlut_memo[kind == kTlutMath][transparency == 4][flags & 3];
  const uint16_t bd = kind == kTlutMath ? backdrop : 0;
  if (m->valid && m->count == ppu->cgramWriteCount && m->backdrop == bd &&
      m->brightness == PPU_brightness(ppu))
    return m->slot;
  const uint8_t slot = get_tlut_slow(f, ppu, kind, transparency, backdrop, flags, full);
  if (!*full) {
    m->valid = 1; m->count = ppu->cgramWriteCount; m->backdrop = bd;
    m->brightness = PPU_brightness(ppu); m->slot = slot;
  }
  return slot;
}

static uint8_t get_tlut_slow(N64PpuFrame *f, const Ppu *ppu, uint8_t kind, uint8_t transparency,
                             uint16_t backdrop, uint8_t flags, bool *full) {
  struct TlutKey key;
  memset(&key, 0, sizeof key);
  key.cgram_count = ppu->cgramWriteCount;
  key.kind = kind;
  key.brightness = PPU_brightness(ppu);
  key.transparency = transparency;
  if (kind == kTlutMath) {
    key.backdrop = backdrop;
    key.flags = flags;
  }
  for (int i = 2; i < f->tlut_count; ++i)
    if (!memcmp(&s_state.keys[i], &key, sizeof key)) return (uint8_t)i;
  if (f->tlut_count == N64PPU_MAX_TLUTS) { *full = true; return 0; }
  const int i = f->tlut_count++;
  s_state.keys[i] = key;
  uint16_t *t = f->tlut[i];
  const unsigned b = key.brightness;
  for (int c = 0; c < 256; ++c) {
    const uint16_t raw = ppu->cgram[c];
    uint16_t v;
    if (kind == kTlutMath)
      v = math_color(backdrop, raw, flags, b, (flags & kMathHalf) != 0);
    else
      v = pack5551(bright(raw & 31, b, false), bright((raw >> 5) & 31, b, false),
                   bright((raw >> 10) & 31, b, false));
    t[c] = (c % transparency) ? v : (uint16_t)(v & ~1u);
  }
  return (uint8_t)i;
}

void n64_ppu_build_begin(N64PpuFrame *f, const uint32_t *sw_rows) {
  s_state.frame = f;
  f->sw_rows = sw_rows;
  f->rdp_lines = f->software_lines = f->black_lines = 0;
  f->fallback_reasons = 0;
  memset(f->rows, 0, sizeof(f->rows));
  /* Fixed black palettes for clipped main pixels. */
  for (int c = 0; c < 256; ++c) {
    f->tlut[N64PPU_TLUT_BLACK16][c] = (c & 15) ? 1 : 0;
    f->tlut[N64PPU_TLUT_BLACK4][c] = (c & 3) ? 1 : 0;
  }
  f->tlut_count = 2;
  memset(s_tlut_memo, 0, sizeof s_tlut_memo);   /* slots are per frame */
}

/* ---- Windows ------------------------------------------------------------- */

/* Window state of layer (0-3 BG, 4 OBJ, 5 colour) at x: the per-span bit
 * PpuWindows_CalcWithExtra computes. */
static bool window_bit(const Ppu *ppu, int layer, int x) {
  const uint32_t flags = GET_WINDOW_FLAGS(ppu, layer);
  const int w1l = ppu->window1left, w1r = ppu->window1right;
  const int w2l = ppu->window2left, w2r = ppu->window2right;
  bool in1 = (flags & kWindow1Enabled) && w1l <= w1r && x >= w1l && x <= w1r;
  if ((flags & (kWindow1Enabled | kWindow1Inversed)) == (kWindow1Enabled | kWindow1Inversed))
    in1 = !in1;
  bool in2 = (flags & kWindow2Enabled) && w2l <= w2r && x >= w2l && x <= w2r;
  if ((flags & (kWindow2Enabled | kWindow2Inversed)) == (kWindow2Enabled | kWindow2Inversed))
    in2 = !in2;
  if ((flags & (kWindow1Enabled | kWindow2Enabled)) == (kWindow1Enabled | kWindow2Enabled)) {
    switch ((ppu->wbgobjlog >> (layer * 2)) & 3) {
      case 0: return in1 || in2;
      case 1: return in1 && in2;
      case 2: return in1 != in2;
      default: return in1 == in2;
    }
  }
  return in1 || in2;
}

/* Colour-window decision (kCwBitsMod): true = keep colour / allow math. */
static inline bool cw_keep(unsigned mode, bool bit) {
  switch (mode) {
    case 0: return true;
    case 1: return bit;
    case 2: return !bit;
    default: return false;
  }
}

typedef struct {
  int n;
  uint16_t x0[N64PPU_MAX_SPANS], x1[N64PPU_MAX_SPANS];
  uint8_t mask_main[N64PPU_MAX_SPANS];   /* bit L: main layer L hidden here */
  uint8_t mask_sub[N64PPU_MAX_SPANS];
  uint8_t clip[N64PPU_MAX_SPANS], math[N64PPU_MAX_SPANS];
} Spans;

/* Window edges split the line into at most five spans with constant state. */
static void compute_spans(const Ppu *ppu, Spans *s) {
  const int edges[6] = {0, 256, ppu->window1left, ppu->window1right + 1,
                        ppu->window2left, ppu->window2right + 1};
  int n = 0, sorted[6];
  for (int i = 0; i < 6; ++i) {
    const int e = edges[i] < 0 ? 0 : edges[i] > 256 ? 256 : edges[i];
    bool dup = false;
    for (int j = 0; j < n; ++j) dup |= sorted[j] == e;
    if (!dup) sorted[n++] = e;
  }
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) {
      const int t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
    }
  s->n = 0;
  for (int i = 0; i + 1 < n; ++i) {
    const int x = sorted[i];
    const int k = s->n++;
    s->x0[k] = (uint16_t)sorted[i];
    s->x1[k] = (uint16_t)sorted[i + 1];
    s->mask_main[k] = s->mask_sub[k] = 0;
    for (int layer = 0; layer < 5; ++layer) {
      const bool bit = window_bit(ppu, layer, x);
      if (bit && (ppu->screenWindowed[0] & (1 << layer))) s->mask_main[k] |= 1 << layer;
      if (bit && (ppu->screenWindowed[1] & (1 << layer))) s->mask_sub[k] |= 1 << layer;
    }
    const bool cbit = window_bit(ppu, 5, x);
    s->clip[k] = !cw_keep(PPU_clipMode(ppu), cbit);
    s->math[k] = cw_keep(PPU_preventMathMode(ppu), cbit);
  }
}

/* Clear the spans where a layer is windowed out. */
static void apply_mask(N64PpuFrame *f, const Spans *s, const uint8_t *masks, int layer,
                       const int *planes, int nplanes, int r) {
  for (int k = 0; k < s->n; ++k) {
    if (!(masks[k] & (1 << layer))) continue;
    for (int i = 0; i < nplanes; ++i)
      if (row_marked(f, planes[i], r))
        memset(plane_row(f, planes[i], r) + N64PPU_PAD + s->x0[k], 0, s->x1[k] - s->x0[k]);
  }
}

/* ---- Backgrounds ----------------------------------------------------------- */

/* One background layer, one line. bpp is 2 or 4; pal_base is added to every
 * index (mode 0 layer groups). Writes both priority rows in full. */
static void expand_bg(N64PpuFrame *f, const Ppu *ppu, int layer, int line,
                      int bpp, unsigned pal_base, int plane_lo, int plane_hi) {
  const int r = line - 1;
  const uint16_t *vram = ppu->vram;
  const unsigned y = (unsigned)line + ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16_t *tps[2] = {
    &vram[sc_offs & 0x7fff],
    &vram[(sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0)) & 0x7fff],
  };
  const int tileadr = PPU_bgTileAdr(ppu, layer);
  const unsigned x = ppu->hScroll[layer];
  uint8_t *lo = plane_row(f, plane_lo, r) + N64PPU_PAD - (x & 7);
  uint8_t *hi = plane_row(f, plane_hi, r) + N64PPU_PAD - (x & 7);
  bool any_lo = false, any_hi = false;
  for (int k = 0; k < 33; ++k) {
    const unsigned mx = (x >> 3) + (unsigned)k;
    const uint16_t entry = tps[(mx >> 5) & 1][mx & 31];
    const unsigned row = (entry & 0x8000) ? 7 - (y & 7) : (y & 7);
    const uint64_t *tab = (entry & 0x4000) ? s_expand_rev : s_expand;
    uint64_t c;
    unsigned pal;
    if (bpp == 4) {
      const uint16_t w0 = vram[(tileadr + (entry & 0x3ff) * 16 + row) & 0x7fff];
      const uint16_t w1 = vram[(tileadr + (entry & 0x3ff) * 16 + row + 8) & 0x7fff];
      c = tab[w0 & 0xff] | tab[w0 >> 8] << 1 | tab[w1 & 0xff] << 2 | tab[w1 >> 8] << 3;
      pal = pal_base + ((entry >> 10) & 7) * 16;
    } else {
      const uint16_t w0 = vram[(tileadr + (entry & 0x3ff) * 8 + row) & 0x7fff];
      c = tab[w0 & 0xff] | tab[w0 >> 8] << 1;
      pal = pal_base + ((entry >> 10) & 7) * 4;
    }
    /* Colour 0 of every palette (index pal*16 or pal*4) is transparent in
     * the palettes these planes are drawn with, so the palette can be OR-ed
     * into every pixel without masking. */
    const uint64_t v = c | splat8((uint8_t)pal);
    if (entry & 0x2000) {
      store8(hi + k * 8, v);
      store8(lo + k * 8, 0);
      any_hi |= c != 0;
    } else {
      store8(lo + k * 8, v);
      store8(hi + k * 8, 0);
      any_lo |= c != 0;
    }
  }
  /* Both rows now hold this line's bytes (transparent where empty). */
  s_state.dirty[plane_lo][r >> 5] |= 1u << (r & 31);
  s_state.dirty[plane_hi][r >> 5] |= 1u << (r & 31);
  if (any_lo) row_mark(f, plane_lo, r);
  if (any_hi) row_mark(f, plane_hi, r);
}

/* ---- Sprites --------------------------------------------------------------- */

/* Sprites for one line, mirroring ppu_evaluateSprites without widescreen:
 * OAM rotation, 32 sprites and 34 tiles per line, lower index wins. */
static void expand_obj(N64PpuFrame *f, const Ppu *ppu, int line) {
  static const uint8_t kSizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}
  };
  const int r = line - 1;
  const int eval_line = line - 1;            /* the PPU evaluates line - 1 */
  uint8_t pix[256], pri[256];
  uint8_t found[32];
  int n = 0;
  uint8_t index = PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) : 0;
  for (int i = 0; i < 128; ++i, index += 2) {
    const uint8_t row = (uint8_t)(eval_line - (ppu->oam[index] >> 8));
    const int size = kSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    if (row >= size) continue;
    int sx = (ppu->oam[index] & 0xff) | (((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8);
    if (sx >= 256) sx -= 512;
    if (sx + size <= 0) continue;
    if (n == 32) break;                        /* range over */
    found[n++] = index;
  }
  if (!n) return;
  memset(pix, 0, sizeof(pix));
  bool any = false;
  int tiles = 0;
  for (int i = n; i > 0 && tiles <= 34; --i) {
    index = found[i - 1];
    unsigned row = (uint8_t)(eval_line - (ppu->oam[index] >> 8));
    const int size = kSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int sx = (ppu->oam[index] & 0xff) | (((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8);
    if (sx >= 256) sx -= 512;
    const unsigned oam1 = ppu->oam[index + 1];
    const int obj_adr = (oam1 & 0x100) ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
    if (oam1 & 0x8000) row = (unsigned)size - 1 - row;
    const uint8_t pal = (uint8_t)(0x80 + 16 * ((oam1 >> 9) & 7));
    const uint8_t prio = (uint8_t)((oam1 >> 12) & 3);
    for (int col = 0; col < size; col += 8) {
      if (col + sx <= -8 || col + sx >= 256) continue;
      if (++tiles > 34) break;                 /* time over */
      const int used_col = (oam1 & 0x4000) ? size - 1 - col : col;
      const int tile = ((((oam1 & 0xff) >> 4) + (int)(row >> 3)) << 4) |
                       (((oam1 & 0xf) + (used_col >> 3)) & 0xf);
      const int a = obj_adr + tile * 16 + (int)(row & 7);
      const uint32_t plane = ppu->vram[a & 0x7fff] | (uint32_t)ppu->vram[(a + 8) & 0x7fff] << 16;
      for (int px = 0; px < 8; ++px) {
        const int x = col + sx + px;
        if (x < 0 || x >= 256) continue;
        const int shift = (oam1 & 0x4000) ? px : 7 - px;
        const uint32_t bits = plane >> shift;
        const int c = (bits & 1) | ((bits >> 7) & 2) | ((bits >> 14) & 4) | ((bits >> 21) & 8);
        if (!c) continue;
        pix[x] = (uint8_t)(pal + c);           /* lower OAM index drawn later wins */
        pri[x] = prio;
        any = true;
      }
    }
  }
  if (!any) return;
  for (int x = 0; x < 256; ++x) {
    if (!pix[x]) continue;
    const int p = N64PPU_OBJ0 + pri[x];
    plane_row(f, p, r)[N64PPU_PAD + x] = pix[x];
    row_mark(f, p, r);
    s_state.dirty[p][r >> 5] |= 1u << (r & 31);
  }
}

/* ---- Lines --------------------------------------------------------------- */

/* Expand the enabled BG layers of one screen into planes base..base+7. */
static void expand_screen_bgs(N64PpuFrame *f, const Ppu *ppu, int line, uint8_t enabled,
                              int base) {
  if (PPU_mode(ppu) == 0) {
    for (int layer = 0; layer < 4; ++layer)
      if (enabled & (1 << layer))
        expand_bg(f, ppu, layer, line, 2, 32u * layer, base + layer * 2, base + layer * 2 + 1);
  } else {
    for (int layer = 0; layer < 3; ++layer)
      if (enabled & (1 << layer))
        expand_bg(f, ppu, layer, line, layer == 2 ? 2 : 4, 0, base + layer * 2, base + layer * 2 + 1);
  }
}

static uint32_t line_unsupported(const Ppu *ppu, const Spans *s, bool *backdrop_math) {
  uint32_t why = 0;
  const int mode = PPU_mode(ppu);
  const uint8_t main = ppu->screenEnabled[0], sub = ppu->screenEnabled[1];
  if (mode > 1) why |= N64PPU_WHY_MODE;
  if (PPU_mosaicSize(ppu) > 1 && (ppu->mosaic & (main | sub) & 0xf)) why |= N64PPU_WHY_MOSAIC;
  if ((ppu->bgmode >> 4) & (main | sub) & 0xf) why |= N64PPU_WHY_BIG_TILES;
  if (ppu->setini & 0x0b) why |= N64PPU_WHY_HIRES_INTERLACE;
  /* Colour math is visible only where some span allows it and it changes
   * something: a non-zero fixed colour, halving, or a composed subscreen. */
  *backdrop_math = false;
  bool any_math_span = false;
  for (int k = 0; k < s->n; ++k) any_math_span |= s->math[k] != 0;
  const bool nontrivial = ppu->fixedColor || PPU_halfColor(ppu) ||
                          (PPU_addSubscreen(ppu) && sub);
  if (PPU_mathEnabled(ppu) && any_math_span && nontrivial) {
    if (PPU_mathEnabled(ppu) & 0x1f) why |= N64PPU_WHY_COLOR_MATH;   /* layer math */
    else *backdrop_math = true;
    if (*backdrop_math && PPU_addSubscreen(ppu) && (sub & 0x10)) why |= N64PPU_WHY_SUB_OBJ;
  }
  return why;
}

bool n64_ppu_build_line(const Ppu *ppu, int line) {
  N64PpuFrame *f = s_state.frame;
  const int r = line - 1;
  N64PpuLine *L = &f->line[r];
  memset(L, 0, sizeof *L);
  if (PPU_forcedBlank(ppu)) {
    for (int p = 0; p < N64PPU_PLANES; ++p) clear_if_dirty(f, p, r);
    L->kind = N64PPU_LINE_BLACK;
    f->black_lines++;
    return true;
  }
  Spans s;
  compute_spans(ppu, &s);
  bool backdrop_math;
  uint32_t why = line_unsupported(ppu, &s, &backdrop_math);

  if (!why) {
    bool full = false;
    const uint16_t bd = ppu->cgram[0];
    const unsigned b = PPU_brightness(ppu);
    L->tl_main16 = get_tlut(f, ppu, kTlutNormal, 16, 0, 0, &full);
    L->tl_main4 = get_tlut(f, ppu, kTlutNormal, 4, 0, 0, &full);
    L->fill = (uint16_t)(f->tlut[L->tl_main16][0] | 1);
    L->fill_clip = 1;   /* black, opaque */
    if (backdrop_math) {
      const uint8_t mflags = (uint8_t)((PPU_subtractColor(ppu) ? kMathSubtract : 0) |
                                       (PPU_halfColor(ppu) ? kMathHalf : 0));
      const bool add_sub = PPU_addSubscreen(ppu);
      /* Where the subscreen is empty the fixed colour is used, halved only
       * without addSubscreen (PpuDrawWholeLine). Clipped: main colour 0 and
       * no halving. */
      L->fill_math = math_color(bd, ppu->fixedColor, mflags, b, !add_sub && PPU_halfColor(ppu));
      L->fill_math_clip = math_color(0, ppu->fixedColor, mflags, b, false);
      if (add_sub) {
        L->sub_layers = ppu->screenEnabled[1] & 0xf;
        L->tl_math16 = get_tlut(f, ppu, kTlutMath, 16, bd, mflags, &full);
        L->tl_math4 = get_tlut(f, ppu, kTlutMath, 4, bd, mflags, &full);
        const uint8_t cflags = (uint8_t)(mflags & ~kMathHalf);
        L->tl_math16_clip = get_tlut(f, ppu, kTlutMath, 16, 0, cflags, &full);
        L->tl_math4_clip = get_tlut(f, ppu, kTlutMath, 4, 0, cflags, &full);
      }
    }
    if (full) why |= N64PPU_WHY_PALETTES;
  }
  if (why) {
#ifdef N64PPU_LOG_FALLBACK
    {
      static unsigned s_logged;
      extern void n64_platform_log(const char *fmt, ...);
      if ((line == 1 || line == 112) && s_logged++ < 400)
        n64_platform_log("[rdp-why] line %d why %02lx mode %d main %02x sub %02x mwin %02x swin %02x "
                         "wsel %06lx w1 %d-%d w2 %d-%d log %04x cgwsel %02x cgadsub %02x fixed %04x\n",
                         line, (unsigned long)why, PPU_mode(ppu), ppu->screenEnabled[0],
                         ppu->screenEnabled[1], ppu->screenWindowed[0], ppu->screenWindowed[1],
                         (unsigned long)ppu->windowsel, ppu->window1left, ppu->window1right,
                         ppu->window2left, ppu->window2right, ppu->wbgobjlog, ppu->cgwsel,
                         ppu->cgadsub, ppu->fixedColor);
    }
#endif
    memset(L, 0, sizeof *L);
    for (int p = 0; p < N64PPU_PLANES; ++p) clear_if_dirty(f, p, r);
    L->kind = N64PPU_LINE_SOFTWARE;
    f->fallback_reasons |= why;
    f->software_lines++;
    return false;
  }

  L->kind = N64PPU_LINE_RDP;
  L->order = PPU_mode(ppu) == 0 ? N64PPU_ORDER_MODE0
           : (PPU_bg3priority(ppu) ? N64PPU_ORDER_MODE1_BG3TOP : N64PPU_ORDER_MODE1);
  L->nspans = (uint8_t)s.n;
  for (int k = 0; k < s.n; ++k) {
    L->spans[k].x0 = s.x0[k];
    L->spans[k].x1 = s.x1[k];
    L->spans[k].clip = s.clip[k];
    L->spans[k].math = (uint8_t)(backdrop_math && s.math[k]);
  }

  /* Main screen, then its window masks. Rows of layers expanded this line
   * are rewritten in full; every other plane row must not keep stale pixels. */
  const uint8_t main = ppu->screenEnabled[0];
  const uint8_t bg_layers = PPU_mode(ppu) == 0 ? 0xf : 0x7;
  for (int layer = 0; layer < 4; ++layer) {
    if ((main & bg_layers) & (1 << layer)) continue;
    clear_if_dirty(f, N64PPU_BG1L + 2 * layer, r);
    clear_if_dirty(f, N64PPU_BG1L + 2 * layer + 1, r);
  }
  for (int layer = 0; layer < 4; ++layer) {
    if ((L->sub_layers & bg_layers) & (1 << layer)) continue;
    clear_if_dirty(f, N64PPU_SUB_BG1L + 2 * layer, r);
    clear_if_dirty(f, N64PPU_SUB_BG1L + 2 * layer + 1, r);
  }
  for (int p = N64PPU_OBJ0; p <= N64PPU_OBJ3; ++p) clear_if_dirty(f, p, r);
  expand_screen_bgs(f, ppu, line, main, N64PPU_BG1L);
  if (main & 0x10) expand_obj(f, ppu, line);
  for (int layer = 0; layer < 4; ++layer) {
    const int planes[2] = {N64PPU_BG1L + 2 * layer, N64PPU_BG1L + 2 * layer + 1};
    apply_mask(f, &s, s.mask_main, layer, planes, 2, r);
  }
  {
    const int planes[4] = {N64PPU_OBJ0, N64PPU_OBJ1, N64PPU_OBJ2, N64PPU_OBJ3};
    apply_mask(f, &s, s.mask_main, 4, planes, 4, r);
  }
  /* Subscreen, only when backdrop math composes it. */
  if (L->sub_layers) {
    expand_screen_bgs(f, ppu, line, L->sub_layers, N64PPU_SUB_BG1L);
    for (int layer = 0; layer < 4; ++layer) {
      const int planes[2] = {N64PPU_SUB_BG1L + 2 * layer, N64PPU_SUB_BG1L + 2 * layer + 1};
      apply_mask(f, &s, s.mask_sub, layer, planes, 2, r);
    }
  }
  f->rdp_lines++;
  return true;
}
