# SNES background layers on the RSP: expand a run of scanlines of one BG
# layer into palette-indexed plane rows (n64/include/n64_ppu_rdp.h), exactly
# as flush_run in n64/src/n64_ppu_build.c does on the CPU.
#
# One command per run (BgCmd_Job, the RDRAM address of a job block, see
# bg_rsp_layout.h). Lines are processed in groups that share a tile row (at
# most 8), tile-major: each tile's bitplanes are fetched once per group and
# decoded for every line of the group. The next tile's DMA runs while the
# current one is decoded.
#
# Decode, 8 pixels per vector (lane j = pixel j). A VRAM word holds two
# bitplanes (high byte plane 1, low byte plane 0). VMUDN by 2^(8+j) (plane
# 0) or 2^j (plane 1) leaves pixel j's bit in bit 15 of the low product (low
# product bits do not depend on signedness); VAND 0x8000 isolates it, and
# VMUDL/VMADL by 2^(9+p) (= (bit << 15) * 2^(9+p) >> 16 = bit << (8+p))
# assemble the colour index in the high byte, where SPV stores it from.
# Horizontally flipped tiles use the reversed powers (vshift8 / vshift).
#
# Full groups (8 lines, the tile row's rows 0-7): one vector load holds the
# tile's eight row words, line i broadcasts element i (7 - i when the tile
# is flipped vertically), fully unrolled. Partial groups (a run's first and
# last tile rows) go through a per-line loop.
#
# Fine scroll: a row's pixel for screen x lands at byte N64PPU_PAD + x, so a
# tile's 8 pixels start at an 8-byte boundary minus (hscroll & 7). For each
# scanline, the previous tile's 8 lanes stay in a 32-byte carry buffer; LQV/LRV
# at byte 2 * fine then read the 8 lanes of one aligned output group.
#
# Rows are written whole over [N64PPU_PAD, N64PPU_PAD + 256); per row the
# job also reports whether any low- or high-priority pixel was non-zero.
#
# VRAM alignment: the VRAM image is the PPU's array, only 2-aligned in RDRAM
# (the Ppu struct puts it at 4 mod 8), and DMA ignores the low 3 address
# bits. Reads fetch 8 bytes more and use the data from offset (vram & 7).

#include <rsp_queue.inc>
#include "bg_rsp_layout.h"

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand BgCmd_Job, 4
    RSPQ_DefineCommand BgCmd_Copy, 12
RSPQ_EndOverlayHeader

RSPQ_BeginSavedState
    .align 3
SAVED_UNUSED: .ds.b 8               # rspq needs a non-empty saved state
RSPQ_EndSavedState

    .align 4
POWLO:  .half 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000
POWHI:  .half 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080

    .bss
    .align 4
JOB:    .ds.b BGJ_SIZE
# Per job, derived from the job block:
CHSH:   .ds.b 4                     # character shift: 4 (4bpp) or 3 (2bpp)
PALSH:  .ds.b 4                     # palette shift on top of * 4: 2 or 0
TLEN:   .ds.b 4                     # tile DMA size word
NTILES: .ds.b 4                     # 33 with fine scroll, else 32
# Next tile (prefetched).
NBASE:  .ds.b 4                     # DMEM address of its first byte
NATTR:  .ds.b 4                     # pal << 8 | hflip << 16 | vflip << 17 | hi << 18
CATTR:  .ds.b 4                     # the current tile's attributes
# Non-zero lines of a full group: low, high priority (bit i = line i).
NZ:     .ds.b 8
    .align 3                        # every DMA buffer: 8-aligned
TMAP:   .ds.b 2 * 72                # two 32-entry tilemap rows (+8: VRAM alignment)
    .align 3
TB0:    .ds.b 40                    # tile data, double-buffered (+8)
TB1:    .ds.b 40
    .align 4
CARRY:  .ds.b 8 * 32                # per line: previous tile, current tile
FLAGS:  .ds.b BGJ_MAX_ROWS + 8      # the last 8 bytes take tile -1's output
    .align 3
OUT:    .ds.b 8 * 256               # per line: bytes [PAD, PAD + 256) of the row

    .text

# Persistent registers during a job:
#   s5 VRAM RDRAM base        s6 RDRAM address of the group's first row (+PAD)
#   s7 rows left              fp BG y of the group's first row
#   s1 hscroll                s2 character base (words)
#   s3 return address         k0 BGnSC          k1 bpp (2 or 4)
#   a1 palette base           a2 fine scroll    v1 DMEM flags of the group
#   sp RDRAM flags address    a3 rows in the job
# Per group: a0 lines (n). Per tile: t9 index k, t7 data base, t8 attributes.
# DMA helpers clobber t0, t2, at, s0, s4 (and read t1 as a pitch).

BgCmd_Job:
    move s3, ra
    li t0, 0xffffff
    and s0, a0, t0
    li s4, %lo(JOB)
    jal DMAIn
    li t0, DMA_SIZE(BGJ_SIZE, 1)

    lw s5, %lo(JOB + BGJ_VRAM)
    lw s6, %lo(JOB + BGJ_DST)
    lw sp, %lo(JOB + BGJ_FLAGS)
    lhu s7, %lo(JOB + BGJ_NROWS)
    move a3, s7
    lhu fp, %lo(JOB + BGJ_Y)
    lhu s1, %lo(JOB + BGJ_HS)
    lhu s2, %lo(JOB + BGJ_TILEADR)
    lbu k0, %lo(JOB + BGJ_XSC)
    lbu k1, %lo(JOB + BGJ_BPP)
    lbu a1, %lo(JOB + BGJ_PAL)
    andi a2, s1, 7
    li v1, %lo(FLAGS)
    ctc2 zero, COP2_CTRL_VCO        # VNE also reads VCO's "not equal" bits

    # Derived per-job values.
    li t3, 3
    li t4, 0
    li t5, DMA_SIZE(24, 1)
    li t6, 4
    bne k1, t6, 1f
    nop
    li t3, 4
    li t4, 2
    li t5, DMA_SIZE(40, 1)
1:  sw t3, %lo(CHSH)
    sw t4, %lo(PALSH)
    sw t5, %lo(TLEN)
    sltu t3, zero, a2
    addiu t3, t3, 32
    sw t3, %lo(NTILES)

    # Clear the row flags.
    li t1, %lo(FLAGS)
    li t3, %lo(FLAGS) + BGJ_MAX_ROWS + 8
1:  sdv vzero, 0, 0, t1
    addiu t1, t1, 8
    bne t1, t3, 1b
    nop

    li t1, %lo(POWLO)
    lqv $v20, 0x00, t1              # POWLO
    lqv $v21, 0x10, t1              # POWHI

Group_loop:
    beqz s7, Job_done
    # n = min(rows left, lines left in this tile row)
    andi t1, fp, 7
    li a0, 8
    subu a0, a0, t1
    slt t6, s7, a0
    beqz t6, 1f
    nop
    move a0, s7
1:
    sw zero, %lo(NZ) + 0
    sw zero, %lo(NZ) + 4

    # Tilemap rows: sc = ((xsc & 0xfc) << 8) + (((y >> 3) & 31) << 5)
    #   + ((y & 0x100) && (xsc & 2) ? ((xsc & 1) ? 0x800 : 0x400) : 0)
    andi t3, k0, 0xfc
    sll t3, t3, 8
    srl t4, fp, 3
    andi t4, t4, 0x1f
    sll t4, t4, 5
    addu t3, t3, t4
    srl t4, fp, 8
    andi t4, t4, 1
    srl t5, k0, 1
    and t4, t4, t5
    andi t5, k0, 1
    addiu t5, t5, 1
    sll t5, t5, 10                  # 0x400 or 0x800
    subu t4, zero, t4               # all ones when it applies
    and t4, t4, t5
    addu t3, t3, t4                 # sc (words)
    andi t5, k0, 1
    sll t5, t5, 10                  # second screen: +0x400 when wider
    addu t4, t3, t5
    andi t3, t3, 0x7fff
    andi t4, t4, 0x7fff
    sll t3, t3, 1
    sll t4, t4, 1
    addu s0, s5, t3
    li s4, %lo(TMAP)
    jal DMAInAsync
    li t0, DMA_SIZE(72, 1)
    addu s0, s5, t4
    li s4, %lo(TMAP) + 72
    jal DMAIn
    li t0, DMA_SIZE(72, 1)

    # First tile: no current tile yet (its buffer reads as TB1).
    li t7, %lo(TB1)
    move t1, zero
    jal Prefetch
    nop
    move t9, zero

Tile_loop:
    jal DMAWaitIdle                 # tile k has landed
    nop
    lw t7, %lo(NBASE)
    lw t8, %lo(NATTR)
    # Tile k + 1's DMA is started below, while tile k is decoded.
    mtc2 t8, $v24.e0                # palette byte << 8
    srl t6, t8, 16
    andi t3, t6, 1                  # horizontal flip
    bnez t3, 1f
    nop
    vor $v22, vzero, $v20
    b 2f
    vor $v23, vzero, $v21
1:  vor $v22, vzero, vshift8        # 2^(15-j)
    vor $v23, vzero, vshift         # 2^(7-j)
2:
    li t3, 8
    bne a0, t3, Generic
    nop

    ####################################################################
    # Full group: rows 0-7 of the tile, line i = row i (or 7 - i).
    ####################################################################
    # Row words (planes 1|0, and 3|2 for 4bpp), element i = line i: row i,
    # or row 7 - i for vertically flipped tiles.
    andi t4, t6, 2
    bnez t4, Load_flip
    li t3, 4
    lqv $v26, 0x00, t7
    lrv $v26, 0x10, t7
    bne k1, t3, Loaded
    vor $v28, vzero, $v26
    lqv $v27, 0x10, t7
    lrv $v27, 0x20, t7
    b Loaded
    vor $v28, $v28, $v27
Load_flip:
    lsv $v26, 14, 0, t7
    lsv $v26, 12, 2, t7
    lsv $v26, 10, 4, t7
    lsv $v26, 8, 6, t7
    lsv $v26, 6, 8, t7
    lsv $v26, 4, 10, t7
    lsv $v26, 2, 12, t7
    lsv $v26, 0, 14, t7
    bne k1, t3, Loaded
    vor $v28, vzero, $v26
    lsv $v27, 14, 16, t7
    lsv $v27, 12, 18, t7
    lsv $v27, 10, 20, t7
    lsv $v27, 8, 22, t7
    lsv $v27, 6, 24, t7
    lsv $v27, 4, 26, t7
    lsv $v27, 2, 28, t7
    lsv $v27, 0, 30, t7
    vor $v28, $v28, $v27
Loaded:
    vne $v29, $v28, vzero
    cfc2 t3, COP2_CTRL_VCC
    andi t3, t3, 0xff               # bit i: line i has a non-zero pixel
    srl t4, t8, 16
    andi t4, t4, 4                  # hi: NZ + 4
    lw t5, %lo(NZ)(t4)
    or t5, t5, t3
    sw t5, %lo(NZ)(t4)
    li t3, 4
    bne k1, t3, Dec2
    nop
    b Dec4
    nop

    # Unrolled decode with the next tile's prefetch interleaved (gen_full.py).
#include "rsp_bg_full.inc"

Full_out:
    sll t6, t9, 3
    addiu t6, t6, %lo(OUT)          # OUT_0 + 8k
    bnez a2, Full_fine
    nop
    # No fine scroll: tile k is output group k (k < 32).
    spv $v08, 0, 0, t6
    spv $v09, 0, 256, t6
    addiu t6, t6, 512
    spv $v10, 0, 0, t6
    spv $v11, 0, 256, t6
    addiu t6, t6, 512
    spv $v12, 0, 0, t6
    spv $v13, 0, 256, t6
    addiu t6, t6, 512
    spv $v14, 0, 0, t6
    b Tile_next
    spv $v15, 0, 256, t6

.macro SHIFTOUT i, v, tmp
    sqv \v, (32 * \i + 16), t4
    lqv \tmp, (32 * \i), t3
    lrv \tmp, (32 * \i + 16), t3
    sqv \v, (32 * \i), t4
.endm

Full_fine:
    # Group k - 1 holds tile k - 1 from pixel fine, then tile k. For k = 0
    # this writes 8 bytes before each row, which tile 32 of the row before
    # (or the flags padding) overwrites or ignores.
    addiu t6, t6, -8
    li t4, %lo(CARRY)
    sll t3, a2, 1
    addu t3, t3, t4
    SHIFTOUT 0, $v08, $v06
    spv $v06, 0, 0, t6
    SHIFTOUT 1, $v09, $v07
    spv $v07, 0, 256, t6
    addiu t6, t6, 512
    SHIFTOUT 2, $v10, $v06
    spv $v06, 0, 0, t6
    SHIFTOUT 3, $v11, $v07
    spv $v07, 0, 256, t6
    addiu t6, t6, 512
    SHIFTOUT 4, $v12, $v06
    spv $v06, 0, 0, t6
    SHIFTOUT 5, $v13, $v07
    spv $v07, 0, 256, t6
    addiu t6, t6, 512
    SHIFTOUT 6, $v14, $v06
    spv $v06, 0, 0, t6
    SHIFTOUT 7, $v15, $v07
    b Tile_next
    spv $v07, 0, 256, t6

    ####################################################################
    # Partial group: per-line loop.
    ####################################################################
Generic:
    sw t8, %lo(CATTR)               # Prefetch replaces NATTR
    jal Prefetch                    # tile k + 1
    addiu t1, t9, 1
    lw t8, %lo(CATTR)
    srl t6, t8, 16
    andi t8, t6, 2                  # vertical flip
    andi v0, t6, 4                  # hi
    srl v0, v0, 2
    addiu v0, v0, 1                 # flag value: 2 high, 1 low
    move t1, zero                   # line i
Line_loop:
    # row = (y + i) & 7, flipped: 7 - row
    addu t3, fp, t1
    andi t3, t3, 7
    beqz t8, 1f
    nop
    xori t3, t3, 7
1:  sll t3, t3, 1
    addu t3, t3, t7                 # the row's first byte
    lsv $v25, 0, 0, t3              # plane 1 << 8 | plane 0
    lhu t4, 0(t3)
    vmudn $v01, $v22, $v25, e(0)
    vmudn $v02, $v23, $v25, e(0)
    li t6, 4
    bne k1, t6, Decode2
    nop
    lsv $v25, 2, 16, t3             # plane 3 << 8 | plane 2
    lhu t5, 16(t3)
    or t4, t4, t5
    vmudn $v03, $v22, $v25, e(1)
    vmudn $v04, $v23, $v25, e(1)
    vand $v01, $v01, vshift8, e(0)
    vand $v02, $v02, vshift8, e(0)
    vand $v03, $v03, vshift8, e(0)
    vand $v04, $v04, vshift8, e(0)
    vmudl $v05, $v01, vshift8, e(6)
    vmadl $v05, $v02, vshift8, e(5)
    vmadl $v05, $v03, vshift8, e(4)
    b Decoded
    vmadl $v05, $v04, vshift8, e(3)
Decode2:
    vand $v01, $v01, vshift8, e(0)
    vand $v02, $v02, vshift8, e(0)
    vmudl $v05, $v01, vshift8, e(6)
    vmadl $v05, $v02, vshift8, e(5)
Decoded:
    vor $v05, $v05, $v24, e(0)

    # Row flags.
    beqz t4, 1f
    addu t5, v1, t1
    lbu t6, 0(t5)
    or t6, t6, v0
    sb t6, 0(t5)
1:
    sll t5, t1, 8
    addiu t5, t5, %lo(OUT)
    sll t6, t9, 3
    bnez a2, Fine
    addu t5, t5, t6                 # OUT_i + 8k
    b Line_next
    spv $v05, 0, 0, t5
Fine:
    sll t6, t1, 5
    addiu t6, t6, %lo(CARRY)
    sqv $v05, 0x10, t6
    sll t3, a2, 1
    addu t3, t3, t6
    lqv $v06, 0x00, t3
    lrv $v06, 0x10, t3
    sqv $v05, 0x00, t6
    addiu t5, t5, -8                # group k - 1 (k = 0: see Full_fine)
    spv $v06, 0, 0, t5
Line_next:
    addiu t1, t1, 1
    bne t1, a0, Line_loop
    nop

Tile_next:
    addiu t9, t9, 1
    lw t6, %lo(NTILES)
    bne t9, t6, Tile_loop
    nop

    # Full group: row flags from the non-zero masks (bit i = line i).
    li t3, 8
    bne a0, t3, 3f
    lw t3, %lo(NZ) + 0              # low priority
    lw t4, %lo(NZ) + 4              # high priority
    move t1, zero
1:  srlv t0, t3, t1
    andi t0, t0, 1
    srlv t2, t4, t1
    andi t2, t2, 1
    sll t2, t2, 1
    or t0, t0, t2
    addu t2, v1, t1
    sb t0, 0(t2)
    addiu t1, t1, 1
    bne t1, a0, 1b
    nop
3:
    # Write the group's rows.
    move t1, zero
    move t3, s6
1:  sll t5, t1, 8
    addiu s4, t5, %lo(OUT)
    move s0, t3
    jal DMAOutAsync
    li t0, DMA_SIZE(256, 1)
    addiu t3, t3, BGJ_ROW_STRIDE
    addiu t1, t1, 1
    bne t1, a0, 1b
    nop

    move s6, t3
    addu fp, fp, a0
    subu s7, s7, a0
    b Group_loop
    addu v1, v1, a0

Job_done:
    jal DMAWaitIdle
    nop
    # Flags: rows rounded up to 8 bytes.
    addiu t0, a3, 7
    srl t0, t0, 3
    sll t0, t0, 3
    addiu t0, t0, -1                # DMA_SIZE(len, 1) = len - 1
    move s0, sp
    li s4, %lo(FLAGS)
    move ra, s3                     # DMAOut returns straight to rspq
    j DMAOut
    nop

########################################################################
# Prefetch tile t1: compute its attributes and start its DMA into the
# buffer the current tile (t7) is not using. Result in NBASE/NATTR.
# Clobbers t0, t2-t6, at, s0, s4, v0. (Full groups use the same steps,
# interleaved with the decode: gen_full.py.)
########################################################################
Prefetch:
    move v0, ra
    # entry = tilemap[(mx >> 5) & 1][mx & 31], mx = (hs >> 3) + k
    srl t3, s1, 3
    addu t3, t3, t1
    andi t4, t3, 0x20
    srl t4, t4, 2                   # 8 or 0
    sll t6, t4, 3
    addu t4, t4, t6                 # 72 or 0: second row
    andi t3, t3, 0x1f
    sll t3, t3, 1
    addu t3, t3, t4
    andi t6, s5, 7                  # VRAM misalignment
    addu t3, t3, t6
    lhu t3, %lo(TMAP)(t3)           # entry

    # Attributes: palette byte (hi ? 128 : 0) + base + palette * (4 or 16)
    srl t4, t3, 10
    andi t4, t4, 7
    sll t4, t4, 2
    lw t6, %lo(PALSH)
    sllv t4, t4, t6
    addu t4, t4, a1
    andi t6, t3, 0x2000
    srl t5, t6, 6                   # 0x80
    addu t4, t4, t5
    sll t4, t4, 8
    sll t6, t6, 5                   # hi << 18
    or t4, t4, t6
    srl t6, t3, 14                  # hflip | vflip << 1
    sll t6, t6, 16
    or t4, t4, t6
    sw t4, %lo(NATTR)

    # Character data: base + c * 16 (4bpp) or c * 8 (2bpp) words.
    andi t3, t3, 0x3ff
    lw t6, %lo(CHSH)
    sllv t3, t3, t6
    addu t3, t3, s2
    andi t3, t3, 0x7fff
    sll t3, t3, 1                   # byte offset
    srl t5, t7, 3                   # the buffer the current tile is not in
    sll t5, t5, 3
    li s4, %lo(TB0)
    addiu s4, s4, %lo(TB1)
    subu s4, s4, t5
    addu s0, s5, t3
    jal DMAInAsync                  # s4 := buffer + (address & 7)
    lw t0, %lo(TLEN)
    sw s4, %lo(NBASE)
    jr v0
    nop

########################################################################
# BgCmd_Copy: copy len bytes RDRAM -> RDRAM through DMEM (the VRAM snapshot
# the jobs read while the CPU runs the next frame). a0: source, a1:
# destination (both 8-aligned), a2: length (a multiple of 8).
########################################################################
BgCmd_Copy:
    move s3, ra
    li t0, 0xffffff
    and s5, a0, t0
    move s6, a1
    move s7, a2
    move t9, zero                   # which half of OUT
Copy_loop:
    beqz s7, Copy_done
    li t3, 1024
    sltu t6, s7, t3
    beqz t6, 1f
    nop
    move t3, s7                     # last chunk
1:  sll t4, t9, 10
    addiu t4, t4, %lo(OUT)
    addiu t5, t3, -1                # DMA_SIZE(len, 1)
    move s0, s5
    move s4, t4
    jal DMAIn                       # also waits for the previous chunk's write
    move t0, t5
    move s0, s6
    move s4, t4
    jal DMAOutAsync
    move t0, t5
    addu s5, s5, t3
    addu s6, s6, t3
    subu s7, s7, t3
    b Copy_loop
    xori t9, t9, 1
Copy_done:
    move ra, s3
    j DMAWaitIdle
    nop
