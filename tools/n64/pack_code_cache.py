#!/usr/bin/env python3
"""Pack the N64 code-cache modules into one file, rom:/cc.pack.

Each routine module (OBJ_DIR/<name>.o) is linked on its own at 0x80000000
(n64/cc_module.ld) against the main program's symbols (--just-symbols), so
every import is already the runtime's absolute address. Only relocations
that point inside the module are kept: the loader adds (load address -
0x80000000) at each. Routines are stored in g_recomp_routines order (read
from the resident dispatch_v2.c), so record i is pack entry i.

File layout (big-endian u32 words):
  header : magic 'RCM1', count, max_reloc_count, 0
  index  : count entries of { data, image, mem, entry, relocs, nrelocs }
           data/relocs are file offsets; image = bytes to copy; mem = bytes
           to allocate (image + bss); entry = offset of <name>__body
  data   : images and relocation lists, 8-byte aligned
  reloc  : { type << 28 | offset, extra } with type 2 (R_MIPS_32), 4 (26),
           5 (HI16: extra = the paired LO16 immediate, sign-extended), 6 (LO16)

Usage: pack_code_cache.py OUT DISPATCH_C OBJ_DIR MAIN_ELF LD SCRIPT WORK_DIR [JOBS]
"""
import concurrent.futures
import pathlib
import re
import struct
import subprocess
import sys

BASE = 0x80000000
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16, R_MIPS_GPREL16, R_MIPS_PC16 = 2, 4, 5, 6, 7, 10
SHN_ABS = 0xFFF1
SHF_ALLOC = 2
SHT_NOBITS, SHT_REL, SHT_RELA, SHT_SYMTAB = 8, 9, 4, 2


def parse_elf(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        raise ValueError("expected a 32-bit big-endian ELF")
    shoff, = struct.unpack_from(">I", data, 0x20)
    shentsize, shnum = struct.unpack_from(">HH", data, 0x2E)
    secs = []
    for i in range(shnum):
        (name, typ, flags, addr, off, size, link, info, align, entsize) = struct.unpack_from(
            ">IIIIIIIIII", data, shoff + i * shentsize)
        secs.append(dict(type=typ, flags=flags, addr=addr, off=off, size=size,
                         link=link, info=info, entsize=entsize))
    return secs


def pack_one(args):
    name, obj, main_elf, ld, script, work = args
    out = pathlib.Path(work) / f"{name}.elf"
    subprocess.run([ld, "-T", script, f"--just-symbols={main_elf}", "--emit-relocs",
                    "--no-warn-mismatch", "-o", str(out), obj], check=True,
                   capture_output=True, text=True)
    data = out.read_bytes()
    secs = parse_elf(data)
    alloc = [i for i, s in enumerate(secs) if s["flags"] & SHF_ALLOC and s["size"]]
    if not alloc:
        raise ValueError(f"{name}: empty module")
    end_image = max(s["addr"] + s["size"] for s in secs if s["flags"] & SHF_ALLOC
                    and s["type"] != SHT_NOBITS and s["size"])
    end_mem = max(s["addr"] + s["size"] for s in secs if s["flags"] & SHF_ALLOC and s["size"])
    image = bytearray(end_image - BASE)
    for s in secs:
        if s["flags"] & SHF_ALLOC and s["type"] != SHT_NOBITS and s["size"]:
            image[s["addr"] - BASE:s["addr"] - BASE + s["size"]] = data[s["off"]:s["off"] + s["size"]]
    symtab = next(s for s in secs if s["type"] == SHT_SYMTAB)
    strtab = secs[symtab["link"]]
    syms = []
    for i in range(symtab["size"] // 16):
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
            ">IIIBBH", data, symtab["off"] + i * 16)
        nm_off = strtab["off"] + st_name
        nm = data[nm_off:data.index(b"\0", nm_off)].decode()
        syms.append((nm, st_value, st_shndx))
    entry = next(v for n, v, sh in syms if n == f"{name}__body") - BASE
    internal = {i for i in alloc}
    relocs = []
    for s in secs:
        if s["type"] == SHT_RELA:
            raise ValueError(f"{name}: RELA relocations are not expected")
        if s["type"] != SHT_REL or not (secs[s["info"]]["flags"] & SHF_ALLOC):
            continue
        ents = [struct.unpack_from(">II", data, s["off"] + k * 8) for k in range(s["size"] // 8)]
        for k, (r_off, r_info) in enumerate(ents):
            typ, symi = r_info & 0xFF, r_info >> 8
            sym_shndx = syms[symi][2]
            if typ in (R_MIPS_PC16, R_MIPS_GPREL16) or sym_shndx == SHN_ABS:
                continue        # PC-relative, or an import already resolved
            if sym_shndx not in internal:
                raise ValueError(f"{name}: relocation against {syms[symi][0]} (section {sym_shndx})")
            if typ not in (R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16):
                raise ValueError(f"{name}: relocation type {typ}")
            off = r_off - BASE
            extra = 0
            if typ == R_MIPS_HI16:
                lo = next((o for o, i2 in ents[k + 1:] if i2 & 0xFF == R_MIPS_LO16 and i2 >> 8 == symi), None)
                if lo is None:
                    raise ValueError(f"{name}: HI16 without LO16")
                v, = struct.unpack_from(">I", image, lo - BASE)
                extra = (v & 0xFFFF) - 0x10000 if v & 0x8000 else v & 0xFFFF
            relocs.append(((typ << 28) | off, extra & 0xFFFFFFFF))
    return name, bytes(image), end_mem - BASE, entry, relocs


def main(out, dispatch_c, obj_dir, main_elf, ld, script, work, jobs="8"):
    names = re.findall(r"^\s+&([A-Za-z_]\w*)__rt,$", pathlib.Path(dispatch_c).read_text(), re.M)
    pathlib.Path(work).mkdir(parents=True, exist_ok=True)
    tasks = [(n, str(pathlib.Path(obj_dir) / f"{n}.o"), main_elf, ld, script, work) for n in names]
    with concurrent.futures.ThreadPoolExecutor(int(jobs)) as ex:
        mods = list(ex.map(pack_one, tasks))
    count = len(mods)
    head = 16 + count * 24
    blob = bytearray()
    index = []
    for name, image, mem, entry, relocs in mods:
        while (head + len(blob)) % 8:
            blob.append(0)
        data_off = head + len(blob)
        blob += image
        while (head + len(blob)) % 8:
            blob.append(0)
        rel_off = head + len(blob)
        for a, b in relocs:
            blob += struct.pack(">II", a, b)
        index.append((data_off, len(image), mem, entry, rel_off, len(relocs)))
    max_rel = max((m[5] for m in index), default=0)
    out_bytes = bytearray(struct.pack(">4sIII", b"RCM1", count, max_rel, 0))
    for e in index:
        out_bytes += struct.pack(">IIIIII", *e)
    out_bytes += blob
    pathlib.Path(out).write_bytes(out_bytes)
    code = sum(m[2] for m in index)
    nrel = sum(m[5] for m in index)
    print(f"pack_code_cache: {count} modules, {code / 1e6:.2f} MB in memory, "
          f"{nrel} relocations (max {max_rel} per module), file {len(out_bytes) / 1e6:.2f} MB")


if __name__ == "__main__":
    main(*sys.argv[1:])
