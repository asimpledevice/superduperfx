/* RDP backend, runtime half: expand scanlines into palette-indexed planes.
 * See n64_ppu_rdp.h. The tile, sprite, window and colour-math rules mirror
 * runner/src/snes/ppu.c (PpuDrawBackground_4bpp/_2bpp, ppu_evaluateSprites,
 * PpuWindows_CalcWithExtra, PpuDrawWholeLine) for the cases handled here;
 * anything else is reported so the caller renders that line in software.
 *
 * Background layers are expanded tile-major: consecutive lines with the same
 * layer registers (and no VRAM writes in between) are deferred as a run and
 * expanded a tile column at a time across up to 8 lines, so each tile's data
 * is read from VRAM once instead of once per line. This is memory-bound code
 * on the N64. */
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

/* Claim the 16-byte data-cache lines that a row write will overwrite in
 * full, without reading them from RDRAM first (VR4300 CACHE op 0x0D, Create
 * Dirty Exclusive). Lines 1..15 of a plane row are always fully rewritten by
 * a BG row (bytes [8-f, 272-f), f = fine scroll 0..7). No-op off the N64. */
static inline void claim_row_lines(uint8_t *row) {
#if defined(__mips__) && defined(N64)
  for (int i = 1; i <= 15; ++i)
    __asm__ volatile("cache 0x0D, 0(%0)" : : "r"(row + 16 * i) : "memory");
#else
  (void)row;
#endif
}
static inline void store8(uint8_t *dst, uint64_t v) { memcpy(dst, &v, 8); }

/* ---- Frame state ------------------------------------------------------- */

struct TlutKey {
  uint32_t cgram_count;
  uint16_t backdrop;
  uint8_t kind, brightness, sel, flags, pad[2];
};

typedef struct {
  int n;
  uint16_t x0[N64PPU_MAX_SPANS], x1[N64PPU_MAX_SPANS];
  uint8_t mask_main[N64PPU_MAX_SPANS];   /* bit L: main layer L hidden here */
  uint8_t mask_sub[N64PPU_MAX_SPANS];
  uint8_t clip[N64PPU_MAX_SPANS], math[N64PPU_MAX_SPANS];
} Spans;

/* A deferred run of lines for one background layer of one screen. */
typedef struct {
  bool active;
  int l0, l1;                 /* lines [l0, l1) */
  uint16_t hs, vs;
  uint8_t xsc, bpp;
  uint16_t tileadr;
  uint8_t pal_base;
  uint32_t vram_count;
} BgRun;

/* Field order matters on the N64: the run state and the PPU pointer are
 * used on every tile of a deferred run, so they come first, next to the
 * frame pointer, rather than after the 10 KB span array where they shared
 * data-cache lines with the PPU's own state. */
typedef struct {
  N64PpuFrame *frame;
  const Ppu *ppu;                          /* VRAM source for deferred runs */
  BgRun runs[2][4];                        /* [main/sub][layer] */
  /* Rows that may hold non-zero bytes (persistent across frames). A row that
   * is not rewritten on a line is cleared before it can be drawn again. */
  uint32_t dirty[N64PPU_PLANES][(N64PPU_LINES + 31) / 32];
  struct TlutKey keys[N64PPU_MAX_TLUTS];   /* parallel to frame->tlut */
  Spans spans[N64PPU_LINES];               /* per line, for deferred masks */
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

static inline void bit_set(uint32_t *bits, int r) { bits[r >> 5] |= 1u << (r & 31); }
static inline bool bit_get(const uint32_t *bits, int r) { return (bits[r >> 5] >> (r & 31)) & 1; }

static inline void clear_if_dirty(N64PpuFrame *f, int plane, int r) {
  if (bit_get(s_state.dirty[plane], r)) {
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

/* Which CGRAM entry palette index i shows under selector sel, or -1 when it
 * is transparent. */
static inline int sel_source(int sel, int i) {
  switch (sel) {
    case N64PPU_SEL_LO16: return (i < 128 && (i & 15)) ? i : -1;
    case N64PPU_SEL_HI16: return (i >= 128 && (i & 15)) ? i - 128 : -1;
    case N64PPU_SEL_LO4:  return (i < 128 && (i & 3)) ? i : -1;
    case N64PPU_SEL_HI4:  return (i >= 128 && (i & 3)) ? i - 128 : -1;
    default:              return (i >= 128 && (i & 15)) ? i : -1;   /* OBJ */
  }
}

static uint8_t get_tlut_slow(N64PpuFrame *f, const Ppu *ppu, uint8_t kind, uint8_t sel,
                             uint16_t backdrop, uint8_t flags, bool *full) {
  struct TlutKey key;
  memset(&key, 0, sizeof key);
  key.cgram_count = ppu->cgramWriteCount;
  key.kind = kind;
  key.brightness = PPU_brightness(ppu);
  key.sel = sel;
  if (kind == kTlutMath) {
    key.backdrop = backdrop;
    key.flags = flags;
  }
  for (int i = N64PPU_SELS; i < f->tlut_count; ++i)
    if (!memcmp(&s_state.keys[i], &key, sizeof key)) return (uint8_t)i;
  if (f->tlut_count == N64PPU_MAX_TLUTS) { *full = true; return 0; }
  const int i = f->tlut_count++;
  s_state.keys[i] = key;
  uint16_t *t = f->tlut[i];
  const unsigned b = key.brightness;
  for (int c = 0; c < 256; ++c) {
    const int src = sel_source(sel, c);
    if (src < 0) { t[c] = 0; continue; }
    const uint16_t raw = ppu->cgram[src];
    t[c] = kind == kTlutMath
        ? math_color(backdrop, raw, flags, b, (flags & kMathHalf) != 0)
        : pack5551(bright(raw & 31, b, false), bright((raw >> 5) & 31, b, false),
                   bright((raw >> 10) & 31, b, false));
  }
  return (uint8_t)i;
}

/* Most lines reuse the previous line's palettes: remember the last result per
 * (kind, selector, flags) and skip the pool search when nothing moved. */
static struct { uint32_t count; uint16_t backdrop; uint8_t brightness, valid, slot; }
    s_tlut_memo[2][N64PPU_SELS][4];

static uint8_t get_tlut(N64PpuFrame *f, const Ppu *ppu, uint8_t kind, uint8_t sel,
                        uint16_t backdrop, uint8_t flags, bool *full) {
  typeof(s_tlut_memo[0][0][0]) *m = &s_tlut_memo[kind == kTlutMath][sel][flags & 3];
  const uint16_t bd = kind == kTlutMath ? backdrop : 0;
  if (m->valid && m->count == ppu->cgramWriteCount && m->backdrop == bd &&
      m->brightness == PPU_brightness(ppu))
    return m->slot;
  const uint8_t slot = get_tlut_slow(f, ppu, kind, sel, backdrop, flags, full);
  if (!*full) {
    m->valid = 1; m->count = ppu->cgramWriteCount; m->backdrop = bd;
    m->brightness = PPU_brightness(ppu); m->slot = slot;
  }
  return slot;
}

void n64_ppu_build_begin(N64PpuFrame *f, const uint32_t *sw_rows) {
  s_state.frame = f;
  f->sw_rows = sw_rows;
  f->rdp_lines = f->software_lines = f->black_lines = 0;
  f->fallback_reasons = 0;
  memset(f->rows, 0, sizeof(f->rows));
  memset(f->rows_hi, 0, sizeof(f->rows_hi));
  memset(s_state.runs, 0, sizeof(s_state.runs));
  /* Fixed black palettes, one per selector, for clipped main pixels. */
  for (int sel = 0; sel < N64PPU_SELS; ++sel)
    for (int c = 0; c < 256; ++c)
      f->tlut[N64PPU_TLUT_BLACK + sel][c] = sel_source(sel, c) >= 0 ? 1 : 0;
  f->tlut_count = N64PPU_SELS;
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

/* Clear the spans of row r of a plane where its layer is windowed out. */
static void apply_mask(N64PpuFrame *f, const Spans *s, const uint8_t *masks, int layer,
                       int plane, int r) {
  if (!bit_get(f->rows[plane], r) && !bit_get(f->rows_hi[plane], r)) return;
  for (int k = 0; k < s->n; ++k)
    if (masks[k] & (1 << layer))
      memset(plane_row(f, plane, r) + N64PPU_PAD + s->x0[k], 0, s->x1[k] - s->x0[k]);
}

/* ---- Backgrounds ----------------------------------------------------------- */

/* Expand a deferred run, tile-major. */
static void flush_run(N64PpuFrame *f, int screen, int layer) {
  BgRun *run = &s_state.runs[screen][layer];
  if (!run->active) return;
  run->active = false;
  const int plane = (screen ? N64PPU_SUB_BG1 : N64PPU_BG1) + layer;
  /* VRAM as of the flush. Only registers are journaled per line; VRAM
   * changes during the raster replay only through HDMA to $2118/9, which
   * would also end the run (vram_count) but after the write. */
  const uint16_t *vram = s_state.ppu->vram;
  const bool wider = run->xsc & 1, higher = run->xsc & 2;
  const int tilemap = (run->xsc & 0xfc) << 8;
  const unsigned x = run->hs;
  for (int g0 = run->l0; g0 < run->l1;) {
    /* Lines whose BG row is in the same tile row. */
    const unsigned y0 = (unsigned)g0 + run->vs;
    int g1 = g0 + 1;
    while (g1 < run->l1 && (((unsigned)g1 + run->vs) >> 3) == (y0 >> 3)) ++g1;
    int sc_offs = tilemap + (((y0 >> 3) & 0x1f) << 5);
    if ((y0 & 0x100) && higher) sc_offs += wider ? 0x800 : 0x400;
    const uint16_t *tps[2] = {
      &vram[sc_offs & 0x7fff],
      &vram[(sc_offs + (wider ? 0x400 : 0)) & 0x7fff],
    };
    const int n = g1 - g0;
    uint8_t *dst[8];
    bool any_lo[8] = {false}, any_hi[8] = {false};
    for (int i = 0; i < n; ++i) {
      uint8_t *row = plane_row(f, plane, g0 - 1 + i);
      claim_row_lines(row);
      dst[i] = row + N64PPU_PAD - (x & 7);
    }
    for (int k = 0; k < 33; ++k) {
      const unsigned mx = (x >> 3) + (unsigned)k;
      const uint16_t entry = tps[(mx >> 5) & 1][mx & 31];
      const uint64_t *tab = (entry & 0x4000) ? s_expand_rev : s_expand;
      const bool hi = entry & 0x2000;
      if (run->bpp == 4) {
        const int base = run->tileadr + (entry & 0x3ff) * 16;
        const uint64_t pal = splat8((uint8_t)((hi ? 128 : 0) + run->pal_base + ((entry >> 10) & 7) * 16));
        for (int i = 0; i < n; ++i) {
          const unsigned yy = (y0 + (unsigned)i) & 7;
          const unsigned row = (entry & 0x8000) ? 7 - yy : yy;
          const uint16_t w0 = vram[(base + row) & 0x7fff];
          const uint16_t w1 = vram[(base + row + 8) & 0x7fff];
          const uint64_t c = tab[w0 & 0xff] | tab[w0 >> 8] << 1 | tab[w1 & 0xff] << 2 | tab[w1 >> 8] << 3;
          store8(dst[i] + k * 8, c | pal);
          if (hi) any_hi[i] |= c != 0; else any_lo[i] |= c != 0;
        }
      } else {
        const int base = run->tileadr + (entry & 0x3ff) * 8;
        const uint64_t pal = splat8((uint8_t)((hi ? 128 : 0) + run->pal_base + ((entry >> 10) & 7) * 4));
        for (int i = 0; i < n; ++i) {
          const unsigned yy = (y0 + (unsigned)i) & 7;
          const unsigned row = (entry & 0x8000) ? 7 - yy : yy;
          const uint16_t w0 = vram[(base + row) & 0x7fff];
          const uint64_t c = tab[w0 & 0xff] | tab[w0 >> 8] << 1;
          store8(dst[i] + k * 8, c | pal);
          if (hi) any_hi[i] |= c != 0; else any_lo[i] |= c != 0;
        }
      }
    }
    for (int i = 0; i < n; ++i) {
      const int r = g0 - 1 + i;
      bit_set(s_state.dirty[plane], r);
      if (any_lo[i]) bit_set(f->rows[plane], r);
      if (any_hi[i]) bit_set(f->rows_hi[plane], r);
      const Spans *s = &s_state.spans[r];
      apply_mask(f, s, screen ? s->mask_sub : s->mask_main, layer, plane, r);
    }
    g0 = g1;
  }
}

/* Queue one line of one layer, flushing the run when its state changes. */
static void queue_bg(N64PpuFrame *f, const Ppu *ppu, int screen, int layer, int line,
                     int bpp, unsigned pal_base) {
  BgRun *run = &s_state.runs[screen][layer];
  BgRun want = {
    .active = true, .l0 = line, .l1 = line + 1,
    .hs = ppu->hScroll[layer], .vs = ppu->vScroll[layer], .xsc = ppu->bgXsc[layer],
    .bpp = (uint8_t)bpp, .tileadr = (uint16_t)PPU_bgTileAdr(ppu, layer),
    .pal_base = (uint8_t)pal_base, .vram_count = ppu->vramWriteCount,
  };
  if (run->active && run->l1 == line && run->hs == want.hs && run->vs == want.vs &&
      run->xsc == want.xsc && run->bpp == want.bpp && run->tileadr == want.tileadr &&
      run->pal_base == want.pal_base && run->vram_count == want.vram_count) {
    run->l1 = line + 1;
    return;
  }
  flush_run(f, screen, layer);
  *run = want;
}

/* ---- Sprites --------------------------------------------------------------- */

/* Which sprites cover each line: bit s of line L set when sprite s is in
 * range there and not wholly off the left edge. Built once per OAM upload
 * and sprite size setting (oamWriteCount counts every OAM change), instead
 * of testing all 128 sprites on every line. */
static const uint8_t kObjSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}
};
static struct {
  const Ppu *ppu;
  uint32_t oam_count;
  uint8_t obj_size, valid;
  uint32_t mask[N64PPU_LINES][4];
} s_obj_lines;

static inline int obj_size_of(const Ppu *ppu, unsigned index) {
  return kObjSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
}
static inline int obj_x_of(const Ppu *ppu, unsigned index) {
  int sx = (ppu->oam[index] & 0xff) | (((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8);
  return sx >= 256 ? sx - 512 : sx;
}

static void obj_lines_build(const Ppu *ppu) {
  memset(s_obj_lines.mask, 0, sizeof s_obj_lines.mask);
  for (unsigned spr = 0; spr < 128; ++spr) {
    const unsigned index = spr * 2;
    const int size = obj_size_of(ppu, index);
    if (obj_x_of(ppu, index) + size <= 0) continue;
    const unsigned y = ppu->oam[index] >> 8;
    for (int d = 0; d < size; ++d) {
      const unsigned l = (y + (unsigned)d) & 255;   /* row = (uint8)(line - y) < size */
      if (l < N64PPU_LINES) s_obj_lines.mask[l][spr >> 5] |= 1u << (spr & 31);
    }
  }
  s_obj_lines.ppu = ppu;
  s_obj_lines.oam_count = ppu->oamWriteCount;
  s_obj_lines.obj_size = (uint8_t)PPU_objSize(ppu);
  s_obj_lines.valid = 1;
}

/* Sprites in range on a line, in OAM rotation order from sprite `first`,
 * at most 32 (the range-over limit); returns the count. */
static int obj_lines_collect(const uint32_t m[4], unsigned first, uint8_t found[32]) {
  int n = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const unsigned lo = pass ? 0 : first, hi = pass ? first : 128;
    for (unsigned w = lo >> 5; w < 4 && w * 32 < hi; ++w) {
      uint32_t bits = m[w];
      if (w * 32 < lo) bits &= ~0u << (lo & 31);
      if ((w + 1) * 32 > hi) bits &= (1u << (hi & 31)) - 1;
      while (bits) {
        if (n == 32) return n;
        found[n++] = (uint8_t)((w * 32 + (unsigned)__builtin_ctz(bits)) * 2);
        bits &= bits - 1;
      }
    }
  }
  return n;
}

/* Sprites for one line, mirroring ppu_evaluateSprites without widescreen:
 * OAM rotation, 32 sprites and 34 tiles per line, lower index wins. */
static void expand_obj(N64PpuFrame *f, const Ppu *ppu, int line) {
  const int r = line - 1;
  const int eval_line = line - 1;            /* the PPU evaluates line - 1 */
  if (!s_obj_lines.valid || s_obj_lines.ppu != ppu || s_obj_lines.oam_count != ppu->oamWriteCount ||
      s_obj_lines.obj_size != PPU_objSize(ppu))
    obj_lines_build(ppu);
  uint8_t found[32];
  const unsigned first = PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) >> 1 : 0;
  const int n = obj_lines_collect(s_obj_lines.mask[eval_line], first, found);
  if (!n) return;
  /* 8 bytes of margin each side: a tile is drawn 8 pixels at a time at any
   * x in [-7, 255]; the margins take what falls off screen. */
  uint8_t pixb[8 + 256 + 8], prib[8 + 256 + 8];
  uint8_t *const pix = pixb + 8, *const pri = prib + 8;
  memset(pixb, 0, sizeof(pixb));
  memset(prib, 0, sizeof(prib));
  bool any = false;
  int tiles = 0;
  for (int i = n; i > 0 && tiles <= 34; --i) {
    const unsigned index = found[i - 1];
    unsigned row = (uint8_t)(eval_line - (ppu->oam[index] >> 8));
    const int size = obj_size_of(ppu, index);
    const int sx = obj_x_of(ppu, index);
    const unsigned oam1 = ppu->oam[index + 1];
    const int obj_adr = (oam1 & 0x100) ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
    if (oam1 & 0x8000) row = (unsigned)size - 1 - row;
    const uint64_t pal = splat8((uint8_t)(0x80 + 16 * ((oam1 >> 9) & 7)));
    const uint64_t prio = splat8((uint8_t)((oam1 >> 12) & 3));
    const uint64_t *tab = (oam1 & 0x4000) ? s_expand_rev : s_expand;
    for (int col = 0; col < size; col += 8) {
      if (col + sx <= -8 || col + sx >= 256) continue;
      if (++tiles > 34) break;                 /* time over */
      const int used_col = (oam1 & 0x4000) ? size - 1 - col : col;
      const int tile = ((((oam1 & 0xff) >> 4) + (int)(row >> 3)) << 4) |
                       (((oam1 & 0xf) + (used_col >> 3)) & 0xf);
      const int a = obj_adr + tile * 16 + (int)(row & 7);
      const uint16_t w0 = ppu->vram[a & 0x7fff], w1 = ppu->vram[(a + 8) & 0x7fff];
      const uint64_t c = tab[w0 & 0xff] | tab[w0 >> 8] << 1 | tab[w1 & 0xff] << 2 | tab[w1 >> 8] << 3;
      if (!c) continue;
      /* Byte mask of the opaque pixels (colour 1-15 in each byte). */
      const uint64_t m = ((c | c >> 1 | c >> 2 | c >> 3) & 0x0101010101010101ull) * 0xff;
      uint8_t *const px = pix + col + sx, *const pr = pri + col + sx;
      uint64_t v, q;
      memcpy(&v, px, 8);
      memcpy(&q, pr, 8);
      v = (v & ~m) | ((c + pal) & m);          /* lower OAM index drawn later wins */
      q = (q & ~m) | (prio & m);
      memcpy(px, &v, 8);
      memcpy(pr, &q, 8);
      any = true;
    }
  }
  if (!any) return;
  unsigned used = 0;                         /* priority planes written */
  for (int x0 = 0; x0 < 256; x0 += 8) {
    uint64_t v;
    memcpy(&v, pix + x0, 8);
    if (!v) continue;
    for (int x = x0; x < x0 + 8; ++x) {
      if (!pix[x]) continue;
      plane_row(f, N64PPU_OBJ0 + pri[x], r)[N64PPU_PAD + x] = pix[x];
      used |= 1u << pri[x];
    }
  }
  for (int p = 0; p < 4; ++p)
    if (used & (1u << p)) {
      bit_set(f->rows[N64PPU_OBJ0 + p], r);
      bit_set(s_state.dirty[N64PPU_OBJ0 + p], r);
    }
}

/* ---- Lines --------------------------------------------------------------- */

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

/* Clear a line's rows in every plane (BG runs for it are flushed first). */
static void clear_line(N64PpuFrame *f, int r) {
  for (int p = 0; p < N64PPU_PLANES; ++p) clear_if_dirty(f, p, r);
}

static void flush_all(N64PpuFrame *f) {
  for (int screen = 0; screen < 2; ++screen)
    for (int layer = 0; layer < 4; ++layer) flush_run(f, screen, layer);
}

bool n64_ppu_build_line(const Ppu *ppu, int line) {
  N64PpuFrame *f = s_state.frame;
  s_state.ppu = ppu;
  const int r = line - 1;
  N64PpuLine *L = &f->line[r];
  memset(L, 0, sizeof *L);
  if (PPU_forcedBlank(ppu)) {
    flush_all(f);
    clear_line(f, r);
    L->kind = N64PPU_LINE_BLACK;
    f->black_lines++;
    return true;
  }
  Spans *s = &s_state.spans[r];
  compute_spans(ppu, s);
  bool backdrop_math;
  uint32_t why = line_unsupported(ppu, s, &backdrop_math);

  if (!why) {
    bool full = false;
    const uint16_t bd = ppu->cgram[0];
    const unsigned b = PPU_brightness(ppu);
    for (int sel = 0; sel < N64PPU_SELS; ++sel)
      L->tl_main[sel] = get_tlut(f, ppu, kTlutNormal, (uint8_t)sel, 0, 0, &full);
    L->fill = pack5551(bright(bd & 31, b, false), bright((bd >> 5) & 31, b, false),
                       bright((bd >> 10) & 31, b, false));
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
        const uint8_t cflags = (uint8_t)(mflags & ~kMathHalf);
        for (int sel = 0; sel < N64PPU_SEL_OBJ; ++sel) {
          L->tl_math[sel] = get_tlut(f, ppu, kTlutMath, (uint8_t)sel, bd, mflags, &full);
          L->tl_math_clip[sel] = get_tlut(f, ppu, kTlutMath, (uint8_t)sel, 0, cflags, &full);
        }
      }
    }
    if (full) why |= N64PPU_WHY_PALETTES;
  }
  if (why) {
    memset(L, 0, sizeof *L);
    flush_all(f);
    clear_line(f, r);
    L->kind = N64PPU_LINE_SOFTWARE;
    f->fallback_reasons |= why;
    f->software_lines++;
    return false;
  }

  L->kind = N64PPU_LINE_RDP;
  L->order = PPU_mode(ppu) == 0 ? N64PPU_ORDER_MODE0
           : (PPU_bg3priority(ppu) ? N64PPU_ORDER_MODE1_BG3TOP : N64PPU_ORDER_MODE1);
  L->nspans = (uint8_t)s->n;
  for (int k = 0; k < s->n; ++k) {
    L->spans[k].x0 = s->x0[k];
    L->spans[k].x1 = s->x1[k];
    L->spans[k].clip = s->clip[k];
    L->spans[k].math = (uint8_t)(backdrop_math && s->math[k]);
  }

  /* Background layers of both screens: queued (rows rewritten at flush);
   * layers off on this line get their row cleared now. */
  const int nbg = PPU_mode(ppu) == 0 ? 4 : 3;
  const uint8_t enabled[2] = {ppu->screenEnabled[0], L->sub_layers};
  for (int screen = 0; screen < 2; ++screen) {
    for (int layer = 0; layer < 4; ++layer) {
      const int plane = (screen ? N64PPU_SUB_BG1 : N64PPU_BG1) + layer;
      if (layer < nbg && (enabled[screen] & (1 << layer))) {
        const int bpp = (PPU_mode(ppu) == 0 || layer == 2) ? 2 : 4;
        const unsigned pal_base = PPU_mode(ppu) == 0 ? 32u * layer : 0;
        queue_bg(f, ppu, screen, layer, line, bpp, pal_base);
      } else {
        flush_run(f, screen, layer);
        clear_if_dirty(f, plane, r);
      }
    }
  }
  /* Sprites: per line (they read OAM and VRAM live). */
  for (int p = N64PPU_OBJ0; p <= N64PPU_OBJ3; ++p) clear_if_dirty(f, p, r);
  if (ppu->screenEnabled[0] & 0x10) {
    expand_obj(f, ppu, line);
    for (int p = N64PPU_OBJ0; p <= N64PPU_OBJ3; ++p)
      apply_mask(f, s, s->mask_main, 4, p, r);
  }
  f->rdp_lines++;
  return true;
}

void n64_ppu_build_end(void) {
  flush_all(s_state.frame);
}
