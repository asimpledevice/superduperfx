#!/usr/bin/env python3
"""Fail if any code touches an FPU register SuperDuperFX reserves.

    check_fpu_reserved.py --objdump OBJDUMP [--allow FUNC ...] FILE...

FILE is an ELF, an object or an archive (.a). The reserved list is
n64/fpu_reserve.mk's SDFX_FPU_RESERVED. The functions in its SDFX_FPU_OWNERS,
and any named with --allow, own reserved registers on purpose (the context
switch, the run-flag writer, the exception frame save) and are not reported. Prints each offending
function with the registers it uses, and exits 1 if there is one.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def mk_list(name):
    text = (ROOT / "n64" / "fpu_reserve.mk").read_text().replace("\\\n", " ")
    m = re.search(r"^" + name + r"\s*:=\s*(.+)$", text, re.M)
    return m.group(1).split()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--allow", action="append", default=[])
    ap.add_argument("--shared", action="store_true",
                    help="any function may use SDFX_FPU_SHARED (CPUFPU=1 builds)")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    reserved = {int(x) for x in mk_list("SDFX_FPU_RESERVED")}
    if a.shared:
        reserved -= {int(x) for x in mk_list("SDFX_FPU_SHARED")}
    allow = set(mk_list("SDFX_FPU_OWNERS")) | set(a.allow)
    bad = collections.defaultdict(set)
    for f in a.files:
        out = subprocess.run([a.objdump, "-d", "--no-show-raw-insn", f],
                             capture_output=True, text=True, check=True).stdout
        unit, fn = f, None
        for line in out.splitlines():
            if line.startswith("In archive ") or line.endswith("file format elf32-bigmips"):
                unit = line.split(":")[0].replace("In archive ", "")
                continue
            m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
            if m:
                fn = m.group(1)
                continue
            for r in re.findall(r"\$f(\d+)\b", line):
                if int(r) in reserved and fn not in allow:
                    bad[(unit, fn)].add(int(r))
    for (unit, fn), regs in sorted(bad.items()):
        print(f"{unit}: {fn}: " + " ".join(f"$f{r}" for r in sorted(regs)))
    if bad:
        print(f"{len(bad)} function(s) touch reserved FPU registers", file=sys.stderr)
        return 1
    print("no reserved FPU register touched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
