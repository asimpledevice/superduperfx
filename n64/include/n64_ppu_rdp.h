/* RDP backend for the SNES PPU: the frame description shared by the two
 * halves.
 *
 *   n64_ppu_build.c (runtime side, sees the PPU) expands each scanline on the
 *   CPU into palette-indexed "planes": one per background layer and priority,
 *   one per sprite priority, and subscreen copies of the background layers.
 *   Tile bitplanes become 8-bit palette indices through lookup tables;
 *   per-line scroll, VRAM, OAM and window registers are read live, so HDMA
 *   effects come for free. Window masks and per-line layer enables are not
 *   applied to the planes: the compositor skips a layer in a span where it
 *   is hidden (N64PpuSpan.hide/hide_sub, N64PpuLine.main_layers/sub_layers).
 *   So a BG row is only ever written whole by its expansion, and a row of a
 *   layer that is off on its line is never drawn (it may hold stale data).
 *
 *   n64_ppu_draw.c (libdragon side) composites the planes on the RDP in SNES
 *   priority order with 256-entry palettes built from CGRAM.
 *
 * Colour math is exact for the case it supports: math on backdrop pixels
 * only (add, subtract, half; subscreen or fixed colour). The backdrop is one
 * colour per line, so "backdrop (+/-) subscreen pixel" is a palette lookup:
 * subscreen planes are drawn under the main screen with a math palette.
 * Lines using anything else are rendered by the software PPU and drawn as an
 * image (N64PPU_LINE_SOFTWARE). This header includes neither libdragon nor
 * the SNES runtime. */
#ifndef SNESRECOMP_N64_PPU_RDP_H
#define SNESRECOMP_N64_PPU_RDP_H

#include <stdbool.h>
#include <stdint.h>

enum {
  N64PPU_LINES = 224,
  N64PPU_PAD = 8,                       /* left pad: fine scroll lands here */
  N64PPU_STRIDE = 8 + 256 + 8,          /* bytes per plane row (CI8) */
  N64PPU_MAX_TLUTS = 48,                /* palettes per frame */
  N64PPU_MAX_SPANS = 5,
};

/* Planes. A BG plane holds one layer: index = priority << 7 | palette
 * index (BG colours use CGRAM 0-127), and the RDP draws it twice, through a
 * palette that shows only the low half (priority 0 tiles) and one that shows
 * only the high half. OBJ planes hold the winning sprite pixel of each
 * priority (lower OAM index resolved as on hardware). SUB_* planes are the
 * subscreen copies of the BG layers. */
enum {
  N64PPU_BG1, N64PPU_BG2, N64PPU_BG3, N64PPU_BG4,
  N64PPU_OBJ0, N64PPU_OBJ1, N64PPU_OBJ2, N64PPU_OBJ3,
  N64PPU_SUB_BG1, N64PPU_SUB_BG2, N64PPU_SUB_BG3, N64PPU_SUB_BG4,
  N64PPU_PLANES
};

/* Order entries: a plane, plus N64PPU_HI for a BG layer's priority-1 pass. */
enum { N64PPU_HI = 0x10 };

/* Palette selectors: which half of a BG plane shows, and the transparency
 * pattern (every 16th entry for 4bpp, every 4th for 2bpp); OBJ for sprites. */
enum {
  N64PPU_SEL_LO16, N64PPU_SEL_HI16, N64PPU_SEL_LO4, N64PPU_SEL_HI4, N64PPU_SEL_OBJ,
  N64PPU_SELS
};

enum {
  N64PPU_LINE_RDP = 0,       /* composite planes */
  N64PPU_LINE_SOFTWARE = 1,  /* software PPU row in sw_rows */
  N64PPU_LINE_BLACK = 2,     /* forced blank */
};

/* Per-line composition order (back to front). */
enum {
  N64PPU_ORDER_MODE0 = 0,
  N64PPU_ORDER_MODE1 = 1,
  N64PPU_ORDER_MODE1_BG3TOP = 2,
  N64PPU_ORDERS
};

/* Palette pool slots 0..N64PPU_SELS-1 are fixed black palettes, one per
 * selector, for main-screen pixels clipped to black. */
enum { N64PPU_TLUT_BLACK = 0 };

typedef struct N64PpuSpan {
  uint16_t x0, x1;           /* [x0, x1) in 0..256 */
  uint8_t clip;              /* colour window clips main pixels to black */
  uint8_t math;              /* backdrop math applies here */
  uint8_t hide;              /* main screen: bit L = layer L windowed out (4 = OBJ) */
  uint8_t hide_sub;          /* subscreen: bit L = BG layer L windowed out */
} N64PpuSpan;

/* Layer bit of a plane (BG1-4 and SUB_BG1-4: 0-3; OBJ planes: 4). */
static inline int n64ppu_plane_layer(int plane) {
  return plane >= N64PPU_SUB_BG1 ? plane - N64PPU_SUB_BG1
       : plane >= N64PPU_OBJ0 ? 4 : plane;
}

/* Everything the RDP needs for one line besides the plane rows. Lines whose
 * N64PpuLine compares equal are drawn as one band. */
typedef struct N64PpuLine {
  uint8_t kind, order, nspans, sub_layers;   /* sub_layers: subscreen BG mask */
  uint8_t main_layers;                       /* main screen: BG bits 0-3, OBJ bit 4 */
  uint8_t obj_scene;                         /* sprites drawn from the frame's OBJ strips */
  uint8_t obj_first;                         /* with obj_scene: the OAM rotation's first sprite */
  uint8_t scene_bg;                          /* BG layers drawn by the scene: 1 main screen, 2 subscreen */
  uint8_t lmath;                             /* main layers with fixed-colour math (BG bits 0-3, OBJ bit 4) */
  N64PpuSpan spans[N64PPU_MAX_SPANS];
  uint8_t tl_main[N64PPU_SELS];               /* normal palettes */
  uint8_t tl_math[N64PPU_SELS];               /* subscreen math palettes */
  uint8_t tl_math_clip[N64PPU_SELS];          /* same with backdrop clipped */
  uint8_t tl_fix[N64PPU_SELS];                /* main palettes with fixed-colour math */
  uint8_t tl_fix_clip[N64PPU_SELS];           /* same with the main colour clipped */
  uint16_t fill, fill_clip;                   /* backdrop without math */
  uint16_t fill_math, fill_math_clip;         /* backdrop with fixed-colour math */
} N64PpuLine;

/* A sprite's 8x8 tile over a run of rows (the scene builder's sprites,
 * N64PpuLine.obj_scene): C4 tile-cache index, screen x of its left column
 * (-7..255), screen rows [r0, r1), the tile row shown at r0 before any
 * flip (d0), and flags. Built once per frame from OAM (n64_ppu_build.c). */
typedef struct N64PpuObjStrip {
  uint16_t tile;
  int16_t x;
  uint8_t r0, r1, d0, flags;   /* flags: palette 0-7, N64PPU_OBJ_HFLIP, N64PPU_OBJ_VFLIP */
} N64PpuObjStrip;
enum {
  N64PPU_OBJ_HFLIP = 0x40,
  N64PPU_OBJ_VFLIP = 0x80,
  N64PPU_MAX_OBJ_STRIPS = 2048,
};

typedef struct N64PpuFrame {
  uint8_t *plane[N64PPU_PLANES];        /* N64PPU_LINES rows of N64PPU_STRIDE */
  /* Row r of plane p holds visible pixels this frame: rows[p] for OBJ planes
   * and priority-0 BG pixels, rows_hi[p] for priority-1 BG pixels. */
  uint32_t rows[N64PPU_PLANES][(N64PPU_LINES + 31) / 32];
  uint32_t rows_hi[N64PPU_PLANES][(N64PPU_LINES + 31) / 32];

  N64PpuLine line[N64PPU_LINES];
  /* Row r is described by line[ref[r]], ref[r] <= r: a row that equals the
   * row above in every field shares its N64PpuLine instead of copying it. */
  uint8_t ref[N64PPU_LINES];

  /* RGBA5551 palettes, brightness applied. */
  uint16_t tlut[N64PPU_MAX_TLUTS][256] __attribute__((aligned(16)));
  int tlut_count;

  const uint32_t *sw_rows;              /* XRGB8888, 256 wide, for SOFTWARE lines */

  /* Sprites for lines with obj_scene: strips of sprite s are
   * obj_strip[obj_start[s] .. obj_start[s + 1]), its priority obj_prio[s]. */
  N64PpuObjStrip obj_strip[N64PPU_MAX_OBJ_STRIPS];
  uint16_t obj_start[129];
  uint8_t obj_prio[128];

  /* Statistics for the current frame. */
  int rdp_lines, software_lines, black_lines;
  uint32_t fallback_reasons;            /* N64PPU_WHY_* bits seen this frame */
} N64PpuFrame;

/* Why a line fell back to the software PPU. */
enum {
  N64PPU_WHY_MODE = 1u << 0,
  N64PPU_WHY_MOSAIC = 1u << 1,
  N64PPU_WHY_BIG_TILES = 1u << 2,
  N64PPU_WHY_HIRES_INTERLACE = 1u << 3,
  N64PPU_WHY_SUB_OBJ = 1u << 4,
  N64PPU_WHY_COLOR_MATH = 1u << 5,
  N64PPU_WHY_PALETTES = 1u << 6,
  N64PPU_WHY_DIRECT_COLOR = 1u << 7,
};

/* Back-to-front composition per order: planes, with N64PPU_HI marking a BG
 * layer's priority-1 pass. Mirrors the software PPU's priority values
 * (PpuDrawBackgrounds / SPRITE_PRIO_TO_PRIO). Shared by the RDP compositor
 * and the CPU reference (n64/profile/rdp_reference.c). */
static const int8_t kN64PpuOrder[N64PPU_ORDERS][13] = {
  [N64PPU_ORDER_MODE0] = {
    N64PPU_BG4, N64PPU_BG3, N64PPU_OBJ0, N64PPU_BG4 | N64PPU_HI, N64PPU_BG3 | N64PPU_HI,
    N64PPU_OBJ1, N64PPU_BG2, N64PPU_BG1, N64PPU_OBJ2, N64PPU_BG2 | N64PPU_HI,
    N64PPU_BG1 | N64PPU_HI, N64PPU_OBJ3, -1 },
  [N64PPU_ORDER_MODE1] = {
    N64PPU_BG3, N64PPU_OBJ0, N64PPU_BG3 | N64PPU_HI, N64PPU_OBJ1, N64PPU_BG2, N64PPU_BG1,
    N64PPU_OBJ2, N64PPU_BG2 | N64PPU_HI, N64PPU_BG1 | N64PPU_HI, N64PPU_OBJ3, -1 },
  [N64PPU_ORDER_MODE1_BG3TOP] = {
    N64PPU_BG3, N64PPU_OBJ0, N64PPU_OBJ1, N64PPU_BG2, N64PPU_BG1, N64PPU_OBJ2,
    N64PPU_BG2 | N64PPU_HI, N64PPU_BG1 | N64PPU_HI, N64PPU_OBJ3, N64PPU_BG3 | N64PPU_HI, -1 },
};

/* Palette selector for an order entry (plane | N64PPU_HI). */
static inline int n64ppu_entry_sel(int order, int entry) {
  const int plane = entry & 0xf;
  if (plane >= N64PPU_OBJ0) return N64PPU_SEL_OBJ;
  const bool two_bpp = order == N64PPU_ORDER_MODE0 || plane == N64PPU_BG3;
  if (two_bpp) return (entry & N64PPU_HI) ? N64PPU_SEL_HI4 : N64PPU_SEL_LO4;
  return (entry & N64PPU_HI) ? N64PPU_SEL_HI16 : N64PPU_SEL_LO16;
}

/* Draws a built frame into the display (libdragon side) and shows it.
 * Returns the number of pixels on RDP lines that differ from the software
 * rows when compare_with_software is set, else -1. */
/* 1: n64_ppu_draw_and_show issues the frame's commands and returns: the
 * RDP shows the frame when it has drawn it (the status line included), and
 * the next frame's emulation runs meanwhile. n64_ppu_present_sync, called
 * before anything the queued commands read is written again (the planes,
 * TLUTs and VRAM snapshot: before the next frame is built), waits for them.
 * 0: wait for the RDP in n64_ppu_draw_and_show, as the comparison with the
 * software PPU and the RDP-time measurement need. */
#ifndef N64_PRESENT_ASYNC
#define N64_PRESENT_ASYNC 1
#endif
void n64_ppu_present_sync(void);
int n64_ppu_draw_and_show(const N64PpuFrame *frame, const char *status,
                          int compare_with_software);

#endif
