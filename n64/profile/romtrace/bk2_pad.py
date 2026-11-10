#!/usr/bin/env python3
"""Convert a BizHawk SNES movie (.bk2, or a TASVideos zip holding one) to the
native profile host's replay file: one little-endian 16-bit controller word
per frame for player 1 (see linux_platform.c). Reset/power columns are
reported, not replayed.

Usage: bk2_pad.py movie.bk2 out.pad
"""
import io, struct, sys, zipfile

# BizHawk SNES P1 column order -> host bit.
BITS = {'Up': 4, 'Down': 5, 'Left': 6, 'Right': 7, 'Select': 2, 'Start': 3,
        'Y': 1, 'B': 0, 'X': 9, 'A': 8, 'L': 10, 'R': 11}


def open_bk2(path):
    z = zipfile.ZipFile(path)
    if 'Input Log.txt' in z.namelist():
        return z
    inner = [n for n in z.namelist() if n.endswith('.bk2')]
    return zipfile.ZipFile(io.BytesIO(z.read(inner[0])))


def main(src, dst):
    lines = open_bk2(src).read('Input Log.txt').decode().splitlines()
    key = next(l for l in lines if l.startswith('LogKey:'))
    groups = key[len('LogKey:'):].strip('#').split('#')
    cols = [c for g in groups for c in g.strip('|').split('|') if c]
    p1 = [c[3:] for c in cols if c.startswith('P1 ')]
    sys_cols = [c for c in cols if not c.startswith('P1 ')]
    words, resets = [], 0
    for l in lines:
        if not l.startswith('|'):
            continue
        fields = [f for f in l.strip('|').split('|')]
        sysf, padf = fields[0], fields[1]
        if any(ch != '.' for ch in sysf):
            resets += 1
        w = 0
        for name, ch in zip(p1, padf):
            if ch != '.':
                w |= 1 << BITS[name]
        words.append(w)
    with open(dst, 'wb') as f:
        f.write(struct.pack('<%dH' % len(words), *words))
    lr = sum(1 for w in words if (w >> 6) & 3 == 3)
    ud = sum(1 for w in words if (w >> 4) & 3 == 3)
    print(f'{len(words)} frames, columns {sys_cols} {p1}; frames with reset/power: {resets}; '
          f'left+right: {lr}; up+down: {ud}')


if __name__ == '__main__':
    main(*sys.argv[1:3])
