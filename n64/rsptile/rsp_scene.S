# Scene builder on the RSP (docs/n64/PPU_DESIGN.md, work plan step 4c).
#
# SceneCmd_Bg: one background layer, one priority, a run of screen rows and
# one horizontal span: reads the tilemap rows from the VRAM image and emits,
# for every tile of that priority, the RDP commands that draw it from the
# tile cache (n64/rsptile/tile_cache.h) in copy mode, straight into rdpq's
# command stream (rsp_rdpq.inc). The C reference is n64_scene_bg
# (n64/src/n64_ppu_scene.c); the drawing rules are scene_draw_bg's
# (n64_ppu_draw.c): tiles clipped to the span here (a copy-mode rectangle
# cut by the scissor's left edge starts its texture wrong), s from the
# normal or flipped half of the cache row, a negative DtDy for vertical
# flips.
#
# The caller has set copy mode, the TLUT (loaded) and the scissor. Per tile:
#   SYNC_LOAD, SET_TEXTURE_IMAGE (the tile, as RGBA16), LOAD_BLOCK (tile 1:
#   32 16-bit texels, one 8-byte TMEM word per row), [SYNC_TILE, SET_TILE 0
#   when the palette changes], TEXTURE_RECTANGLE (tile 0).
# The command ends with SYNC_LOAD, SYNC_TILE and SYNC_PIPE, so rdpq's own
# commands that follow need no knowledge of what ran here.

#include <rsp_queue.inc>
#include "scene_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand SceneCmd_Bg, 4
    RSPQ_DefineCommand SceneCmd_Tiles, 4
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
ROOM_WAITS:   .long 0               # retries for lack of RDP buffer room (statistics)
SAVED_PAD:    .long 0
RSPQ_EndSavedState

#define OUTB_SIZE 1024

    .bss
    .align 4
JOB:     .ds.b SCJ_SIZE
JOBADR:  .ds.b 4
    .align 3
TMAP:    .ds.b 2 * 72               # the two screens' tilemap rows (+8: alignment)
    .align 3
OUTB:    .ds.b OUTB_SIZE
    .align 3
ZB:      .ds.b 512                  # empty-tile bitmap
    .align 3
RECB:    .ds.b 512                  # tile records (SceneCmd_Tiles), 64 at a time

    .text

.macro ROOM_CHECK go
    lw t0, %lo(RDPQ_SENTINEL)
    lw t1, %lo(RDPQ_CURRENT)
    li t3, 0xFFFFFF
    and t1, t1, t3
    sub t0, t0, t1
    slti t0, t0, SCENE_RDP_ROOM
    beqz t0, \go               # room in the current buffer
    mfc0 t1, COP0_DP_STATUS
    lbu t3, %lo(RDPQ_SYNCFULL_ONGOING)
    ori t3, DP_STATUS_END_VALID
    and t1, t1, t3
    beqz t1, \go               # the next buffer can start now
    lw t1, %lo(ROOM_WAITS)
    addiu t1, t1, 1
    sw t1, %lo(ROOM_WAITS)
    j RSPQ_Loop
    addiu rspq_dmem_buf_ptr, rspq_dmem_buf_ptr, -4   # this command, again
.endm

# Emit one 8-byte RDP command (w0, w1) at v1.
.macro EMIT w0, w1
    sw \w0, 0(v1)
    sw \w1, 4(v1)
    addiu v1, v1, 8
.endm

# Registers (RDPQ_Send clobbers s0, s3, s4, a0-a2, t0-t7, sp, at, ra):
#   s1 row g0      s2 row g1      v0 BG line y0     t8 tile k
#   a3 palette in tile 0 (-1: none)   k1 rectangles   fp fine scroll
#   s5 first map column   s6 1 if the map is 64 wide   s7 VRAM & 7
#   v1 output pointer
    .func SceneCmd_Bg
SceneCmd_Bg:
    # Start only with SCENE_RDP_ROOM bytes of room in the RDP buffer: room
    # left in the current buffer, or no buffer pending at the RDP (so
    # RDPQ_Send can switch at once; as RSPQCmd_RdpSetBuffer waits, a SYNC_FULL
    # in progress included). A command emits less than that, so it never
    # spins on the RDP. Without room, step back over this command and return
    # to the dispatch loop, which serves Red's ring (the S-DSP) first and
    # then runs this command again.
    ROOM_CHECK Scene_go
Scene_go:
    li t0, 0xffffff
    and s0, a0, t0
    sw s0, %lo(JOBADR)
    li s4, %lo(JOB)
    jal DMAIn
    li t0, DMA_SIZE(SCJ_SIZE, 1)

    lw s0, %lo(JOB + SCJ_EMPTY)
    beqz s0, 1f
    li s4, %lo(ZB)
    jal DMAIn
    li t0, DMA_SIZE(512, 1)
1:
    li v1, %lo(OUTB)
    lui t0, 0x2600                  # SYNC_LOAD
    EMIT t0, zero
    lui t0, 0x2800                  # SYNC_TILE
    EMIT t0, zero
    lui t0, 0x3510                  # SET_TILE 1: RGBA16, TMEM 0, pitch 0 (LOAD_BLOCK)
    lui t1, 0x0100
    EMIT t0, t1
    lui t0, 0x3200                  # SET_TILE_SIZE 0: (0,0)-(15,7)
    li t1, (60 << 12) | 28
    EMIT t0, t1
    li a3, -1
    move k1, zero

    lw t0, %lo(JOB + SCJ_VRAM)
    andi s7, t0, 7
    lhu t0, %lo(JOB + SCJ_HS)
    andi fp, t0, 7
    srl s5, t0, 3
    lbu t0, %lo(JOB + SCJ_XSC)
    andi s6, t0, 1
    lbu s1, %lo(JOB + SCJ_R0)

Row_loop:
    lbu t0, %lo(JOB + SCJ_R1)
    slt t1, s1, t0
    beqz t1, Done
    lhu t1, %lo(JOB + SCJ_VS)
    addiu v0, s1, 1
    addu v0, v0, t1                 # y0 = row + 1 + vs
    andi t2, v0, 7
    li t3, 8
    subu t3, t3, t2
    addu s2, s1, t3                 # rows left in this tile row
    slt t3, t0, s2
    beqz t3, 1f
    nop
    move s2, t0
1:
    # sc = ((xsc & 0xfc) << 8) + (((y0 >> 3) & 31) << 5)
    #    + ((y0 & 0x100) && (xsc & 2) ? (wider ? 0x800 : 0x400) : 0)
    lbu t4, %lo(JOB + SCJ_XSC)
    andi t5, t4, 0xfc
    sll t5, t5, 8
    srl t6, v0, 3
    andi t6, t6, 31
    sll t6, t6, 5
    addu t5, t5, t6
    andi t6, v0, 0x100
    beqz t6, 1f
    andi t7, t4, 2
    beqz t7, 1f
    sll t7, s6, 10
    addiu t7, t7, 0x400
    addu t5, t5, t7
1:  sll t6, s6, 10
    addu t6, t5, t6                 # second screen (64-wide maps)
    andi t5, t5, 0x7fff
    andi t6, t6, 0x7fff
    sll t5, t5, 1
    sll t6, t6, 1
    lw t7, %lo(JOB + SCJ_VRAM)
    addu s0, t7, t5
    li t1, ~7
    and s0, s0, t1
    li s4, %lo(TMAP)
    jal DMAInAsync
    li t0, DMA_SIZE(72, 1)
    lw t7, %lo(JOB + SCJ_VRAM)
    addu s0, t7, t6
    li t1, ~7
    and s0, s0, t1
    li s4, %lo(TMAP) + 72
    jal DMAIn
    li t0, DMA_SIZE(72, 1)

    move t8, zero
Tile_loop:
    sll t2, t8, 3
    subu t2, t2, fp                 # sx
    slti t3, t2, 256
    beqz t3, Row_next
    # The entry: screen ((mx >> 5) & 1 when 64 wide), column mx & 31.
    addu t3, s5, t8
    srl t4, t3, 5
    andi t4, t4, 1
    and t4, t4, s6
    andi t3, t3, 31
    sll t3, t3, 1
    beqz t4, 1f
    addu t3, t3, s7
    addiu t3, t3, 72
1:  lhu t5, %lo(TMAP)(t3)           # entry

    # Priority.
    srl t6, t5, 13
    andi t6, t6, 1
    lbu t7, %lo(JOB + SCJ_PRIO)
    bne t6, t7, Tile_next
    # Clip to the span: x0 = max(sx, sx0), x1 = min(sx + 8, sx1).
    lhu t6, %lo(JOB + SCJ_SX0)
    lhu t7, %lo(JOB + SCJ_SX1)
    move t0, t2
    slt t3, t0, t6
    beqz t3, 1f
    addiu t1, t2, 8
    move t0, t6
1:  slt t3, t7, t1
    beqz t3, 1f
    nop
    move t1, t7
1:  slt t3, t0, t1
    beqz t3, Tile_next

    # Tile address: cache + ((tileadr + (e & 0x3ff) << wsh) & 0x7fff) >> wsh << 6.
    andi t3, t5, 0x3ff
    lbu t6, %lo(JOB + SCJ_WSH)
    sllv t3, t3, t6
    lhu t7, %lo(JOB + SCJ_TILEADR)
    addu t3, t3, t7
    andi t3, t3, 0x7fff
    srlv t3, t3, t6                 # tile index
    # Empty tiles draw nothing.
    lw t4, %lo(JOB + SCJ_EMPTY)
    beqz t4, 1f
    srl t4, t3, 3
    lbu t4, %lo(ZB)(t4)
    andi t6, t3, 7
    srlv t4, t4, t6
    andi t4, t4, 1
    bnez t4, Tile_next
    nop
1:  sll t3, t3, 6
    lw t9, %lo(JOB + SCJ_CACHE)
    addu t9, t9, t3

    lui t3, 0x2600                  # SYNC_LOAD
    EMIT t3, zero
    lui t3, 0x3D10                  # SET_TEXTURE_IMAGE: RGBA16, width 4
    ori t3, t3, 3
    EMIT t3, t9
    lui t3, 0x3300                  # LOAD_BLOCK tile 1: 32 texels, dxt 1.0
    lui t4, 0x0101
    ori t4, t4, 0xF800
    EMIT t3, t4
    srl t6, t5, 10
    andi t6, t6, 7                  # palette
    beq t6, a3, 1f
    lui t3, 0x2800                  # SYNC_TILE
    EMIT t3, zero
    lui t3, 0x3540                  # SET_TILE 0: CI4, pitch 8 bytes, TMEM 0
    ori t3, t3, 0x0200
    sll t4, t6, 20
    EMIT t3, t4
    move a3, t6
1:
    # TEXTURE_RECTANGLE (copy mode: lower right inclusive, DsDx 4.0).
    lhu t6, %lo(JOB + SCJ_OX)
    lhu t7, %lo(JOB + SCJ_OY)
    addu t3, t6, t1
    addiu t3, t3, -1
    sll t3, t3, 14                  # X1 << 12 (10.2)
    addu t4, t7, s2
    addiu t4, t4, -1
    sll t4, t4, 2                   # Y1
    or t3, t3, t4
    lui t4, 0x2400
    or t3, t3, t4                   # w0
    addu t4, t6, t0
    sll t4, t4, 14                  # X0 << 12; tile 0
    addu t6, t7, s1
    sll t6, t6, 2
    or t4, t4, t6                   # w1
    sw t3, 0(v1)
    sw t4, 4(v1)
    # s = (hflip ? 8 : 0) + x0 - sx; t = vflip ? 7 - row : row
    subu t3, t0, t2
    andi t6, t5, 0x4000
    beqz t6, 1f
    andi t7, v0, 7
    addiu t3, t3, 8
1:  andi t6, t5, 0x8000
    beqz t6, 1f
    li t4, 0x0400                   # DtDy +1.0
    li t6, 7
    subu t7, t6, t7
    li t4, 0xFC00                   # DtDy -1.0
1:  sll t3, t3, 21                  # s << 5 << 16
    sll t7, t7, 5
    or t3, t3, t7
    lui t6, 0x1000                  # DsDx 4.0
    or t4, t4, t6
    sw t3, 8(v1)
    sw t4, 12(v1)
    addiu v1, v1, 16
    addiu k1, k1, 1

    # Room for another tile (at most 64 bytes)?
    li t3, %lo(OUTB) + OUTB_SIZE - 64
    slt t3, v1, t3
    bnez t3, Tile_next
    nop
    li s4, %lo(OUTB)
    jal RDPQ_Send
    move s3, v1
    li v1, %lo(OUTB)

Tile_next:
    addiu t8, t8, 1
    slti t3, t8, 33
    bnez t3, Tile_loop
    nop
Row_next:
    b Row_loop
    move s1, s2

Done:
    lui t0, 0x2600                  # SYNC_LOAD
    EMIT t0, zero
    lui t0, 0x2800                  # SYNC_TILE
    EMIT t0, zero
    lui t0, 0x2700                  # SYNC_PIPE
    EMIT t0, zero
    li s4, %lo(OUTB)
    jal RDPQ_Send
    move s3, v1
    sw k1, %lo(JOB + SCJ_RESULT)
    lw s0, %lo(JOBADR)
    addiu s0, s0, SCJ_RESULT - 4
    li s4, %lo(JOB) + SCJ_RESULT - 4
    jal DMAOut
    li t0, DMA_SIZE(8, 1)
    j RSPQ_Loop
    nop
    .endfunc

# SceneCmd_Tiles: draw a list of tile records (scene_layout.h, ST_REC) from
# one CI4 tile cache, as SceneCmd_Bg draws its tiles; the CPU has clipped
# each record to its span and rows and ordered them (back to front). Used
# for sprites (n64_ppu_draw.c scene_draw_obj).
#   s1 records left   s2 RDRAM address of the next records   t8 record
#   a3 palette in tile 0 (-1: none)   k1 rectangles   s5 records in RECB
#   v1 output pointer
    .func SceneCmd_Tiles
SceneCmd_Tiles:
    ROOM_CHECK Tiles_go
Tiles_go:
    li t0, 0xffffff
    and s0, a0, t0
    sw s0, %lo(JOBADR)
    li s4, %lo(JOB)
    jal DMAIn
    li t0, DMA_SIZE(SCJ_SIZE, 1)
    lw s0, %lo(JOB + STJ_EMPTY)
    beqz s0, 1f
    li s4, %lo(ZB)
    jal DMAIn
    li t0, DMA_SIZE(256, 1)
1:
    li v1, %lo(OUTB)
    lui t0, 0x2600                  # SYNC_LOAD
    EMIT t0, zero
    lui t0, 0x2800                  # SYNC_TILE
    EMIT t0, zero
    lui t0, 0x3510                  # SET_TILE 1: RGBA16, TMEM 0, pitch 0 (LOAD_BLOCK)
    lui t1, 0x0100
    EMIT t0, t1
    lui t0, 0x3200                  # SET_TILE_SIZE 0: (0,0)-(15,7)
    li t1, (60 << 12) | 28
    EMIT t0, t1
    li a3, -1
    move k1, zero
    lhu s1, %lo(JOB + STJ_N)
    lw s2, %lo(JOB + STJ_LIST)
    move s5, zero

Rec_loop:
    beqz s1, Done
    nop
    bnez s5, Rec_have
    nop
    # Next batch of up to 64 records.
    move s0, s2
    li s4, %lo(RECB)
    jal DMAIn
    li t0, DMA_SIZE(512, 1)
    addiu s2, s2, 512
    li s5, 64
    li t8, %lo(RECB)
Rec_have:
    lhu t3, 0(t8)                   # tile
    lw t4, %lo(JOB + STJ_EMPTY)
    beqz t4, 1f
    srl t4, t3, 3
    lbu t4, %lo(ZB)(t4)
    andi t6, t3, 7
    srlv t4, t4, t6
    andi t4, t4, 1
    bnez t4, Rec_next
    nop
1:  sll t3, t3, 6
    lw t9, %lo(JOB + STJ_CACHE)
    addu t9, t9, t3
    lui t3, 0x2600                  # SYNC_LOAD
    EMIT t3, zero
    lui t3, 0x3D10                  # SET_TEXTURE_IMAGE: RGBA16, width 4
    ori t3, t3, 3
    EMIT t3, t9
    lui t3, 0x3300                  # LOAD_BLOCK tile 1: 32 texels, dxt 1.0
    lui t4, 0x0101
    ori t4, t4, 0xF800
    EMIT t3, t4
    lbu t5, 7(t8)                   # t | palette << 4
    srl t6, t5, 4                   # palette
    beq t6, a3, 1f
    lui t3, 0x2800                  # SYNC_TILE
    EMIT t3, zero
    lui t3, 0x3540                  # SET_TILE 0: CI4, pitch 8 bytes, TMEM 0
    ori t3, t3, 0x0200
    sll t4, t6, 20
    EMIT t3, t4
    move a3, t6
1:
    # TEXTURE_RECTANGLE (copy mode: lower right inclusive, DsDx 4.0).
    lhu t6, %lo(JOB + STJ_OX)
    lhu t7, %lo(JOB + STJ_OY)
    lbu t3, 3(t8)                   # x1 (inclusive)
    addu t3, t3, t6
    sll t3, t3, 14
    lbu t4, 5(t8)                   # r1 (inclusive)
    addu t4, t4, t7
    sll t4, t4, 2
    or t3, t3, t4
    lui t4, 0x2400
    or t3, t3, t4                   # w0
    lbu t4, 2(t8)                   # x0
    addu t4, t4, t6
    sll t4, t4, 14
    lbu t6, 4(t8)                   # r0
    addu t6, t6, t7
    sll t6, t6, 2
    or t4, t4, t6                   # w1
    sw t3, 0(v1)
    sw t4, 4(v1)
    lbu t3, 6(t8)                   # s | vflip
    andi t6, t3, 0x7f
    sll t6, t6, 21                  # s << 5 << 16
    andi t5, t5, 7
    sll t5, t5, 5                   # t << 5
    or t6, t6, t5
    andi t3, t3, ST_VFLIP
    beqz t3, 1f
    li t4, 0x0400                   # DtDy +1.0
    li t4, 0xFC00                   # DtDy -1.0
1:  lui t3, 0x1000                  # DsDx 4.0
    or t4, t4, t3
    sw t6, 8(v1)
    sw t4, 12(v1)
    addiu v1, v1, 16
    addiu k1, k1, 1
    li t3, %lo(OUTB) + OUTB_SIZE - 64
    slt t3, v1, t3
    bnez t3, Rec_next
    nop
    li s4, %lo(OUTB)
    jal RDPQ_Send
    move s3, v1
    li v1, %lo(OUTB)
Rec_next:
    addiu t8, t8, ST_REC
    addiu s5, s5, -1
    b Rec_loop
    addiu s1, s1, -1
    .endfunc

#include <rsp_rdpq.inc>
