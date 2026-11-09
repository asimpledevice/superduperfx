/* CPU stand-in for n64_ppu_draw.c: composites an N64PpuFrame with the same
 * rules the RDP path uses (fills, spans as scissors, palette lookups, alpha
 * transparency, plane order from kN64PpuOrder), so the build half can be
 * checked against the software PPU on the desktop in seconds instead of in
 * an emulator. Keep the composition rules in step with n64_ppu_draw.c. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n64_ppu_rdp.h"

static void blit_plane(uint16_t *out, const N64PpuFrame *f, int p, const uint32_t *rows,
                       int r, const uint16_t *tlut, int x0, int x1) {
  if (!((rows[r >> 5] >> (r & 31)) & 1)) return;
  const uint8_t *row = f->plane[p] + (size_t)r * N64PPU_STRIDE + N64PPU_PAD;
  for (int x = x0; x < x1; ++x) {
    const uint16_t c = tlut[row[x]];
    if (c & 1) out[x] = c;
  }
}

static uint16_t s_out[N64PPU_LINES][256];
static unsigned s_frames;

int n64_ppu_draw_and_show(const N64PpuFrame *f, const char *status, int compare_with_software) {
  (void)status;
  int mismatches = 0, logged = 0;
  for (int r = 0; r < N64PPU_LINES; ++r) {
    const N64PpuLine *L = &f->line[r];
    uint16_t *out = s_out[r];
    if (L->kind != N64PPU_LINE_RDP) continue;
    for (int k = 0; k < L->nspans; ++k) {
      const N64PpuSpan *sp = &L->spans[k];
      const uint16_t fill = sp->math ? (sp->clip ? L->fill_math_clip : L->fill_math)
                                     : (sp->clip ? L->fill_clip : L->fill);
      for (int x = sp->x0; x < sp->x1; ++x) out[x] = fill;
      if (sp->math && L->sub_layers)
        for (const int8_t *o = kN64PpuOrder[L->order]; *o >= 0; ++o) {
          if ((*o & 0xf) >= N64PPU_OBJ0) continue;
          const int p = (*o & 0xf) - N64PPU_BG1 + N64PPU_SUB_BG1;
          const int layer = n64ppu_plane_layer(p);
          if (!(L->sub_layers & (1 << layer)) || (sp->hide_sub & (1 << layer))) continue;
          const int sel = n64ppu_entry_sel(L->order, *o);
          const int t = sp->clip ? L->tl_math_clip[sel] : L->tl_math[sel];
          blit_plane(out, f, p, (*o & N64PPU_HI) ? f->rows_hi[p] : f->rows[p], r,
                     f->tlut[t], sp->x0, sp->x1);
        }
      for (const int8_t *o = kN64PpuOrder[L->order]; *o >= 0; ++o) {
        const int p = *o & 0xf;
        const int layer = n64ppu_plane_layer(p);
        if (!(L->main_layers & (1 << layer)) || (sp->hide & (1 << layer))) continue;
        const int sel = n64ppu_entry_sel(L->order, *o);
        const int t = sp->clip ? N64PPU_TLUT_BLACK + sel : L->tl_main[sel];
        blit_plane(out, f, p, (*o & N64PPU_HI) ? f->rows_hi[p] : f->rows[p], r,
                   f->tlut[t], sp->x0, sp->x1);
      }
    }
    if (!compare_with_software) continue;
    const uint32_t *ref = f->sw_rows + (size_t)r * 256;
    for (int x = 0; x < 256; ++x) {
      const uint32_t p = ref[x];
      const uint16_t want = (uint16_t)((((p >> 19) & 31) << 10) | (((p >> 11) & 31) << 5) | ((p >> 3) & 31));
      const uint16_t got = out[x] >> 1;
      if (got != want) {
        if (logged++ < 3)
          fprintf(stderr, "[rdp-ref] f%u line %d x %d: got %04x want %04x (spans %d)\n",
                  s_frames, r + 1, x, got, want, L->nspans);
        mismatches++;
      }
    }
  }
  s_frames++;
  {
    /* The desktop host ends the run here (linux_platform.c does it in
     * n64_platform_present, which the RDP path does not call). */
    const char *n = getenv("N64_PROFILE_FRAMES");
    if (n && s_frames >= (unsigned)atoi(n)) exit(0);
  }
  return compare_with_software ? mismatches : -1;
}
