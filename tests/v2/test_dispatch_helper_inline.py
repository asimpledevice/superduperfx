"""A `JSL helper` + inline jump table is emitted with the helper's own
instructions run at the call site, then a switch on the table index.

The switch alone would leave out what the helper does on the way to its
indirect jump: it pulls the return frame, writes direct-page scratch, leaves
the table pointer in A and takes its cycles. A caller compiled without them
reaches the handler with different memory and a different clock than the
interpreter does, so such callers could only be kept in the interpreter.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402

from test_dispatcher_phk_per_jml import EXECUTE_PTR_BYTES  # noqa: E402


def _rom(helper=EXECUTE_PTR_BYTES):
    return make_lorom_bank0({
        0x86DF: helper,
        0x9000: bytes([
            0xA5, 0x10,                 # LDA $10
            0x22, 0xDF, 0x86, 0x00,     # JSL $00:86DF
            0x00, 0xC0,                 # -> $C000
            0x00, 0xC1,                 # -> $C100
        ]),
        0xC000: bytes([0x60]),
        0xC100: bytes([0x60]),
    })


def _emit(rom):
    return emit_function(
        rom=rom, bank=0, start=0x9000, entry_m=1, entry_x=1,
        func_name='Caller', dispatch_helpers={0x0086DF: 'short'})


def test_helper_runs_inline_before_the_switch():
    src = _emit(_rom())
    assert 'JSL dispatch -- helper inline, then the table' in src
    body = src[src.index('helper inline'):]
    switch_at = body.index('switch (_idx)')
    prelude = body[:switch_at]
    # The JSL's own frame: PB, then the address of its last byte ($9005).
    assert '0x90' in prelude and '0x05' in prelude
    # The helper's scratch stores and its 16-bit pull happen for real.
    assert 'CPU_STACK_OP_PLY' in prelude and 'CPU_STACK_OP_PLA' in prelude
    assert prelude.count('cpu_write') >= 6
    # Its cycles are charged: STY 3, PLY 4, STY 3, REP 3, AND 3, ASL 2,
    # TAY 2, PLA 5, STA 4, INY 2, LDA [dp],Y 7, STA 4, SEP 3, LDY 3, JML 6.
    assert 'g_master_cycles += 54u' in prelude
    # The handler is chosen by the pointer the helper computed.
    assert 'uint32 _tgt' in prelude
    assert 'case 0: if (_tgt == 0x00c000u)' in body
    assert 'case 1: if (_tgt == 0x00c100u)' in body
    # Anything else continues in the interpreter at that pointer.
    assert 'interp_tier_dispatch_tail_ex(cpu, _tgt' in body


def test_index_is_sampled_before_the_helper_overwrites_a():
    body = _emit(_rom())
    body = body[body.index('helper inline'):]
    assert body.index('uint16 _idx') < body.index('uint32 _tgt')
    assert body.index('uint16 _idx') < body.index('CPU_STACK_OP_PLA')


def test_helper_that_is_not_a_straight_line_stays_interpreted():
    # A branch before the indirect jump: not run inline, and never reduced
    # to the bare switch.
    helper = bytes([0xD0, 0x00]) + EXECUTE_PTR_BYTES   # BNE +0
    src = _emit(_rom(helper))
    assert 'helper inline' not in src
    assert 'JSL dispatch: helper interpreted' in src
    assert 'interp_tier_dispatch_tail(cpu, 0x0086dfu' in src
