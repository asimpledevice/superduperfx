/* Two-screen colour math on the RSP: the libdragon half. See cmp_rsp.h. */
#include "cmp_rsp.h"

#include <libdragon.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "cmp_rsp_layout.h"

DEFINE_RSP_UCODE(rsp_cmp);

enum { kMaxBands = 48 };

static uint32_t s_ovl;
static uint8_t *s_desc;   /* kMaxBands descriptors */
static int s_nbands;
uint32_t g_cmprsp_lines, g_cmprsp_bands;

void n64_cmprsp_init(void) {
  s_ovl = rspq_overlay_register(&rsp_cmp);
  s_desc = memalign(16, (size_t)CMP_SIZE * kMaxBands);
  if (!s_desc) abort();
}

void n64_cmprsp_frame_begin(void) { s_nbands = 0; }

static inline void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xffff); }

/* An entry's parameters by its palette selector (sel_source,
 * n64_ppu_build.c); false for a selector the microcode has not. */
static bool entry_params(int sel, uint8_t *p, unsigned flag_all, unsigned flag_obj, unsigned spans) {
  unsigned hx = 0, cmask, keep = 0xfe;
  switch (sel) {
    case N64PPU_SEL_LO16: hx = 0x4000; cmask = 0x0f; break;
    case N64PPU_SEL_HI16: cmask = 0x0f; break;
    case N64PPU_SEL_LO4: hx = 0x4000; cmask = 0x03; break;
    case N64PPU_SEL_HI4: cmask = 0x03; break;
    case N64PPU_SEL_M7E_LO: hx = 0x4000; cmask = 0x7f; break;
    case N64PPU_SEL_M7E_HI: cmask = 0x7f; break;
    case N64PPU_SEL_OBJ: cmask = 0x0f; keep = 0x1fe; break;
    case N64PPU_SEL_M7: cmask = 0; keep = 0x1fe; break;   /* any byte but 0 */
    default: return false;
  }
  if (sel == N64PPU_SEL_M7) {
    put16(p + 0, 0); put16(p + 2, 0x7f80); put16(p + 4, 0x0080);
  } else {
    put16(p + 0, hx); put16(p + 2, 0x4000 | (cmask << 7)); put16(p + 4, 0x4001);
  }
  put16(p + 6, keep);
  put16(p + 8, flag_obj);
  put16(p + 10, flag_all);
  put16(p + 12, spans);
  put16(p + 14, 0);
  return true;
}

bool n64_cmprsp_band(const N64PpuFrame *f, int a, int b, uint16_t *out) {
  const N64PpuLine *L = &f->line[f->ref[a]];
  if (s_nbands == kMaxBands) return false;
  uint8_t *d = s_desc + (size_t)s_nbands * CMP_SIZE;
  memset(d, 0, CMP_SIZE);
  const int8_t *order = kN64PpuOrder[L->order];
  int n = 0;
  while (order[n] >= 0) ++n;
  if (n > CMP_MAX_ENTRIES) return false;
  unsigned math_spans = 0, clip_spans = 0;
  for (int k = 0; k < L->nspans; ++k) {
    if (L->spans[k].math) math_spans |= 1u << k;
    if (L->spans[k].clip) clip_spans |= 1u << k;
    put16(d + CMP_EDGES + 2 * k, L->spans[k].x0);
    put16(d + CMP_EDGES + 2 * k + 2, L->spans[k].x1);
  }
  const bool addsub = (L->c_flags & 8) != 0, half = (L->c_flags & 2) != 0;
  const bool sub = addsub && L->sub_layers;
  uint32_t planes[2 * CMP_MAX_ENTRIES];   /* the plane of each entry, or -1 */
  bool hi[2 * CMP_MAX_ENTRIES];
  for (int i = 0; i < 2 * CMP_MAX_ENTRIES; ++i) { planes[i] = ~0u; hi[i] = false; }
  for (int e = 0; e < n; ++e) {
    const int o = order[e];
    const int p = o & 0xf, layer = n64ppu_plane_layer(p);
    const int sel = n64ppu_entry_sel(L->order, o);
    const bool obj = p >= N64PPU_OBJ0;
    const unsigned math = (L->c_math >> layer) & 1 ? 0x200u : 0;
    for (int s = 0; s < 2; ++s) {
      const int i = s * CMP_MAX_ENTRIES + e;
      planes[i] = ~0u;
      const unsigned layers = s ? L->sub_layers : L->main_layers;
      if (s && !sub) continue;
      if (!(layers & (1u << layer))) continue;
      unsigned spans = 0;
      for (int k = 0; k < L->nspans; ++k)
        if (!((s ? L->spans[k].hide_sub : L->spans[k].hide) & (1u << layer))) spans |= 1u << k;
      if (!spans) continue;
      const int pl = (s && !obj) ? p - N64PPU_BG1 + N64PPU_SUB_BG1 : p;
      if (!entry_params(sel, d + CMP_PARAMS + 16 * i, s ? 0x200u : (obj ? 0 : math), (!s && obj) ? math : 0, spans))
        return false;
      put32(d + CMP_BASES + 4 * i, PhysicalAddr(f->plane[pl] + N64PPU_PAD));
      planes[i] = (uint32_t)pl;
      hi[i] = (o & N64PPU_HI) != 0;
    }
  }
  put32(d + CMP_OUT, PhysicalAddr(out));
  put32(d + CMP_TABLE, PhysicalAddr(f->tlut[L->tl_cgram]));
  put16(d + CMP_NSPANS, L->nspans);
  put16(d + CMP_BDFLAG, (L->c_math & 0x20) ? 0x200 : 0);
  put16(d + CMP_MPARAMS + 0, math_spans);
  put16(d + CMP_MPARAMS + 2, clip_spans);
  put16(d + CMP_MPARAMS + 4, L->c_fixed);
  put16(d + CMP_MPARAMS + 6, (L->c_flags & 1) ? 0xffff : 1);
  put16(d + CMP_MPARAMS + 8, 2u * L->c_bright);
  put16(d + CMP_MPARAMS + 10, half ? 0x180 : 0);
  put16(d + CMP_MPARAMS + 12, (half && !addsub) ? 0x180 : 0);
  data_cache_hit_writeback(d, CMP_SIZE);
  /* The sprites' plane rows are the CPU's: the RSP reads RDRAM. */
  for (int p = N64PPU_OBJ0; p <= N64PPU_OBJ3; ++p)
    data_cache_hit_writeback(f->plane[p] + (size_t)a * N64PPU_STRIDE, (size_t)(b - a) * N64PPU_STRIDE);
  s_nbands++;
  g_cmprsp_bands++;
  for (int r = a; r < b; ++r) {
    uint32_t active = 0;
    for (int i = 0; i < 2 * CMP_MAX_ENTRIES; ++i) {
      if (planes[i] == ~0u) continue;
      const uint32_t *rows = hi[i] ? f->rows_hi[planes[i]] : f->rows[planes[i]];
      if ((rows[r >> 5] >> (r & 31)) & 1) active |= 1u << i;
    }
    rspq_write(s_ovl, 0, PhysicalAddr(d), ((uint32_t)r << 24) | active);
    g_cmprsp_lines++;
  }
  return true;
}
