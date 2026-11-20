/* Scene builder, C reference (include/n64_ppu_scene.h). Portable. */
#include "n64_ppu_scene.h"

/* Mirrors flush_run (n64_ppu_build.c): the tilemap entry for tile column mx
 * of the tile row holding BG line y. */
static inline uint16_t map_entry(const N64SceneBg *bg, const uint16_t *vram, unsigned y,
                                 unsigned mx) {
  const unsigned wider = bg->xsc & 1, higher = bg->xsc & 2;
  const unsigned sh = bg->big ? 4 : 3;   /* a tilemap entry covers 16 or 8 lines (and columns: mx is in entries) */
  unsigned sc = ((unsigned)(bg->xsc & 0xfc) << 8) + (((y >> sh) & 0x1f) << 5);
  if ((y & (0x20u << sh)) && higher) sc += wider ? 0x800 : 0x400;
  if ((mx >> 5) & 1 && wider) sc += 0x400;
  return vram[(sc + (mx & 31)) & 0x7fff];
}

int n64_scene_bg(const N64SceneBg *bg, const uint16_t *vram, int r0, int r1,
                 N64SceneQuad *out, int max) {
  int n = 0;
  const unsigned x = bg->hs;
  const int fine = (int)(x & 7);
  for (int g0 = r0; g0 < r1;) {
    /* Screen row r shows BG line r + 1 + vs (rows are lines - 1). */
    const unsigned y0 = (unsigned)g0 + 1 + bg->vs;
    int g1 = g0 + 1;
    while (g1 < r1 && (((unsigned)g1 + 1 + bg->vs) >> 3) == (y0 >> 3)) ++g1;
    for (int k = 0; k < 33; ++k) {
      const int sx = k * 8 - fine;
      if (sx >= 256) break;
      /* The 8-pixel column of the BG this quad shows, and its tilemap
       * entry. With 16x16 tiles an entry is four characters: the one named,
       * the next (right half), and those 16 further on (lower half), the
       * halves swapped by the entry's flips (PpuDrawBackgroundBig). */
      const unsigned col = (x >> 3) + (unsigned)k;
      const uint16_t e = map_entry(bg, vram, y0, bg->big ? col >> 1 : col);
      unsigned ch = e & 0x3ff;
      if (bg->big) {
        const unsigned hc = (col & 1) ^ ((e >> 14) & 1), hr = ((y0 >> 3) & 1) ^ ((e >> 15) & 1);
        ch = (ch + hc + (hr << 4)) & 0x3ff;
      }
      const unsigned words = bg->bpp == 4 ? 16 : 8;
      const unsigned base = (bg->tileadr + ch * words) & 0x7fff;   /* word address */
      if (n == max) return n;
      N64SceneQuad *q = &out[n++];
      q->x0 = (int16_t)sx;
      q->r0 = (uint8_t)g0;
      q->r1 = (uint8_t)g1;
      q->tile = (uint16_t)(base / words);   /* C4: byte / 32, C2: byte / 16 */
      q->row = (uint8_t)(y0 & 7);
      q->flags = (uint8_t)(((e & 0x4000) ? N64SCENE_HFLIP : 0) | ((e & 0x8000) ? N64SCENE_VFLIP : 0) |
                           ((e & 0x2000) ? N64SCENE_PRIO : 0));
      q->pal = (uint8_t)((e >> 10) & 7);
      q->pad_[0] = q->pad_[1] = q->pad_[2] = 0;
    }
    g0 = g1;
  }
  return n;
}
