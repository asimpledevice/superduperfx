/* Scene builder job block (SceneCmd_Bg, rsp_scene.S; scene_rsp.c),
 * big-endian, 32 bytes: one background layer, one priority, a run of rows
 * and one span. */
#ifndef N64_SCENE_LAYOUT_H
#define N64_SCENE_LAYOUT_H

#define SCJ_VRAM     0   /* u32 RDRAM address of the VRAM image (2-aligned) */
#define SCJ_CACHE    4   /* u32 tile cache (C4 for 4bpp layers, C2 for 2bpp) */
#define SCJ_HS       8   /* u16 */
#define SCJ_VS      10   /* u16 */
#define SCJ_XSC     12   /* u8 BGnSC */
#define SCJ_WSH     13   /* u8 log2 of the tile's VRAM words: 4 (4bpp) or 3 (2bpp) */
#define SCJ_TILEADR 14   /* u16 character base (VRAM word address) */
#define SCJ_R0      16   /* u8 first screen row */
#define SCJ_R1      17   /* u8 end row (exclusive) */
#define SCJ_PRIO    18   /* u8 0 or 1 */
#define SCJ_PAD     19
#define SCJ_SX0     20   /* u16 span [sx0, sx1) */
#define SCJ_SX1     22
#define SCJ_OX      24   /* u16 screen position of SNES x 0 */
#define SCJ_OY      26   /* u16 screen position of row 0 */
#define SCJ_RESULT  28   /* u32 written by the RSP: rectangles emitted */
#define SCJ_EMPTY   32   /* u32 the cache's empty-tile bitmap (512 bytes read), 0: none */
#define SCJ_SIZE    48

/* RDP buffer room a scene command requires before it starts (rsp_scene.S):
 * every command emits less than this (the CPU limits a command to 32 rows),
 * so once started it never waits for the RDP. */
#define SCENE_RDP_ROOM 0x4400

#endif
