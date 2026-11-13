"""SNESRECOMP_EMIT_LEAN: rewrite the default emitter's memory-access, cycle
charge and P-sync shapes into the helpers of runner/src/cpu_lean_access.h.

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
# A register read (runner/src/cpu_regs.h) is a call in form only: it has no
# side effects, so it does not order against anything.
_REG_READ = re.compile(r'\bcpu_(A|X|Y|S|D|DB|PB|P|m_flag|x_flag|emulation)\(\)')


def _value_ok(v: str) -> bool:
    return _CALL.search(_REG_READ.sub('r', v)) is None


def _address_forms():
    dbx = (r'\(uint8\)\(\(\(\(\(uint32\)cpu_DB\(\) << 16\) \+ \(uint32\)' + _K +
           r' \+ \(uint32\)cpu_([XY])\(\)\)\) >> 16\), \(uint16\)\(\(\(\(uint32\)cpu_DB\(\) << 16\)'
           r' \+ \(uint32\)\3 \+ \(uint32\)cpu_\4\(\)\)\)')
    longx = (r'\(uint8\)\(\(\(\(uint32\)' + _K + r' \+ \(uint32\)cpu_X\(\)\)\) >> 16\), '
             r'\(uint16\)\(\(\(uint32\)\3 \+ \(uint32\)cpu_X\(\)\)\)')
    return [
        # (address text after "cpu_<rw><w>(cpu, ", helper suffix)
        (r'0x00, \(uint16\)\(cpu_D\(\) \+ ' + _K + r'\)', lambda m: 'dp'),
        (r'0x00, \(uint16\)\(cpu_D\(\) \+ ' + _K + r' \+ cpu_X\(\)\)', lambda m: 'dpx'),
        (dbx, lambda m: 'abs' + m.group(4)),
        (longx, lambda m: 'longx'),
    ]


_FORMS = [(re.compile(r'cpu_(read|write)(8|16)\(cpu, ' + a + r'(\)|, )'), name)
          for a, name in _address_forms()]
_PUSH = re.compile(r'cpu_write8\(cpu, 0x00, cpu_S\(\), ([^;\n]*?)\);\s*cpu_set_S\(\(uint16\)\(cpu_S\(\) - 1\)\);')
# A pull into a variable (`v = (cast)cpu_read8(...)`) or a register (`cpu_set_R(...)`).
_PULL = re.compile(r'cpu_set_S\(\(uint16\)\(cpu_S\(\) \+ 1\)\);(\s*)(?:([^;\n=()]*?) = (\(uint\d+\))?'
                   r'cpu_read8\(cpu, 0x00, cpu_S\(\)\);|cpu_set_(\w+)\((\(uint\d+\))?'
                   r'cpu_read8\(cpu, 0x00, cpu_S\(\)\)\);)')

# 16-bit pushes and pulls (PHA/PHX/PHY/PHD with 16-bit registers, PLA/...):
# S moves before and after the word access.
_PUSH16 = re.compile(r'cpu_set_S\(\(uint16\)\(cpu_S\(\) - 1\)\);\s*cpu_write16\(cpu, 0x00, cpu_S\(\), '
                     r'([^;\n]*?)\);\s*cpu_set_S\(\(uint16\)\(cpu_S\(\) - 1\)\);')
_PULL16 = re.compile(r'cpu_set_S\(\(uint16\)\(cpu_S\(\) \+ 1\)\);(\s*)(?:([^;\n=()]*?) = (\(uint\d+\))?'
                     r'cpu_read16\(cpu, 0x00, cpu_S\(\)\);|cpu_set_(\w+)\((\(uint\d+\))?'
                     r'cpu_read16\(cpu, 0x00, cpu_S\(\)\)\);)\s*cpu_set_S\(\(uint16\)\(cpu_S\(\) \+ 1\)\);')

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


_SPD = r'\(uint32\)g_code_speed\[cpu_PB\(\) >> 7\]'
# A charge of one or N CPU cycles at the code-region speed (dynamic charges,
# and a block's static charge when it is not merged).
_CHARGE1 = re.compile(r'g_master_cycles \+= ' + _SPD + ';')
_CHARGEN = re.compile(r'g_master_cycles \+= (\d+) \* ' + _SPD + ';')
_CHARGE_DL = re.compile(r'if \(cpu_D\(\) & 0xFF\) \{ cpu_charge\(cpu, 1u\); \}')
_CHARGE_PAGE = re.compile(r'if \(\((0x[0-9a-fA-F]+) & 0x[fF]{2}00\) != \(\(\1 \+ cpu_([XY])\(\)\) & 0x[fF]{2}00\)\) '
                          r'\{ cpu_charge\(cpu, 1u\); \}')


def _charges(src: str) -> str:
    src = _CHARGE1.sub('cpu_charge(cpu, 1u);', src)
    src = _CHARGEN.sub(lambda m: f'cpu_charge(cpu, {m.group(1)}u);', src)
    src = _CHARGE_DL.sub('cpu_charge_dl(cpu);', src)
    return _CHARGE_PAGE.sub(lambda m: f'cpu_charge_page(cpu, {m.group(1)}u, cpu_{m.group(2)}());', src)


def lean_rewrite(src: str) -> str:
    src = _charges(src)
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
    # The inline word push evaluates its value after the first decrement; the
    # helper evaluates it before, so a value that reads S stays inline.
    src = _PUSH16.sub(lambda m: (f'cpu_push16(cpu, {m.group(1)});'
                                 if _value_ok(m.group(1)) and 'cpu_S()' not in m.group(1)
                                 else m.group(0)), src)
    src = _PULL16.sub(lambda m: (m.group(0) if m.group(4) == 'S' else
                                 f'{m.group(1)}{m.group(2)} = {m.group(3) or ""}cpu_pull16(cpu);'
                                 if m.group(2) is not None else
                                 f'{m.group(1)}cpu_set_{m.group(4)}({m.group(5) or ""}cpu_pull16(cpu));'), src)
    src = _PUSH.sub(lambda m: (f'cpu_push8(cpu, {m.group(1)});' if _value_ok(m.group(1))
                               else m.group(0)), src)
    src = _PULL.sub(lambda m: (f'{m.group(1)}{m.group(2)} = {m.group(3) or ""}cpu_pull8(cpu);'
                               if m.group(2) is not None else
                               f'{m.group(1)}cpu_set_{m.group(4)}({m.group(5) or ""}cpu_pull8(cpu));'), src)
    return src
