#!/usr/bin/env python3
"""Generate a PPU test ROM for the RDP renderer's picture comparison.

The ROM is a LoROM image holding generated data and nothing else (no game
code or data): pseudo-random VRAM, CGRAM and OAM contents, a 60-byte loop
that plays a register script, and the script -- for every frame, the PPU
register writes that frame's picture is made with. Build the N64 runner
around it with RENDER=2 (the RDP picture is compared with the software
PPU's, frame by frame) and read the [rdp] lines: frames that differ, and
the lines that are still software lines (n64/tools/ppu_test.sh).

    n64/tools/ppu_test_rom.py OUT.sfc [--seed N] [--scenes LIST] [--frames N]

Scenes (--scenes, comma-separated; default all): each is a PPU setting held
for --frames frames (default 12) while scroll, matrix and windows move.
--list prints them with the frame each starts at.

The script: bank 1 from $8000; entries of two bytes, register low byte + 1
and value (a write to $2100 + low byte); 0 ends a frame, $FF the script
(which then starts again).
"""
import argparse
import math
import random

BANK = 0x8000


class Asm:
    def __init__(self):
        self.b = bytearray(); self.labels = {}; self.fix = []

    def e(self, *v):
        self.b += bytes(v)

    def label(self, n):
        self.labels[n] = len(self.b)

    def br(self, op, n):
        self.e(op, 0); self.fix.append((len(self.b) - 1, n))

    def done(self):
        for at, n in self.fix:
            d = self.labels[n] - (at + 1)
            assert -128 <= d < 128
            self.b[at] = d & 0xFF
        return bytes(self.b)


def code():
    a = Asm()
    sta = lambda adr: a.e(0x8D, adr & 0xFF, adr >> 8)
    stz = lambda adr: a.e(0x9C, adr & 0xFF, adr >> 8)
    lda = lambda v: a.e(0xA9, v)
    a.e(0x78, 0x18, 0xFB, 0xC2, 0x30, 0xA2, 0xFF, 0x1F, 0x9A, 0xA9, 0x00, 0x00, 0x5B, 0xE2, 0x20)
    lda(0x80); sta(0x2100)                      # forced blank
    stz(0x4200); stz(0x420C); stz(0x2140)       # no interrupts, no HDMA; the runner's SPC timeline starts

    def dma(mode, reg, bank, adr, size):
        lda(mode); sta(0x4300); lda(reg); sta(0x4301)
        lda(adr & 0xFF); sta(0x4302); lda(adr >> 8); sta(0x4303); lda(bank); sta(0x4304)
        lda(size & 0xFF); sta(0x4305); lda(size >> 8); sta(0x4306)
        lda(0x01); sta(0x420B)
    lda(0x80); sta(0x2115); stz(0x2116); stz(0x2117)
    dma(0x01, 0x18, 2, 0x8000, 0x8000)          # VRAM words $0000-$3FFF
    dma(0x01, 0x18, 3, 0x8000, 0x8000)          # VRAM words $4000-$7FFF
    stz(0x2121); dma(0x00, 0x22, 4, 0x8000, 0x200)           # CGRAM
    stz(0x2102); stz(0x2103); dma(0x00, 0x04, 4, 0x8200, 0x220)   # OAM
    lda(0x01); a.e(0x48, 0xAB)                  # data bank 1: the script, and its I/O mirror
    a.e(0xEB); lda(0x00); a.e(0xEB)             # B = 0 (TAX takes all of A)
    a.e(0xA0, 0x00, 0x00)                       # LDY #0
    a.label('frame')
    a.label('w1'); a.e(0xAD, 0x12, 0x42); a.br(0x30, 'w1')   # leave the vertical blank
    a.label('w2'); a.e(0xAD, 0x12, 0x42); a.br(0x10, 'w2')   # its start
    a.label('next')
    a.e(0xB9, 0x00, 0x80)                       # LDA $8000,Y
    a.br(0xF0, 'endframe')
    a.e(0xC9, 0xFF); a.br(0xF0, 'restart')
    a.e(0xAA, 0xC8, 0xB9, 0x00, 0x80, 0x9D, 0xFF, 0x20, 0xC8)   # TAX, INY, LDA $8000,Y, STA $20FF,X, INY
    a.br(0x80, 'next')
    a.label('endframe'); a.e(0xC8); a.br(0x80, 'frame')
    a.label('restart'); a.e(0xA0, 0x00, 0x00); a.br(0x80, 'frame')
    return a.done()


class Frame:
    """One frame's register writes, in order."""
    def __init__(self):
        self.w = []

    def b(self, reg, v):
        self.w.append((reg & 0xFF, v & 0xFF))

    def w2(self, reg, v):      # the write-twice registers: low, then high
        self.b(reg, v); self.b(reg, v >> 8)


def base(f, mode, tm, ts=0):
    f.b(0x2100, 0x0F)
    f.b(0x2101, 0x02)          # sprites 8x8 / 16x16, characters at word $4000
    f.b(0x2105, mode)
    f.b(0x2106, 0)
    for r in range(0x2107, 0x210B): f.b(r, 0x10 + 4 * (r - 0x2107))   # tilemaps at $1000.. (32x32)
    f.b(0x210B, 0x22); f.b(0x210C, 0x22)                              # characters at word $2000
    f.b(0x211A, 0)
    f.b(0x2123, 0); f.b(0x2124, 0); f.b(0x2125, 0)
    f.b(0x212A, 0); f.b(0x212B, 0)
    f.b(0x212C, tm); f.b(0x212D, ts); f.b(0x212E, 0); f.b(0x212F, 0)
    f.b(0x2130, 0); f.b(0x2131, 0)
    f.b(0x2132, 0xE0)          # fixed colour 0
    f.b(0x2133, 0)
    f.b(0x2133, 0)


def m7(f, a, b, c, d, hofs, vofs, cx, cy, sel=0):
    f.b(0x211A, sel)
    for reg, v in ((0x211B, a), (0x211C, b), (0x211D, c), (0x211E, d), (0x211F, cx), (0x2120, cy)):
        f.w2(reg, v & 0xFFFF)
    f.w2(0x210D, hofs & 0x1FFF); f.w2(0x210E, vofs & 0x1FFF)


def rot(t, zoom):
    c = int(round(math.cos(t) * zoom)); s = int(round(math.sin(t) * zoom))
    return c, s, -s, c


def scenes(n):
    """name, function(frame index k in 0..n-1) -> Frame."""
    out = []

    def add(name):
        def deco(fn):
            out.append((name, fn)); return fn
        return deco

    @add('m7-plain')
    def _(k):
        f = Frame(); base(f, 7, 0x01); m7(f, 0x100, 0, 0, 0x100, 3 * k, 5 * k, 128, 112); return f

    @add('m7-zoom')
    def _(k):
        z = [0x040, 0x080, 0x0C0, 0x100, 0x180, 0x200, 0x300, 0x400, 0x7FF, 0x020, 0x008, 0x002][k % 12]
        f = Frame(); base(f, 7, 0x01); m7(f, z, 0, 0, z, 40 + k, 60, 128, 112); return f

    @add('m7-rotate')
    def _(k):
        a, b, c, d = rot(k * 0.37, 0x100 + 24 * k)
        f = Frame(); base(f, 7, 0x01); m7(f, a, b, c, d, 100 + 7 * k, 300 - 9 * k, 128 + k, 112 - k); return f

    @add('m7-flips-field')
    def _(k):
        sel = [0x00, 0x01, 0x02, 0x03, 0x80, 0x81, 0xC0, 0xC2, 0x83, 0xC3, 0x40, 0x41][k % 12]
        a, b, c, d = rot(0.3 + k * 0.21, 0x180)
        f = Frame(); base(f, 7, 0x01); m7(f, a, b, c, d, 900 + 31 * k, 940 + 17 * k, 512, 512, sel); return f

    @add('m7-negative')
    def _(k):
        a, b, c, d = rot(3.3 + k * 0.4, 0x0C0)
        f = Frame(); base(f, 7, 0x01); m7(f, a, b, c, d, -200 + 13 * k, -77 * k, -300 + 50 * k, 1000 - 60 * k, 0x80 if k & 1 else 0); return f

    @add('m7-sprites-windows')
    def _(k):
        f = Frame(); base(f, 7, 0x11)
        a, b, c, d = rot(k * 0.15, 0x140)
        m7(f, a, b, c, d, 5 * k, 3 * k, 128, 112)
        f.b(0x2123, 0x03 if k & 1 else 0x02)   # BG1: window 1 (inverted on odd frames)
        f.b(0x2125, 0x02 if k & 2 else 0x00)   # OBJ: window 1 on some frames
        f.b(0x2126, 20 + 4 * k); f.b(0x2127, 200 - 3 * k)
        f.b(0x212E, 0x11)
        return f

    @add('m7-fixed-math')
    def _(k):
        f = Frame(); base(f, 7, 0x11)
        m7(f, 0x120, 0x30, -0x30, 0x120, 2 * k, 7 * k, 128, 112)
        f.b(0x2130, 0x00)
        f.b(0x2131, [0x21, 0x31, 0xA1, 0x61, 0x11, 0xB1][k % 6])   # add/sub/half on BG1, OBJ, backdrop
        f.b(0x2132, 0x20 | (k & 31)); f.b(0x2132, 0x40 | (9 + k & 31)); f.b(0x2132, 0x80 | (20 - k & 31))
        return f

    @add('m7-subscreen')
    def _(k):
        f = Frame(); base(f, 7, 0x10, 0x01)
        m7(f, 0x0E0, 0, 0, 0x0E0, 4 * k, 2 * k, 128, 112)
        f.b(0x2130, 0x02); f.b(0x2131, [0x20, 0x60, 0xA0][k % 3])   # backdrop + subscreen BG1
        f.b(0x2132, 0xE0 | (k & 31))
        return f

    @add('m7-mosaic')
    def _(k):
        f = Frame(); base(f, 7, 0x11)
        a, b, c, d = rot(k * 0.11, 0x100)
        m7(f, a, b, c, d, 6 * k, 6 * k, 128, 112, 0x01 if k % 5 == 4 else 0)
        f.b(0x2106, ((k % 15) + 1) << 4 | 0x01)
        return f

    @add('m7-extbg')
    def _(k):
        f = Frame(); base(f, 7, [0x13, 0x12, 0x03, 0x11][k % 4])
        a, b, c, d = rot(k * 0.2, 0x100 + 16 * k)
        m7(f, a, b, c, d, 9 * k, 4 * k, 128, 112)
        f.b(0x2133, 0x40)
        return f

    @add('m7-direct')
    def _(k):
        f = Frame(); base(f, 7, 0x11)
        a, b, c, d = rot(k * 0.25, 0x0C0 + 20 * k)
        m7(f, a, b, c, d, 11 * k, 3 * k, 128, 112)
        f.b(0x2130, 0x01)
        return f

    @add('mode1')
    def _(k):
        f = Frame(); base(f, 0x09 if k & 4 else 0x01, 0x17)
        for i, r in enumerate((0x210D, 0x210F, 0x2111)):
            f.w2(r, (3 + i) * k); f.w2(r + 1, (2 * i + 1) * k)
        return f

    @add('mode1-mosaic')
    def _(k):
        f = Frame(); base(f, 0x01, 0x17)
        for i, r in enumerate((0x210D, 0x210F, 0x2111)):
            f.w2(r, 5 * k + 17 * i); f.w2(r + 1, 3 * k + 40 * i)
        f.b(0x2106, ((k % 15) + 1) << 4 | [0x01, 0x03, 0x07, 0x02, 0x04][k % 5])
        return f

    @add('mosaic-windows')
    def _(k):
        f = Frame()
        if k & 1:
            base(f, 7, 0x11)
            a, b, c, d = rot(k * 0.13, 0x110)
            m7(f, a, b, c, d, 4 * k, 9 * k, 128, 112)
            f.b(0x2123, 0x0A if k & 2 else 0x02)   # BG1: window 1, and window 2 on some frames
        else:
            base(f, 0x01 if k & 4 else 0x00, 0x17)
            for i, r in enumerate((0x210D, 0x210F, 0x2111)):
                f.w2(r, 7 * k + 11 * i); f.w2(r + 1, 2 * k + 33 * i)
            f.b(0x2123, 0x2A if k & 2 else 0x22)   # BG1 and BG2: window 1 (BG1: and window 2)
            f.b(0x2124, 0x03)                      # BG3: window 1 inverted
        f.b(0x2126, 30 + 5 * k); f.b(0x2127, 150 + 4 * k); f.b(0x2128, 90 - 3 * k); f.b(0x2129, 230 - 2 * k)
        f.b(0x212A, [0x00, 0x01, 0x02, 0x03][k % 4])   # the two windows' logic for BG1
        f.b(0x212E, 0x17); f.b(0x212F, 0x00)
        f.b(0x2106, ((3 * k) % 15 + 1) << 4 | 0x07)
        return f

    @add('mode0')
    def _(k):
        f = Frame(); base(f, 0x00, 0x1F)
        for i, r in enumerate((0x210D, 0x210F, 0x2111, 0x2113)):
            f.w2(r, (i + 1) * k); f.w2(r + 1, 256 - (i + 2) * k)
        return f

    for mode in (2, 3, 4, 5, 6):
        def fn(k, mode=mode):
            f = Frame(); base(f, mode, 0x13 if mode != 6 else 0x11)
            for i, r in enumerate((0x210D, 0x210F, 0x2111)):
                f.w2(r, 4 * k + 9 * i); f.w2(r + 1, 2 * k + 30 * i)
            return f
        out.append(('mode%d' % mode, fn))

    @add('mode2-big-windows')
    def _(k):
        f = Frame(); base(f, 0x02 | [0x00, 0x10, 0x20, 0x30, 0x40, 0x50][k % 6], 0x13)
        for i, r in enumerate((0x210D, 0x210F, 0x2111)):
            f.w2(r, 6 * k + 13 * i); f.w2(r + 1, 3 * k + 21 * i)
        f.b(0x2123, 0x23 if k & 1 else 0x02)
        f.b(0x2126, 24 + 6 * k); f.b(0x2127, 180 + 2 * k)
        f.b(0x212E, 0x03); f.b(0x212F, 0x00)
        return f

    for mode in (4, 5, 6):
        def fn(k, mode=mode):
            lay = 0x13 if mode != 6 else 0x11
            f = Frame(); base(f, mode | [0x00, 0x10, 0x20, 0x40][k % 4], lay if k % 3 else 0x10, lay & 3 if k % 3 == 0 else 0)
            for i, r in enumerate((0x210D, 0x210F, 0x2111)):
                f.w2(r, 5 * k + 19 * i); f.w2(r + 1, 4 * k + 27 * i)
            f.b(0x2106, ((k % 15) + 1) << 4 | [0x00, 0x01, 0x03, 0x02][k % 4])
            f.b(0x2123, 0x32 if k & 2 else 0x00)
            f.b(0x2126, 40 + 3 * k); f.b(0x2127, 210 - 5 * k)
            f.b(0x212E, lay & 3); f.b(0x212F, 0x00)
            f.b(0x2130, 0x02 if k % 3 == 0 else 0x00)
            f.b(0x2131, [0x00, 0x23, 0x63, 0xA1, 0x33][k % 5])
            f.b(0x2132, 0xE0 | (k * 3 & 31))
            return f
        out.append(('mode%d-mix' % mode, fn))

    @add('mode3-big')
    def _(k):
        f = Frame(); base(f, 0x03 | [0x10, 0x20, 0x30][k % 3], 0x13 if k % 4 else 0x12, 0x01 if k % 4 == 0 else 0)
        f.w2(0x210D, 7 * k); f.w2(0x210E, 3 * k); f.w2(0x210F, 90 - 5 * k); f.w2(0x2110, 2 * k)
        f.b(0x2130, 0x02 if k % 4 == 0 else 0); f.b(0x2131, 0x20 if k % 4 == 0 else 0)
        return f

    @add('mode1-mosaic-big')
    def _(k):
        f = Frame(); base(f, 0x01 | [0x10, 0x20, 0x40, 0x70, 0x30][k % 5], 0x17)
        for i, r in enumerate((0x210D, 0x210F, 0x2111)):
            f.w2(r, 5 * k + 17 * i); f.w2(r + 1, 3 * k + 40 * i)
        f.b(0x2106, ((k % 15) + 1) << 4 | [0x01, 0x03, 0x07, 0x02, 0x04, 0x05][k % 6])
        f.b(0x2123, 0x22 if k & 1 else 0); f.b(0x2126, 50 + 2 * k); f.b(0x2127, 190 - k); f.b(0x212E, 0x03)
        return f

    @add('mode4-direct')
    def _(k):
        f = Frame(); base(f, 4, 0x13 if k % 3 else 0x12, 0x01 if k % 3 == 0 else 0)
        f.w2(0x210D, 3 * k); f.w2(0x210E, 5 * k); f.w2(0x210F, 60 - k); f.w2(0x2110, k)
        if k % 4: f.b(0x2107, 0x7C)   # BG1's map: the one of palette number 0 (other frames: any palette, software lines)
        f.b(0x2130, 0x03 if k % 3 == 0 else 0x01); f.b(0x2131, 0x20 if k % 3 == 0 else 0)
        return f

    @add('setini')
    def _(k):
        mode = [1, 5, 6, 3, 7, 0][k % 6]
        f = Frame(); base(f, mode, {1: 0x17, 5: 0x13, 6: 0x11, 3: 0x13, 7: 0x11, 0: 0x1F}[mode])
        for i, r in enumerate((0x210D, 0x210F, 0x2111)):
            f.w2(r, 3 * k + 9 * i); f.w2(r + 1, 6 * k + 20 * i)
        if mode == 7: m7(f, 0x100, 0x20, -0x20, 0x100, 4 * k, 2 * k, 128, 112)
        f.b(0x2133, [0x01, 0x02, 0x08, 0x03, 0x0B, 0x09][(k // 2) % 6])
        return f

    # Colour math with a colour from each screen (composed lines).
    @add('math-two-screens')
    def _(k):
        mode = [0x01, 0x09, 0x00, 0x03, 0x07, 0x02, 0x11, 0x01][k % 8]
        m = mode & 7
        lay = {0: 0x1F, 1: 0x17, 2: 0x13, 3: 0x13, 7: 0x11}[m]
        tm = [lay, lay & 0x15, lay & 0x13, 0x10][k % 4]
        ts = [lay & 0x0F, lay, 0x10, lay & 0x12][k % 4] or 0x01
        f = Frame(); base(f, mode, tm, ts)
        for i, r in enumerate((0x210D, 0x210F, 0x2111, 0x2113)):
            f.w2(r, (2 + i) * k + 7 * i); f.w2(r + 1, (3 - i) * k + 50 * i)
        if m == 7:
            a, b, c, d = rot(k * 0.17, 0x120)
            m7(f, a, b, c, d, 3 * k, 5 * k, 128, 112)
        f.b(0x2100, [0x0F, 0x0F, 0x09, 0x04][(k // 3) % 4])
        f.b(0x2130, 0x02 | [0x00, 0x10, 0x20, 0x80, 0x90, 0x00][k % 6])      # subscreen; math and clip windows
        f.b(0x2131, [0x3F, 0x7F, 0xBF, 0xFF, 0x21, 0x52, 0x13, 0xB5, 0x30, 0x0F][k % 10])
        f.b(0x2132, 0x20 | (k & 31)); f.b(0x2132, 0x40 | (2 * k & 31)); f.b(0x2132, 0x80 | (31 - k & 31))
        f.b(0x2125, 0x20 if k % 3 else 0xA2)                               # colour window: 1 (and 2, OBJ window 1)
        f.b(0x2126, 30 + 4 * k); f.b(0x2127, 190 - 2 * k); f.b(0x2128, 100); f.b(0x2129, 240 - 3 * k)
        f.b(0x212B, [0x00, 0x04, 0x08, 0x0C][k % 4])
        f.b(0x2123, 0x02 if k % 5 == 0 else 0)
        f.b(0x212E, 0x11 if k % 5 == 1 else 0x01); f.b(0x212F, 0x13 if k % 5 == 2 else 0x00)
        return f

    # The common shape of it: one layer shown through another, whole screen.
    @add('math-one-layer')
    def _(k):
        f = Frame(); base(f, 0x01, 0x01, 0x02)
        f.w2(0x210D, 2 * k); f.w2(0x210E, k); f.w2(0x210F, 90 - 3 * k); f.w2(0x2110, 2 * k)
        f.b(0x2130, 0x02); f.b(0x2131, [0x61, 0x21, 0xA1][k % 3])
        return f

    @add('mode3-direct')
    def _(k):
        f = Frame(); base(f, 3, 0x13)
        f.w2(0x210D, 3 * k); f.w2(0x210E, 5 * k); f.w2(0x210F, 60 - k); f.w2(0x2110, k)
        f.b(0x2130, 0x01)
        return f

    return out


def build(seed, which, nframes):
    rng = random.Random(seed)
    rom = bytearray([0xFF]) * (8 * BANK)
    c = code()
    rom[0:len(c)] = c
    assert len(c) < 0x7F00
    rom[0x7F00] = 0x40                                                  # $FF00: RTI
    rom[0x7FC0:0x7FC0 + 21] = b'PPU TEST'.ljust(21)
    rom[0x7FD5] = 0x20; rom[0x7FD6] = 0x00; rom[0x7FD7] = 8; rom[0x7FD8] = 0x00
    rom[0x7FD9] = 0x01; rom[0x7FDA] = 0x00; rom[0x7FDB] = 0x00
    for v in range(0x7FE0, 0x8000, 2):
        rom[v] = 0x00; rom[v + 1] = 0xFF                                # $FF00: RTI
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80
    # VRAM (banks 2-3), little-endian words. Pixels (mode 7: the high bytes)
    # are 0 a quarter of the time, so layers show through each other.
    for i in range(0x8000):
        lo = rng.randrange(256)
        hi = 0 if rng.random() < 0.25 else rng.randrange(256)
        # runs of equal words now and then: flat areas as well as noise
        if i & 0x3F0 == 0x100: lo, hi = (i >> 4) & 0xFF, (i >> 6) & 0xFF
        rom[2 * BANK + 2 * i] = lo; rom[2 * BANK + 2 * i + 1] = hi
    # A tilemap whose entries all have palette number 0 (word $7C00; mode 4's direct colour).
    for i in range(0x7C00, 0x8000):
        v = rng.randrange(0x10000) & 0xE3FF
        rom[2 * BANK + 2 * i] = v & 0xFF; rom[2 * BANK + 2 * i + 1] = v >> 8
    # CGRAM: 256 colours, entry 0 a dark blue.
    for i in range(256):
        v = rng.randrange(0x8000) if i else 0x2800
        rom[4 * BANK + 2 * i] = v & 0xFF; rom[4 * BANK + 2 * i + 1] = v >> 8
    # OAM: 24 sprites on screen (the rest below it), every priority and palette.
    oam = bytearray(0x220)
    for s in range(128):
        if s < 24:
            x = rng.randrange(-8, 250); y = rng.randrange(0, 210)
            oam[4 * s] = x & 0xFF; oam[4 * s + 1] = y
            oam[4 * s + 2] = rng.randrange(256)
            oam[4 * s + 3] = rng.randrange(256) & 0xFE                  # (name table 0)
            hi = (1 if x < 0 else 0) | (2 if rng.random() < 0.4 else 0)
        else:
            oam[4 * s] = 0; oam[4 * s + 1] = 0xF0; hi = 0
        oam[0x200 + s // 4] |= hi << (2 * (s & 3))
    rom[4 * BANK + 0x200:4 * BANK + 0x200 + 0x220] = oam
    # The script.
    script = bytearray()
    starts = []
    frame = 0
    for name, fn in scenes(nframes):
        if which and name not in which: continue
        starts.append((frame, name))
        for k in range(nframes):
            for reg, v in fn(k).w:
                script += bytes([reg + 1, v])
            script.append(0)
            frame += 1
    script.append(0xFF)
    assert len(script) <= BANK, 'script too long: %d bytes' % len(script)
    rom[BANK:BANK + len(script)] = script
    rom[0x7FDC:0x7FE0] = b'\xff\xff\x00\x00'
    s = sum(rom) & 0xFFFF
    rom[0x7FDE] = s & 0xFF; rom[0x7FDF] = s >> 8
    rom[0x7FDC] = (s ^ 0xFFFF) & 0xFF; rom[0x7FDD] = (s ^ 0xFFFF) >> 8
    return rom, starts, frame


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--scenes', default='')
    ap.add_argument('--frames', type=int, default=12)
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    which = set(x for x in a.scenes.split(',') if x)
    rom, starts, total = build(a.seed, which, a.frames)
    open(a.out, 'wb').write(rom)
    print('%s: %d bytes, %d scenes, %d frames a pass, seed %d' % (a.out, len(rom), len(starts), total, a.seed))
    if a.list:
        for f, n in starts: print('  frame %4d  %s' % (f, n))


if __name__ == '__main__':
    main()
