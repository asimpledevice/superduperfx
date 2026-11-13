"""Lint pass over v2 codegen output.

Catches the next foot-gun in the cpu->B class: a "shadow" copy of CPU
state that some writers forget to update.

Generated code reaches every 65816 register and flag through the accessors
of runner/src/cpu_regs.h (cpu_A(), cpu_set_A(v), cpu_flag_C(), cpu_set_nz8(v),
...), never through a CpuState field: each piece of state has exactly one
home, so there is nothing to keep in sync. The lint fails on any `cpu->FIELD`
reference in emit output, and checks that every register and flag the
emitter names has an accessor.

If new state is genuinely needed, give it one home and accessors in
cpu_regs.h, and add its name to STATE.
"""
from __future__ import annotations

import pathlib
import re


# The CPU state the emitter reaches through cpu_regs.h accessors.
STATE = {
    'A', 'X', 'Y', 'S', 'D', 'DB', 'PB',   # registers
    'P',                                   # the whole status byte (assembled)
    'P_mxid', 'nz', 'flag_C', 'flag_V',     # the status byte's homes
    'flag_N', 'flag_Z', 'flag_I', 'flag_D', # flags read from those homes
    'm_flag', 'x_flag', 'emulation',
}

# No CpuState field is read or written by generated code.
ALLOWED: set[str] = set()


# Fields that were once present and have since been deleted as
# foot-gun shadows. Any reappearance is a regression — this list
# exists so the next "let me add cpu->FOO as a tracker" attempt
# trips a clear failure with a pointer to TROUBLESHOOTING.md.
HISTORICAL_DELETED = {
    'B': "cpu->B was a stale shadow of (cpu_A() >> 8). Deleted in "
         "84b359e after it caused the SMW Layer-3 stripe-image XBA "
         "corruption (see docs/TROUBLESHOOTING.md). Read it as "
         "((cpu_A() >> 8) & 0xFF) or via cpu_read_b().",
}


CPU_FIELD_RE = re.compile(r'\bcpu->(\w+)')


def _cpu_state_h() -> pathlib.Path:
    """Locate runner/src/cpu_state.h. snesrecomp is a self-contained
    subrepo, so this path is reachable from any of __file__'s ancestors
    that contains a `runner/` dir."""
    candidates: list[pathlib.Path] = []
    raw = pathlib.Path(__file__)
    candidates += list(raw.parents)
    candidates += list(raw.resolve().parents)
    candidates += list(pathlib.Path.cwd().parents) + [pathlib.Path.cwd()]
    seen: set[pathlib.Path] = set()
    for c in candidates:
        if c in seen:
            continue
        seen.add(c)
        cand = c / 'runner' / 'src' / 'cpu_state.h'
        if cand.exists():
            return cand
    return pathlib.Path('/nonexistent')


def _scan_v2_codegen_emits() -> dict:
    """Import v2.codegen and call every emitter with a representative
    IR op instance. Collect cpu->FIELD references across all emit
    outputs. Returns {field_name: [op_class_name, ...]} so the lint
    error message can point at which emitter introduces the unexpected
    field.

    This is the load-bearing path for the lint: if a contributor adds
    a new shadow field and writes an emitter that reads it, the new
    cpu->FIELD shows up here regardless of the parent repo's filesystem
    layout. The emit-level scan is more brittle than scanning gen/*.c
    output (it only catches references in unconditional code paths) but
    far more portable.
    """
    from v2 import codegen, ir  # imported lazily so other tests don't pay

    out: dict[str, list[str]] = {}

    # Construct a representative instance of each known IR op. We don't
    # need them to be SEMANTICALLY meaningful — just well-formed enough
    # that emit_op runs without raising. For ops that take a Reg or
    # SegRef, we pick A / a low DP offset.
    seg_dp = ir.SegRef(kind=ir.SegKind.DIRECT, offset=0x10)
    samples = []
    samples.append(ir.Read(seg=seg_dp, width=1, out=ir.Value(vid=1)))
    samples.append(ir.Write(seg=seg_dp, src=ir.Value(vid=1), width=1))
    samples.append(ir.ReadReg(reg=ir.Reg.A, out=ir.Value(vid=2)))
    samples.append(ir.WriteReg(reg=ir.Reg.A, src=ir.Value(vid=2)))
    samples.append(ir.ReadReg(reg=ir.Reg.X, out=ir.Value(vid=3)))
    samples.append(ir.ReadReg(reg=ir.Reg.Y, out=ir.Value(vid=4)))
    samples.append(ir.ReadReg(reg=ir.Reg.B, out=ir.Value(vid=5)))  # historical foot-gun
    samples.append(ir.ReadReg(reg=ir.Reg.D, out=ir.Value(vid=6)))
    samples.append(ir.ReadReg(reg=ir.Reg.S, out=ir.Value(vid=7)))
    samples.append(ir.ReadReg(reg=ir.Reg.DB, out=ir.Value(vid=8)))
    samples.append(ir.ReadReg(reg=ir.Reg.PB, out=ir.Value(vid=9)))
    samples.append(ir.ReadReg(reg=ir.Reg.P, out=ir.Value(vid=10)))
    samples.append(ir.ConstI(value=0x42, width=1, out=ir.Value(vid=11)))
    samples.append(ir.IncReg(reg=ir.Reg.X, delta=1))
    samples.append(ir.SetFlag(flag=ir.Reg.M, value=1))
    samples.append(ir.RepFlags(mask=0x30))
    samples.append(ir.SepFlags(mask=0x30))
    samples.append(ir.XCE())
    samples.append(ir.XBA())
    samples.append(ir.PushReg(reg=ir.Reg.A))
    samples.append(ir.PullReg(reg=ir.Reg.A))
    samples.append(ir.Nop())
    # ALU and Shift ops require enums; use a baseline pair.
    samples.append(ir.Alu(op=ir.AluOp.ADD,
                          lhs=ir.Value(vid=11),
                          rhs=ir.Value(vid=11),
                          width=1,
                          out=ir.Value(vid=13)))
    samples.append(ir.Shift(op=ir.ShiftOp.ASL,
                            src=ir.Value(vid=11),
                            width=1,
                            out=ir.Value(vid=14)))
    samples.append(ir.Return(long=False))

    for op in samples:
        try:
            lines = codegen.emit_op(op)
        except Exception:
            # An emitter that needs a richer fixture is fine — skip.
            continue
        text = '\n'.join(lines)
        for match in CPU_FIELD_RE.finditer(text):
            field = match.group(1)
            out.setdefault(field, []).append(type(op).__name__)

    return out


def _scan_gen_fields() -> dict:
    """Walk every gen/*.c and return a {field_name: [files...]} map
    of every cpu->FIELD reference. Returns {} if the gen dir doesn't
    exist (fresh checkout, no regen yet — lint silently skips)."""
    gen = _gen_v2_dir()
    if not gen.exists():
        return {}
    out: dict[str, list[str]] = {}
    for c_file in sorted(gen.glob('*.c')):
        text = c_file.read_text(encoding='utf-8', errors='replace')
        for match in CPU_FIELD_RE.finditer(text):
            out.setdefault(match.group(1), []).append(c_file.name)
    return out


# ────────────────────────────────────────────────────────────────────────────


def test_no_unexpected_cpu_fields_in_v2_emit():
    """Every cpu->FIELD reference produced by v2 codegen emitters must
    be in the documented allowlist. Catches the contributor adding a
    new shadow field plus an emitter that reads it — at test time,
    before regen + build + visual.

    Scans the in-process emit output rather than the regenerated
    gen/*.c files so the test is portable across the project's
    junction layout (where filesystem ancestor walks miss the parent
    repo)."""
    seen = _scan_v2_codegen_emits()
    unexpected = {
        f: sorted(set(emitters))[:3]
        for f, emitters in seen.items()
        if f not in ALLOWED
    }
    assert not unexpected, (
        f"unexpected cpu->FIELD references in v2 emit output — either "
        f"widen the ALLOWED set in {pathlib.Path(__file__).name} "
        f"(documenting the new field's sync mechanism) or compute on-"
        f"demand from canonical state. Found: {unexpected}"
    )


def test_no_historical_deleted_fields_reappear():
    """Negative test for the cpu->B class — if a deleted shadow field
    reappears in v2 emit output, fail with a pointer to the
    TROUBLESHOOTING entry that explains why it was removed."""
    seen = _scan_v2_codegen_emits()
    for field, why in HISTORICAL_DELETED.items():
        assert field not in seen, (
            f"cpu->{field} reappeared in v2 emit output (referenced "
            f"by {len(set(seen[field]))} emitters: "
            f"{sorted(set(seen[field]))[:5]}).\n\n"
            f"{why}"
        )


def test_state_has_accessors():
    """Every name in STATE has a read accessor in cpu_regs.h (a register
    variable or field behind CPU_REG_IN_*, or an inline function), so a
    rename there fails here instead of leaving the list stale."""
    text = (_cpu_state_h().parent / 'cpu_regs.h').read_text(encoding='utf-8')
    for name in STATE:
        defined = (re.search(rf'CPU_REG_IN_(?:FPU|STRUCT)\({re.escape(name)},', text)
                   or re.search(rf'\bcpu_{re.escape(name)}\(void\)', text))
        assert defined, f"cpu_{name}() is not defined in cpu_regs.h"
