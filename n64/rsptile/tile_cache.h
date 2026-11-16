/* SNES tile cache (docs/n64/PPU_DESIGN.md, work plan step 3).
 *
 * The RSP keeps RDRAM copies of VRAM's tiles as RDP textures, for each
 * enabled depth: 2bpp and 4bpp tiles as CI4, 8bpp tiles as CI8. A tile is
 * 8 rows; each row holds the tile's pixels left to right, then right to
 * left, so a texture rectangle reads the normal half (s 0-7) or the
 * horizontally flipped half (s 8-15) in copy mode, which cannot flip
 * horizontally; a negative DtDy flips vertically. Rows are 8 bytes (CI4)
 * or 16 bytes (CI8), so consecutive tiles form one texture 16 texels wide:
 * tile k at t = 8k. Tiles are indexed by VRAM byte address: 2bpp tile at
 * address a is C2 tile a / 16, 4bpp C4 tile a / 32, 8bpp C8 tile a / 64.
 * The colour indices exclude the palette number (the tile descriptor's
 * palette supplies it for CI4).
 *
 * Palettes (TLUTs) come from CGRAM with brightness applied; transparency is
 * the TLUT entry's alpha (alpha compare), so the arrangement depends on the
 * layer's depth (TC_TLUT_*, tile_cache_layout.h).
 *
 * tile_cache_ref.c holds the C reference of both conversions (portable,
 * also built natively); the RSP overlay (rsp_tile.S) must match it byte for
 * byte (rsptile/test). */
#ifndef N64_TILE_CACHE_H
#define N64_TILE_CACHE_H

#include <stdint.h>

#include "tile_cache_layout.h"

enum {
  TC_FMT_2BPP = 1u << 0,
  TC_FMT_4BPP = 1u << 1,
  TC_FMT_8BPP = 1u << 2,
};
enum {
  TC_C2_TILES = 0x10000 / 16,
  TC_C4_TILES = 0x10000 / 32,
  TC_C8_TILES = 0x10000 / 64,
};

/* ---- C reference (tile_cache_ref.c) ---- */
/* Tile `tile` of depth bpp (2, 4 or 8) from VRAM words, in the cache format
 * (TC_CI4_TILE or TC_CI8_TILE bytes). */
void tc_ref_tile(const uint16_t *vram, int bpp, unsigned tile, uint8_t *out);
/* A TLUT (256 big-endian RGBA5551 entries, as the RDP reads them, written
 * as uint16_t values). */
void tc_ref_tlut(const uint16_t *cgram, unsigned brightness, unsigned kind,
                 unsigned base_lo, unsigned base_hi, uint16_t *out);

/* ---- The RSP side (tile_cache.c, N64 only) ---- */
/* Allocate the enabled caches and the overlay's copy of VRAM (all zero:
 * consistent, since zero VRAM converts to zero tiles). */
void n64_tile_cache_init(unsigned formats);
/* Queue a sync of every cache with this VRAM image (the RSP reads it when
 * the command runs; the caller keeps it unchanged until then). Returns the
 * job's result slot: groups converted, valid after rspq_wait. */
volatile uint32_t *n64_tile_cache_sync(const uint16_t *vram);
/* Queue a TLUT build into dst (256 entries, 8-aligned). */
void n64_tile_cache_tlut(const uint16_t *cgram, unsigned brightness, unsigned kind,
                         unsigned base_lo, unsigned base_hi, uint16_t *dst);
const uint8_t *n64_tile_cache_c2(void);
const uint8_t *n64_tile_cache_c4(void);
const uint8_t *n64_tile_cache_c8(void);
/* Empty-tile bitmaps of the 2bpp and 4bpp caches (RDRAM, written by the
 * RSP): bit t of byte t / 8 set when tile t is all colour 0. */
const uint8_t *n64_tile_cache_empty2(void);
const uint8_t *n64_tile_cache_empty4(void);

#endif
