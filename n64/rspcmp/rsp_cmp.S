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
#
# Timing. The RSP issues one scalar and one vector instruction a cycle at
# best, and an instruction that reads a vector register stalls until three
# cycles after the one that wrote it (a general register: two). So every
# loop below works on several groups of pixels at once, a stage at a time
# -- the same operation for each group, then the next -- which puts four
# or more instructions between a result and its use. Compare and VMRG stay
# together (VCC is one register, and no stall hangs on it).

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
CONSTS: .half 0x7fff, 0x1f00, 31, 15, 2185, 0x0100, 0xffff, 1

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
# s2 the subscreen has entries
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
    lqv $v03, 0x20, t3
    lqv $v04, 0x30, t3
    vor $v01, $v01, vshift8, e(0)
    vor $v02, $v02, vshift8, e(0)
    vor $v03, $v03, vshift8, e(0)
    vor $v04, $v04, vshift8, e(0)
    addiu t3, t3, 64
    sqv $v01, 0x1c0, t3
    sqv $v02, 0x1d0, t3
    sqv $v03, 0x1e0, t3
    bne t3, t4, 1b
    sqv $v04, 0x1f0, t3

    # Planes: main = the backdrop (index 0) with its math flag, sub = nothing,
    # spans = span 0.
    lhu t3, %lo(DESC + CMP_BDFLAG)
    mtc2 t3, $v01.e0
    li t3, %lo(SPANP)
    li t4, %lo(SPANP) + 512
    vor $v02, vzero, vshift, e(7)
    vor $v01, vzero, $v01, e(0)
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
    srl s2, s6, 12                  # the subscreen's entries (for step 2)
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
    # One span and no per-pixel flag (every entry but the main screen's
    # sprites with math): the short loop. 32 pixels a pass in both.
    lhu t3, 8(t3)                   # e4
    lhu t8, %lo(DESC + CMP_NSPANS)
    addiu t8, t8, -1
    or t3, t3, t8
    bnez t3, 2f
    nop
4:
    luv $v01, 0, 0, t5
    luv $v02, 0, 8, t5
    luv $v03, 0, 16, t5
    luv $v04, 0, 24, t5
    lqv $v05, 0x00, t4
    lqv $v06, 0x10, t4
    lqv $v07, 0x20, t4
    lqv $v08, 0x30, t4
    vxor $v11, $v01, $v10, e(0)
    vxor $v12, $v02, $v10, e(0)
    vxor $v13, $v03, $v10, e(0)
    vxor $v14, $v04, $v10, e(0)
    vmudl $v15, $v01, vshift8, e(5)
    vmudl $v16, $v02, vshift8, e(5)
    vmudl $v17, $v03, vshift8, e(5)
    vmudl $v18, $v04, vshift8, e(5)
    vand $v11, $v11, $v10, e(1)
    vand $v12, $v12, $v10, e(1)
    vand $v13, $v13, $v10, e(1)
    vand $v14, $v14, $v10, e(1)
    vand $v15, $v15, $v10, e(3)
    vand $v16, $v16, $v10, e(3)
    vand $v17, $v17, $v10, e(3)
    vand $v18, $v18, $v10, e(3)
    vor $v15, $v15, $v10, e(5)
    vor $v16, $v16, $v10, e(5)
    vor $v17, $v17, $v10, e(5)
    vor $v18, $v18, $v10, e(5)
    vge $v09, $v11, $v10, e(2)
    vmrg $v05, $v15, $v05
    vge $v09, $v12, $v10, e(2)
    vmrg $v06, $v16, $v06
    vge $v09, $v13, $v10, e(2)
    vmrg $v07, $v17, $v07
    vge $v09, $v14, $v10, e(2)
    vmrg $v08, $v18, $v08
    addiu t5, t5, 32
    sqv $v05, 0x00, t4
    sqv $v06, 0x10, t4
    sqv $v07, 0x20, t4
    sqv $v08, 0x30, t4
    bne t5, t7, 4b
    addiu t4, t4, 64
    b Entry_next
    nop
2:
    luv $v01, 0, 0, t5
    luv $v02, 0, 8, t5
    luv $v03, 0, 16, t5
    luv $v04, 0, 24, t5
    lqv $v05, 0x00, t4
    lqv $v06, 0x10, t4
    lqv $v07, 0x20, t4
    lqv $v08, 0x30, t4
    lqv $v19, 0x00, t6
    lqv $v20, 0x10, t6
    lqv $v21, 0x20, t6
    lqv $v22, 0x30, t6
    vxor $v11, $v01, $v10, e(0)
    vxor $v12, $v02, $v10, e(0)
    vxor $v13, $v03, $v10, e(0)
    vxor $v14, $v04, $v10, e(0)
    vmudl $v15, $v01, vshift8, e(5)
    vmudl $v16, $v02, vshift8, e(5)
    vmudl $v17, $v03, vshift8, e(5)
    vmudl $v18, $v04, vshift8, e(5)
    vmudl $v23, $v01, vshift8, e(3)
    vmudl $v24, $v02, vshift8, e(3)
    vmudl $v25, $v03, vshift8, e(3)
    vmudl $v26, $v04, vshift8, e(3)
    vand $v11, $v11, $v10, e(1)
    vand $v12, $v12, $v10, e(1)
    vand $v13, $v13, $v10, e(1)
    vand $v14, $v14, $v10, e(1)
    vand $v15, $v15, $v10, e(3)
    vand $v16, $v16, $v10, e(3)
    vand $v17, $v17, $v10, e(3)
    vand $v18, $v18, $v10, e(3)
    vand $v23, $v23, $v10, e(4)
    vand $v24, $v24, $v10, e(4)
    vand $v25, $v25, $v10, e(4)
    vand $v26, $v26, $v10, e(4)
    vand $v19, $v19, $v10, e(6)
    vand $v20, $v20, $v10, e(6)
    vand $v21, $v21, $v10, e(6)
    vand $v22, $v22, $v10, e(6)
    vor $v15, $v15, $v23
    vor $v16, $v16, $v24
    vor $v17, $v17, $v25
    vor $v18, $v18, $v26
    vor $v15, $v15, $v10, e(5)
    vor $v16, $v16, $v10, e(5)
    vor $v17, $v17, $v10, e(5)
    vor $v18, $v18, $v10, e(5)
    vne $v09, $v19, vzero
    vmrg $v11, $v11, vzero
    vne $v09, $v20, vzero
    vmrg $v12, $v12, vzero
    vne $v09, $v21, vzero
    vmrg $v13, $v13, vzero
    vne $v09, $v22, vzero
    vmrg $v14, $v14, vzero
    vge $v09, $v11, $v10, e(2)
    vmrg $v05, $v15, $v05
    vge $v09, $v12, $v10, e(2)
    vmrg $v06, $v16, $v06
    vge $v09, $v13, $v10, e(2)
    vmrg $v07, $v17, $v07
    vge $v09, $v14, $v10, e(2)
    vmrg $v08, $v18, $v08
    addiu t5, t5, 32
    addiu t6, t6, 64
    sqv $v05, 0x00, t4
    sqv $v06, 0x10, t4
    sqv $v07, 0x20, t4
    sqv $v08, 0x30, t4
    bne t5, t7, 2b
    addiu t4, t4, 64
Entry_next:
    srl s6, s6, 1
    b Entry_loop
    addiu s5, s5, 1
Entries_done:

    # ---- 2. colours ---------------------------------------------------
    # (a value is the table offset as it stands). 8 pixels a pass; the
    # stores go through t4, which runs 16 behind t5.
    li t5, %lo(MAINP)
    li t4, %lo(MAINP) - 16
    li t6, %lo(MAINP) + 512
    beqz s2, 1f
    nop
    li t6, %lo(MAINP) + 1024        # the subscreen's plane too
1:  addiu t4, t4, 16
    lhu a0, 0(t5)
    lhu a1, 2(t5)
    lhu a2, 4(t5)
    lhu a3, 6(t5)
    lhu v0, 8(t5)
    lhu v1, 10(t5)
    lhu k0, 12(t5)
    lhu k1, 14(t5)
    lhu a0, %lo(TAB)(a0)
    lhu a1, %lo(TAB)(a1)
    lhu a2, %lo(TAB)(a2)
    lhu a3, %lo(TAB)(a3)
    lhu v0, %lo(TAB)(v0)
    lhu v1, %lo(TAB)(v1)
    lhu k0, %lo(TAB)(k0)
    lhu k1, %lo(TAB)(k1)
    addiu t5, t5, 16
    sh a0, 0(t4)
    sh a1, 2(t4)
    sh a2, 4(t4)
    sh a3, 6(t4)
    sh v0, 8(t4)
    sh v1, 10(t4)
    sh k0, 12(t4)
    bne t5, t6, 1b
    sh k1, 14(t4)

    # ---- 3. math, brightness, RGBA5551 --------------------------------
    # 16 pixels a pass: two groups, their three channels as six streams.
    li t3, %lo(CONSTS)
    lqv $v20, 0x00, t3              # 0x7fff, 0x1f00, 31, 15, 2185, 0x100, ones, 1
    li t3, %lo(DESC + CMP_MPARAMS)
    lqv $v21, 0x00, t3
    li t5, %lo(MAINP)
    li t7, 16
    vor $v25, vzero, vshift, e(7)   # ones (the alpha bit)
    vor $v24, vzero, $v21, e(5)     # halving with a subscreen pixel: 0x180 or 0
Math_loop:
    addiu t7, t7, -1
    lqv $v03, -0x200, t5
    lqv $v06, -0x1f0, t5
    lqv $v01, 0x00, t5
    lqv $v04, 0x10, t5
    lqv $v02, 0x200, t5
    lqv $v05, 0x210, t5
    vand $v07, $v03, $v21, e(1)
    vand $v08, $v06, $v21, e(1)
    vand $v09, $v02, $v20, e(0)
    vand $v10, $v05, $v20, e(0)
    vand $v11, $v03, $v21, e(0)
    vand $v12, $v06, $v21, e(0)
    vand $v13, $v01, $v20, e(0)
    vand $v14, $v04, $v20, e(0)
    # ones where the span is not clipped
    vne $v17, $v07, vzero
    vmrg $v07, vzero, $v20, e(6)
    vne $v17, $v08, vzero
    vmrg $v08, vzero, $v20, e(6)
    # the second colour and its halving, by whether the subscreen has a pixel
    vlt $v17, $v02, vzero
    vmrg $v09, $v09, $v21, e(2)
    vmrg $v15, $v24, $v21, e(6)
    vlt $v17, $v05, vzero
    vmrg $v10, $v10, $v21, e(2)
    vmrg $v16, $v24, $v21, e(6)
    # the main value where the span has math
    vne $v17, $v11, vzero
    vmrg $v11, $v01, vzero
    vne $v17, $v12, vzero
    vmrg $v12, $v04, vzero
    vand $v13, $v13, $v07
    vand $v14, $v14, $v08
    # no second colour and no halving where the pixel has no math
    vlt $v17, $v11, vzero
    vmrg $v09, $v09, vzero
    vmrg $v15, $v15, vzero
    vlt $v17, $v12, vzero
    vmrg $v10, $v10, vzero
    vmrg $v16, $v16, vzero
    # the channels, times 256: red, green, blue of the main colour
    vmudn $v18, $v13, vshift8, e(7)
    vmudn $v19, $v13, vshift, e(4)
    vmudl $v22, $v13, vshift8, e(1)
    vmudn $v23, $v14, vshift8, e(7)
    vmudn $v26, $v14, vshift, e(4)
    vmudl $v27, $v14, vshift8, e(1)
    vand $v15, $v15, $v07
    vand $v16, $v16, $v08
    vand $v18, $v18, $v20, e(1)
    vand $v19, $v19, $v20, e(1)
    vand $v22, $v22, $v20, e(1)
    vand $v23, $v23, $v20, e(1)
    vand $v26, $v26, $v20, e(1)
    vand $v27, $v27, $v20, e(1)
    # the factor: 0x100 keeps, 0x80 halves
    vxor $v15, $v15, $v20, e(5)
    vxor $v16, $v16, $v20, e(5)
    vmudn $v28, $v09, vshift8, e(7)
    vmudn $v29, $v09, vshift, e(4)
    vmudl $v01, $v09, vshift8, e(1)
    vmudn $v02, $v10, vshift8, e(7)
    vmudn $v03, $v10, vshift, e(4)
    vmudl $v05, $v10, vshift8, e(1)
    vand $v28, $v28, $v20, e(1)
    vand $v29, $v29, $v20, e(1)
    vand $v01, $v01, $v20, e(1)
    vand $v02, $v02, $v20, e(1)
    vand $v03, $v03, $v20, e(1)
    vand $v05, $v05, $v20, e(1)
    vmudn $v28, $v28, $v21, e(3)
    vmudn $v29, $v29, $v21, e(3)
    vmudn $v01, $v01, $v21, e(3)
    vmudn $v02, $v02, $v21, e(3)
    vmudn $v03, $v03, $v21, e(3)
    vmudn $v05, $v05, $v21, e(3)
    vadd $v18, $v18, $v28
    vadd $v19, $v19, $v29
    vadd $v22, $v22, $v01
    vadd $v23, $v23, $v02
    vadd $v26, $v26, $v03
    vadd $v27, $v27, $v05
    vge $v18, $v18, vzero
    vge $v19, $v19, vzero
    vge $v22, $v22, vzero
    vge $v23, $v23, vzero
    vge $v26, $v26, vzero
    vge $v27, $v27, vzero
    vmudl $v18, $v18, $v15
    vmudl $v19, $v19, $v15
    vmudl $v22, $v22, $v15
    vmudl $v23, $v23, $v16
    vmudl $v26, $v26, $v16
    vmudl $v27, $v27, $v16
    vlt $v18, $v18, $v20, e(2)
    vlt $v19, $v19, $v20, e(2)
    vlt $v22, $v22, $v20, e(2)
    vlt $v23, $v23, $v20, e(2)
    vlt $v26, $v26, $v20, e(2)
    vlt $v27, $v27, $v20, e(2)
    # brightness
    vmudn $v18, $v18, $v21, e(4)
    vmudn $v19, $v19, $v21, e(4)
    vmudn $v22, $v22, $v21, e(4)
    vmudn $v23, $v23, $v21, e(4)
    vmudn $v26, $v26, $v21, e(4)
    vmudn $v27, $v27, $v21, e(4)
    vadd $v18, $v18, $v20, e(3)
    vadd $v19, $v19, $v20, e(3)
    vadd $v22, $v22, $v20, e(3)
    vadd $v23, $v23, $v20, e(3)
    vadd $v26, $v26, $v20, e(3)
    vadd $v27, $v27, $v20, e(3)
    vmudl $v18, $v18, $v20, e(4)
    vmudl $v19, $v19, $v20, e(4)
    vmudl $v22, $v22, $v20, e(4)
    vmudl $v23, $v23, $v20, e(4)
    vmudl $v26, $v26, $v20, e(4)
    vmudl $v27, $v27, $v20, e(4)
    # RGBA5551
    vmudn $v06, $v18, vshift8, e(4)
    vmadn $v06, $v19, vshift, e(1)
    vmadn $v06, $v22, vshift, e(6)
    vmadn $v06, $v25, vshift, e(7)
    vmudn $v04, $v23, vshift8, e(4)
    vmadn $v04, $v26, vshift, e(1)
    vmadn $v04, $v27, vshift, e(6)
    vmadn $v04, $v25, vshift, e(7)
    sqv $v06, 0x00, t5
    nop
    sqv $v04, 0x10, t5
    bnez t7, Math_loop
    addiu t5, t5, 32

    # The row out.
    lw s0, %lo(DESC + CMP_OUT)
    sll t3, s7, 9
    addu s0, s0, t3
    li s4, %lo(MAINP)
    li t0, DMA_SIZE(512, 1)
    move ra, s3                     # DMAOut returns straight to rspq
    j DMAOut
    nop
