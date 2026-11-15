/* C reference of the tile cache conversions (tile_cache.h). Portable. */
#include "tile_cache.h"

#include <string.h>

/* Colour index of pixel x of row r of a bpp tile whose first word is w. */
static unsigned pixel(const uint16_t *vram, unsigned w, int bpp, int r, int x) {
  unsigned v = 0;
  for (int p = 0; p < bpp; p++) {
    const uint16_t word = vram[(w + 8 * (p / 2) + r) & 0x7fff];
    const unsigned byte = p & 1 ? word >> 8 : word & 0xff;
    v |= ((byte >> (7 - x)) & 1u) << p;
  }
  return v;
}

void tc_ref_tile(const uint16_t *vram, int bpp, unsigned tile, uint8_t *out) {
  const unsigned w = tile * (unsigned)bpp * 4;   /* words: 8 per 2 planes */
  for (int r = 0; r < 8; r++) {
    unsigned px[8];
    for (int x = 0; x < 8; x++) px[x] = pixel(vram, w, bpp, r, x);
    if (bpp == 8) {
      uint8_t *row = out + 16 * r;
      for (int x = 0; x < 8; x++) row[x] = (uint8_t)px[x], row[8 + x] = (uint8_t)px[7 - x];
    } else {
      uint8_t *row = out + 8 * r;
      for (int k = 0; k < 4; k++) {
        row[k] = (uint8_t)(px[2 * k] << 4 | px[2 * k + 1]);
        row[4 + k] = (uint8_t)(px[7 - 2 * k] << 4 | px[6 - 2 * k]);
      }
    }
  }
}

/* round(c * l / 15), as ppu_brightness_level's 5-bit value. */
static unsigned bright5(unsigned c, unsigned l) { return (2u * c * l + 15u) / 30u; }

static uint16_t colour(uint16_t c, unsigned l) {
  return (uint16_t)(bright5(c & 31, l) << 11 | bright5((c >> 5) & 31, l) << 6 |
                    bright5((c >> 10) & 31, l) << 1 | 1);
}

void tc_ref_tlut(const uint16_t *cgram, unsigned brightness, unsigned kind,
                 unsigned base_lo, unsigned base_hi, uint16_t *out) {
  const unsigned l = brightness & 15;
  if (kind != TC_TLUT_2BPP) {
    for (int i = 0; i < 256; i++) out[i] = colour(cgram[i], l);
    for (int i = 0; i < 256; i += kind == TC_TLUT_256 ? 256 : 16) out[i] = 0;
    return;
  }
  memset(out, 0, 512);
  for (int half = 0; half < 2; half++) {
    const unsigned base = half ? base_hi : base_lo;
    if (half && base == 0x100) {   /* OBJ */
      for (int i = 128; i < 256; i++) out[i] = i & 15 ? colour(cgram[i], l) : 0;
      continue;
    }
    for (int q = 0; q < 8; q++)
      for (int j = 1; j < 4; j++)
        out[128 * half + 16 * q + j] = colour(cgram[(base + 4 * q + j) & 255], l);
  }
}
