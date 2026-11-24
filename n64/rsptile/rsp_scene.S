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
# The caller has set copy mode, the TLUT (loaded) and the scissor. Tiles are
# loaded in aligned groups of 8 (512 bytes: 64 TMEM words, a tile row a
# word), and TMEM's lower half keeps four groups (Tile_slot), the least
# recently used one replaced; a group is tile descriptor 0's rows
# 64 * slot .. 64 * slot + 63. Per tile:
#   [SYNC_LOAD, SET_TEXTURE_IMAGE (the group, as RGBA16), LOAD_BLOCK (tile
#   1 + slot: 256 16-bit texels) when its group is not in TMEM], [SYNC_TILE,
#   SET_TILE 0 when the palette changes], TEXTURE_RECTANGLE (tile 0).
# (The owner's console, rdptest page 6: a layer's 896 tiles with a load
# each 5.4 ms of RDP time, with 28 loads 3.1 ms; TMEM time follows the bytes
# loaded, so one 32-tile group would cost more than it saves: the games'
# tile rows jump between groups.)
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
ROW_SC2:   .ds.b 4                  # SceneCmd_Bg: the row's second-screen offset
ROW_PWRAP: .ds.b 4                  # SceneCmd_Bg: TMAP address of the column after the wrap
# 16x16 tiles (SCJ_BIG): which half of its tilemap entry an 8-pixel column
# and the row's lines are in. A tilemap entry then names four characters:
# itself, the next (right half), and those 16 further on (lower half), the
# halves swapped by the entry's flips. All 0 with 8x8 tiles.
ROW_HC:    .ds.b 1                  # the next column's half (0 left, 1 right)
ROW_HCUR:  .ds.b 1                  # this column's
ROW_HR:    .ds.b 1                  # the row's half (0 upper, 1 lower)
           .ds.b 1
    .align 3
RECB:    .ds.b 512                  # tile records (SceneCmd_Tiles), 64 at a time
# The tile groups in TMEM (Tile_slot): group number (tile >> 3, 0xffff:
# none) and time of last use of each of the four slots, and the clock.
SLOT_GRP:   .ds.b 8
SLOT_AGE:   .ds.b 8
SLOT_CLOCK: .ds.b 4

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

# Registers. Kept across RDPQ_Send (which clobbers s0, s3, s4, a0-a2,
# t0-t7, sp, at, ra) and the DMA helpers (t2, at, s4):
#   s1 row g0      s2 row g1      v0 BG line y0      v1 output pointer
#   a3 palette in tile 0 (-1: none)   k1 rectangles
#   fp screen x of the current tile (sx)   s5 its tilemap entry in TMAP
#   s6 the sx at which the map column wraps to the next screen
#   t8, s7 the row's TEXTURE_RECTANGLE words without the x parts
# Reloaded by Scene_reload after each RDPQ_Send and the row's DMAs:
#   a0 priority bit (0 or 0x2000)   a1 sx0   a2 sx1   s0 cache
#   s3 log2 of the tile's VRAM words   s4 character base
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

    # The empty-tile bitmap; without one, no tile is empty.
    lw s0, %lo(JOB + SCJ_EMPTY)
    bnez s0, 2f
    li s4, %lo(ZB)
    li t0, %lo(ZB) + 512
1:  addiu s4, s4, 4
    bne s4, t0, 1b
    sw zero, -4(s4)
    b 3f
    nop
2:  jal DMAIn
    li t0, DMA_SIZE(512, 1)
3:
    li v1, %lo(OUTB)
    lui t0, 0x2600                  # SYNC_LOAD
    EMIT t0, zero
    lui t0, 0x2800                  # SYNC_TILE
    EMIT t0, zero
    jal Slots_begin
    nop
    li a3, -1
    move k1, zero
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
    # (16x16 tiles: an entry covers 16 lines, so y0 >> 4 and y0 & 0x200)
    lbu t4, %lo(JOB + SCJ_XSC)
    andi s6, t4, 1                  # wider (s6 is free until the tile loop)
    andi t5, t4, 0xfc
    sll t5, t5, 8
    lbu t3, %lo(JOB + SCJ_BIG)
    srl t6, v0, 3
    and t7, t6, t3
    sb t7, %lo(ROW_HR)              # the lines' half of a 16-line entry
    srlv t6, t6, t3
    andi t6, t6, 31
    sll t6, t6, 5
    addu t5, t5, t6
    li t6, 0x100
    sllv t6, t6, t3
    and t6, v0, t6
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
    sw t6, %lo(ROW_SC2)
    lw t7, %lo(JOB + SCJ_VRAM)
    addu s0, t7, t5
    li t1, ~7
    and s0, s0, t1
    li s4, %lo(TMAP)
    jal DMAInAsync
    li t0, DMA_SIZE(72, 1)
    lw t7, %lo(JOB + SCJ_VRAM)
    lw t6, %lo(ROW_SC2)
    addu s0, t7, t6
    li t1, ~7
    and s0, s0, t1
    li s4, %lo(TMAP) + 72
    jal DMAIn
    li t0, DMA_SIZE(72, 1)
    jal Scene_reload
    nop

    # The row's rectangle words without x: t8 = TEXTURE_RECTANGLE w0 less
    # (x1 << 14), s7 = w1 less (x0 << 14).
    lhu t0, %lo(JOB + SCJ_OX)
    lhu t1, %lo(JOB + SCJ_OY)
    addu t2, t1, s2
    addiu t2, t2, -1
    sll t2, t2, 2                   # Y1
    lui t3, 0x2400
    or t2, t2, t3
    addiu t3, t0, -1
    sll t3, t3, 14
    addu t8, t2, t3                 # w0 = t8 + (x1 << 14), x1 exclusive
    addu t2, t1, s1
    sll t2, t2, 2                   # Y0
    sll t3, t0, 14
    addu s7, t2, t3                 # w1 = s7 + (x0 << 14)

    # The first tile's entry and where the map wraps: column m = hs >> 3,
    # screen (m >> 5) & 1 of a 64-wide map, then column 0 of the other
    # screen (of the same one when 32 wide).
    lw t0, %lo(JOB + SCJ_VRAM)
    andi t0, t0, 7
    addiu t7, t0, %lo(TMAP)         # screen A's column 0
    lhu t1, %lo(JOB + SCJ_HS)
    andi t2, t1, 7
    subu fp, zero, t2               # sx of tile 0 = -fine
    lbu t3, %lo(JOB + SCJ_BIG)
    srl t0, t1, 3
    and t0, t0, t3                  # the first column's half of its 16-pixel entry
    sb t0, %lo(ROW_HC)
    addiu t2, t3, 3
    srlv t1, t1, t2                 # m: the tilemap column (hs >> 3, or >> 4)
    srl t4, t1, 5
    andi t4, t4, 1
    and t4, t4, s6                  # screen of column m
    xor t9, t4, s6                  # screen after the wrap
    andi t5, t1, 31
    sll t6, t5, 1
    addu s5, t7, t6
    beqz t4, 1f
    li t6, 32
    addiu s5, s5, 72
1:  beqz t9, 1f
    subu t6, t6, t5
    addiu t7, t7, 72
1:  sw t7, %lo(ROW_PWRAP)
    sllv t6, t6, t2                 # 8 or 16 pixels an entry
    sll t0, t0, 3
    subu t6, t6, t0                 # less the half of the first entry already past
    addu s6, t6, fp                 # sx at the wrap

Tile_loop:
    slti t3, fp, 256
    beqz t3, Row_next
    nop
    bne fp, s6, 1f
    nop
    lw s5, %lo(ROW_PWRAP)
1:  lhu t5, 0(s5)                   # entry
    # The next column's entry: the next one, or with 16x16 tiles the same
    # one again for its right half.
    lbu t6, %lo(JOB + SCJ_BIG)
    lbu t7, %lo(ROW_HC)
    sb t7, %lo(ROW_HCUR)
    beqz t6, 2f
    addiu s5, s5, 2
    xori t3, t7, 1
    sb t3, %lo(ROW_HC)
    bnez t7, 2f
    nop
    addiu s5, s5, -2
2:
    # Priority.
    andi t6, t5, 0x2000
    bne t6, a0, Tile_next
    # Clip to the span: x0 = max(sx, sx0), x1 = min(sx + 8, sx1).
    move t0, fp
    slt t3, fp, a1
    beqz t3, 1f
    addiu t1, fp, 8
    move t0, a1
1:  slt t3, a2, t1
    beqz t3, 1f
    nop
    move t1, a2
1:  slt t3, t0, t1
    beqz t3, Tile_next

    # Tile index: ((tileadr + (e & 0x3ff) << wsh) & 0x7fff) >> wsh.
    andi t3, t5, 0x3ff
    # 16x16 tiles: + (half column ^ hflip) + 16 * (half row ^ vflip).
    lbu t4, %lo(JOB + SCJ_BIG)
    beqz t4, 5f
    lbu t4, %lo(ROW_HCUR)
    srl t6, t5, 14
    andi t6, t6, 1
    xor t4, t4, t6
    addu t3, t3, t4
    lbu t4, %lo(ROW_HR)
    srl t6, t5, 15
    xor t4, t4, t6
    sll t4, t4, 4
    addu t3, t3, t4
    andi t3, t3, 0x3ff
5:
    sllv t3, t3, s3
    addu t3, t3, s4
    andi t3, t3, 0x7fff
    srlv t3, t3, s3
    # Empty tiles draw nothing.
    srl t4, t3, 3
    lbu t4, %lo(ZB)(t4)
    andi t6, t3, 7
    srlv t4, t4, t6
    andi t4, t4, 1
    bnez t4, Tile_next
    nop
    jal Tile_slot                   # t9 = the tile's first row in TMEM
    move t9, s0
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
    sll t3, t1, 14
    addu t3, t3, t8                 # w0
    sll t4, t0, 14
    addu t4, t4, s7                 # w1
    sw t3, 0(v1)
    sw t4, 4(v1)
    # s = (hflip ? 8 : 0) + x0 - sx; t = vflip ? 7 - row : row
    subu t3, t0, fp
    andi t6, t5, 0x4000
    beqz t6, 1f
    andi t7, v0, 7
    addiu t3, t3, 8
1:  andi t6, t5, 0x8000
    lui t4, 0x1000                  # DsDx 4.0
    beqz t6, 1f
    ori t4, t4, 0x0400              # DtDy +1.0
    xori t7, t7, 7                  # 7 - row
    ori t4, t4, 0xFC00              # DtDy -1.0
1:  sll t3, t3, 21                  # s << 5 << 16
    addu t7, t7, t9
    sll t7, t7, 5
    or t3, t3, t7
    sw t3, 8(v1)
    sw t4, 12(v1)
    addiu v1, v1, 16
    addiu k1, k1, 1

    # Room for another tile (at most 64 bytes)?
    slti t3, v1, %lo(OUTB) + OUTB_SIZE - 64
    bnez t3, Tile_next
    nop
    li s4, %lo(OUTB)
    jal RDPQ_Send
    move s3, v1
    jal Scene_reload
    li v1, %lo(OUTB)

Tile_next:
    b Tile_loop
    addiu fp, fp, 8
Row_next:
    b Row_loop
    move s1, s2
    .endfunc

# Start of a command: no group in TMEM; the load descriptors 1-4 (RGBA16,
# pitch 0, TMEM word 64 * slot) and tile 0's size, (0,0)-(15,255): the four
# groups' rows (SET_TILE 0 has no mask, so the size clamps).
    .func Slots_begin
Slots_begin:
    li t0, -1
    sw t0, %lo(SLOT_GRP) + 0
    sw t0, %lo(SLOT_GRP) + 4
    sw zero, %lo(SLOT_AGE) + 0
    sw zero, %lo(SLOT_AGE) + 4
    sw zero, %lo(SLOT_CLOCK)
    lui t0, 0x3510                  # SET_TILE 1: TMEM 0
    lui t1, 0x0100
    EMIT t0, t1
    ori t3, t0, 64                  # SET_TILE 2: TMEM 64
    lui t1, 0x0200
    EMIT t3, t1
    ori t3, t0, 128
    lui t1, 0x0300
    EMIT t3, t1
    ori t3, t0, 192
    lui t1, 0x0400
    EMIT t3, t1
    lui t0, 0x3200                  # SET_TILE_SIZE 0: (0,0)-(15,255)
    li t1, (60 << 12) | 1020
    EMIT t0, t1
    jr ra
    nop
    .endfunc

# The TMEM rows of tile t3 (cache index) of the cache at t9: returns
# t9 = the tile's first row for tile descriptor 0, after loading the tile's
# group over the least recently used slot when no slot holds it.
# Clobbers t2, t3, t4, t6, t7.
    .func Tile_slot
Tile_slot:
    srl t4, t3, 3                   # the group
    lhu t6, %lo(SLOT_GRP) + 0
    beq t6, t4, Slot_have
    li t7, 0                        # t7: slot * 2
    lhu t6, %lo(SLOT_GRP) + 2
    beq t6, t4, Slot_have
    li t7, 2
    lhu t6, %lo(SLOT_GRP) + 4
    beq t6, t4, Slot_have
    li t7, 4
    lhu t6, %lo(SLOT_GRP) + 6
    beq t6, t4, Slot_have
    li t7, 6
    # Not in TMEM: the slot used longest ago (an unused one's time is 0).
    lhu t6, %lo(SLOT_AGE) + 0
    li t7, 0
    lhu t2, %lo(SLOT_AGE) + 2
    subu t2, t2, t6
    bgez t2, 1f
    nop
    lhu t6, %lo(SLOT_AGE) + 2
    li t7, 2
1:  lhu t2, %lo(SLOT_AGE) + 4
    subu t2, t2, t6
    bgez t2, 1f
    nop
    lhu t6, %lo(SLOT_AGE) + 4
    li t7, 4
1:  lhu t2, %lo(SLOT_AGE) + 6
    subu t2, t2, t6
    bgez t2, 1f
    nop
    li t7, 6
1:  sh t4, %lo(SLOT_GRP)(t7)
    sll t6, t4, 9                   # 512 bytes a group
    addu t9, t9, t6
    lui t6, 0x2600                  # SYNC_LOAD
    EMIT t6, zero
    lui t6, 0x3D10                  # SET_TEXTURE_IMAGE: RGBA16, width 4
    ori t6, t6, 3
    EMIT t6, t9
    srl t2, t7, 1
    addiu t2, t2, 1
    sll t2, t2, 24                  # LOAD_BLOCK tile 1 + slot: 256 texels, dxt 1.0
    lui t6, 0x000F
    ori t6, t6, 0xF800
    or t2, t2, t6
    lui t6, 0x3300
    EMIT t6, t2
Slot_have:
    lhu t6, %lo(SLOT_CLOCK) + 2
    addiu t6, t6, 1
    sh t6, %lo(SLOT_CLOCK) + 2
    sh t6, %lo(SLOT_AGE)(t7)
    andi t3, t3, 7
    sll t9, t7, 2                   # slot * 8 tiles
    addu t9, t9, t3
    jr ra
    sll t9, t9, 3                   # 8 rows a tile
    .endfunc

# The pass's values that RDPQ_Send and the DMA helpers clobber (see the
# register list above SceneCmd_Bg).
    .func Scene_reload
Scene_reload:
    lbu a0, %lo(JOB + SCJ_PRIO)
    sll a0, a0, 13
    lhu a1, %lo(JOB + SCJ_SX0)
    lhu a2, %lo(JOB + SCJ_SX1)
    lw s0, %lo(JOB + SCJ_CACHE)
    lbu s3, %lo(JOB + SCJ_WSH)
    jr ra
    lhu s4, %lo(JOB + SCJ_TILEADR)
    .endfunc

    .func Scene_done
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
    jal Slots_begin
    nop
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
1:  jal Tile_slot                   # t9 = the tile's first row in TMEM
    lw t9, %lo(JOB + STJ_CACHE)
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
    addu t5, t5, t9
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
