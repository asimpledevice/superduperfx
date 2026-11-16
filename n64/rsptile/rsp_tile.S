# SNES tile cache on the RSP (docs/n64/PPU_DESIGN.md, work plan step 3).
#
# TileCmd_Sync: bring the RDRAM tile caches up to date with a VRAM image.
# The overlay keeps "prev", the VRAM bytes it last converted. VRAM is
# walked in 256-byte blocks; each 16-byte unit is compared with prev, and
# every 64-byte group with a changed unit is converted into each enabled
# cache: four 2bpp tiles (C2), two 4bpp tiles (C4), one 8bpp tile (C8).
# The block is then written back to prev. Tiles are indexed by VRAM byte
# address (C2 tile = address / 16, C4 / 32, C8 / 64), so a cache holds every
# tile of its depth; the conversion (rsp_tile_conv.inc, gen_conv.py) is
# palette-independent: colour indices without the palette number, which the
# RDP tile descriptor supplies.
#
# TileCmd_Tlut: CGRAM to a 256-entry RGBA5551 TLUT with INIDISP brightness
# (round(c * l / 15) per 5-bit component, as ppu_brightness_level), in one
# of three arrangements (tile_cache_layout.h TC_TLUT_*). Transparency is
# the entry's alpha (the RDP's alpha compare), not the colour index.
#
# The VRAM image may sit at any even address (the PPU's array is 4 mod 8):
# DMA ignores the low three address bits, so blocks are read 8 bytes long
# and units loaded with LQV/LRV from (address & 7).

#include <rsp_queue.inc>
#include "tile_cache_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand TileCmd_Sync, 4
    RSPQ_DefineCommand TileCmd_Tlut, 4
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8
RSPQ_EndSavedState

    .align 4
# TLUT constants: e0 31, e1 2185 (x / 30 = x * 2185 >> 16 for x < 946),
# e7 2 * brightness (set per job).
TLK:    .half 31, 2185, 15, 0, 0, 0, 0, 0
# Lanes 1-3 of a 2bpp palette block.
TLM4:   .half 0, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0
K15:    .half 15, 15, 15, 15, 15, 15, 15, 15

    .bss
    .align 4
JOB:    .ds.b TCJ_SIZE
    .align 4
BUFV:   .ds.b TC_BLOCK + 16         # VRAM block as read (+ address & 7)
    .align 4
BUFA:   .ds.b TC_BLOCK              # the block, aligned
    .align 4
BUFP:   .ds.b TC_BLOCK              # prev
    .align 4
OUT2:   .ds.b 4 * TC_CI4_TILE
OUT4:   .ds.b 2 * TC_CI4_TILE
OUT8:   .ds.b TC_CI8_TILE
    .align 4
ZB2:    .ds.b 512                   # empty-tile bitmaps (TCJ_Z2, TCJ_Z4)
ZB4:    .ds.b 256
# The TLUT command reuses the sync's buffers.
#define TLIN  BUFV                  /* 528 bytes: BUFV and BUFA */
#define TLOUT OUT2                  /* 512 bytes: OUT2, OUT4, OUT8 */

    .text

# Registers during a sync:
#   s5 VRAM address of the block   s6 prev address of the block
#   s7 blocks left                 k0 VRAM address & 7
#   k1 block index                 s1 C2  s2 C4  s3 C8
#   fp return address              t9 changed units of the block
#   a1 group                       v1 groups converted
# DMA helpers clobber t0, t2, at, s0, s4.

    .func TileCmd_Sync
TileCmd_Sync:
    move fp, ra
    li t0, 0xffffff
    and s0, a0, t0
    move a3, s0                     # the job's RDRAM address
    li s4, %lo(JOB)
    jal DMAIn
    li t0, DMA_SIZE(TCJ_SIZE, 1)
    lw s5, %lo(JOB + TCJ_VRAM)
    lw s6, %lo(JOB + TCJ_PREV)
    lw s1, %lo(JOB + TCJ_C2)
    lw s2, %lo(JOB + TCJ_C4)
    lw s3, %lo(JOB + TCJ_C8)
    lhu k1, %lo(JOB + TCJ_FIRST)
    lhu s7, %lo(JOB + TCJ_NBLOCKS)
    lw s0, %lo(JOB + TCJ_Z2)
    beqz s0, 1f
    li s4, %lo(ZB2)
    jal DMAIn
    li t0, DMA_SIZE(512, 1)
1:  lw s0, %lo(JOB + TCJ_Z4)
    beqz s0, 1f
    li s4, %lo(ZB4)
    jal DMAIn
    li t0, DMA_SIZE(256, 1)
1:
    andi k0, s5, 7
    sll t1, k1, 8
    addu s5, s5, t1
    addu s6, s6, t1
    move v1, zero

Block_loop:
    beqz s7, Sync_done
    nop
    # The block, and prev's copy of it.
    li t1, ~7
    and s0, s5, t1
    li s4, %lo(BUFV)
    jal DMAInAsync
    li t0, DMA_SIZE(TC_BLOCK + 8, 1)
    move s0, s6
    li s4, %lo(BUFP)
    jal DMAIn
    li t0, DMA_SIZE(TC_BLOCK, 1)

    # Compare and align, 16 bytes at a time: t9 bit u = unit u changed.
    move t9, zero
    li t3, %lo(BUFV)
    addu t3, t3, k0
    li t4, %lo(BUFP)
    li t5, %lo(BUFA)
    li t6, 1
    li t7, %lo(BUFA) + TC_BLOCK
1:  lqv $v01, 0x00, t3
    lrv $v01, 0x10, t3
    lqv $v02, 0x00, t4
    sqv $v01, 0x00, t5
    veq $v03, $v01, $v02
    cfc2 t1, COP2_CTRL_VCC
    andi t1, t1, 0xff
    xori t1, t1, 0xff
    beqz t1, 2f
    nop
    or t9, t9, t6
2:  sll t6, t6, 1
    addiu t3, t3, 16
    addiu t4, t4, 16
    addiu t5, t5, 16
    bne t5, t7, 1b
    nop
    beqz t9, Block_next
    nop

    # Changed groups (64 bytes: 4 units) of the block.
    move a1, zero
Group_loop:
    srl t1, t9, 0
    sll t2, a1, 2
    srlv t1, t1, t2
    andi t1, t1, 0xf
    beqz t1, Group_next
    nop
    jal DMAWaitIdle                 # the previous group's output has left
    nop
    addiu v1, v1, 1
    li t1, %lo(BUFA)
    sll t2, a1, 6
    addu a2, t1, t2                 # the group's 64 bytes

    # Empty tiles: t5 bit j = unit j all zero.
    move t5, zero
    move t3, a2
    li t6, 1
2:  lw t1, 0(t3)
    lw t2, 4(t3)
    or t1, t1, t2
    lw t2, 8(t3)
    or t1, t1, t2
    lw t2, 12(t3)
    or t1, t1, t2
    bnez t1, 3f
    nop
    or t5, t5, t6
3:  sll t6, t6, 1
    addiu t3, t3, 16
    li t1, 16
    bne t6, t1, 2b
    nop
    # C2: nibble (group & 1) of byte block * 2 + group / 2.
    sll t1, k1, 1
    srl t2, a1, 1
    addu t1, t1, t2
    lbu t3, %lo(ZB2)(t1)
    andi t2, a1, 1
    sll t2, t2, 2                   # 0 or 4
    li t4, 0xf
    sllv t4, t4, t2
    nor t4, t4, zero
    and t3, t3, t4
    sllv t4, t5, t2
    or t3, t3, t4
    sb t3, %lo(ZB2)(t1)
    # C4: bits 2 * group of byte block: tile 0 = units 0 and 1, tile 1 = 2 and 3.
    andi t2, t5, 3
    xori t2, t2, 3
    sltiu t2, t2, 1                 # tile 0 empty
    andi t4, t5, 12
    xori t4, t4, 12
    sltiu t4, t4, 1
    sll t4, t4, 1
    or t2, t2, t4                   # 2 bits
    lbu t3, %lo(ZB4)(k1)
    sll t1, a1, 1
    li t4, 3
    sllv t4, t4, t1
    nor t4, t4, zero
    and t3, t3, t4
    sllv t2, t2, t1
    or t3, t3, t2
    sb t3, %lo(ZB4)(k1)

    beqz s1, 3f                     # C2: four tiles, one unit each
    nop
    li t8, %lo(OUT2)
    move t3, a2
    li t4, 4
2:  lqv $v01, 0x00, t3
    jal Conv2
    addiu t4, t4, -1
    addiu t3, t3, 16
    bnez t4, 2b
    addiu t8, t8, TC_CI4_TILE
    # C2 + (block * 16 + group * 4) * 64
    sll t1, k1, 10
    sll t2, a1, 8
    addu t1, t1, t2
    addu s0, s1, t1
    li s4, %lo(OUT2)
    jal DMAOutAsync
    li t0, DMA_SIZE(4 * TC_CI4_TILE, 1)
3:
    beqz s2, 3f                     # C4: two tiles, two units each
    nop
    li t8, %lo(OUT4)
    lqv $v01, 0x00, a2
    jal Conv4
    lqv $v02, 0x10, a2
    li t8, %lo(OUT4) + TC_CI4_TILE
    lqv $v01, 0x20, a2
    jal Conv4
    lqv $v02, 0x30, a2
    # C4 + (block * 8 + group * 2) * 64
    sll t1, k1, 9
    sll t2, a1, 7
    addu t1, t1, t2
    addu s0, s2, t1
    li s4, %lo(OUT4)
    jal DMAOutAsync
    li t0, DMA_SIZE(2 * TC_CI4_TILE, 1)
3:
    beqz s3, 3f                     # C8: one tile, four units
    nop
    li t8, %lo(OUT8)
    lqv $v01, 0x00, a2
    lqv $v02, 0x10, a2
    lqv $v03, 0x20, a2
    jal Conv8
    lqv $v04, 0x30, a2
    # C8 + (block * 4 + group) * 128
    sll t1, k1, 9
    sll t2, a1, 7
    addu t1, t1, t2
    addu s0, s3, t1
    li s4, %lo(OUT8)
    jal DMAOutAsync
    li t0, DMA_SIZE(TC_CI8_TILE, 1)
3:
Group_next:
    addiu a1, a1, 1
    li t1, TC_BLOCK / 64
    bne a1, t1, Group_loop
    nop

    # prev := the block (BUFA is not written again before the next block's
    # DMAIn, which completes after this transfer).
    move s0, s6
    li s4, %lo(BUFA)
    jal DMAOutAsync
    li t0, DMA_SIZE(TC_BLOCK, 1)

Block_next:
    addiu s5, s5, TC_BLOCK
    addiu s6, s6, TC_BLOCK
    addiu k1, k1, 1
    b Block_loop
    addiu s7, s7, -1

Sync_done:
    jal DMAWaitIdle
    nop
    lw s0, %lo(JOB + TCJ_Z2)
    beqz s0, 1f
    li s4, %lo(ZB2)
    jal DMAOut
    li t0, DMA_SIZE(512, 1)
1:  lw s0, %lo(JOB + TCJ_Z4)
    beqz s0, 1f
    li s4, %lo(ZB4)
    jal DMAOut
    li t0, DMA_SIZE(256, 1)
1:
    sw v1, %lo(JOB + TCJ_RESULT)
    addiu s0, a3, TCJ_PAD
    li s4, %lo(JOB + TCJ_PAD)
    li t0, DMA_SIZE(8, 1)
    move ra, fp
    j DMAOut                        # returns to rspq
    nop
    .endfunc

########################################################################
# TileCmd_Tlut: a0 = RDRAM address of a TLUT job (tile_cache_layout.h).
########################################################################
    .func TileCmd_Tlut
TileCmd_Tlut:
    move fp, ra
    li t0, 0xffffff
    and s0, a0, t0
    li s4, %lo(JOB)
    jal DMAIn
    li t0, DMA_SIZE(TLJ_SIZE, 1)
    lw t1, %lo(JOB + TLJ_CGRAM)
    andi k0, t1, 7
    li t2, ~7
    and s0, t1, t2
    li s4, %lo(TLIN)
    jal DMAIn
    li t0, DMA_SIZE(512 + 8, 1)

    li t1, %lo(TLK)
    lqv $v24, 0x00, t1              # constants
    lqv $v25, 0x10, t1              # TLM4
    lqv $v26, 0x20, t1              # K15
    lhu t1, %lo(JOB + TLJ_BRIGHT)
    sll t1, t1, 1
    mtc2 t1, $v24.e7                # 2 * brightness
    lhu t5, %lo(JOB + TLJ_KIND)
    li t3, %lo(TLIN)
    addu t3, t3, k0                 # CGRAM[0]
    li t4, %lo(TLOUT)
    li t6, TC_TLUT_2BPP
    beq t5, t6, Tlut_2bpp
    nop

    # TC_TLUT_16 / TC_TLUT_256: entry i = colour(CGRAM[i]).
    li t7, %lo(TLOUT) + 512
1:  lqv $v01, 0x00, t3
    jal Colour8
    lrv $v01, 0x10, t3
    sqv $v02, 0x00, t4
    addiu t3, t3, 16
    addiu t4, t4, 16
    bne t4, t7, 1b
    nop
    # Transparent entries: 0, or 0 and every 16th.
    li t4, %lo(TLOUT)
    li t1, TC_TLUT_256
    beq t5, t1, Tlut_out
    sh zero, 0(t4)
    li t7, %lo(TLOUT) + 512
1:  sh zero, 0(t4)
    addiu t4, t4, 32
    bne t4, t7, 1b
    nop
    b Tlut_out
    nop

Tlut_2bpp:
    # Blocks 0-7 from base_lo, 8-15 from base_hi (or OBJ: CGRAM 128-255).
    lhu t1, %lo(JOB + TLJ_BASE_LO)
    jal Tlut_half
    nop
    lhu t1, %lo(JOB + TLJ_BASE_HI)
    li t2, 0x100
    bne t1, t2, 2f
    nop
    # OBJ: entries 128-255 = CGRAM 128-255, every 16th transparent.
    li t3, %lo(TLIN) + 256
    addu t3, t3, k0
    li t4, %lo(TLOUT) + 256
    li t7, %lo(TLOUT) + 512
1:  lqv $v01, 0x00, t3
    jal Colour8
    lrv $v01, 0x10, t3
    sqv $v02, 0x00, t4
    addiu t3, t3, 16
    addiu t4, t4, 16
    bne t4, t7, 1b
    nop
    li t4, %lo(TLOUT) + 256
1:  sh zero, 0(t4)
    addiu t4, t4, 32
    bne t4, t7, 1b
    nop
    b Tlut_out
    nop
2:  jal Tlut_half
    nop

Tlut_out:
    lw s0, %lo(JOB + TLJ_DST)
    li s4, %lo(TLOUT)
    li t0, DMA_SIZE(512, 1)
    move ra, fp
    j DMAOut
    nop
    .endfunc

# Eight 2bpp palette blocks from CGRAM base t1 into TLOUT + t4 (t4 advances
# by 256): entry 16q + j = colour(CGRAM[base + 4q + j]) for j = 1-3, else 0.
    .func Tlut_half
Tlut_half:
    move v1, ra
    sll t1, t1, 1
    li t3, %lo(TLIN)
    addu t3, t3, k0
    addu t3, t3, t1                 # CGRAM[base]
    addiu t7, t4, 256
1:  lqv $v01, 0x00, t3
    jal Colour8
    lrv $v01, 0x10, t3
    vand $v02, $v02, $v25           # lanes 1-3
    sqv $v02, 0x00, t4
    sqv vzero, 0x10, t4
    addiu t3, t3, 8                 # 4 colours
    addiu t4, t4, 32                # 16 entries
    bne t4, t7, 1b
    nop
    jr v1
    nop
    .endfunc

# $v02 = RGBA5551 of the eight BGR555 colours in $v01, at brightness
# $v24.e7 / 2 ($v26: 15 in every lane). Clobbers $v03-$v07.
    .func Colour8
Colour8:
    vand $v03, $v01, $v24, e(0)     # r
    vmudl $v04, $v01, vshift8, e(4) # c >> 5 (c * 2^11 >> 16)
    vand $v04, $v04, $v24, e(0)     # g
    vmudl $v05, $v01, vshift, e(1)  # c >> 10 (c * 2^6 >> 16)
    vand $v05, $v05, $v24, e(0)     # b
    # v' = (2 * l * v + 15) * 2185 >> 16 = round(v * l / 15)
    vmudn $v06, $v03, $v24, e(7)
    vmadn $v06, $v26, vshift, e(7)  # + 15 * 1
    vsar $v06, COP2_ACC_LO
    vmudl $v03, $v06, $v24, e(1)    # r'
    vmudn $v06, $v04, $v24, e(7)
    vmadn $v06, $v26, vshift, e(7)
    vsar $v06, COP2_ACC_LO
    vmudl $v04, $v06, $v24, e(1)    # g'
    vmudn $v06, $v05, $v24, e(7)
    vmadn $v06, $v26, vshift, e(7)
    vsar $v06, COP2_ACC_LO
    vmudl $v05, $v06, $v24, e(1)    # b'
    # r' << 11 | g' << 6 | b' << 1 | 1
    vmudn $v07, $v03, vshift8, e(4)
    vmadn $v07, $v04, vshift, e(1)
    vmadn $v07, $v05, vshift, e(6)
    vsar $v02, COP2_ACC_LO
    vor $v02, $v02, vshift, e(7)
    jr ra
    nop
    .endfunc

#include "rsp_tile_conv.inc"
