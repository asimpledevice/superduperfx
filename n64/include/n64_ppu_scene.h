/* Scene builder (docs/n64/PPU_DESIGN.md, work plan step 4): a frame's
 * background layers as RDP texture rectangles from the tile cache
 * (n64/rsptile), instead of expanded planes.
 *
 * One quad is one 8x8 tile drawn over the lines of one tile row: screen x
 * (x0 may be negative: the left tile under fine scroll), lines [r0, r1)
 * (0-based screen rows), the tile's cache index, its first texture row, its
 * flips, priority and palette. The RDP draws it with a texture rectangle at
 * s = 0 (or 8 when flipped horizontally: the cache rows hold both halves)
 * and t = 8 * tile + row (DtDy -1 from 8 * tile + 7 - row when flipped
 * vertically). The palette selects the 16-entry TLUT block: 4bpp layers
 * use TC_TLUT_16 (block = palette, BG palettes 0-7), 2bpp layers
 * TC_TLUT_2BPP built from the layer's palette base (block = palette).
 *
 * The C version here is the reference; the RSP version must match it. This
 * header includes neither libdragon nor the SNES runtime. */
#ifndef SNESRECOMP_N64_PPU_SCENE_H
#define SNESRECOMP_N64_PPU_SCENE_H

#include <stdint.h>

enum {
  N64SCENE_HFLIP = 1u << 0,
  N64SCENE_VFLIP = 1u << 1,
  N64SCENE_PRIO = 1u << 2,
};

typedef struct N64SceneQuad {
  int16_t x0;          /* screen x of the tile's left edge, -7..255 */
  uint8_t r0, r1;      /* screen rows [r0, r1), within one tile row */
  uint16_t tile;       /* cache index: C4 for 4bpp layers, C2 for 2bpp */
  uint8_t row;         /* tile row shown on r0, before any flip */
  uint8_t flags;       /* N64SCENE_* */
  uint8_t pal;         /* palette number 0-7 (the layer's base excluded) */
  uint8_t pad_[3];
} N64SceneQuad;

/* One background layer's state over a run of lines (as queue_bg in
 * n64_ppu_build.c gathers it from the PPU registers). */
typedef struct N64SceneBg {
  uint16_t hs, vs;     /* scroll */
  uint8_t xsc;         /* BGnSC */
  uint8_t bpp;         /* 2 or 4 */
  uint16_t tileadr;    /* character base, VRAM word address */
} N64SceneBg;

/* Quads for rows [r0, r1) of one layer; returns how many were written (at
 * most 33 per tile row crossed). */
int n64_scene_bg(const N64SceneBg *bg, const uint16_t *vram, int r0, int r1,
                 N64SceneQuad *out, int max);

#endif
