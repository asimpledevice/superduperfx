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
#include "n64_memeq.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/ppu_brightness.h"
#if N64_RSPBG
#include "bg_rsp.h"
#endif

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

/* Claim the 16-byte data-cache lines of a plane row that a BG row write
 * replaces, without reading them from RDRAM first (VR4300 CACHE op 0x0D,
 * Create Dirty Exclusive). A BG row rewrites bytes [8-f, 272-f) (f = fine
 * scroll 0..7), which covers lines 1..15 in full; lines 0 and 16 are
 * claimed too: their other bytes are row margins outside [N64PPU_PAD,
 * N64PPU_PAD + 256), which the RDP never uploads, so their contents do not
 * matter. A row is exactly 17 lines (stride 272, 16-aligned planes), so no
 * neighbouring row is touched. No-op off the N64. */
static inline void claim_row_lines(uint8_t *row) {
#if defined(__mips__) && defined(N64)
  _Static_assert(N64PPU_STRIDE == 17 * 16, "plane rows must be whole cache lines");
  for (int i = 0; i <= 16; ++i)
    __asm__ volatile("cache 0x0D, 0(%0)" : : "r"(row + 16 * i) : "memory");
#else
  (void)row;
#endif
}
/* Unaligned 8-byte store (sdl/sdr on MIPS; memcpy went through the stack). */
typedef struct __attribute__((packed, may_alias)) { uint64_t v; } Unaligned64;
static inline void store8(uint8_t *dst, uint64_t v) { ((Unaligned64 *)dst)->v = v; }

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

/* The previous RDP line's palettes and backdrop colours, and their inputs
 * (palette slots are per frame: cleared in n64_ppu_build_begin). */
typedef struct {
  uint32_t cgram_count;
  uint16_t bd, fixed;
  uint8_t brightness, mode, sub, pad;
} LinePalKey;
typedef struct { LinePalKey key; N64PpuLine line; bool full, valid; } LinePalCache;

/* The previous line's window spans and the registers they come from. */
typedef struct {
  uint32_t windowsel;
  uint16_t wbgobjlog;
  uint8_t w[4], windowed[2], cgwsel, pad;
} SpanKey;
typedef struct { SpanKey key; Spans spans; bool valid; } SpanCache;

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
  /* Sized as the per-plane dirty bits once were, so this struct (and every
   * static placed after it) keeps its layout. */
  union {
    uint32_t layout_[N64PPU_PLANES][(N64PPU_LINES + 31) / 32];
    struct {
      /* Sprite rows that may hold non-zero bytes (persistent across
       * frames), per OBJ plane. A row that is not rewritten on a line is
       * cleared before it can be drawn. */
      uint32_t dirty[4][(N64PPU_LINES + 31) / 32];
      LinePalCache linepal;
      SpanCache lastspans;
    };
  };
  struct TlutKey keys[N64PPU_MAX_TLUTS];   /* parallel to frame->tlut */
  Spans spans[N64PPU_LINES];               /* per line, for deferred masks */
} BuildState;

static BuildState s_state;
_Static_assert(sizeof(((BuildState *)0)->layout_) >=
               sizeof(uint32_t[4][(N64PPU_LINES + 31) / 32]) + sizeof(LinePalCache) + sizeof(SpanCache),
               "per-line caches must fit the old dirty-bit space");

N64PpuFrame *n64_ppu_build_frame_alloc(void) {
  init_tables();
  N64PpuFrame *f = calloc(1, sizeof(*f));
  if (!f) return NULL;
  for (int p = 0; p < N64PPU_PLANES; ++p) {
    f->plane[p] = memalign(16, (size_t)N64PPU_STRIDE * N64PPU_LINES);
    if (!f->plane[p]) return NULL;
    memset(f->plane[p], 0, (size_t)N64PPU_STRIDE * N64PPU_LINES);
#if N64_RSPBG
    if (p < N64PPU_OBJ0 || p >= N64PPU_SUB_BG1)   /* BG planes: the RSP writes them */
      n64_bgrsp_cpu_wrote(f->plane[p], (unsigned)N64PPU_STRIDE * N64PPU_LINES);
#endif
  }
#if N64_RSPBG
  n64_bgrsp_init();
#endif
  return f;
}

static inline uint8_t *plane_row(N64PpuFrame *f, int plane, int r) {
  return f->plane[plane] + (size_t)r * N64PPU_STRIDE;
}

static inline void bit_set(uint32_t *bits, int r) { bits[r >> 5] |= 1u << (r & 31); }
static inline bool bit_get(const uint32_t *bits, int r) { return (bits[r >> 5] >> (r & 31)) & 1; }

/* Rows with a dirty bit in any OBJ plane: one word to look at per line. */
static uint32_t s_obj_dirty_rows[(N64PPU_LINES + 31) / 32];

static inline void clear_if_dirty(N64PpuFrame *f, int plane, int r) {   /* OBJ planes */
  uint32_t *dirty = s_state.dirty[plane - N64PPU_OBJ0];
  if (bit_get(dirty, r)) {
    memset(plane_row(f, plane, r), 0, N64PPU_STRIDE);
    dirty[r >> 5] &= ~(1u << (r & 31));
  }
}

/* ---- Palettes ------------------------------------------------------------ */

enum { kTlutNormal = 1, kTlutMath = 2, kTlutFixed = 3 };
enum { kMathSubtract = 1, kMathHalf = 2, kMathClip = 4 };

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
    case N64PPU_SEL_M7:   return i ? i : -1;
    case N64PPU_SEL_M7D:  return i ? i : -1;                         /* (its colour: sel_color) */
    case N64PPU_SEL_M7E_LO: return (i < 128 && i) ? i : -1;
    case N64PPU_SEL_M7E_HI: return (i > 128) ? i - 128 : -1;
    default:              return (i >= 128 && (i & 15)) ? i : -1;   /* OBJ */
  }
}

/* The 15-bit colour of source entry src under selector sel: CGRAM, or with
 * direct colour the entry number itself as BBGGGRRR (ppu_mode7_color). */
static inline uint16_t sel_color(const Ppu *ppu, int sel, int src) {
  if (sel == N64PPU_SEL_M7D)
    return (uint16_t)(((src & 7) << 2) | ((src & 56) << 4) | ((src & 192) << 7));
  return ppu->cgram[src];
}

static uint8_t get_tlut_slow(N64PpuFrame *f, const Ppu *ppu, uint8_t kind, uint8_t sel,
                             uint16_t backdrop, uint8_t flags, bool *full) {
  struct TlutKey key;
  memset(&key, 0, sizeof key);
  key.cgram_count = ppu->cgramWriteCount;
  key.kind = kind;
  key.brightness = PPU_brightness(ppu);
  key.sel = sel;
  if (kind != kTlutNormal) {
    key.backdrop = backdrop;
    key.flags = flags;
  }
  for (int i = N64PPU_SELS; i < f->tlut_count; ++i)
    if (n64_mem_eq(&s_state.keys[i], &key, sizeof key)) return (uint8_t)i;
  if (f->tlut_count == N64PPU_MAX_TLUTS) { *full = true; return 0; }
  const int i = f->tlut_count++;
  s_state.keys[i] = key;
  uint16_t *t = f->tlut[i];
  const unsigned b = key.brightness;
  for (int c = 0; c < 256; ++c) {
    const int src = sel_source(sel, c);
    if (src < 0) { t[c] = 0; continue; }
    const uint16_t raw = sel_color(ppu, sel, src);
    if (kind == kTlutFixed && (sel != N64PPU_SEL_OBJ || c >= 192)) {
      /* Main colour (zero where clipped) with the fixed colour (`backdrop`);
       * OBJ palettes 0-3 take no math. */
      t[c] = math_color((flags & kMathClip) ? 0 : raw, backdrop, flags, b, (flags & kMathHalf) != 0);
      continue;
    }
    if (kind == kTlutFixed && (flags & kMathClip)) { t[c] = 1; continue; }
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
    s_tlut_memo[3][N64PPU_SELS][8];

static uint8_t get_tlut(N64PpuFrame *f, const Ppu *ppu, uint8_t kind, uint8_t sel,
                        uint16_t backdrop, uint8_t flags, bool *full) {
  typeof(s_tlut_memo[0][0][0]) *m = &s_tlut_memo[kind - 1][sel][flags & 7];
  const uint16_t bd = kind != kTlutNormal ? backdrop : 0;
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


#if N64PPU_SCENE
#include "tile_cache_layout.h"
/* A scene-builder TLUT in the tile cache's arrangement (tile_cache.h:
 * TC_TLUT_16, or TC_TLUT_2BPP with palette base `base` in the low half)
 * for the colour rules of n64_ppu_draw.c's plane palettes:
 * N64PPU_SCENE_TL_BLACK main pixels clipped to black (as
 * N64PPU_TLUT_BLACK), N64PPU_SCENE_TL_MATH subscreen pixels with backdrop
 * math (as tl_math), N64PPU_SCENE_TL_MATH_CLIP the same with the backdrop
 * clipped (tl_math_clip). The registers are the band's; the backdrop is
 * cgram[0]. (Plain palettes come from tc_ref_tlut.) */
void n64_ppu_scene_tlut(const uint16_t *cgram, const struct Ppu *regs, unsigned kind, unsigned base,
                        unsigned mode, uint16_t *out) {
  const unsigned b = PPU_brightness(regs);
  const uint8_t mflags = (uint8_t)((PPU_subtractColor(regs) ? kMathSubtract : 0) |
                                   (PPU_halfColor(regs) ? kMathHalf : 0));
  /* math_color one component at a time: for each component k, the result
   * for every source value, already in its RGBA5551 position. */
  uint16_t comp[3][32];
  const bool fixed = mode == N64PPU_SCENE_TL_FIXED || mode == N64PPU_SCENE_TL_FIXED_CLIP;
  if (fixed) {
    /* Main colour (zero where clipped) with the fixed colour; halved only
     * unclipped and without addSubscreen (PpuDrawWholeLine). */
    const bool clip = mode == N64PPU_SCENE_TL_FIXED_CLIP;
    const bool half = !clip && (mflags & kMathHalf) && !PPU_addSubscreen(regs);
    for (int k = 0; k < 3; ++k) {
      const unsigned y = (regs->fixedColor >> (5 * k)) & 31;
      for (unsigned c = 0; c < 32; ++c) {
        const unsigned x = clip ? 0 : c;
        const unsigned v = (mflags & kMathSubtract) ? (x >= y ? x - y : 0) : x + y;
        comp[k][c] = (uint16_t)(bright(v, b, half) << (11 - 5 * k));
      }
    }
  } else if (mode != N64PPU_SCENE_TL_BLACK) {
    const uint16_t bd = mode == N64PPU_SCENE_TL_MATH ? cgram[0] : 0;
    const uint8_t fl = mode == N64PPU_SCENE_TL_MATH ? mflags : (uint8_t)(mflags & ~kMathHalf);
    const bool half = mode == N64PPU_SCENE_TL_MATH && (mflags & kMathHalf);
    for (int k = 0; k < 3; ++k) {
      const unsigned x = (bd >> (5 * k)) & 31;
      for (unsigned y = 0; y < 32; ++y) {
        const unsigned v = (fl & kMathSubtract) ? (x >= y ? x - y : 0) : x + y;
        comp[k][y] = (uint16_t)(bright(v, b, half) << (11 - 5 * k));
      }
    }
  }
  for (int i = 0; i < 256; ++i) {
    int src;
    if (kind == TC_TLUT_16) src = (i & 15) ? i : -1;
    else {
      const int j = i & 15, q = (i >> 4) & 7;
      src = (i < 128 && j >= 1 && j <= 3) ? (int)((base + 4u * (unsigned)q + (unsigned)j) & 255) : -1;
    }
    if (src < 0) { out[i] = 0; continue; }
    if (mode == N64PPU_SCENE_TL_BLACK) { out[i] = 1; continue; }
    const uint16_t raw = cgram[src];
    if (fixed && kind == TC_TLUT_16 && i >= 128 && i < 192) {   /* OBJ palettes 0-3: no math */
      out[i] = mode == N64PPU_SCENE_TL_FIXED_CLIP ? 1
             : pack5551(bright(raw & 31, b, false), bright((raw >> 5) & 31, b, false),
                        bright((raw >> 10) & 31, b, false));
      continue;
    }
    out[i] = (uint16_t)(comp[0][raw & 31] | comp[1][(raw >> 5) & 31] | comp[2][(raw >> 10) & 31] | 1);
  }
}

static void obj_scene_frame_begin(void);
#endif

void n64_ppu_build_begin(N64PpuFrame *f, const uint32_t *sw_rows) {
  s_state.linepal.valid = false;
  s_state.lastspans.valid = false;
  s_state.frame = f;
#if N64_RSPBG
  n64_bgrsp_frame_begin();
#endif
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
#if N64PPU_SCENE
  obj_scene_frame_begin();
#endif
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

/* ---- Backgrounds ----------------------------------------------------------- */

#if N64_RSPBG
/* Runs queued on the RSP this frame; their row flags are merged at the end. */
static struct { int16_t job; uint8_t plane, r0, n; } s_pend[64];
static int s_npend;
#ifdef N64_RSPBG_CHECK
static struct { BgRun run; } s_pend_run[64];
#endif

static bool flush_run_rsp(N64PpuFrame *f, const BgRun *run, int plane) {
  const Ppu *ppu = s_state.ppu;
  if (s_npend == (int)(sizeof s_pend / sizeof s_pend[0])) return false;
  n64_bgrsp_sync_vram(ppu->vram, ppu->vramWriteCount);
  const int r0 = run->l0 - 1, n = run->l1 - run->l0;
  const int job = n64_bgrsp_submit(ppu->vram, plane_row(f, plane, r0) + N64PPU_PAD, n,
                                   (uint16_t)(run->l0 + run->vs), run->hs, run->tileadr,
                                   run->xsc, run->bpp, run->pal_base);
  if (job < 0) return false;
  s_pend[s_npend].job = (int16_t)job;
  s_pend[s_npend].plane = (uint8_t)plane;
  s_pend[s_npend].r0 = (uint8_t)r0;
  s_pend[s_npend].n = (uint8_t)n;
#ifdef N64_RSPBG_CHECK
  s_pend_run[s_npend].run = *run;
#endif
  s_npend++;
  return true;
}

#ifdef N64_RSPBG_CHECK
/* Debug build option: after the RSP finishes, compare every row it wrote
 * with a CPU expansion of the same run (the rules of flush_run below), and
 * log a per-frame count and the first difference. */
extern void n64_platform_log(const char *, ...);
static unsigned s_chk_bad, s_chk_rows, s_chk_frame;
static void check_rsp_rows(N64PpuFrame *f, int k) {
  const BgRun *run = &s_pend_run[k].run;
  const uint16_t *vram = s_state.ppu->vram;
  const bool wider = run->xsc & 1, higher = run->xsc & 2;
  const int tilemap = (run->xsc & 0xfc) << 8;
  for (int i = 0; i < s_pend[k].n; ++i) {
    const unsigned y = (unsigned)(run->l0 + i) + run->vs;
    int sc = tilemap + (((y >> 3) & 0x1f) << 5);
    if ((y & 0x100) && higher) sc += wider ? 0x800 : 0x400;
    uint8_t want[N64PPU_STRIDE];
    memset(want, 0, sizeof want);
    for (int t = 0; t < 33; ++t) {
      const unsigned mx = (run->hs >> 3) + (unsigned)t;
      const int map = ((mx >> 5) & 1) && wider ? sc + 0x400 : sc;
      const uint16_t entry = vram[(map & 0x7fff) + (mx & 31)];
      const unsigned yy = y & 7, row = (entry & 0x8000) ? 7 - yy : yy;
      const int base = run->tileadr + (entry & 0x3ff) * (run->bpp == 4 ? 16 : 8);
      const uint16_t w0 = vram[(base + row) & 0x7fff];
      const uint16_t w1 = run->bpp == 4 ? vram[(base + row + 8) & 0x7fff] : 0;
      const uint8_t pal = (uint8_t)(((entry & 0x2000) ? 128 : 0) + run->pal_base +
                                    ((entry >> 10) & 7) * (run->bpp == 4 ? 16 : 4));
      for (int j = 0; j < 8; ++j) {
        const int b = (entry & 0x4000) ? j : 7 - j;
        const unsigned c = ((w0 >> b) & 1) | ((w0 >> (b + 8)) & 1) << 1 |
                           ((w1 >> b) & 1) << 2 | ((w1 >> (b + 8)) & 1) << 3;
        const int pos = N64PPU_PAD - (int)(run->hs & 7) + t * 8 + j;
        if (pos >= 0 && pos < N64PPU_STRIDE) want[pos] = (uint8_t)(c | pal);
      }
    }
    const uint8_t *got = (const uint8_t *)((uintptr_t)plane_row(f, s_pend[k].plane, s_pend[k].r0 + i) |
                                           0x20000000u);   /* uncached */
    s_chk_rows++;
    if (memcmp(got + N64PPU_PAD, want + N64PPU_PAD, 256)) {
      if (!s_chk_bad++) {
        int x = 0;
        while (got[N64PPU_PAD + x] == want[N64PPU_PAD + x]) ++x;
        n64_platform_log("[bgchk] plane %d row %d x %d: rsp %02x cpu %02x (bpp %d hs %u y %u)\n",
                         s_pend[k].plane, s_pend[k].r0 + i, x, got[N64PPU_PAD + x],
                         want[N64PPU_PAD + x], run->bpp, run->hs, y);
      }
    }
  }
}
#endif

static void merge_rsp_flags(N64PpuFrame *f) {
  if (!s_npend) return;
  n64_bgrsp_wait();
#ifdef N64_RSPBG_CHECK
  for (int k = 0; k < s_npend; ++k) check_rsp_rows(f, k);
  if (s_chk_bad || s_chk_frame % 120 == 0)
    n64_platform_log("[bgchk] frame %u: %u of %u rows differ\n", s_chk_frame, s_chk_bad, s_chk_rows);
  s_chk_frame++;
  s_chk_bad = s_chk_rows = 0;
#endif
  for (int k = 0; k < s_npend; ++k) {
    const uint8_t *fl = n64_bgrsp_flags(s_pend[k].job);
    const int plane = s_pend[k].plane;
    for (int i = 0; i < s_pend[k].n; ++i) {
      const int r = s_pend[k].r0 + i;
      if (fl[i] & 1) bit_set(f->rows[plane], r);
      if (fl[i] & 2) bit_set(f->rows_hi[plane], r);
    }
  }
  s_npend = 0;
}
#endif

/* Expand a deferred run, tile-major. */
#ifdef N64PPU_SCENE_CHECK
/* Scene builder check (native): the run's quads (n64_ppu_scene.c), drawn
 * from the tile cache's C reference the way the RDP will draw them, must
 * reproduce the plane rows flush_run has just written, byte for byte. */
#include <stdio.h>
#include "n64_ppu_scene.h"
#include "tile_cache.h"
static uint32_t s_scn_runs, s_scn_rows, s_scn_bad, s_scn_quads;
static void scene_check(N64PpuFrame *f, const BgRun *run, int plane) {
  static N64SceneQuad q[33 * 30];
  static uint8_t row[N64PPU_LINES][256];
  const N64SceneBg bg = {.hs = run->hs, .vs = run->vs, .xsc = run->xsc, .bpp = run->bpp,
                         .tileadr = run->tileadr};
  const int r0 = run->l0 - 1, r1 = run->l1 - 1;
  const uint16_t *vram = s_state.ppu->vram;
  const int n = n64_scene_bg(&bg, vram, r0, r1, q, (int)(sizeof q / sizeof q[0]));
  s_scn_quads += (uint32_t)n;
#ifdef N64PPU_SCENE_CHECK_BREAK   /* the check must fail: one tile drawn unflipped */
  if (n > 5) q[5].flags &= (uint8_t)~N64SCENE_HFLIP, q[5].pal ^= 1;
#endif
  uint8_t tile[TC_CI4_TILE];
  for (int k = 0; k < n; ++k) {
    tc_ref_tile(vram, run->bpp, q[k].tile, tile);
    const unsigned pal = (q[k].flags & N64SCENE_PRIO ? 128u : 0u) + run->pal_base +
                         q[k].pal * (run->bpp == 4 ? 16u : 4u);
    for (int r = q[k].r0; r < q[k].r1; ++r) {
      unsigned tr = q[k].row + (unsigned)(r - q[k].r0);
      if (q[k].flags & N64SCENE_VFLIP) tr = 7 - tr;
      for (int j = 0; j < 8; ++j) {
        const int x = q[k].x0 + j;
        if (x < 0 || x >= 256) continue;
        const int s = (q[k].flags & N64SCENE_HFLIP) ? 8 + j : j;
        const uint8_t b = tile[8 * tr + s / 2];
        row[r][x] = (uint8_t)(pal + (s & 1 ? b & 15 : b >> 4));
      }
    }
  }
  s_scn_runs++;
  for (int r = r0; r < r1; ++r) {
    s_scn_rows++;
    if (memcmp(row[r], plane_row(f, plane, r) + N64PPU_PAD, 256)) {
      if (s_scn_bad++ < 8)
        fprintf(stderr, "[scene] plane %d row %d differs (hs %u vs %u xsc %02x bpp %u)\n", plane, r,
                run->hs, run->vs, run->xsc, run->bpp);
    }
  }
  if (s_scn_runs % 1000 == 0)
    fprintf(stderr, "[scene] %lu runs, %lu rows, %lu differ, %.1f quads a run\n",
            (unsigned long)s_scn_runs, (unsigned long)s_scn_rows, (unsigned long)s_scn_bad,
            (double)s_scn_quads / s_scn_runs);
}
#endif

static void flush_run(N64PpuFrame *f, int screen, int layer) {
  BgRun *run = &s_state.runs[screen][layer];
  if (!run->active) return;
  run->active = false;
  const int plane = (screen ? N64PPU_SUB_BG1 : N64PPU_BG1) + layer;
#if N64_RSPBG
  if (flush_run_rsp(f, run, plane)) return;
#endif
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
#if N64_RSPBG
      n64_bgrsp_cpu_wrote(plane_row(f, plane, r), N64PPU_STRIDE);
#endif
      if (any_lo[i]) bit_set(f->rows[plane], r);
      if (any_hi[i]) bit_set(f->rows_hi[plane], r);
    }
    g0 = g1;
  }
#ifdef N64PPU_SCENE_CHECK
  scene_check(f, run, plane);
#endif
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
  uint32_t math_pal[4];   /* sprites of palettes 4-7: the ones colour math applies to */
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
  memset(s_obj_lines.math_pal, 0, sizeof s_obj_lines.math_pal);
  for (unsigned spr = 0; spr < 128; ++spr) {
    const unsigned index = spr * 2;
    if (ppu->oam[index + 1] & 0x0800) s_obj_lines.math_pal[spr >> 5] |= 1u << (spr & 31);
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
static inline void obj_lines_ensure(const Ppu *ppu) {
  if (!s_obj_lines.valid || s_obj_lines.ppu != ppu || s_obj_lines.oam_count != ppu->oamWriteCount ||
      s_obj_lines.obj_size != PPU_objSize(ppu))
    obj_lines_build(ppu);
}

static void expand_obj(N64PpuFrame *f, const Ppu *ppu, int line) {
  const int r = line - 1;
  const int eval_line = line - 1;            /* the PPU evaluates line - 1 */
  obj_lines_ensure(ppu);
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
      bit_set(s_state.dirty[p], r);
      bit_set(s_obj_dirty_rows, r);
    }
}

#if N64PPU_SCENE
/* ---- Sprites for the scene builder ---------------------------------------
 * On a line whose sprites the scene draws (N64PpuLine.obj_scene), every
 * sprite in range is drawn whole as copy-mode rectangles from the 4bpp tile
 * cache, one priority pass at a time, back to front in OAM rotation order
 * (n64_ppu_draw.c scene_draw_obj). That equals expand_obj exactly when
 *   - no sprite is dropped: at most 32 in range and 34 tiles (range and
 *     time over), and
 *   - no sprite is in front of (earlier in rotation than) a sprite of
 *     higher priority it overlaps: on hardware the front sprite's pixel
 *     wins and takes its own, lower, priority, hiding the other sprite
 *     there; per-priority passes would show the back sprite instead.
 *     (Front sprites of higher or equal priority are fine: they are drawn
 *     later, over the back one.) Checked conservatively on the sprites'
 *     on-screen x ranges.
 * Other lines keep the OBJ planes. The strips come from OAM and OBSEL as
 * of the frame's first such line; a line after either changes keeps the
 * planes. The pixels come from the tile cache, synced from the frame's
 * VRAM snapshot, as for the BG layers. */
static struct {
  uint32_t oam_count;
  uint8_t obsel, built, ok;
  int8_t vis[128];          /* on-screen tile columns per sprite */
  int16_t x0[128], x1[128];  /* on-screen x range [x0, x1) */
  /* The last line's verdict, reused while its sprites and rotation repeat. */
  uint32_t last_mask[4];
  uint8_t last_first, last_valid, last_ok;
} s_objscene;
uint32_t g_scene_obj_lines, g_scene_obj_plane_lines;

static void obj_scene_frame_begin(void) { s_objscene.built = 0; }

static bool obj_strips_build(N64PpuFrame *f, const Ppu *ppu) {
  int n = 0;
  for (unsigned spr = 0; spr < 128; ++spr) {
    f->obj_start[spr] = (uint16_t)n;
    const unsigned index = spr * 2;
    const int size = obj_size_of(ppu, index);
    const int sx = obj_x_of(ppu, index);
    const unsigned oam1 = ppu->oam[index + 1];
    f->obj_prio[spr] = (uint8_t)((oam1 >> 12) & 3);
    s_objscene.vis[spr] = 0;
    s_objscene.x0[spr] = (int16_t)(sx < 0 ? 0 : sx);
    s_objscene.x1[spr] = (int16_t)(sx + size > 256 ? 256 : sx + size);
    if (sx + size <= 0) continue;                 /* never in range (obj_lines_build) */
    for (int col = 0; col < size; col += 8)
      if (col + sx > -8 && col + sx < 256) s_objscene.vis[spr]++;
    const unsigned y = ppu->oam[index] >> 8;
    const int obj_adr = (oam1 & 0x100) ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
    const bool hflip = (oam1 & 0x4000) != 0, vflip = (oam1 & 0x8000) != 0;
    const uint8_t flags = (uint8_t)(((oam1 >> 9) & 7) | (hflip ? N64PPU_OBJ_HFLIP : 0) |
                                    (vflip ? N64PPU_OBJ_VFLIP : 0));
    for (int j = 0; j < size / 8; ++j) {
      /* Sprite rows 8j..8j+7 sit on screen rows (y + 8j + d) & 255. */
      const unsigned start = (y + 8u * (unsigned)j) & 255;
      for (int part = 0; part < 2; ++part) {
        int r0, r1, d0;
        if (!part) { r0 = (int)start; r1 = start + 8 > 256 ? 256 : (int)start + 8; d0 = 0; }
        else if (start + 8 > 256) { r0 = 0; r1 = (int)start + 8 - 256; d0 = 256 - (int)start; }
        else break;
        if (r1 > N64PPU_LINES) r1 = N64PPU_LINES;
        if (r0 >= r1) continue;
        const int trow = vflip ? size / 8 - 1 - j : j;
        for (int col = 0; col < size; col += 8) {
          if (col + sx <= -8 || col + sx >= 256) continue;
          const int ucol = hflip ? size / 8 - 1 - col / 8 : col / 8;
          const int tile = ((((oam1 & 0xff) >> 4) + trow) << 4) | (((oam1 & 0xf) + ucol) & 0xf);
          if (n == N64PPU_MAX_OBJ_STRIPS) return false;
          N64PpuObjStrip *st = &f->obj_strip[n++];
          st->tile = (uint16_t)(((obj_adr + tile * 16) & 0x7fff) >> 4);
          st->x = (int16_t)(sx + col);
          st->r0 = (uint8_t)r0;
          st->r1 = (uint8_t)r1;
          st->d0 = (uint8_t)d0;
          st->flags = flags;
        }
      }
    }
  }
  f->obj_start[128] = (uint16_t)n;
  return true;
}

/* Whether the scene draws line r's sprites (see above). */
static bool obj_line_scene(N64PpuFrame *f, const Ppu *ppu, int r) {
  if (!s_objscene.built) {
    s_objscene.built = 1;
    s_objscene.oam_count = ppu->oamWriteCount;
    s_objscene.obsel = ppu->obsel;
    s_objscene.ok = obj_strips_build(f, ppu);
    s_objscene.last_valid = 0;
  }
  if (!s_objscene.ok || s_objscene.oam_count != ppu->oamWriteCount || s_objscene.obsel != ppu->obsel)
    return false;
  const uint32_t *m = s_obj_lines.mask[r];
  const unsigned first = PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) >> 1 : 0;
  if (s_objscene.last_valid && s_objscene.last_first == first &&
      n64_mem_eq(s_objscene.last_mask, m, sizeof s_objscene.last_mask))
    return s_objscene.last_ok;
  bool ok = true;
  int n = 0, tiles = 0;
  for (int w = 0; w < 4; ++w) n += __builtin_popcount(m[w]);
  if (n > 32) ok = false;
  uint8_t found[32];
  if (ok) {
    obj_lines_collect(m, first, found);   /* front to back */
    for (int i = 0; i < n && ok; ++i) {
      const unsigned a = found[i] >> 1;
      tiles += s_objscene.vis[a];
      for (int k = i + 1; k < n; ++k) {   /* a in front of b */
        const unsigned b = found[k] >> 1;
        if (f->obj_prio[a] < f->obj_prio[b] && s_objscene.x0[a] < s_objscene.x1[b] &&
            s_objscene.x0[b] < s_objscene.x1[a]) {
          ok = false;
          break;
        }
      }
    }
    if (tiles > 34) ok = false;
  }
  memcpy(s_objscene.last_mask, m, sizeof s_objscene.last_mask);
  s_objscene.last_first = (uint8_t)first;
  s_objscene.last_ok = ok;
  s_objscene.last_valid = 1;
  return ok;
}
#endif

/* ---- Lines --------------------------------------------------------------- */

/* Colour math between a main-screen layer and a composed subscreen needs
 * two colours a pixel, which the RDP path does not have (software lines).
 * When OBJ is the only main-screen layer with math, only sprites of
 * palettes 4-7 take it, so a line without one in range has no such pixel.
 * objmath_regs: the registers say so; objmath_line: and row r holds one. */
static inline bool objmath_regs(const Ppu *ppu) {
  return (PPU_mathEnabled(ppu) & ppu->screenEnabled[0] & 0x1f) == 0x10 && PPU_addSubscreen(ppu) &&
         (ppu->screenEnabled[1] & 0x1f);
}
static __attribute__((noinline)) bool objmath_line(const Ppu *ppu, int r) {
  if (!objmath_regs(ppu)) return false;
  obj_lines_ensure(ppu);
  const uint32_t *m = s_obj_lines.mask[r], *h = s_obj_lines.math_pal;
  return ((m[0] & h[0]) | (m[1] & h[1]) | (m[2] & h[2]) | (m[3] & h[3])) != 0;
}

static uint32_t line_unsupported(const Ppu *ppu, const Spans *s, int r, bool *backdrop_math, uint8_t *lmath) {
  uint32_t why = 0;
  const int mode = PPU_mode(ppu);
  /* Mode 7 has BG1 and OBJ (BG2 only with EXTBG, which is not drawn here,
   * nor direct colour). */
  const uint8_t lmask = mode != 7 ? 0x1f : PPU_m7extBg(ppu) ? 0x13 : 0x11;
  const uint8_t main = ppu->screenEnabled[0] & lmask, sub = ppu->screenEnabled[1] & lmask;
  if (mode > 1 && mode != 7) why |= N64PPU_WHY_MODE;
  /* (mode 7's mosaic is the sampler's: mode7_row) */
  if (mode != 7 && PPU_mosaicSize(ppu) > 1 && (ppu->mosaic & (main | sub) & 0xf)) why |= N64PPU_WHY_MOSAIC;
#if !N64PPU_SCENE   /* (the scene draws 16x16 tiles from the tile cache; the planes cannot) */
  if ((ppu->bgmode >> 4) & (main | sub) & 0xf) why |= N64PPU_WHY_BIG_TILES;
#endif
  if (ppu->setini & 0x0b) why |= N64PPU_WHY_HIRES_INTERLACE;
  /* Colour math is visible only where some span allows it and it changes
   * something: a non-zero fixed colour, halving, or a composed subscreen. */
  *backdrop_math = false;
  *lmath = 0;
  bool any_math_span = false;
  for (int k = 0; k < s->n; ++k) any_math_span |= s->math[k] != 0;
  const bool nontrivial = ppu->fixedColor || PPU_halfColor(ppu) ||
                          (PPU_addSubscreen(ppu) && sub);
  if (PPU_mathEnabled(ppu) && any_math_span && nontrivial) {
    /* Layer math with the fixed colour is a palette (tl_fix); with a
     * composed subscreen it needs two colours per pixel. */
    const uint8_t layers = (uint8_t)(PPU_mathEnabled(ppu) & main & 0x1f);
    if (layers && PPU_addSubscreen(ppu) && (sub & 0x1f)) {
      if (layers != 0x10 || objmath_line(ppu, r)) why |= N64PPU_WHY_COLOR_MATH;
    } else *lmath = layers;
    if (PPU_mathEnabled(ppu) & 0x20) *backdrop_math = true;
    if (*backdrop_math && PPU_addSubscreen(ppu) && (sub & 0x10)) why |= N64PPU_WHY_SUB_OBJ;
  }
  return why;
}

/* Clear a line's sprite rows (BG runs for it are flushed first). BG rows
 * need no clearing: they are written whole wherever their layer is drawn. */
static void clear_line(N64PpuFrame *f, int r) {
  if (!bit_get(s_obj_dirty_rows, r)) return;
  for (int p = N64PPU_OBJ0; p <= N64PPU_OBJ3; ++p) clear_if_dirty(f, p, r);
  s_obj_dirty_rows[r >> 5] &= ~(1u << (r & 31));
}

static void flush_all(N64PpuFrame *f) {
  for (int screen = 0; screen < 2; ++screen)
    for (int layer = 0; layer < 4; ++layer) flush_run(f, screen, layer);
}

/* Spans depend only on the window registers; most lines repeat the
 * previous line's. (Out of line: n64_ppu_build_line must fit its slot in
 * icache_order.ld, or every function placed after it moves.) */
static __attribute__((noinline)) void line_spans(const Ppu *ppu, Spans *s) {
  SpanKey key;
  memset(&key, 0, sizeof key);
  key.windowsel = ppu->windowsel;
  key.wbgobjlog = ppu->wbgobjlog;
  key.w[0] = ppu->window1left;
  key.w[1] = ppu->window1right;
  key.w[2] = ppu->window2left;
  key.w[3] = ppu->window2right;
  key.windowed[0] = ppu->screenWindowed[0];
  key.windowed[1] = ppu->screenWindowed[1];
  key.cgwsel = ppu->cgwsel;
  if (s_state.lastspans.valid && n64_mem_eq(&s_state.lastspans.key, &key, sizeof key)) {
    *s = s_state.lastspans.spans;
    return;
  }
  compute_spans(ppu, s);
  s_state.lastspans.key = key;
  s_state.lastspans.spans = *s;
  s_state.lastspans.valid = true;
}

/* A line's palettes and backdrop colours (out of line, see line_spans).
 * Returns N64PPU_WHY_PALETTES when the palette pool ran out. */
static __attribute__((noinline)) uint32_t line_palettes(N64PpuFrame *f, const Ppu *ppu,
                                                        N64PpuLine *L, bool backdrop_math,
                                                        uint8_t lmath) {
  uint32_t why = 0;
  /* A line's palettes and backdrop colours depend only on CGRAM (by its
   * write counter), brightness, the backdrop and fixed colours and the
   * colour-math settings; most lines repeat the previous line's. */
  const uint16_t bd = ppu->cgram[0];
  const unsigned b = PPU_brightness(ppu);
  const uint8_t mflags = (uint8_t)((PPU_subtractColor(ppu) ? kMathSubtract : 0) |
                                   (PPU_halfColor(ppu) ? kMathHalf : 0));
  const bool add_sub = PPU_addSubscreen(ppu);
  LinePalKey key;
  memset(&key, 0, sizeof key);
  key.cgram_count = ppu->cgramWriteCount;
  key.bd = bd;
  key.fixed = ppu->fixedColor;
  key.brightness = (uint8_t)b;
  const bool m7 = PPU_mode(ppu) == 7;
  const bool m7ext = m7 && PPU_m7extBg(ppu), m7direct = m7 && PPU_directColor(ppu);
  const uint8_t submask = !m7 ? 0xf : m7ext ? 0x3 : 0x1;
  key.mode = (uint8_t)(mflags | (add_sub ? 4 : 0) | (backdrop_math ? 8 : 0) | (lmath ? 16 : 0) | (m7 ? 32 : 0));
  key.pad = (uint8_t)((m7ext ? 1 : 0) | (m7direct ? 2 : 0));
  key.sub = (uint8_t)(ppu->screenEnabled[1] & submask);
  if (s_state.linepal.valid && n64_mem_eq(&s_state.linepal.key, &key, sizeof key)) {
    memcpy(L->tl_main, s_state.linepal.line.tl_main, sizeof L->tl_main);
    memcpy(L->tl_math, s_state.linepal.line.tl_math, sizeof L->tl_math);
    memcpy(L->tl_math_clip, s_state.linepal.line.tl_math_clip, sizeof L->tl_math_clip);
    memcpy(L->tl_fix, s_state.linepal.line.tl_fix, sizeof L->tl_fix);
    memcpy(L->tl_fix_clip, s_state.linepal.line.tl_fix_clip, sizeof L->tl_fix_clip);
    L->fill = s_state.linepal.line.fill;
    L->fill_clip = s_state.linepal.line.fill_clip;
    L->fill_math = s_state.linepal.line.fill_math;
    L->fill_math_clip = s_state.linepal.line.fill_math_clip;
    L->sub_layers = s_state.linepal.line.sub_layers;
    why = s_state.linepal.full ? N64PPU_WHY_PALETTES : 0;
  } else {
    bool full = false;
    /* With the scene only sprites can still come from planes (lines
     * obj_line_scene refuses): the BG and subscreen plane palettes are
     * never loaded. */
#if N64PPU_SCENE
    const int sel0 = N64PPU_SEL_OBJ;
#else
    const int sel0 = 0;
#endif
    /* (mode 7: its BG1 palette and the sprites'; otherwise not mode 7's) */
    #define SEL_USED(sel) ((sel) == N64PPU_SEL_OBJ || \
        (!m7 ? (sel) < N64PPU_SEL_OBJ \
             : (sel) == (m7direct ? N64PPU_SEL_M7D : N64PPU_SEL_M7) || \
               (m7ext && ((sel) == N64PPU_SEL_M7E_LO || (sel) == N64PPU_SEL_M7E_HI))))
    for (int sel = sel0; sel < N64PPU_SELS; ++sel)
      if (SEL_USED(sel)) L->tl_main[sel] = get_tlut(f, ppu, kTlutNormal, (uint8_t)sel, 0, 0, &full);
    L->fill = pack5551(bright(bd & 31, b, false), bright((bd >> 5) & 31, b, false),
                       bright((bd >> 10) & 31, b, false));
    L->fill_clip = 1;   /* black, opaque */
    if (backdrop_math) {
      /* Where the subscreen is empty the fixed colour is used, halved only
       * without addSubscreen (PpuDrawWholeLine). Clipped: main colour 0 and
       * no halving. */
      L->fill_math = math_color(bd, ppu->fixedColor, mflags, b, !add_sub && PPU_halfColor(ppu));
      L->fill_math_clip = math_color(0, ppu->fixedColor, mflags, b, false);
      if (add_sub) {
        L->sub_layers = ppu->screenEnabled[1] & submask;
        const uint8_t cflags = (uint8_t)(mflags & ~kMathHalf);
        for (int sel = m7 ? N64PPU_SEL_M7 : sel0; sel < N64PPU_SELS; ++sel) {
          if (sel == N64PPU_SEL_OBJ || !SEL_USED(sel)) continue;
          L->tl_math[sel] = get_tlut(f, ppu, kTlutMath, (uint8_t)sel, bd, mflags, &full);
          L->tl_math_clip[sel] = get_tlut(f, ppu, kTlutMath, (uint8_t)sel, 0, cflags, &full);
        }
      }
    }
    if (lmath) {
      const uint8_t fl = (uint8_t)((mflags & kMathSubtract) | (!add_sub && PPU_halfColor(ppu) ? kMathHalf : 0));
      for (int sel = sel0; sel < N64PPU_SELS; ++sel) {
        if (!SEL_USED(sel)) continue;
        L->tl_fix[sel] = get_tlut(f, ppu, kTlutFixed, (uint8_t)sel, ppu->fixedColor, fl, &full);
        L->tl_fix_clip[sel] = get_tlut(f, ppu, kTlutFixed, (uint8_t)sel, ppu->fixedColor,
                                       (uint8_t)((fl & kMathSubtract) | kMathClip), &full);
      }
    }
    #undef SEL_USED
    if (full) why = N64PPU_WHY_PALETTES;
    s_state.linepal.key = key;
    s_state.linepal.line = *L;
    s_state.linepal.full = full;
    s_state.linepal.valid = true;
  }
  return why;
}

static void line_sprites(N64PpuFrame *f, const Ppu *ppu, N64PpuLine *L, int line);

/* Mode 7's BG1 (or BG2 with EXTBG: the same picture, bit 7 a priority) for
 * one line, into a plane row. A pixel is a CGRAM index, 0 transparent
 * (N64PPU_SEL_M7*). Two software renderers are the reference, and they
 * place a mosaic differently:
 * - plain lines: PpuDrawBackground_mode7 (ppu.c). The mosaic's lines are
 *   those of ppu->mosaicModulo; across, the blocks lie on the screen's
 *   grid, but each part of the line the layer's own windows cut it into
 *   starts again, with the texel under the part's first pixel.
 * - lines with EXTBG or direct colour: ppu_draw_mode7_line_native
 *   (ppu_mode7_line.h). The mosaic's lines count from mosaicStartLine;
 *   across, every block shows the texel under its own first pixel.
 * Windows are otherwise the spans' hide bits. */
uint32_t g_n64ppu_m7_rows;
static __attribute__((noinline)) void mode7_row(N64PpuFrame *f, const Ppu *ppu, int plane, int line,
                                                int screen, int layer) {
  const int r = line - 1;
  const uint16_t *vram = PpuRenderVram(ppu);
  const bool native = PPU_m7extBg(ppu) || PPU_directColor(ppu);
  const int size = PPU_mosaicSize(ppu);
  const int hScroll = ((int16_t)(ppu->m7matrix[6] << 3)) >> 3;
  const int vScroll = ((int16_t)(ppu->m7matrix[7] << 3)) >> 3;
  const int xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3;
  const int yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  int y = line;
  if (size > 1 && PPU_mosaicEnabled(ppu, 0))   /* (BG1's setting, for BG2 too) */
    y -= native ? (y - (int)ppu->mosaicStartLine) % size : y % size;
  const uint32_t ry = PPU_m7yFlip(ppu) ? (uint8_t)(255 - y) : (uint8_t)y;
  const uint32_t startX = (uint32_t)((ppu->m7matrix[0] * clippedH & ~63) + (ppu->m7matrix[1] * (int)ry & ~63) +
                                     (ppu->m7matrix[1] * clippedV & ~63) + (xCenter << 8));
  const uint32_t startY = (uint32_t)((ppu->m7matrix[2] * clippedH & ~63) + (ppu->m7matrix[3] * (int)ry & ~63) +
                                     (ppu->m7matrix[3] * clippedV & ~63) + (yCenter << 8));
  const uint32_t rx = PPU_m7xFlip(ppu) ? 255u : 0u;
  uint32_t xpos = startX + (uint32_t)(ppu->m7matrix[0] * (int)rx);
  uint32_t ypos = startY + (uint32_t)(ppu->m7matrix[2] * (int)rx);
  const uint32_t dx = (uint32_t)(PPU_m7xFlip(ppu) ? -ppu->m7matrix[0] : ppu->m7matrix[0]);
  const uint32_t dy = (uint32_t)(PPU_m7xFlip(ppu) ? -ppu->m7matrix[2] : ppu->m7matrix[2]);
  const uint32_t outside_value = PPU_m7largeField(ppu) ? 0x3ffffu : 0xffffffffu;
  const bool char_fill = PPU_m7charFill(ppu) != 0;
  uint8_t *dst = f->plane[plane] + (size_t)r * N64PPU_STRIDE + N64PPU_PAD;
  uint32_t any = 0;
  /* VRAM words are read as bytes: the tile number is a map word's low
   * byte, the pixel a character word's high byte (the N64 is big-endian:
   * offsets 1 and 0). */
  const uint8_t *const vb = (const uint8_t *)vram;
  if (size > 1 && PPU_mosaicEnabled(ppu, layer)) {
    /* Mosaic: block by block. */
    int edge[6] = {0, 256}, ne = 1;
    if (!native && (ppu->screenWindowed[screen] & (1 << layer))) {
      const unsigned wf = GET_WINDOW_FLAGS(ppu, layer);
      const int cand[4] = {ppu->window1left, ppu->window1right + 1, ppu->window2left, ppu->window2right + 1};
      const bool on[2] = {(wf & kWindow1Enabled) && ppu->window1left <= ppu->window1right,
                          (wf & kWindow2Enabled) && ppu->window2left <= ppu->window2right};
      for (int k = 0; k < 4; ++k) {
        const int e = cand[k];
        if (!on[k >> 1] || e <= 0 || e >= 256) continue;
        int at = 1;
        while (at < ne && edge[at] < e) ++at;
        if (at < ne && edge[at] == e) continue;
        for (int m = ne + 1; m > at; --m) edge[m] = edge[m - 1];
        edge[at] = e; ++ne;
      }
    }
    for (int part = 0; part < ne; ++part) {
      for (int x = edge[part]; x < edge[part + 1];) {
        const int org = x - x % size;
        const int from = native ? org : x;                /* where the block's texel is */
        int w = org + size - x;
        if (w > edge[part + 1] - x) w = edge[part + 1] - x;
        const int sx = PPU_m7xFlip(ppu) ? 255 - from : from;
        const uint32_t bx = startX + (uint32_t)(ppu->m7matrix[0] * sx);
        const uint32_t by = startY + (uint32_t)(ppu->m7matrix[2] * sx);
        uint8_t pixel = 0;
        if ((bx | by) > outside_value) {
          if (char_fill) pixel = vb[((by >> 8 & 7) * 8 + (bx >> 8 & 7)) * 2];
        } else {
          const unsigned tile = vb[((by >> 11 & 0x7f) * 128 + (bx >> 11 & 0x7f)) * 2 + 1];
          pixel = vb[(tile * 64 + (by >> 8 & 7) * 8 + (bx >> 8 & 7)) * 2];
        }
        memset(dst + x, pixel, (size_t)w);
        any |= pixel;
        x += w;
      }
    }
  } else {
  /* A texel is looked up when the position leaves the last one's (its
   * tile when it leaves the tile's): zoomed in, most pixels repeat the
   * one before; near 1:1, eight share a tile. The position's bits above
   * the field (the large field's "outside") are part of what is compared,
   * so a pixel outside never passes for one inside. */
  uint32_t px = ~xpos;                    /* the last x position looked up (none) */
  uint8_t pixel = 0;
  if (dy == 0) {
    /* No rotation: the line stays on one row of the field. */
    if (ypos > outside_value && !char_fill) {
      memset(dst, 0, 256);
    } else {
      const bool row_out = ypos > outside_value;                       /* (then: tile 0's row everywhere) */
      const uint8_t *const map = vb + (ypos >> 11 & 0x7f) * 256 + 1;   /* the map row's tile bytes, 2 apart */
      const uint32_t crow = (ypos >> 8 & 7) * 16;                      /* the row in a character (128 bytes) */
      const uint8_t *cp = vb + crow;                                   /* the current tile's row (tile 0) */
      for (int x = 0; x < 256; ++x, xpos += dx) {
        const uint32_t ch = xpos ^ px;
        if (ch >> 8) {
          if (xpos > outside_value) {
            if (!char_fill) { pixel = 0; px = xpos; dst[x] = 0; continue; }
            cp = vb + crow;
          } else if (row_out) {
            cp = vb + crow;
          } else if ((ch >> 11) || px > outside_value) {
            cp = vb + (uint32_t)map[(xpos >> 11 & 0x7f) * 2] * 128 + crow;
          }
          pixel = cp[(xpos >> 8 & 7) * 2];
          px = xpos;
          any |= pixel;
        }
        dst[x] = pixel;
      }
    }
  } else {
    uint32_t py = ~ypos;
    unsigned tile = 0;
    for (int x = 0; x < 256; ++x, xpos += dx, ypos += dy) {
      const uint32_t ch = (xpos ^ px) | (ypos ^ py);
      if (ch >> 8) {                         /* another texel */
        if ((xpos | ypos) > outside_value) {
          if (!char_fill) { pixel = 0; px = xpos; py = ypos; dst[x] = 0; continue; }
          tile = 0;
        } else if ((ch >> 11) || (px | py) > outside_value) {   /* another tile (or back inside) */
          tile = vb[((ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)) * 2 + 1];
        }
        pixel = vb[(tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)) * 2];
        px = xpos; py = ypos;
        any |= pixel;
      }
      dst[x] = pixel;
    }
  }
  }
  if (layer == 0) {
    if (any) bit_set(f->rows[plane], r);
  } else {
    /* BG2: which of its two passes have pixels (bit 7; colour 0 of either is transparent). */
    uint32_t lo = 0, hi = 0;
    for (int x = 0; x < 256; ++x) {
      const uint8_t p = dst[x];
      if (p & 0x80) hi |= p & 0x7fu; else lo |= p;
    }
    if (lo) bit_set(f->rows[plane], r);
    if (hi) bit_set(f->rows_hi[plane], r);
  }
  g_n64ppu_m7_rows++;
}

bool n64_ppu_build_line(const Ppu *ppu, int line, bool same_regs) {
  N64PpuFrame *f = s_state.frame;
  s_state.ppu = ppu;
  const int r = line - 1;
  N64PpuLine *L = &f->line[r];
  f->ref[r] = (uint8_t)r;
#if N64PPU_SCENE
  /* Same registers and CGRAM as the line above, which the RDP draws: spans,
   * palettes and layers are that line's (they depend on nothing else, and
   * the scene draws every BG layer of an RDP line, so there are no plane
   * rows to queue). Only the sprites are per line. */
  if (same_regs && r > 0 && f->line[f->ref[r - 1]].kind == N64PPU_LINE_RDP && !objmath_line(ppu, r) &&
      PPU_mode(ppu) <= 1) {   /* (mode 7's BG1 is a plane row a line: below) */
    const N64PpuLine *P = &f->line[f->ref[r - 1]];
    N64PpuLine t;   /* line_sprites writes these three fields only */
    t.obj_scene = 0;
    t.obj_first = 0;
    t.main_layers = (uint8_t)(P->main_layers & ~0x10);
    line_sprites(f, ppu, &t, line);
    if (t.obj_scene == P->obj_scene && t.obj_first == P->obj_first &&
        t.main_layers == P->main_layers) {
      f->ref[r] = f->ref[r - 1];   /* the row above, whole */
    } else {
      *L = *P;
      L->obj_scene = t.obj_scene;
      L->obj_first = t.obj_first;
      L->main_layers = t.main_layers;
    }
    f->rdp_lines++;
    return true;
  }
#else
  (void)same_regs;
#endif
  memset(L, 0, sizeof *L);
  if (PPU_forcedBlank(ppu)) {
    flush_all(f);
    clear_line(f, r);
    L->kind = N64PPU_LINE_BLACK;
    f->black_lines++;
    return true;
  }
  Spans *s = &s_state.spans[r];
  line_spans(ppu, s);
  bool backdrop_math;
  uint8_t lmath;
  uint32_t why = line_unsupported(ppu, s, r, &backdrop_math, &lmath);

  if (!why) why |= line_palettes(f, ppu, L, backdrop_math, lmath);
  if (why) {
#ifdef N64PPU_WHY_LOG
    { /* The registers behind a fallback, once per change. */
      static uint32_t s_last;
      const uint32_t k = (uint32_t)ppu->cgwsel | ((uint32_t)ppu->cgadsub << 8) |
                         ((uint32_t)ppu->screenEnabled[0] << 16) | ((uint32_t)ppu->screenEnabled[1] << 24);
      if (k != s_last) {
        s_last = k;
        extern void n64_platform_log(const char *fmt, ...);
        n64_platform_log("[ppuwhy-reg] line %d why %03lx cgwsel %02x cgadsub %02x main %02x sub %02x fixed %04x mode %d spans %d\n",
                         line, (unsigned long)why, ppu->cgwsel, ppu->cgadsub, ppu->screenEnabled[0],
                         ppu->screenEnabled[1], ppu->fixedColor, PPU_mode(ppu), s->n);
      }
    }
#endif
    memset(L, 0, sizeof *L);
    flush_all(f);
    clear_line(f, r);
    L->kind = N64PPU_LINE_SOFTWARE;
    f->fallback_reasons |= why;
    f->software_lines++;
    return false;
  }

  L->kind = N64PPU_LINE_RDP;
  L->lmath = lmath;
  L->order = PPU_mode(ppu) == 7 ? (PPU_m7extBg(ppu) ? (PPU_directColor(ppu) ? N64PPU_ORDER_MODE7_EXT_DIRECT : N64PPU_ORDER_MODE7_EXT)
                                                    : (PPU_directColor(ppu) ? N64PPU_ORDER_MODE7_DIRECT : N64PPU_ORDER_MODE7))
           : PPU_mode(ppu) == 0 ? N64PPU_ORDER_MODE0
           : (PPU_bg3priority(ppu) ? N64PPU_ORDER_MODE1_BG3TOP : N64PPU_ORDER_MODE1);
  L->nspans = (uint8_t)s->n;
  for (int k = 0; k < s->n; ++k) {
    L->spans[k].x0 = s->x0[k];
    L->spans[k].x1 = s->x1[k];
    L->spans[k].clip = s->clip[k];
    /* bit 0: backdrop math here; bit 1: layer math (L->lmath) here */
    L->spans[k].math = (uint8_t)(s->math[k] ? (backdrop_math ? 1 : 0) | (lmath ? 2 : 0) : 0);
    L->spans[k].hide = s->mask_main[k];
    L->spans[k].hide_sub = s->mask_sub[k];
  }

  /* Background layers of both screens: queued (rows rewritten at flush);
   * layers off on this line get their row cleared now. */
  const bool m7 = PPU_mode(ppu) == 7;
  const bool m7ext = m7 && PPU_m7extBg(ppu);
  const int nbg = m7 ? 1 : PPU_mode(ppu) == 0 ? 4 : 3;
#if N64PPU_SCENE
  /* Clipped spans and the subscreen are drawn by the scene too, through
   * the black and math palettes (n64_ppu_scene_tlut). */
  const bool scene_line_ok = PPU_mode(ppu) <= 1;
  L->scene_bg = scene_line_ok ? 3 : 0;
#endif
  const uint8_t enabled[2] = {ppu->screenEnabled[0], L->sub_layers};
  for (int screen = 0; screen < 2; ++screen) {
    for (int layer = 0; layer < 4; ++layer) {
      if (m7 && (layer == 0 || (layer == 1 && m7ext)) && (enabled[screen] & (1 << layer))) {
        /* Mode 7: the row is sampled now, on the CPU (mode7_row). */
        flush_run(f, screen, layer);
        mode7_row(f, ppu, (screen ? N64PPU_SUB_BG1 : N64PPU_BG1) + layer, line, screen, layer);
        if (!screen) L->main_layers |= (uint8_t)(1 << layer);
      } else if (layer < nbg && (enabled[screen] & (1 << layer))) {
        const int bpp = (PPU_mode(ppu) == 0 || layer == 2) ? 2 : 4;
        const unsigned pal_base = PPU_mode(ppu) == 0 ? 32u * layer : 0;
#if N64PPU_SCENE
        /* Drawn by the scene builder (n64_ppu_draw.c scene_draw_bg, same
         * rule): no plane. */
        if (L->scene_bg & (1 << screen)) {
          flush_run(f, screen, layer);
          if (!screen) L->main_layers |= (uint8_t)(1 << layer);
          continue;
        }
#endif
        queue_bg(f, ppu, screen, layer, line, bpp, pal_base);
        if (!screen) L->main_layers |= (uint8_t)(1 << layer);
      } else {
        /* Not drawn on this line (main_layers/sub_layers): the row keeps
         * whatever it held. */
        flush_run(f, screen, layer);
        if (screen) L->sub_layers &= (uint8_t)~(1 << layer);
      }
    }
  }
  line_sprites(f, ppu, L, line);
  f->rdp_lines++;
  return true;
}

/* Sprites of an RDP line: per line (they read OAM and VRAM live). */
static void line_sprites(N64PpuFrame *f, const Ppu *ppu, N64PpuLine *L, int line) {
  const int r = line - 1;
  clear_line(f, r);
  if (ppu->screenEnabled[0] & 0x10) {
#if N64PPU_SCENE
    obj_lines_ensure(ppu);
    if (obj_line_scene(f, ppu, r)) {
      L->obj_scene = 1;
      L->obj_first = (uint8_t)(PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) >> 1 : 0);
      g_scene_obj_lines++;
    } else {
      expand_obj(f, ppu, line);
      g_scene_obj_plane_lines++;
    }
#else
    expand_obj(f, ppu, line);
#endif
    L->main_layers |= 0x10;
  }
}

#if N64PPU_SCENE
/* VRAM blocks stored to up to the last snapshot and not yet taken. */
static uint32_t s_vram_dirty[8];

void n64_ppu_build_take_vram_dirty(uint32_t out[8]) {
  memcpy(out, s_vram_dirty, sizeof s_vram_dirty);
  memset(s_vram_dirty, 0, sizeof s_vram_dirty);
}
#endif

void n64_ppu_build_end(struct Ppu *ppu) {
  flush_all(s_state.frame);
#if N64_RSPBG
#if N64PPU_SCENE   /* the scene draws from this frame's VRAM: snapshot it */
  n64_bgrsp_sync_vram(ppu->vram, ppu->vramWriteCount);
  /* The snapshot holds every store marked so far: hand the marks on. */
  for (int i = 0; i < 8; ++i) {
    s_vram_dirty[i] |= ppu->vramDirty[i];
    ppu->vramDirty[i] = 0;
  }
#endif
  n64_bgrsp_kick();
#endif
}

void n64_ppu_build_finish(void) {
#if N64_RSPBG
  merge_rsp_flags(s_state.frame);
#endif
}

void n64_ppu_build_release_vram(void) {
#if N64_RSPBG
  n64_bgrsp_release_vram();
#endif
}
