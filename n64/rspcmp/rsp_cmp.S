# Two-screen colour math on the RSP: one composed line (N64PPU_LINE_COMPOSE)
# a command, exactly as n64_ppu_compose_line does on the CPU
# (n64/src/n64_ppu_build.c), which stays as the reference.
#
# CmpCmd_Line: word 0 the RDRAM address of the band's descriptor
# (cmp_rsp_layout.h), word 1 row << 24 | entries with pixels on the row
# (bit i: main entry i, bit 12 + i: subscreen entry i).
#
# 1. Top pixels. Each screen has a plane of 256 16-bit values, 2 * CGRAM
#    index, + 0x200 for a main pixel with math or a subscreen pixel that is
#    there. The entries are painted bottom first, 8 pixels a step: the
#    entry's plane row comes in by DMA, LUV loads 8 bytes as byte << 7, and
#    a pixel is taken where it is opaque for the entry's selector and its
#    span shows the layer.
# 2. Colours. One table serves every entry (a BG plane byte's low 7 bits,
#    an OBJ or 256-colour byte's 8, are a CGRAM index); its second half is
#    the same colours with bit 15 set, so the 0x200 flag survives the
#    lookup as the colour's bit 15. Scalar, a pixel at a time.
# 3. Math, 8 pixels a step: the second colour is the subscreen's where it
#    has a pixel, else the fixed colour; add or subtract a channel, clamp,
#    halve where asked, keep it where the main pixel has math and the span
#    allows it; brightness as (2 c l + 15) / 30 (ppu_brightness_level), the
#    division as * 2185 >> 16 (exact for c < 32, l < 16); pack RGBA5551.
#
# Not here: direct colour (the CPU composes those lines).

#include <rsp_queue.inc>
#include "cmp_rsp_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand CmpCmd_Line, 8
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8               # rspq needs a non-empty saved state
RSPQ_EndSavedState

    .align 4
CONSTS: .half 0x7fff, 31, 15, 2185, 0, 0, 0, 0

    .bss
    .align 4
DESC:   .ds.b CMP_SIZE
    .align 4
TAB:    .ds.b 1024                  # 256 colours, and again with bit 15
    .align 4
SPANP:  .ds.b 512                   # per pixel: 1 << its span
MAINP:  .ds.b 512                   # values, then colours, then the finished row
SUBP:   .ds.b 512
    .align 3
ROW:    .ds.b 256

    .text

# s3 return address   s7 row   s6 entries left (shifted)   s5 entry index
# DMA helpers clobber t0, t2, at, s0, s4.

CmpCmd_Line:
    move s3, ra
    li t0, 0xffffff
    and s0, a0, t0
    srl s7, a1, 24
    and s6, a1, t0
    li s4, %lo(DESC)
    jal DMAIn
    li t0, DMA_SIZE(CMP_SIZE, 1)
    ctc2 zero, COP2_CTRL_VCO

    # The colour table, and its copy with bit 15.
    lw s0, %lo(DESC + CMP_TABLE)
    li s4, %lo(TAB)
    jal DMAIn
    li t0, DMA_SIZE(512, 1)
    li t3, %lo(TAB)
    li t4, %lo(TAB) + 512
1:  lqv $v01, 0x00, t3
    lqv $v02, 0x10, t3
    vor $v01, $v01, vshift8, e(0)
    vor $v02, $v02, vshift8, e(0)
    addiu t3, t3, 32
    sqv $v01, 0x1e0, t3
    bne t3, t4, 1b
    sqv $v02, 0x1f0, t3

    # Planes: main = the backdrop (index 0) with its math flag, sub = nothing,
    # spans = span 0.
    lhu t3, %lo(DESC + CMP_BDFLAG)
    mtc2 t3, $v01.e0
    vor $v01, vzero, $v01, e(0)
    vor $v02, vzero, vshift, e(7)
    li t3, %lo(SPANP)
    li t4, %lo(SPANP) + 512
1:  sqv $v02, 0x00, t3
    sqv $v01, 0x200, t3
    addiu t3, t3, 16
    bne t3, t4, 1b
    sqv vzero, 0x3f0, t3

    # More than one span: mark the others' pixels.
    lhu t8, %lo(DESC + CMP_NSPANS)
    li t3, 1
    beq t8, t3, Spans_done
    li t4, 2
1:  sll t5, t3, 1
    lhu t6, %lo(DESC + CMP_EDGES)(t5)
    lhu t7, %lo(DESC + CMP_EDGES + 2)(t5)
    sll t6, t6, 1
    sll t7, t7, 1
2:  beq t6, t7, 3f
    nop
    sh t4, %lo(SPANP)(t6)
    b 2b
    addiu t6, t6, 2
3:  addiu t3, t3, 1
    bne t3, t8, 1b
    sll t4, t4, 1
Spans_done:

    # ---- 1. top pixels ------------------------------------------------
    move s5, zero
    srl t9, s6, 12                  # the subscreen's entries (for step 2)
Entry_loop:
    beqz s6, Entries_done
    andi t3, s6, 1
    beqz t3, Entry_next
    sll t3, s5, 2
    lw s0, %lo(DESC + CMP_BASES)(t3)
    sll t4, s7, 8
    sll t5, s7, 4
    addu s0, s0, t4
    addu s0, s0, t5                 # + row * 272
    li s4, %lo(ROW)
    jal DMAIn
    li t0, DMA_SIZE(256, 1)
    sll t3, s5, 4
    addiu t3, t3, %lo(DESC + CMP_PARAMS)
    lqv $v10, 0x00, t3
    li t4, %lo(MAINP)
    sltiu t5, s5, CMP_MAX_ENTRIES
    bnez t5, 1f
    li t5, %lo(ROW)
    li t4, %lo(SUBP)
1:  li t6, %lo(SPANP)
    li t7, %lo(ROW) + 256
2:  luv $v01, 0, 0, t5              # byte << 7
    lqv $v05, 0x00, t6
    lqv $v06, 0x00, t4
    vxor $v02, $v01, $v10, e(0)
    vand $v02, $v02, $v10, e(1)
    vmudl $v03, $v01, vshift8, e(5) # >> 6
    vand $v03, $v03, $v10, e(3)
    vmudl $v04, $v01, vshift8, e(3) # >> 4
    vand $v04, $v04, $v10, e(4)
    vor $v03, $v03, $v04
    vor $v03, $v03, $v10, e(5)
    vand $v05, $v05, $v10, e(6)
    vge $v07, $v02, $v10, e(2)      # opaque
    vmrg $v05, $v05, vzero          # ... its span bit, or 0
    vne $v07, $v05, vzero           # opaque and shown in its span
    vmrg $v06, $v03, $v06
    addiu t5, t5, 8
    addiu t6, t6, 16
    sqv $v06, 0x00, t4
    bne t5, t7, 2b
    addiu t4, t4, 16
Entry_next:
    srl s6, s6, 1
    b Entry_loop
    addiu s5, s5, 1
Entries_done:

    # ---- 2. colours ---------------------------------------------------
    li t5, %lo(MAINP)
    li t6, %lo(MAINP) + 512
    beqz t9, 1f
    nop
    li t6, %lo(MAINP) + 1024        # the subscreen's plane too
1:  lhu t3, 0(t5)
    lhu t4, 2(t5)
    andi t3, t3, 0x3fe
    andi t4, t4, 0x3fe
    lhu t3, %lo(TAB)(t3)
    lhu t4, %lo(TAB)(t4)
    sh t3, 0(t5)
    addiu t5, t5, 4
    bne t5, t6, 1b
    sh t4, -2(t5)

    # ---- 3. math, brightness, RGBA5551 --------------------------------
    li t3, %lo(CONSTS)
    lqv $v20, 0x00, t3              # 0x7fff, 31, 15, 2185
    li t3, %lo(DESC + CMP_MPARAMS)
    lqv $v21, 0x00, t3
    vnor $v23, vzero, vzero         # ones
    vor $v24, vzero, $v21, e(5)     # halve with a subscreen pixel
    vor $v25, vzero, $v21, e(6)     # halve with the fixed colour
    vor $v26, vzero, $v21, e(2)     # the fixed colour
    li t5, %lo(MAINP)
    li t6, %lo(SPANP)
    li t7, %lo(MAINP) + 512

.macro CHANNEL a, b, place, pe
    # \a: the main screen's channel, \b: the second colour's
    vmudn \b, \b, $v21, e(3)        # +b or -b
    vadd \b, \a, \b
    vge \b, \b, vzero               # not below 0
    vmudl $v14, \b, vshift8, e(0)   # >> 1
    vxor $v14, $v14, \b
    vand $v14, $v14, $v08
    vxor \b, \b, $v14               # halved where asked
    vlt \b, \b, $v20, e(1)          # not above 31
    vxor \b, \b, \a
    vand \b, \b, $v05
    vxor \a, \a, \b                 # the result where the pixel has math
    vmudn \a, \a, $v21, e(4)        # * 2 l
    vadd \a, \a, $v20, e(2)         # + 15
    vmudl \a, \a, $v20, e(3)        # / 30
    vmudn \a, \a, \place, e(\pe)
    vor $v15, $v15, \a
.endm

Math_loop:
    lqv $v01, 0x00, t5              # main: colour, bit 15 math
    lqv $v02, 0x200, t5             # sub: colour, bit 15 present
    lqv $v03, 0x00, t6              # span bit
    vand $v04, $v03, $v21, e(1)
    vne $v04, $v04, vzero
    vmrg $v04, vzero, $v23          # ones where the span is not clipped
    vand $v05, $v03, $v21, e(0)
    vne $v05, $v05, vzero
    vmrg $v05, $v01, vzero          # the main value where the span has math
    vlt $v06, $v05, vzero
    vmrg $v05, $v23, vzero          # ones where the pixel has math
    vand $v11, $v02, $v20, e(0)     # the subscreen's colour
    vlt $v06, $v02, vzero           # ... where it has a pixel
    vmrg $v08, $v24, $v25           # halve?
    vmrg $v11, $v11, $v26           # the second colour
    vand $v08, $v08, $v04           # (never where clipped)
    vand $v10, $v01, $v20, e(0)
    vand $v10, $v10, $v04           # the main colour, black where clipped
    vor $v15, vzero, vshift, e(7)   # alpha
    # red
    vand $v12, $v10, $v20, e(1)
    vand $v13, $v11, $v20, e(1)
    CHANNEL $v12, $v13, vshift8, 4  # << 11
    # green
    vmudl $v12, $v10, vshift8, e(4) # >> 5
    vmudl $v13, $v11, vshift8, e(4)
    vand $v12, $v12, $v20, e(1)
    vand $v13, $v13, $v20, e(1)
    CHANNEL $v12, $v13, vshift, 1   # << 6
    # blue
    vmudl $v12, $v10, vshift, e(1)  # >> 10
    vmudl $v13, $v11, vshift, e(1)
    CHANNEL $v12, $v13, vshift, 6   # << 1
    addiu t6, t6, 16
    sqv $v15, 0x00, t5
    addiu t5, t5, 16
    bne t5, t7, Math_loop
    nop

    # The row out.
    lw s0, %lo(DESC + CMP_OUT)
    sll t3, s7, 9
    addu s0, s0, t3
    li s4, %lo(MAINP)
    li t0, DMA_SIZE(512, 1)
    move ra, s3                     # DMAOut returns straight to rspq
    j DMAOut
    nop
