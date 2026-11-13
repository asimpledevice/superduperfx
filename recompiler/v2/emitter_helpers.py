"""snesrecomp.recompiler.v2.emitter_helpers

Cross-cutting emitter patterns that aren't width-bound.

Distinct from `widths.py` — this module collects code-shape helpers
that don't take an `op.width` parameter. The first inhabitant is
the JSL/JSR bank save+restore envelope.

Background — DRY_REFACTOR.md follow-up B (2026-04-30): the JSL
emitter and the dispatch-table emitter both inline the same 6-line
"save PB; trace; set PB to target bank; CALL; trace; restore PB"
sequence. The duplication has the same risk shape as the width-mask
class — a future emitter forgets one of the trace calls or the
restore, and PB drifts. Centralizing here keeps the call-site
shape in one place.
"""
import re
from typing import List
from snes_cycles import region_speed


from v2.emit_options import lean_emit


def runtime_code_speed(pc24: int):
    """Keep code-region cycle weighting correct through bank-bit-7 mirrors.

    The generated body's bank is not the live PBR. In particular a slow-bank
    body dispatched through FastROM must use the live bank and MEMSEL.
    """
    low = pc24 & 0x7FFFFF
    slow, low_fast = region_speed(low, 0), region_speed(low, 1)
    high_slow, high_fast = region_speed(low | 0x800000, 0), region_speed(low | 0x800000, 1)
    if slow == low_fast == high_slow == high_fast:
        return str(slow), slow
    assert slow == low_fast == high_slow
    if lean_emit() and (slow, high_fast) == (8, 6):
        # g_code_speed[PB >> 7]: 8, then 6 or 8 by MEMSEL ($420D), kept by
        # the runtime wherever g_memsel changes. Same value, less code.
        return "(uint32)g_code_speed[cpu_PB() >> 7]", None
    return f"((g_memsel && (cpu_PB() & 0x80)) ? {high_fast} : {slow})", None


# ── Stack push/pop micro-pattern helpers (DRY_REFACTOR follow-up C) ────
#
# The 65816 stack has post-decrement-push, pre-increment-pop semantics.
# A byte push decrements S after the write; a word push decrements S
# both before and after, so the word ends up at S+1 (low) and S+2 (high).
# Pop is the inverse.
#
# Without these helpers, every stack-touching emitter reimplements the
# decrement/increment dance. Risk class: get the order wrong (decrement
# before write in a byte push, or skip the second decrement on a word
# push) and the stack frame ends up shifted by one byte. Symptom would
# be an apparent off-by-one in subsequent PLA/PLB pulls.

# ── Assignment to a register or a variable ──────────────────────────────
#
# The 65816 registers are written through runner/src/cpu_regs.h's setters;
# reads are the accessor calls `cpu_A()` etc. Code that holds a register's
# read text as a "field" assigns through here, so the same text works for a
# register and for a plain variable.
_ACCESSOR = re.compile(r"cpu_(A|X|Y|S|D|DB|PB|P|m_flag|x_flag|emulation|flag_[NVZCID])\(\)")


def assign(target: str, value: str) -> str:
    """The C statement storing `value` into `target` (with its `;`)."""
    m = _ACCESSOR.fullmatch(target.strip())
    if m:
        return f"cpu_set_{m.group(1)}({value});"
    return f"{target} = {value};"


def push_byte(val_expr: str) -> List[str]:
    """8-bit stack push: write at S, then decrement S. cpu_write8 stores the
    low byte of its 32-bit value, so a register can be passed whole."""
    return [
        f"cpu_write8(cpu, 0x00, cpu_S(), {val_expr});",
        "cpu_set_S((uint16)(cpu_S() - 1));",
    ]


def push_word(val_expr: str) -> List[str]:
    """16-bit stack push: pre-decrement S, write 2 bytes at S, post-decrement.
    After the push, the pushed word occupies S+1 (low) and S+2 (high)."""
    return [
        "cpu_set_S((uint16)(cpu_S() - 1));",
        f"cpu_write16(cpu, 0x00, cpu_S(), {val_expr});",
        "cpu_set_S((uint16)(cpu_S() - 1));",
    ]


def pop_byte_assign(target_decl: str) -> List[str]:
    """8-bit stack pop: increment S, read at S into target_decl.

    target_decl is the full LHS — e.g. `uint8 _v` (a fresh decl) or
    `cpu_A()` (a register, written through its setter). Caller chooses.
    """
    return [
        "cpu_set_S((uint16)(cpu_S() + 1));",
        assign(target_decl, "cpu_read8(cpu, 0x00, cpu_S())"),
    ]


def pop_word_assign(target_decl: str) -> List[str]:
    """16-bit stack pop: increment S, read 2 bytes at S into target_decl,
    increment S again."""
    return [
        "cpu_set_S((uint16)(cpu_S() + 1));",
        assign(target_decl, "cpu_read16(cpu, 0x00, cpu_S())"),
        "cpu_set_S((uint16)(cpu_S() + 1));",
    ]


# ── Stack-op trace wrapper ──────────────────────────────────────────────
#
# Wraps an existing push/pull C-line sequence with a "save old S → emit ops
# → emit cpu_trace_stack_op" envelope. The trace call is gated at runtime by
# g_stack_op_trace_enabled, so when disabled it's a single load+branch.
#
# `op_id` is one of CPU_STACK_OP_* (see cpu_trace.h enum). `delta` is +N for
# pulls (S grows), -N for pushes (S shrinks). The wrapper opens its own
# `{ uint16 _old_s = cpu->S; ... }` scope so the temporary doesn't leak.
def stack_op_traced(op_id: str, delta: int, body: List[str]) -> List[str]:
    """Wrap `body` (push/pull lines) with old-S capture + post-trace call."""
    return [
        "{",
        "  uint16 _old_s = cpu_S();",
        *(f"  {ln}" for ln in body),
        f"  cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, {delta});",
        "}",
    ]


# ── REP/SEP ────────────────────────────────────────────────────────────
#
# P is kept in parts (runner/src/cpu_regs.h): M, X, I and D as bits of
# P_mxid, N/Z as the last result, C and V on their own. REP and SEP change
# only the parts their mask names.

# Trace-event kind constants (match cpu_trace.h):
PX_REP = 0  # /*REP*/
PX_SEP = 1  # /*SEP*/

def modify_p(mask: int, kind: str) -> List[str]:
    """REP (kind "rep": clear the mask's bits) or SEP ("sep": set them), as
    raw C statements (no leading indent, no enclosing braces)."""
    if kind not in ("rep", "sep"):
        raise ValueError(f"kind must be 'rep' or 'sep', got {kind!r}")
    sep = kind == "sep"
    v = 1 if sep else 0
    lines = ["uint8 _old_p = cpu_P();"]
    mxid = mask & 0x3C
    if mxid:
        op = f"| {mxid:#04x}u" if sep else f"& ~{mxid:#04x}u"
        lines.append(f"cpu_set_P_mxid(cpu_P_mxid() {op});")
        if sep and mxid & 0x10:
            # 8-bit index registers have no high byte.
            lines.append("cpu_set_X(cpu_X() & 0xFFu); cpu_set_Y(cpu_Y() & 0xFFu);")
    if mask & 0x01:
        lines.append(f"cpu_set_flag_C({v});")
    if mask & 0x40:
        lines.append(f"cpu_set_flag_V({v});")
    if (mask & 0x82) == 0x82:
        lines.append(f"cpu_set_flags_nz({v}, {v});")
    elif mask & 0x80:
        lines.append(f"cpu_set_flag_N({v});")
    elif mask & 0x02:
        lines.append(f"cpu_set_flag_Z({v});")
    px_kind, px_label = (PX_SEP, "SEP") if sep else (PX_REP, "REP")
    lines.append(f"cpu_trace_px_record(cpu, 0, {px_kind} /*{px_label}*/, _old_p, cpu_P());")
    return lines


def pb_save_restore_envelope(
    target_bank: int | None,
    body: List[str],
    *,
    trace_pc24: int = 0,
) -> List[str]:
    """Wrap a call, changing PB only for a long transfer.

    ``target_bank=None`` denotes a short transfer: it preserves the live PB,
    including when the generated body executes through a cartridge mirror.

    ``body`` must declare or assign ``RecompReturn _r``. Statements are raw C
    lines with indentation relative to the envelope; callers add the envelope's
    outer indentation and braces.

    Real 65816 hardware sets PB to the target bank for the call's
    duration, then RTL restores it. PHK inside the callee must push
    the CALLEE's bank, not the caller's — without the explicit save
    and restore, inner PHK/PLB sequences poison DB to bank $00.

    RecompReturn ABI (2026-05-02): callee returns RecompReturn.
    NORMAL → continue. SKIP_N → restore PB, then propagate via
    `return SKIP_(N-1)`. A pending LLE yield/deadline is a suspended guest
    continuation: preserve its live PB through every ancestor envelope.
    Testing bridge ownership avoids guessing from the decremented sentinel.

    NB: emit_function.py's per-line scanner auto-injects
    RecompStackPop() before any line whose stripped text starts with
    "return" — that catches the propagation `return` here without an
    explicit pop (which would double-pop).
    """
    trace_pc = f"0x{trace_pc24 & 0xFFFFFF:06x}u"
    bank_enter = [] if target_bank is None else [
        "uint8 _saved_pb = cpu_PB();",
        f"cpu_trace_pb_change(cpu, {trace_pc}, _saved_pb, {target_bank:#04x}, CPU_TR_JSL);",
        f"cpu_set_PB({target_bank:#04x});",
    ]
    bank_leave = [] if target_bank is None else [
        "if (!interp_bridge_lle_unwind_pending()) {",
        f"  cpu_trace_pb_change(cpu, {trace_pc}, cpu_PB(), _saved_pb, CPU_TR_RTL);",
        "  cpu_set_PB(_saved_pb);",
        "}",
    ]
    return [
        *bank_enter,
        *body,
        *bank_leave,
        "if (_r != RECOMP_RETURN_NORMAL) {",
        "  cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);",
        # Mark this exit as SKIP-PROPAGATION so the stack-drift
        # tripwire ignores the LEGITIMATE imbalance from skipping
        # this function's post-JSL cleanup.
        "  cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);",
        "  return (RecompReturn)((int)_r - 1);",
        "}",
    ]


def call_with_pb_save(
    target_bank: int | None,
    callee_name: str,
    *,
    trace_pc24: int = 0,
) -> List[str]:
    """Emit the canonical JSL PB envelope around one AOT callee."""
    return pb_save_restore_envelope(
        target_bank,
        [f"RecompReturn _r = {callee_name}(cpu, _call_hrv);"],
        trace_pc24=trace_pc24,
    )
