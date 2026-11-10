"""SNESRECOMP_EMIT_LEAN: rewrite the default emitter's memory-access shapes
into the addressing-mode helpers of runner/src/cpu_lean_access.h.

Each pattern matches exactly the text the default emitter writes for one
addressing mode and becomes the helper that performs that same call with the
same address arithmetic, so behaviour is unchanged; text that matches no
pattern is left as it is. A write whose value contains a call is left inline:
C leaves argument evaluation order open, and moving the address computation
into the helper must not reorder it against the value's side effects.
(dp),Y is left inline for the same reason (its two pointer reads share one
expression).
"""
import re

_K = r'(0x[0-9a-fA-F]+)'
_CALL = re.compile(r'[A-Za-z_]\w*\s*\(')   # an identifier applied: a call


def _value_ok(v: str) -> bool:
    return _CALL.search(v) is None


def _address_forms():
    dbx = (r'\(uint8\)\(\(\(\(\(uint32\)cpu->DB << 16\) \+ \(uint32\)' + _K +
           r' \+ \(uint32\)cpu->([XY])\)\) >> 16\), \(uint16\)\(\(\(\(uint32\)cpu->DB << 16\)'
           r' \+ \(uint32\)\3 \+ \(uint32\)cpu->\4\)\)')
    longx = (r'\(uint8\)\(\(\(\(uint32\)' + _K + r' \+ \(uint32\)cpu->X\)\) >> 16\), '
             r'\(uint16\)\(\(\(uint32\)\3 \+ \(uint32\)cpu->X\)\)')
    return [
        # (address text after "cpu_<rw><w>(cpu, ", helper suffix)
        (r'0x00, \(uint16\)\(cpu->D \+ ' + _K + r'\)', lambda m: 'dp'),
        (r'0x00, \(uint16\)\(cpu->D \+ ' + _K + r' \+ cpu->X\)', lambda m: 'dpx'),
        (dbx, lambda m: 'abs' + m.group(4)),
        (longx, lambda m: 'longx'),
    ]


_FORMS = [(re.compile(r'cpu_(read|write)(8|16)\(cpu, ' + a + r'(\)|, )'), name)
          for a, name in _address_forms()]
_PUSH = re.compile(r'cpu_write8\(cpu, 0x00, cpu->S, ([^;\n]*?)\);\s*cpu->S = \(uint16\)\(cpu->S - 1\);')
_PULL = re.compile(r'cpu->S = \(uint16\)\(cpu->S \+ 1\);(\s*)([^;\n=]*?) = (\(uint\d+\))?'
                   r'cpu_read8\(cpu, 0x00, cpu->S\);')


def _call_end(s: str, start: int) -> int:
    """Index just past the ')' closing the call whose argument list is open
    at `start` (after the address argument's separator)."""
    depth = 1
    i = start
    while depth:
        c = s[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        i += 1
    return i


def lean_rewrite(src: str) -> str:
    for pat, name in _FORMS:
        out, pos = [], 0
        for m in pat.finditer(src):
            if m.start() < pos:
                continue
            # The low word of a 24-bit pointer, `(bank << 16) | (uint32)read`:
            # its partner read shares the expression (see the module note).
            if src[max(0, m.start() - 16):m.start()].rstrip().endswith('| (uint32)'):
                continue
            rw, w, sep = m.group(1), m.group(2), m.group(m.lastindex)
            k = m.group(3)
            if sep == ')':
                repl = f'cpu_{rw}_{name(m)}{w}(cpu, {k})'
                end = m.end()
            else:                                   # a write: ", value)"
                end = _call_end(src, m.end())
                value = src[m.end():end - 1]
                if rw == 'read' or not _value_ok(value):
                    continue
                repl = f'cpu_{rw}_{name(m)}{w}(cpu, {k}, {value})'
            out.append(src[pos:m.start()])
            out.append(repl)
            pos = end
        out.append(src[pos:])
        src = ''.join(out)
    src = _PUSH.sub(lambda m: (f'cpu_push8(cpu, {m.group(1)});' if _value_ok(m.group(1))
                               else m.group(0)), src)
    src = _PULL.sub(lambda m: f'{m.group(1)}{m.group(2)} = {m.group(3) or ""}cpu_pull8(cpu);', src)
    return src
