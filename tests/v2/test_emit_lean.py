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


# ── RTS/RTL decision tree (recomp_return_slow) ────────────────────────────

# Each routine reaches its RTS/RTL with a different stack: balanced,
# shallower (PLA) and deeper (PHA, PHA x3) than at entry.
_RETURNS = {
    'Rts': '60', 'PlaRts': '68 60', 'PhaRts': '48 60', 'Pha3Rts': '48 48 48 60',
    'Pla3Rts': '68 68 68 60',
    'Rtl': '6B', 'PlaRtl': '68 6B', 'PhaRtl': '48 6B', 'Pha3Rtl': '48 48 48 6B',
}

_TREE_STUBS = r'''
static int g_anc = -1, g_owner, g_bounce, g_has_entry, g_pops, g_yields;
static int g_kind; static uint32 g_pc, g_site; static uint16 g_restore;
void RecompStackPop(void) { g_recomp_stack_top--; g_pops++; }
void RecompStackPopYield(void) { g_recomp_stack_top--; g_yields++; }
int cpu_resolve_ancestor_skip(uint16_t ret_s) { (void)ret_s; return g_anc; }
int interp_bridge_return_targets_owner(uint16 a, uint16 b) { (void)a; (void)b; return g_owner; }
int interp_bridge_has_direct_paired_bounce(void) { return g_bounce; }
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) { (void)cpu; (void)pc24; return g_has_entry; }
RecompReturn interp_bridge_lle_yield_unwind(CpuState *cpu, uint32 pc24) {
  (void)cpu; g_kind = 1; g_pc = pc24; return 101; }
RecompReturn interp_tier_dispatch_rewritten_return(CpuState *cpu, uint32 pc24, uint32 site) {
  (void)cpu; g_kind = 2; g_pc = pc24; g_site = site; return 102; }
RecompReturn interp_tier_dispatch_popped_return(CpuState *cpu, uint32 pc24, uint32 site,
    uint16 restore) {
  (void)cpu; g_kind = 3; g_pc = pc24; g_site = site; g_restore = restore; return 103; }
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24, uint16 restore, uint32 site) {
  (void)cpu; g_kind = 4; g_pc = pc24; g_site = site; g_restore = restore; return 104; }
'''

_TREE_MAIN = r'''
typedef struct { int ret, kind, pops, yields, top; uint32 pc, site; uint16 restore, S; uint8 hrv; } Out;
static Out run(RecompReturn (*fn)(CpuState *), int hrv, int pattern) {
  CpuState cpu;
  memset(&cpu, 0, sizeof cpu);
  memset(g_ram, 0, sizeof g_ram);
  cpu.ram = g_ram;
  cpu.S = 0x01F0; cpu.m_flag = 1; cpu.x_flag = 1; cpu.P = 0x30; cpu.PB = 0x00; cpu.A = 0x77;
  for (int i = 0; i < 16; i++)   /* the stack above S: equal or distinct bytes */
    g_ram[0x01F0 + i - 4] = pattern ? (uint8)(0x31 + 0x11 * i) : 0x80;
  cpu.host_return_valid = (uint8)hrv;
  g_tc_valid = 0; g_recomp_stack_top = 1;
  g_kind = 0; g_pc = g_site = 0; g_restore = 0; g_pops = g_yields = 0;
  Out o;
  o.ret = fn(&cpu);
  o.kind = g_kind; o.pc = g_pc; o.site = g_site; o.restore = g_restore;
  o.pops = g_pops; o.yields = g_yields; o.top = g_recomp_stack_top;
  o.S = cpu.S; o.hrv = cpu.host_return_valid;
  return o;
}
int main(void) {
  static const struct { const char *name; RecompReturn (*def)(CpuState *), (*lean)(CpuState *); } fns[] = {
FNS
  };
  long cases = 0; int kinds[5] = {0};
  for (unsigned f = 0; f < sizeof fns / sizeof fns[0]; f++)
    for (int hrv = 0; hrv < 4; hrv++)
      for (int pattern = 0; pattern < 2; pattern++)
        for (int bits = 0; bits < 16; bits++) {
          g_anc = (bits & 1) ? 1 : -1; g_owner = (bits >> 1) & 1;
          g_bounce = (bits >> 2) & 1; g_has_entry = (bits >> 3) & 1;
          Out a = run(fns[f].def, hrv, pattern), b = run(fns[f].lean, hrv, pattern);
          cases++; kinds[a.kind]++;
          if (a.ret != b.ret || a.kind != b.kind || a.pc != b.pc || a.site != b.site ||
              a.restore != b.restore || a.pops != b.pops || a.yields != b.yields ||
              a.top != b.top || a.S != b.S || a.hrv != b.hrv) {
            printf("%s hrv %d pattern %d bits %d: ret %d/%d kind %d/%d pc %06x/%06x "
                   "restore %04x/%04x pops %d/%d yields %d/%d S %04x/%04x\n",
                   fns[f].name, hrv, pattern, bits, a.ret, b.ret, a.kind, b.kind,
                   (unsigned)a.pc, (unsigned)b.pc, a.restore, b.restore, a.pops, b.pops,
                   a.yields, b.yields, a.S, b.S);
            return 1;
          }
        }
  printf("ok %ld normal-or-skip %d yield %d rewritten %d popped %d dispatch %d\n", cases,
         kinds[0], kinds[1], kinds[2], kinds[3], kinds[4]);
  return 0;
}
'''


def test_return_tree_matches_default_on_every_path(monkeypatch, tmp_path):
    """Every RTS/RTL shape against every answer the runtime can give: the
    lean site (host return inline, the rest in recomp_return_slow) must end
    the same way as the written-out tree, down to the exit called, its
    arguments, S and the kind of recomp-stack pop."""
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    generated = []
    for name, hexcode in _RETURNS.items():
        code = bytes.fromhex(hexcode)
        rom = code + bytes(0x8000 - len(code))
        for mode, prefix in (('', 'Def'), ('1', 'Lean')):
            if mode:
                monkeypatch.setenv('SNESRECOMP_EMIT_LEAN', '1')
            else:
                monkeypatch.delenv('SNESRECOMP_EMIT_LEAN', raising=False)
            src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                                end=0x8000 + len(code), func_name=prefix + name)
            if mode:
                assert 'recomp_return_slow' in src, name
            generated.append(src)
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    replaced = {
        'void RecompStackPop(void)': r'void RecompStackPop\(void\) \{[^\n]*\}\n',
        'void RecompStackPopYield(void)': r'void RecompStackPopYield\(void\) \{[^\n]*\}\n',
        'cpu_resolve_ancestor_skip': r'int cpu_resolve_ancestor_skip\(.*?\n?\}\n',
        'targets_owner': r'int interp_bridge_return_targets_owner\(.*?\n\}\n',
        'paired_bounce': r'int interp_bridge_has_direct_paired_bounce\(void\) \{[^\n]*\}\n',
        'has_entry': r'int cpu_dispatch_has_entry\(.*?\n\}\n',
        'yield_unwind': r'RecompReturn interp_bridge_lle_yield_unwind\(.*?\n\}\n',
        'rewritten': r'RecompReturn interp_tier_dispatch_rewritten_return\(.*?\n\}\n',
        'popped': r'RecompReturn interp_tier_dispatch_popped_return\(.*?\n\}\n',
        'pc_from': r'RecompReturn cpu_dispatch_pc_from\(.*?\n\}\n',
    }
    for what, pat in replaced.items():
        harness, n = re.subn(pat, '', harness, count=1, flags=re.S)
        assert n == 1, what
    fns = ',\n'.join(f'    {{"{n}", Def{n}_M1X1, Lean{n}_M1X1}}' for n in _RETURNS)
    source = harness + _TREE_STUBS + ''.join(generated) + _TREE_MAIN.replace('FNS', fns)
    c_file = tmp_path / 'tree.c'
    c_file.write_text(source, encoding='utf-8')
    exe = tmp_path / 'tree'
    repo = pathlib.Path(__file__).resolve().parents[2]
    build = subprocess.run([compiler, '-std=c11', '-O1', '-I', str(repo / 'runner' / 'src'),
                            str(c_file), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout[-3000:] + build.stderr[-3000:]
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.startswith('ok'), run.stdout
    print(run.stdout)
    # Every exit was taken somewhere in the sweep.
    counts = dict(re.findall(r'(\w[\w-]*) (\d+)', run.stdout.split(' ', 2)[2]))
    assert all(int(v) > 0 for v in counts.values()), run.stdout
