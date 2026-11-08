/* RDP backend for the SNES PPU: the frame description shared by the two
 * halves.
 *
 *   n64_ppu_build.c (runtime side, sees the PPU) expands each scanline on the
 *   CPU into palette-indexed "planes": one per background layer and priority,
 *   one per sprite priority, and subscreen copies of the background layers.
 *   Tile bitplanes become 8-bit palette indices through lookup tables;
 *   per-line scroll, VRAM, OAM and window registers are read live, so HDMA
 *   effects come for free. Window masks are applied by clearing spans.
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
  N64PPU_MAX_TLUTS = 40,                /* palettes per frame */
  N64PPU_MAX_SPANS = 5,
};

/* Planes. BG planes hold one layer's low- or high-priority tiles; OBJ planes
 * hold the winning sprite pixel of each priority (lower OAM index resolved
 * as on hardware). SUB_* planes are the subscreen copies of the BG layers. */
enum {
  N64PPU_BG1L, N64PPU_BG1H, N64PPU_BG2L, N64PPU_BG2H,
  N64PPU_BG3L, N64PPU_BG3H, N64PPU_BG4L, N64PPU_BG4H,
  N64PPU_OBJ0, N64PPU_OBJ1, N64PPU_OBJ2, N64PPU_OBJ3,
  N64PPU_SUB_BG1L, N64PPU_SUB_BG1H, N64PPU_SUB_BG2L, N64PPU_SUB_BG2H,
  N64PPU_SUB_BG3L, N64PPU_SUB_BG3H, N64PPU_SUB_BG4L, N64PPU_SUB_BG4H,
  N64PPU_PLANES
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

/* Palette pool slots 0 and 1 are fixed black palettes (16- and 4-entry
 * transparency) for main-screen pixels clipped to black. */
enum { N64PPU_TLUT_BLACK16 = 0, N64PPU_TLUT_BLACK4 = 1 };

typedef struct N64PpuSpan {
  uint16_t x0, x1;           /* [x0, x1) in 0..256 */
  uint8_t clip;              /* colour window clips main pixels to black */
  uint8_t math;              /* backdrop math applies here */
} N64PpuSpan;

/* Everything the RDP needs for one line besides the plane rows. Lines whose
 * N64PpuLine compares equal are drawn as one band. */
typedef struct N64PpuLine {
  uint8_t kind, order, nspans, sub_layers;   /* sub_layers: subscreen BG mask */
  N64PpuSpan spans[N64PPU_MAX_SPANS];
  uint8_t tl_main16, tl_main4;                /* normal palettes */
  uint8_t tl_math16, tl_math4;                /* subscreen math palettes */
  uint8_t tl_math16_clip, tl_math4_clip;      /* same with backdrop clipped */
  uint16_t fill, fill_clip;                   /* backdrop without math */
  uint16_t fill_math, fill_math_clip;         /* backdrop with fixed-colour math */
} N64PpuLine;

typedef struct N64PpuFrame {
  uint8_t *plane[N64PPU_PLANES];        /* N64PPU_LINES rows of N64PPU_STRIDE */
  /* Row r of plane p holds content this frame (bit r of rows[p]). */
  uint32_t rows[N64PPU_PLANES][(N64PPU_LINES + 31) / 32];

  N64PpuLine line[N64PPU_LINES];

  /* RGBA5551 palettes, brightness applied. */
  uint16_t tlut[N64PPU_MAX_TLUTS][256] __attribute__((aligned(16)));
  int tlut_count;

  const uint32_t *sw_rows;              /* XRGB8888, 256 wide, for SOFTWARE lines */

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

/* Draws a built frame into the display (libdragon side) and shows it.
 * Returns the number of pixels on RDP lines that differ from the software
 * rows when compare_with_software is set, else -1. */
int n64_ppu_draw_and_show(const N64PpuFrame *frame, const char *status,
                          int compare_with_software);

#endif
