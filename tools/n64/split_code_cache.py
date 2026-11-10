#!/usr/bin/env python3
"""Split a v2 tree generated with SNESRECOMP_EMIT_CODE_CACHE for the N64
code cache (n64/Makefile CODECACHE=1).

Every compiled routine body F__body goes to its own source file
OUT/cc/F.c (built into the loadable module rom:/cc/F.dso); everything else
(host alias wrappers, declarations, the dispatch table with the routine
records and trampolines, the module descriptor) stays in OUT/resident/,
which is linked into the main program.

Usage: split_code_cache.py GEN_DIR OUT_DIR
"""
import pathlib
import re
import sys

_FN = re.compile(r"^(?:RecompReturn|void)\s+([A-Za-z_]\w*)\s*\(CpuState\s*\*cpu\)\s*\{",
                 re.MULTILINE)
_CALLED = re.compile(r"\b([A-Za-z_]\w*_M[01]X[01])\s*\(")


def split_bank(text):
    """(preamble, [(name, chunk)], residue): chunk is a body definition."""
    matches = list(_FN.finditer(text))
    if not matches:
        return text, [], text
    preamble = text[:matches[0].start()]
    bodies, residue = [], [preamble]
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        chunk = text[m.start():end]
        if m.group(1).endswith("__body"):
            bodies.append((m.group(1)[:-len("__body")], chunk))
        else:
            residue.append(chunk)
    return preamble, bodies, "".join(residue)


def includes_of(preamble):
    return "".join(l + "\n" for l in preamble.splitlines()
                   if l.startswith("#include"))


def write_if_changed(path, text, written):
    written.add(path.name)
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def main(gen_dir, out_dir):
    """Rewrites only files whose text changed, so make rebuilds only those."""
    gen, out = pathlib.Path(gen_dir), pathlib.Path(out_dir)
    (out / "cc").mkdir(parents=True, exist_ok=True)
    (out / "resident").mkdir(parents=True, exist_ok=True)
    cc_written, res_written = set(), set()
    nbodies = 0
    for src in sorted(gen.glob("*.c")):
        text = src.read_text(encoding="utf-8")
        if not src.name.startswith("bank"):
            write_if_changed(out / "resident" / src.name, text, res_written)
            continue
        preamble, bodies, residue = split_bank(text)
        write_if_changed(out / "resident" / src.name, residue, res_written)
        inc = includes_of(preamble)
        for name, chunk in bodies:
            called = sorted(set(_CALLED.findall(chunk)))
            decls = "".join(f"RecompReturn {c}(CpuState *cpu);\n" for c in called)
            write_if_changed(out / "cc" / f"{name}.c",
                             f"/* Code-cache module: {name} (split from {src.name}). */\n"
                             + inc + "\n" + decls + "\n" + chunk, cc_written)
            nbodies += 1
    for extra in gen.glob("*.h"):
        write_if_changed(out / "resident" / extra.name, extra.read_text(encoding="utf-8"),
                         res_written)
    for d, keep in ((out / "cc", cc_written), (out / "resident", res_written)):
        for stale in d.iterdir():
            if stale.name not in keep:
                stale.unlink()
    print(f"split_code_cache: {nbodies} modules")


if __name__ == "__main__":
    main(*sys.argv[1:3])
