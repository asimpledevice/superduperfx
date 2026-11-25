/* Band descriptor shared by rsp_cmp.S and cmp_rsp.c (big-endian). One per
 * band of composed lines (N64PPU_LINE_COMPOSE); a command names a
 * descriptor, a row and the entries with pixels on that row. */
#ifndef N64_CMP_RSP_LAYOUT_H
#define N64_CMP_RSP_LAYOUT_H

#define CMP_OUT      0x00   /* u32 RDRAM address of row 0's 256 RGBA5551 pixels (rows 512 bytes apart) */
#define CMP_TABLE    0x04   /* u32 RDRAM address of 256 15-bit colours (CGRAM) */
#define CMP_NSPANS   0x08   /* u16 spans, 1..5 */
#define CMP_BDFLAG   0x0A   /* u16 0x200 when the backdrop has math */
#define CMP_EDGES    0x0C   /* u16[6] span edges: span k is [edge k, edge k + 1) */
/* 0x20: eight u16, loaded as one vector */
#define CMP_MPARAMS  0x20   /* e0 spans with math (bit k), e1 clipped spans, e2 the fixed colour,
                             * e3 1 to add / 0xffff to subtract, e4 2 * brightness,
                             * e5 0x180: halve with a subscreen pixel (else 0), e6: with the fixed colour */
#define CMP_BASES    0x30   /* u32[24] RDRAM address of the entry's plane row 0 + N64PPU_PAD */
#define CMP_PARAMS   0x90   /* [24] eight u16 an entry (below) */
#define CMP_SIZE     0x210

/* Entries 0-11: main screen, 12-23: subscreen; both bottom first (the
 * order's painter order). An entry's parameters, with w = plane byte << 7:
 *   opaque when ((w ^ e0) & e1) >= e2 (signed) and the pixel's span is in e6
 *   value = ((w >> 6) & e3) | ((w >> 4) & e4) | e5
 * The value is 2 * CGRAM index, with 0x200 for "has math" (main) or
 * "present" (sub). */
#define CMP_MAX_ENTRIES 12
#define CMP_MAX_LINES 16     /* lines a command */
#define CMP_ROW_STRIDE 272   /* N64PPU_STRIDE */

#endif
