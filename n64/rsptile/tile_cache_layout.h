/* Tile cache job block, shared by rsp_tile.S and tile_cache.c
 * (big-endian, 32 bytes). See tile_cache.h. */
#ifndef N64_TILE_CACHE_LAYOUT_H
#define N64_TILE_CACHE_LAYOUT_H

#define TCJ_VRAM     0   /* u32 RDRAM address of the 64 KB VRAM image (2-aligned) */
#define TCJ_PREV     4   /* u32 RDRAM address of the last converted image (8-aligned) */
#define TCJ_C2       8   /* u32 2bpp cache (CI4 rows), 0: off */
#define TCJ_C4      12   /* u32 4bpp cache (CI4 rows), 0: off */
#define TCJ_C8      16   /* u32 8bpp cache (CI8 rows), 0: off */
#define TCJ_FIRST   20   /* u16 first 256-byte block */
#define TCJ_NBLOCKS 22   /* u16 blocks */
#define TCJ_PAD     24   /* u32 */
#define TCJ_RESULT  28   /* u32 written by the RSP: 64-byte groups converted */
#define TCJ_Z2      32   /* u32 empty-tile bitmap of C2 (512 bytes, bit t: tile t all zero), 0: off */
#define TCJ_Z4      36   /* u32 empty-tile bitmap of C4 (256 bytes), 0: off */
#define TCJ_SIZE    48

#define TC_BLOCK    256  /* VRAM bytes per block */

/* Tile sizes in the caches: a CI4 tile is 8 rows of 8 bytes (pixels 0-7,
 * then 7-0), a CI8 tile 8 rows of 16 bytes. */
#define TC_CI4_TILE 64
#define TC_CI8_TILE 128

/* TLUT job (TlutCmd): see tile_cache.h. */
#define TLJ_CGRAM    0   /* u32 RDRAM address of CGRAM (256 x u16 BGR555, 2-aligned) */
#define TLJ_DST      4   /* u32 RDRAM address of the TLUT (256 x u16 RGBA5551, 8-aligned) */
#define TLJ_BRIGHT   8   /* u16 INIDISP brightness 0-15 */
#define TLJ_KIND    10   /* u16 TC_TLUT_* */
#define TLJ_BASE_LO 12   /* u16 TC_TLUT_2BPP: CGRAM base of entries 0-127 */
#define TLJ_BASE_HI 14   /* u16 TC_TLUT_2BPP: CGRAM base of entries 128-255, or 0x100: OBJ */
#define TLJ_SIZE    16

#define TC_TLUT_16   0   /* entry i = CGRAM[i], entries 16n transparent (4bpp BG, OBJ) */
#define TC_TLUT_256  1   /* entry i = CGRAM[i], entry 0 transparent (8bpp) */
#define TC_TLUT_2BPP 2   /* entry 16q + j = CGRAM[base + 4q + j] for j < 4, 16q transparent */

#endif
