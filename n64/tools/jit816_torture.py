#!/usr/bin/env python3
"""Generate a 65816 torture ROM for the JIT's differential test.

The ROM is a LoROM image of random, self-contained 65816 test programs and
nothing else: no game code or data. Build it into the N64 runner twice --
once with JIT816=0 (the interpreter is the reference) and once with JIT816=1
-- with -DN64_STATE_HASH, and compare the [state] lines: the WRAM hash at
every sampled frame must be the same. Each test stores A, X, Y, P, S and D
into WRAM when it ends, and the frame's end cuts the run at a master-clock
deadline, so a wrong result, a wrong flag or a wrong cycle count all show.

    n64/tools/jit816_torture.py OUT.sfc [--seed N] [--banks N] [--only LIST]

Tests come in sets that each run several times before the next set starts
(translated code is made on a block's second visit, so code that ran once
would only ever be interpreted); a seed value that every result changes is
mixed into the next test's accumulator, so the passes differ.

Every test: a prologue (stack, direct page, data bank, register widths,
flags, registers), 6-28 random instructions, an epilogue that stores the
registers. Instructions come from the whole documented set except those
that would leave the test (BRK, COP, STP, WAI, RTI, XCE, WDM) or need
state the generator cannot know. The generator keeps every access inside
work RAM or ROM (never an I/O register), which is what lets two builds be
compared by their RAM alone:

- the data bank is $7E, so absolute and (indirect) operands are RAM;
- the direct page is at most $1C00 and an index register is used with a
  direct-page mode only while its value is known to be small;
- long pointers ([dp], [dp],Y) are written just before they are used;
- PLB, PLD and PLP pull values the generator pushed itself.

--only takes mnemonics or mode names (ORA,dpx,PLP,...) to narrow the body's
instructions when looking for the cause of a difference.
"""
import argparse
import random

BANK = 0x8000
RAMBANK = 0x7E
RESULTS = 0x7FE000          # 256 slots of 16 bytes
PASSES = 0x7FFFF0
REPEAT = 0x7FFFF2            # passes left of the set of tests running
SEED = 0x7FFFF4              # a value every test's result changes, mixed into the next test's accumulator

ALU = {'ORA': 0x00, 'AND': 0x20, 'EOR': 0x40, 'ADC': 0x60, 'STA': 0x80, 'LDA': 0xA0, 'CMP': 0xC0, 'SBC': 0xE0}
ALU_MODES = {'idx': 0x01, 'sr': 0x03, 'dp': 0x05, 'idl': 0x07, 'imm': 0x09, 'abs': 0x0D, 'al': 0x0F,
             'idy': 0x11, 'idp': 0x12, 'isy': 0x13, 'dpx': 0x15, 'ily': 0x17, 'aby': 0x19, 'abx': 0x1D,
             'alx': 0x1F}
# (mnemonic, opcode, mode, register the operation writes: '', 'X' or 'Y')
OTHER = [
    ('LDX', 0xA2, 'immx', 'X'), ('LDX', 0xA6, 'dp', 'X'), ('LDX', 0xAE, 'abs', 'X'), ('LDX', 0xB6, 'dpy', 'X'),
    ('LDX', 0xBE, 'aby', 'X'),
    ('LDY', 0xA0, 'immx', 'Y'), ('LDY', 0xA4, 'dp', 'Y'), ('LDY', 0xAC, 'abs', 'Y'), ('LDY', 0xB4, 'dpx', 'Y'),
    ('LDY', 0xBC, 'abx', 'Y'),
    ('STX', 0x86, 'dp', ''), ('STX', 0x8E, 'abs', ''), ('STX', 0x96, 'dpy', ''),
    ('STY', 0x84, 'dp', ''), ('STY', 0x8C, 'abs', ''), ('STY', 0x94, 'dpx', ''),
    ('STZ', 0x64, 'dp', ''), ('STZ', 0x74, 'dpx', ''), ('STZ', 0x9C, 'abs', ''), ('STZ', 0x9E, 'abx', ''),
    ('CPX', 0xE0, 'immx', ''), ('CPX', 0xE4, 'dp', ''), ('CPX', 0xEC, 'abs', ''),
    ('CPY', 0xC0, 'immx', ''), ('CPY', 0xC4, 'dp', ''), ('CPY', 0xCC, 'abs', ''),
    ('BIT', 0x89, 'imm', ''), ('BIT', 0x24, 'dp', ''), ('BIT', 0x2C, 'abs', ''), ('BIT', 0x34, 'dpx', ''),
    ('BIT', 0x3C, 'abx', ''),
    ('INC', 0xE6, 'dp', ''), ('INC', 0xEE, 'abs', ''), ('INC', 0xF6, 'dpx', ''), ('INC', 0xFE, 'abx', ''),
    ('DEC', 0xC6, 'dp', ''), ('DEC', 0xCE, 'abs', ''), ('DEC', 0xD6, 'dpx', ''), ('DEC', 0xDE, 'abx', ''),
    ('ASL', 0x06, 'dp', ''), ('ASL', 0x0E, 'abs', ''), ('ASL', 0x16, 'dpx', ''), ('ASL', 0x1E, 'abx', ''),
    ('ROL', 0x26, 'dp', ''), ('ROL', 0x2E, 'abs', ''), ('ROL', 0x36, 'dpx', ''), ('ROL', 0x3E, 'abx', ''),
    ('LSR', 0x46, 'dp', ''), ('LSR', 0x4E, 'abs', ''), ('LSR', 0x56, 'dpx', ''), ('LSR', 0x5E, 'abx', ''),
    ('ROR', 0x66, 'dp', ''), ('ROR', 0x6E, 'abs', ''), ('ROR', 0x76, 'dpx', ''), ('ROR', 0x7E, 'abx', ''),
    ('TSB', 0x04, 'dp', ''), ('TSB', 0x0C, 'abs', ''), ('TRB', 0x14, 'dp', ''), ('TRB', 0x1C, 'abs', ''),
    ('PEI', 0xD4, 'dp', ''), ('PEA', 0xF4, 'abs', ''),
]
# (mnemonic, opcode, the index register it writes from a value that may be large: '', 'X', 'Y', 'XY' = copies)
IMPLIED = [
    ('INX', 0xE8, ''), ('INY', 0xC8, ''), ('DEX', 0xCA, ''), ('DEY', 0x88, ''),
    ('TAX', 0xAA, 'X'), ('TAY', 0xA8, 'Y'), ('TXA', 0x8A, ''), ('TYA', 0x98, ''),
    ('TXY', 0x9B, 'XY'), ('TYX', 0xBB, 'YX'), ('TSX', 0xBA, 'X'), ('TSC', 0x3B, ''), ('TDC', 0x7B, ''),
    ('CLC', 0x18, ''), ('SEC', 0x38, ''), ('CLV', 0xB8, ''), ('CLD', 0xD8, ''), ('SED', 0xF8, ''),
    ('CLI', 0x58, ''), ('SEI', 0x78, ''),
    ('ASLA', 0x0A, ''), ('LSRA', 0x4A, ''), ('ROLA', 0x2A, ''), ('RORA', 0x6A, ''), ('INCA', 0x1A, ''),
    ('DECA', 0x3A, ''), ('XBA', 0xEB, ''), ('NOP', 0xEA, ''),
    ('PHA', 0x48, ''), ('PHX', 0xDA, ''), ('PHY', 0x5A, ''), ('PLA', 0x68, ''), ('PLX', 0xFA, 'X'),
    ('PLY', 0x7A, 'Y'), ('PHB', 0x8B, ''), ('PHK', 0x4B, ''), ('PHD', 0x0B, ''), ('PHP', 0x08, ''),
]
BRANCHES = [0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0]


class Gen:
    def __init__(self, rng, only):
        self.r = rng
        self.only = only
        self.mf = self.xf = True
        self.xw = self.yw = False      # the index register may hold a large value
        self.out = bytearray()

    def allowed(self, *names):
        return not self.only or any(n in self.only for n in names)

    def emit(self, *b):
        self.out += bytes(x & 0xFF for x in b)

    def w16(self, v):
        self.emit(v, v >> 8)

    def imm(self, wide, small=False):
        v = self.r.randrange(0x180 if small else 0x10000)
        if self.r.random() < 0.15:
            v = self.r.choice([0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF])
        if wide:
            self.w16(v)
        else:
            self.emit(v)
        return v & (0xFFFF if wide else 0xFF)

    def set_index(self, reg, large):
        large = large and not self.xf
        if reg == 'X':
            self.xw = large
        elif reg == 'Y':
            self.yw = large

    def long_pointer(self, off):
        """Write a pointer into bank $7E at direct page offset off (A is lost)."""
        if not self.mf:
            self.emit(0xA9); self.w16(self.r.randrange(0x10000)); self.emit(0x85, off)
            self.emit(0xA9, RAMBANK, self.r.randrange(256)); self.emit(0x85, off + 2)
        else:
            for k, v in enumerate((self.r.randrange(256), self.r.randrange(256), RAMBANK)):
                self.emit(0xA9, v, 0x85, off + k)

    def operand(self, mode):
        """The operand bytes of a mode; False if the mode cannot be used now."""
        r = self.r
        if mode in ('dpx', 'idx') and self.xw:
            return False
        if mode == 'dpy' and self.yw:
            return False
        if mode in ('dp', 'dpx', 'dpy', 'idp', 'idy', 'idx', 'sr', 'isy'):
            return bytes([r.randrange(256)])
        if mode in ('idl', 'ily'):
            off = r.randrange(0xFC)
            self.long_pointer(off)
            return bytes([off])
        if mode in ('abs', 'abx', 'aby'):
            v = r.randrange(0x10000)
            return bytes([v & 0xFF, v >> 8])
        if mode in ('al', 'alx'):
            v = r.randrange(0x10000)
            return bytes([v & 0xFF, v >> 8, RAMBANK])
        raise ValueError(mode)

    def one(self, loop_reg='', quiet=False):
        """One random instruction (or a short fixed group). loop_reg: a
        register the instruction must not write. quiet: nothing that changes
        the widths or makes an index register's value unknown."""
        r = self.r
        for _ in range(200):
            k = r.random()
            if k < 0.42:                                   # accumulator group
                name = r.choice(list(ALU))
                mode = r.choice(list(ALU_MODES))
                if name == 'STA' and mode == 'imm':
                    continue
                if not self.allowed(name, mode):
                    continue
                if mode == 'imm':
                    self.emit(ALU[name] | ALU_MODES[mode]); self.imm(not self.mf)
                    return
                op = self.operand(mode)
                if op is False:
                    continue
                self.emit(ALU[name] | ALU_MODES[mode]); self.out += op
                return
            if k < 0.70:
                name, opc, mode, writes = r.choice(OTHER)
                if not self.allowed(name, mode):
                    continue
                if writes and writes == loop_reg:
                    continue
                if mode == 'immx':
                    small = r.random() < 0.8
                    if quiet and not small and not self.xf and writes:
                        continue
                    self.emit(opc); v = self.imm(not self.xf, small)
                    if writes:
                        self.set_index(writes, v >= 0x180)
                    return
                if mode == 'imm':
                    self.emit(opc); self.imm(not self.mf)
                    return
                if writes and quiet and not self.xf:
                    continue
                op = self.operand(mode)
                if op is False:
                    continue
                self.emit(opc); self.out += op
                if writes:
                    self.set_index(writes, True)
                return
            if k < 0.93:
                name, opc, eff = r.choice(IMPLIED)
                if not self.allowed(name):
                    continue
                if loop_reg == 'X' and name in ('INX', 'DEX', 'TAX', 'TYX', 'TSX', 'PLX'):
                    continue
                if loop_reg == 'Y' and name in ('INY', 'DEY', 'TAY', 'TXY', 'PLY'):
                    continue
                if eff in ('X', 'Y'):
                    if quiet and not self.xf:
                        continue
                    self.set_index(eff, True)
                elif eff == 'XY':
                    if quiet and self.xw and not self.yw:
                        continue
                    self.yw = self.xw
                elif eff == 'YX':
                    if quiet and self.yw and not self.xw:
                        continue
                    self.xw = self.yw
                self.emit(opc)
                if name == 'PHB':
                    if r.random() < 0.7:
                        self.emit(0xAB)                    # PLB
                elif name == 'PHD':
                    if r.random() < 0.7:
                        self.emit(0x2B)                    # PLD
                elif name == 'PHP':
                    if r.random() < 0.6:
                        self.emit(0x28)                    # PLP (the flags just pushed: widths as they are)
                return
            if quiet:
                continue
            if k < 0.97:                                   # SEP / REP
                if not self.allowed('SEP', 'REP'):
                    continue
                bits = r.randrange(256) & r.choice([0x30, 0x30, 0x31, 0x71, 0xFF, 0x20, 0x10, 0xC3, 0x08])
                if r.random() < 0.5:
                    self.emit(0xE2, bits)
                    if bits & 0x20: self.mf = True
                    if bits & 0x10: self.xf = True; self.xw = self.yw = False
                else:
                    self.emit(0xC2, bits)
                    if bits & 0x20: self.mf = False
                    if bits & 0x10: self.xf = False
                return
            if self.mf and self.allowed('PLP'):            # PLP of a known byte
                p = r.randrange(256)
                self.emit(0xA9, p, 0x48, 0x28)
                self.mf = bool(p & 0x20); self.xf = bool(p & 0x10)
                if self.xf:
                    self.xw = self.yw = False
                return
        self.emit(0xEA)

    def quiet_run(self, n, loop_reg=''):
        for _ in range(n):
            self.one(loop_reg=loop_reg, quiet=True)

    def construct(self, pc_of):
        """A control-flow construct. pc_of(offset) = the address of an offset in self.out."""
        r = self.r
        k = r.randrange(7)
        if k == 0 and self.allowed('Bcc'):                 # a forward conditional branch
            self.emit(r.choice(BRANCHES), 0)
            at = len(self.out)
            self.quiet_run(r.randrange(1, 4))
            d = len(self.out) - at
            if d > 127:
                raise OverflowError
            self.out[at - 1] = d
        elif k == 1 and self.allowed('loop'):              # a counted loop
            reg = r.choice('XY')
            n = r.randrange(2, 6)
            self.emit(0xA2 if reg == 'X' else 0xA0)
            if self.xf: self.emit(n)
            else: self.w16(n)
            self.set_index(reg, False)
            head = len(self.out)
            self.quiet_run(r.randrange(1, 5), loop_reg=reg)
            self.emit(0xCA if reg == 'X' else 0x88)        # DEX / DEY
            d = head - (len(self.out) + 2)
            if d < -128:
                raise OverflowError
            self.emit(0xD0, d)                             # BNE head
        elif k == 2 and self.allowed('BRA'):               # BRA / BRL over unused bytes
            n = r.randrange(0, 4)
            if r.random() < 0.7: self.emit(0x80, n)
            else: self.emit(0x82, n, 0)
            self.emit(*[r.randrange(256) for _ in range(n)])
        elif k == 3 and self.allowed('JMP'):               # JMP / JML over unused bytes
            n = r.randrange(0, 4)
            if r.random() < 0.5:
                t = pc_of(len(self.out) + 3 + n)
                self.emit(0x4C); self.w16(t)
            else:
                t = pc_of(len(self.out) + 4 + n)
                self.emit(0x5C); self.w16(t); self.emit(t >> 16)
            self.emit(*[r.randrange(256) for _ in range(n)])
        elif k in (4, 5) and self.allowed('JSR'):          # a call of a routine placed after it
            lng = r.random() < 0.5
            at = len(self.out)
            self.emit(0x22 if lng else 0x20, 0, 0)
            if lng: self.emit(0)
            self.emit(0x80, 0)                             # BRA over the routine
            bra = len(self.out)
            t = pc_of(len(self.out))
            self.out[at + 1] = t & 0xFF; self.out[at + 2] = (t >> 8) & 0xFF
            if lng: self.out[at + 3] = t >> 16
            for _ in range(r.randrange(1, 4)):             # (registers only: nothing on the stack)
                name, opc, eff = r.choice([i for i in IMPLIED if i[0] not in
                                           ('PHA', 'PHX', 'PHY', 'PLA', 'PLX', 'PLY', 'PHB', 'PHK', 'PHD', 'PHP', 'TSX')])
                if eff == 'X': self.set_index('X', True)
                elif eff == 'Y': self.set_index('Y', True)
                elif eff == 'XY': self.yw = self.xw
                elif eff == 'YX': self.xw = self.yw
                self.emit(opc)
            self.emit(0x6B if lng else 0x60)
            self.out[bra - 1] = len(self.out) - bra
        elif k == 6 and not self.mf and self.allowed('MVN'):   # a short block move inside bank $7E
            self.emit(0xA9); self.w16(r.randrange(24))
            self.emit(r.choice([0x54, 0x44]), RAMBANK, RAMBANK)
        else:
            self.one()

    def test(self, index, pc_of):
        r = self.r
        # Prologue (entered with 16-bit registers).
        self.mf = self.xf = False
        if r.random() < 0.5:
            self.emit(0xA2); self.w16(0x1EFF); self.emit(0x9A)         # LDX #$1EFF, TXS
        else:
            self.emit(0xA9); self.w16(0x1EFF); self.emit(0x1B)         # LDA #$1EFF, TCS
        d = r.choice([0, 0, r.randrange(0x1C01), r.randrange(0x1C) << 8])
        self.emit(0xA9); self.w16(d); self.emit(0x5B)                  # LDA #d, TCD
        self.emit(0xE2, 0x20, 0xA9, RAMBANK, 0x48, 0xAB)               # SEP #$20, LDA #$7E, PHA, PLB
        p = r.randrange(256) & 0x7B                                    # (I clear: no interrupt source is on anyway)
        if r.random() < 0.9:
            p &= ~0x08                                                 # decimal mode in one test of ten
        self.emit(0xA9, p, 0x48, 0x28)                                 # LDA #p, PHA, PLP
        self.mf = bool(p & 0x20); self.xf = bool(p & 0x10)
        self.xw = self.yw = False
        self.emit(0xA2); xv = self.imm(not self.xf, r.random() < 0.8); self.set_index('X', xv >= 0x180)
        self.emit(0xA0); yv = self.imm(not self.xf, r.random() < 0.8); self.set_index('Y', yv >= 0x180)
        self.emit(0xA9); self.imm(not self.mf)
        if self.mf and r.random() < 0.5:                               # the accumulator's hidden byte
            self.emit(0xEB, 0xA9); self.imm(False)
        self.emit(0x4F, SEED & 0xFF, (SEED >> 8) & 0xFF, SEED >> 16)   # EOR seed: a set's passes differ
        # Body.
        for _ in range(r.randrange(6, 29)):
            if r.random() < 0.12:
                self.construct(pc_of)
            else:
                self.one()
        # Epilogue: flags first, then the registers into the test's slot.
        slot = RESULTS + (index & 0xFF) * 16
        def sta(o):
            self.emit(0x8F); self.w16((slot + o) & 0xFFFF); self.emit(slot >> 16)
        self.emit(0x08, 0xC2, 0x30)                                    # PHP, REP #$30
        sta(0)
        self.emit(0x4F, SEED & 0xFF, (SEED >> 8) & 0xFF, SEED >> 16, 0x1A,    # EOR seed, INC A, STA seed
                  0x8F, SEED & 0xFF, (SEED >> 8) & 0xFF, SEED >> 16)
        self.emit(0x8A); sta(2)                                        # TXA
        self.emit(0x98); sta(4)                                        # TYA
        self.emit(0x3B); sta(6)                                        # TSC
        self.emit(0x7B); sta(8)                                        # TDC
        self.emit(0x68); sta(10)                                       # PLA (P and the byte above it)
        self.emit(0xE2, 0x20, 0x8B, 0x68); sta(12)                     # SEP #$20, PHB, PLA
        self.emit(0xC2, 0x30, 0xD8)                                    # REP #$30, CLD


def build(seed, banks, only, set_size, repeat):
    rng = random.Random(seed)
    rom = bytearray([0xFF]) * (banks * BANK)
    # Bank 0: reset, an RTI for every other vector, the header.
    reset = bytes([0x78, 0x18, 0xFB, 0xC2, 0x30, 0xA2, 0xFF, 0x1E, 0x9A, 0xA9, 0x00, 0x00, 0x5B,
                   0xE2, 0x20, 0xA9, 0x80, 0x8D, 0x00, 0x21, 0x9C, 0x00, 0x42, 0x9C, 0x0C, 0x42,
                   0x9C, 0x40, 0x21,                                    # STZ $2140: the runner's SPC timeline starts at the first port write
                   0xC2, 0x30, 0xD8,
                   0xA9, repeat, 0x00, 0x8F, REPEAT & 0xFF, (REPEAT >> 8) & 0xFF, REPEAT >> 16,
                   0x5C, 0x00, 0x80, 0x01])
    rom[0:len(reset)] = reset
    rom[0x100] = 0x40                                                   # RTI
    title = b'JIT816 TORTURE'.ljust(21)
    rom[0x7FC0:0x7FC0 + 21] = title
    rom[0x7FD5] = 0x20; rom[0x7FD6] = 0x00
    rom[0x7FD7] = max(8, (banks * 32 - 1).bit_length()); rom[0x7FD8] = 0x00
    rom[0x7FD9] = 0x01; rom[0x7FDA] = 0x00; rom[0x7FDB] = 0x00
    for v in range(0x7FE0, 0x8000, 2):
        rom[v] = 0x00; rom[v + 1] = 0x81                                # $8100: RTI
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80                              # RESET -> $8000
    index = 0
    for bank in range(1, banks):
        base = bank * BANK
        pos = 0
        set_start = 0
        in_set = 0

        def end_set(p):
            # LDA left, DEC A, STA left, BEQ +4, JML the set's start; then LDA #repeat, STA left
            t = (bank << 16) | (0x8000 + set_start)
            c = [REPEAT & 0xFF, (REPEAT >> 8) & 0xFF, REPEAT >> 16]
            tail = bytes([0xAF] + c + [0x3A, 0x8F] + c + [0xF0, 0x04, 0x5C, t & 0xFF, (t >> 8) & 0xFF, t >> 16,
                          0xA9, repeat, 0x00, 0x8F] + c)
            rom[base + p:base + p + len(tail)] = tail
            return p + len(tail)

        while pos < BANK - 0x440:
            for _ in range(50):
                g = Gen(rng, only)
                try:
                    g.test(index, lambda off, p=pos, b=bank: (b << 16) | (0x8000 + p + off))
                except OverflowError:
                    continue
                break
            rom[base + pos:base + pos + len(g.out)] = g.out
            pos += len(g.out)
            index += 1
            in_set += 1
            if in_set == set_size:
                pos = end_set(pos)
                set_start = pos
                in_set = 0
        if in_set:
            pos = end_set(pos)
        if bank + 1 < banks:
            tail = bytes([0x5C, 0x00, 0x80, bank + 1])                   # JML next bank
        else:                                                            # count the pass, start again
            tail = bytes([0xAF, PASSES & 0xFF, (PASSES >> 8) & 0xFF, PASSES >> 16, 0x1A,
                          0x8F, PASSES & 0xFF, (PASSES >> 8) & 0xFF, PASSES >> 16, 0x5C, 0x00, 0x80, 0x01])
        rom[base + pos:base + pos + len(tail)] = tail
    rom[0x7FDC:0x7FE0] = b'\xff\xff\x00\x00'
    s = sum(rom) & 0xFFFF
    rom[0x7FDE] = s & 0xFF; rom[0x7FDF] = s >> 8
    rom[0x7FDC] = (s ^ 0xFFFF) & 0xFF; rom[0x7FDD] = (s ^ 0xFFFF) >> 8
    return rom, index


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--banks', type=int, default=8, help='32 KB banks, the first for the reset code (default 8)')
    ap.add_argument('--set', type=int, default=40, help='tests in a set (default 40: its translation fits the JIT buffer)')
    ap.add_argument('--repeat', type=int, default=6, help='passes over each set before the next (default 6)')
    ap.add_argument('--only', default='', help='comma-separated mnemonics, modes or constructs to keep')
    a = ap.parse_args()
    only = set(x for x in a.only.split(',') if x)
    rom, n = build(a.seed, a.banks, only, a.set, a.repeat)
    with open(a.out, 'wb') as f:
        f.write(rom)
    print(f'{a.out}: {len(rom)} bytes, {n} tests, seed {a.seed}')


if __name__ == '__main__':
    main()
