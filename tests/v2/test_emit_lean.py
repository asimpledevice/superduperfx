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
        assert re.search(rf'recomp_block_enter_charge\(cpu, 0x{pc:04x}u, \d+u\), 0\)\) '
                         rf'\{{ _yield_pc = 0x{pc:04x}u; goto _lle_yield; \}}', src), pc
    assert 'coprocessor_master_cycles' not in src and 'cpu->cycles +=' not in src
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


# ── Memory-access helpers (cpu_lean_access.h) ─────────────────────────────

def test_rewrite_shapes():
    from v2.lean_rewrite import lean_rewrite
    cases = {
        'cpu_read8(cpu, 0x00, (uint16)(cpu->D + 0x0010))': 'cpu_read_dp8(cpu, 0x0010)',
        'cpu_write16(cpu, 0x00, (uint16)(cpu->D + 0x0010), _v3)': 'cpu_write_dp16(cpu, 0x0010, _v3)',
        'cpu_read8(cpu, 0x00, (uint16)(cpu->D + 0x0020 + cpu->X))': 'cpu_read_dpx8(cpu, 0x0020)',
        ('cpu_read8(cpu, (uint8)(((((uint32)cpu->DB << 16) + (uint32)0x8d90 + (uint32)cpu->X)) >> 16), '
         '(uint16)((((uint32)cpu->DB << 16) + (uint32)0x8d90 + (uint32)cpu->X)))'): 'cpu_read_absX8(cpu, 0x8d90)',
        ('cpu_write8(cpu, (uint8)(((((uint32)cpu->DB << 16) + (uint32)0x1300 + (uint32)cpu->Y)) >> 16), '
         '(uint16)((((uint32)cpu->DB << 16) + (uint32)0x1300 + (uint32)cpu->Y)), (uint8)(_v2 & 0xFF))'):
            'cpu_write_absY8(cpu, 0x1300, (uint8)(_v2 & 0xFF))',
        ('cpu_read16(cpu, (uint8)((((uint32)0x7e2000 + (uint32)cpu->X)) >> 16), '
         '(uint16)(((uint32)0x7e2000 + (uint32)cpu->X)))'): 'cpu_read_longx16(cpu, 0x7e2000)',
        'cpu_write8(cpu, 0x00, cpu->S, (uint8)(cpu->A & 0xFF)); cpu->S = (uint16)(cpu->S - 1);':
            'cpu_push8(cpu, (uint8)(cpu->A & 0xFF));',
        'cpu->S = (uint16)(cpu->S + 1);\n    uint16 _rpcl = (uint16)cpu_read8(cpu, 0x00, cpu->S);':
            '\n    uint16 _rpcl = (uint16)cpu_pull8(cpu);',
    }
    for before, after in cases.items():
        assert lean_rewrite(before) == after, before


def test_rewrite_leaves_order_sensitive_sites_inline():
    from v2.lean_rewrite import lean_rewrite
    # A written value with a call: argument order would change.
    w = 'cpu_write8(cpu, 0x00, (uint16)(cpu->D + 0x0010), cpu_read8(cpu, 0x00, cpu->S))'
    assert lean_rewrite(w).startswith('cpu_write8(cpu, 0x00, (uint16)(cpu->D + 0x0010), ')
    # The low word of a (dp) pointer shares its expression with the bank read.
    p = ('(((uint32)cpu_read8(cpu, 0x00, (uint16)((uint16)(cpu->D + 0x0010) + 2)) << 16) '
         '| (uint32)cpu_read16(cpu, 0x00, (uint16)(cpu->D + 0x0010)))')
    assert lean_rewrite(p) == p
    w2 = 'cpu_write8(cpu, 0x00, cpu->S, f(x)); cpu->S = (uint16)(cpu->S - 1);'
    assert lean_rewrite(w2) == w2


# LDA $10 / STA $12 / LDA $1234,X / STA $1300,Y / LDA $7E2000,X / STA $14,X /
# PHA / LDA #$77 / PLA / PHP / PLP / LDA $20,X / STA $1400,X / BNE +0 / RTS
_MIX = bytes.fromhex('A5 10 85 12 BD 34 12 99 00 13 BF 00 20 7E 95 14 '
                     '48 A9 77 68 08 28 B5 20 9D 00 14 D0 00 60')

_DIFF_MAIN = r'''
static uint8 ram_after[2][0x20000];
static CpuState cpu_after[2];
static uint8 g_pb;
static void run(int which, RecompReturn (*fn)(CpuState *)) {
  CpuState cpu;
  memset(&cpu, 0, sizeof cpu);
  for (unsigned i = 0; i < sizeof g_ram; i++) g_ram[i] = (uint8)(i * 7 + 3);
  cpu.ram = g_ram;
  cpu.S = 0x01FF; cpu.m_flag = 1; cpu.x_flag = 1; cpu.P = 0x30;
  cpu.X = 0xF3; cpu.Y = 5; cpu.D = 0x0101; cpu.DB = 0x7E; cpu.PB = g_pb;
  g_tc_valid = 0; g_recomp_stack_top = 0; g_other_exits = 0;
  cpu_push_jsr_return_frame(&cpu);
  fn(&cpu);
  memcpy(ram_after[which], g_ram, sizeof g_ram);
  cpu.ram = NULL;
  cpu_after[which] = cpu;
}
int main(void) {
  /* Every bank-speed case: slow bank, and the $80 mirror with MEMSEL off/on
   * (the default body tests g_memsel, the lean one reads g_code_speed). */
  for (int c = 0; c < 3; c++) {
    g_pb = c ? 0x80 : 0x00;
    g_memsel = c == 2;
    g_code_speed[1] = g_memsel ? 6 : 8;
    run(0, Def_M1X1);
    run(1, Lean_M1X1);
    if (memcmp(ram_after[0], ram_after[1], sizeof ram_after[0])) { printf("ram differs %d\n", c); return 1; }
    if (memcmp(&cpu_after[0], &cpu_after[1], sizeof cpu_after[0])) {
      printf("cpu differs %d: cycles %llu/%llu master %llu/%llu P %02x/%02x\n", c,
             (unsigned long long)cpu_after[0].cycles, (unsigned long long)cpu_after[1].cycles,
             (unsigned long long)cpu_after[0].master_cycles, (unsigned long long)cpu_after[1].master_cycles,
             cpu_after[0].P, cpu_after[1].P);
      return 1;
    }
    if (cpu_after[0].master_cycles == 0) { puts("no charge"); return 1; }
    if (g_other_exits) { puts("unexpected exit"); return 1; }
  }
  puts("ok");
  return 0;
}
'''


def test_lean_and_default_bodies_leave_the_same_state(monkeypatch, tmp_path):
    """The same routine through every rewritten addressing mode, emitted both
    ways, run from the same state: RAM and the whole CpuState must match."""
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    rom = _MIX + bytes(0x8000 - len(_MIX))
    end = 0x8000 + len(_MIX)
    monkeypatch.delenv('SNESRECOMP_EMIT_LEAN', raising=False)
    default = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1, end=end, func_name='Def')
    monkeypatch.setenv('SNESRECOMP_EMIT_LEAN', '1')
    lean = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1, end=end, func_name='Lean')
    for helper in ('cpu_read_dp8', 'cpu_read_absX8', 'cpu_write_absY8', 'cpu_read_longx8',
                   'cpu_write_dpx8', 'cpu_push8', 'cpu_pull8', 'recomp_frame', 'recomp_enter',
                   'recomp_block_enter_charge', 'cpu_charge_dl', 'cpu_charge_page',
                   'cpu_sync_p_nz', 'cpu_charge(cpu, 1u); goto'):
        assert helper in lean, helper
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    c_file = tmp_path / 'diff.c'
    c_file.write_text(harness + default + lean + _DIFF_MAIN, encoding='utf-8')
    exe = tmp_path / 'diff'
    repo = pathlib.Path(__file__).resolve().parents[2]
    build = subprocess.run([compiler, '-std=c11', '-O1', '-I', str(repo / 'runner' / 'src'),
                            str(c_file), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == 'ok'
