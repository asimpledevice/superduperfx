#!/usr/bin/env python3
"""EXTERN() linker script listing every symbol the code-cache modules import,
so the main program keeps them through --gc-sections (n64.mk links an
*.externs file with -T).

Usage: code_cache_externs.py OUT.externs OBJ_DIR NM
"""
import pathlib
import subprocess
import sys


def main(out, obj_dir, nm):
    objs = sorted(str(p) for p in pathlib.Path(obj_dir).glob("*.o"))
    syms = set()
    for i in range(0, len(objs), 500):
        res = subprocess.run([nm, "-u"] + objs[i:i + 500], capture_output=True,
                             text=True, check=True).stdout
        for line in res.splitlines():
            p = line.split()
            if len(p) == 2 and p[0] == "U":
                syms.add(p[1])
    text = "".join(f"EXTERN({s})\n" for s in sorted(syms))
    p = pathlib.Path(out)
    if not p.exists() or p.read_text() != text:
        p.write_text(text)
    print(f"code_cache_externs: {len(syms)} imports")


if __name__ == "__main__":
    main(*sys.argv[1:4])
