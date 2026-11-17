#!/usr/bin/env python3
"""Fail if an RSP microcode ELF has a branch or jump in a delay slot.

That is undefined on MIPS; on the RSP it sends execution astray. Easy to
create by inserting a label after a branch (rsp_snesdsp.S, the paging
slot). Usage: check_delay_slots.py OBJDUMP ELF..."""
import re, subprocess, sys

BR = set('j jal jr jalr b bal beq bne beqz bnez blez bgtz bltz bgez bltzal bgezal'.split())

def check(objdump, elf):
    out = subprocess.run([objdump, '-d', elf], capture_output=True, text=True, check=True).stdout
    ins = []
    for line in out.splitlines():
        m = re.match(r'^\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)', line)
        if m:
            ins.append((m.group(1), m.group(2)))
        elif line.strip() == '...':
            ins.append(None)   # a gap (zeros): not a delay slot pair
    bad = [(a, b) for a, b in zip(ins, ins[1:]) if a and b and a[1] in BR and b[1] in BR]
    for a, b in bad:
        print(f'{elf}: {b[1]} at {b[0]} is in the delay slot of {a[1]} at {a[0]}', file=sys.stderr)
    return not bad

ok = all([check(sys.argv[1], e) for e in sys.argv[2:]])
sys.exit(0 if ok else 1)
