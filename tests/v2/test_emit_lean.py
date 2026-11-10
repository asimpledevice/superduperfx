"""SNESRECOMP_EMIT_LEAN: smaller generated code, same behaviour.

The default-mode tests pin the default text; these pin the lean shape and
execute it. Whole-program equality (state and audio hashes) is checked by
building a title both ways (docs/n64/BRINGUP_NOTES.md)."""
import pathlib
import re
import subprocess

import pytest

from v2.emit_function import emit_function
from v2 import emitter_helpers

# LDA $10 / BEQ +2 / LDA #$06 / STA $40 / RTS: blocks at $8000, $8004, $8006.
_CODE = bytes.fromhex('A5 10 F0 02 A9 06 85 40 60')
_BLOCKS = (0x8000, 0x8004, 0x8006)


def _rom():
    return _CODE + bytes(0x8000 - len(_CODE))


def _emit():
    return emit_function(_rom(), bank=0, start=0x8000, entry_m=1, entry_x=1,
                         end=0x8000 + len(_CODE), func_name='Lean')


@pytest.fixture
def lean(monkeypatch):
    monkeypatch.setenv('SNESRECOMP_EMIT_LEAN', '1')


def test_blocks_share_one_yield_exit(lean):
    src = _emit()
    assert src.count('_lle_yield:') == 1
    # No deadline unwind is left inline: the shared exit is the only one
    # (the RTS's return-to-owner unwind is a different exit).
    assert 'interp_bridge_lle_yield_unwind(cpu, (((uint32)cpu->PB << 16) | 0x' not in src
    assert src.count('interp_bridge_lle_yield_unwind(cpu, ((uint32)cpu->PB << 16) | _yield_pc)') == 1
    assert 'WatchdogCheck' not in src
    for pc in _BLOCKS:
        assert (f'recomp_block_enter(cpu, 0x{pc:04x}u), 0)) '
                f'{{ _yield_pc = 0x{pc:04x}u; goto _lle_yield; }}') in src
    # The function-entry check keeps its own deadline call and resume PC.
    assert re.search(r'interp_bridge_lle_deadline_pause\(cpu, \(\(\(uint32\)cpu->PB << 16\) '
                     r'\| 0x8000u\)\), 0\)\) \{ _yield_pc = 0x8000u; goto _lle_yield; \}', src)


def test_mirrored_body_speed_reads_the_table(lean):
    assert emitter_helpers.runtime_code_speed(0x008000) == (
        '(uint32)g_code_speed[cpu->PB >> 7]', None)
    # Banks whose speed never varies keep their constant.
    assert emitter_helpers.runtime_code_speed(0x000100) == ('8', 8)   # low WRAM


def test_default_emission_is_unchanged_by_the_module(monkeypatch):
    monkeypatch.delenv('SNESRECOMP_EMIT_LEAN', raising=False)
    src = _emit()
    assert 'goto _lle_yield' not in src and 'recomp_block_enter' not in src
    assert 'g_code_speed' not in src


_MAIN = r'''
static uint32 g_stop_pc = 0xFFFFFFFFu;
int main(void) {
  static const uint32 stops[] = {0xFFFFFFFFu, 0x8000u, 0x8004u, 0x8006u};
  for (unsigned i = 0; i < sizeof stops / sizeof stops[0]; i++) {
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    memset(g_ram, 0, sizeof g_ram);
    cpu.ram = g_ram;
    cpu.S = 0x01FF; cpu.m_flag = 1; cpu.x_flag = 1; cpu.P = 0x30;
    g_tc_valid = 0; g_recomp_stack_top = 0; g_other_exits = 0; g_other_target = 0;
    g_ram[0x10] = 1;      /* BEQ not taken: all three blocks run, $40 gets 6 */
    cpu_push_jsr_return_frame(&cpu);
    g_stop_pc = stops[i];
    Lean_M1X1(&cpu);
    if (g_recomp_stack_top != 0) { printf("stack %d at stop %u\n", g_recomp_stack_top, i); return 1; }
    if (i == 0) {
      if (g_other_exits || g_ram[0x40] != 0x06 || cpu.S != 0x01FF) {
        printf("full run: exits %d $40=%02x S=%04x\n", g_other_exits, g_ram[0x40], cpu.S);
        return 1;
      }
    } else if (g_other_exits != 1 || g_other_target != stops[i]) {
      printf("stop %04x: exits %d target %06x\n", (unsigned)stops[i], g_other_exits,
             (unsigned)g_other_target);
      return 1;
    }
  }
  puts("ok");
  return 0;
}
'''


def test_each_block_yields_at_its_own_pc(lean, tmp_path):
    """Run the lean function with the deadline forced at each block in turn:
    the shared exit must unwind at that block's PC with the recomp stack
    balanced, and with no deadline the function runs to its RTS."""
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    pause = ('int interp_bridge_lle_deadline_pause(CpuState *cpu, uint32 pc) {\n'
             '  (void)pc; return interp_bridge_lle_master_deadline_reached(cpu);\n}')
    assert pause in harness
    harness = harness.replace(pause, (
        'static uint32 g_stop_pc;\n'
        'int interp_bridge_lle_deadline_pause(CpuState *cpu, uint32 pc) {\n'
        '  (void)cpu; return (pc & 0xFFFF) == g_stop_pc;\n}'))
    main = _MAIN.replace('static uint32 g_stop_pc = 0xFFFFFFFFu;\n', '')
    c_file = tmp_path / 'lean.c'
    c_file.write_text(harness + _emit() + main, encoding='utf-8')
    exe = tmp_path / 'lean'
    repo = pathlib.Path(__file__).resolve().parents[2]
    build = subprocess.run([compiler, '-std=c11', '-O1', '-I', str(repo / 'runner' / 'src'),
                            str(c_file), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == 'ok'
