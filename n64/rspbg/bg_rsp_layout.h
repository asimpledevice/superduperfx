/* Job block shared by rsp_bg.S and bg_rsp.c (big-endian, 32 bytes). */
#ifndef N64_BG_RSP_LAYOUT_H
#define N64_BG_RSP_LAYOUT_H

#define BGJ_VRAM     0   /* u32 RDRAM address of the 64 KB VRAM image */
#define BGJ_DST      4   /* u32 RDRAM address of the first row + N64PPU_PAD */
#define BGJ_FLAGS    8   /* u32 RDRAM address of the row flags (8-aligned) */
#define BGJ_NROWS   12   /* u16 rows (lines) in the job, 1..224 */
#define BGJ_Y       14   /* u16 BG y of the first row (line + vscroll) */
#define BGJ_HS      16   /* u16 horizontal scroll */
#define BGJ_TILEADR 18   /* u16 character base (VRAM word address) */
#define BGJ_XSC     20   /* u8  BGnSC: tilemap base and size */
#define BGJ_BPP     21   /* u8  2 or 4 */
#define BGJ_PAL     22   /* u8  palette base (mode 0: 32 * layer) */
#define BGJ_SIZE    32

#define BGJ_ROW_STRIDE 272   /* N64PPU_STRIDE */
#define BGJ_MAX_ROWS   224   /* flags per job: bit 0 low-priority pixels, bit 1 high */

#endif
