"""SNESRECOMP_EMIT_CODE_CACHE: bodies behind resident trampolines
(runner/src/recomp_cache.h). Whole-program exactness (state and audio hashes
with the cache unlimited, 1.5 MB, 512 KB and 128 KB) is in
docs/n64/BRINGUP_NOTES.md."""
import pathlib
import re
import subprocess
import types

import pytest

from v2.emit_function import emit_function
from v2.program_emit import emit_dispatch_table
from v2.translation_units import split_bank_translation_units

pytestmark = pytest.mark.code_cache

# A at $8000: JSR $8010 / STA $40 / RTS.  B at $8010: LDA #$33 / RTS.
_CODE = bytes.fromhex('20 10 80 85 40 60') + bytes(10) + bytes.fromhex('A9 33 60')


@pytest.fixture
def cache_mode(monkeypatch):
    monkeypatch.setenv('SNESRECOMP_EMIT_LEAN', '1')
    monkeypatch.setenv('SNESRECOMP_EMIT_CODE_CACHE', '1')


def _rom():
    return _CODE + bytes(0x8000 - len(_CODE))


# A lone emit_function names the callee by address and calls all four of
# its M/X variants, so all four are emitted.
_B = 'bank_00_8010'
_MODES = [(0, 0), (0, 1), (1, 0), (1, 1)]


def _bodies():
    src = emit_function(_rom(), bank=0, start=0x8000, entry_m=1, entry_x=1, end=0x8006,
                        func_name='CallerA')
    for m, x in _MODES:
        src += emit_function(_rom(), bank=0, start=0x8010, entry_m=m, entry_x=x, end=0x8013,
                             func_name=_B)
    return src


def _dispatch():
    return emit_dispatch_table(types.SimpleNamespace(nodes={}),
                               {0x008000: {(1, 1)}, 0x008010: set(_MODES)},
                               {0x008000: 'CallerA', 0x008010: _B}, {})


def test_bodies_are_renamed_and_trampolines_emitted(cache_mode):
    src = _bodies()
    assert 'RecompReturn CallerA_M1X1__body(CpuState *cpu) {' in src
    assert f'RecompReturn {_B}_M1X1__body(CpuState *cpu) {{' in src
    # The caller still calls the plain name: the trampoline.
    assert re.search(rf'_r = {_B}_M1X1\(cpu\);', src)
    d = _dispatch()
    assert 'RecompRoutine CallerA_M1X1__rt = RECOMP_ROUTINE(CallerA_M1X1__body, "CallerA_M1X1");' in d
    assert f'RecompReturn {_B}_M1X1(CpuState *cpu) {{ return recomp_call(&{_B}_M1X1__rt, cpu); }}' in d
    assert 'const unsigned g_recomp_routine_count = 5u;' in d


def test_default_mode_is_unchanged(monkeypatch):
    monkeypatch.setenv('SNESRECOMP_EMIT_LEAN', '1')
    monkeypatch.delenv('SNESRECOMP_EMIT_CODE_CACHE', raising=False)
    assert '__body' not in _bodies() and 'recomp_call' not in _dispatch()


def test_sharding_places_bodies_by_their_pc(cache_mode):
    head = '#include "cpu_state.h"\n/* forward declarations */\n'
    src = _bodies()
    marker = src.index('RecompReturn CallerA_M1X1__body(CpuState *cpu) {')
    # Shard a bank built from both bodies; only their placement is checked.
    from v2 import translation_units as tu
    preamble = head.replace('/* forward declarations */', tu._FORWARD_DECL_MARKER)
    out = split_bank_translation_units(preamble + src[marker:], 0, {'CallerA': 0x008000},
                                       threshold_bytes=0, pc_span=0x10)
    # Two shards: the caller's ($8000) and the callee's ($8010).
    assert len(out) == 2
    assert any('CallerA_M1X1__body' in t and _B + '_M1X1__body' not in t for t in out.values())


_MAIN = r'''
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv) {
  (void)cpu; (void)site_pc24; (void)entry_s; (void)hrv; return 0;   /* M0 variants only; never reached */
}
#define CPU_LEAN_ACCESS_DEFINE
#include "cpu_lean_access.h"
static uint32_t size_of(const char *name) { (void)name; return 1000; }
#define CalleeB_M1X1__rt bank_00_8010_M1X1__rt
static int run(void) {
  CpuState cpu;
  memset(&cpu, 0, sizeof cpu);
  memset(g_ram, 0, sizeof g_ram);
  cpu.ram = g_ram;
  cpu.S = 0x01FF; cpu.m_flag = 1; cpu.x_flag = 1; cpu.P = 0x30;
  g_tc_valid = 0; g_recomp_stack_top = 0;
  cpu_push_jsr_return_frame(&cpu);
  CallerA_M1X1(&cpu);
  return g_ram[0x40] == 0x33 && cpu.S == 0x01FF;
}
int main(void) {
  recomp_cache_set_size_hook(size_of);
  recomp_cache_set_capacity(1500);       /* room for one routine, not two */
  RecompCacheStats st;
  if (!run()) { puts("first call"); return 1; }
  recomp_cache_stats(&st);
  /* B loaded while A ran: A could not be evicted, the budget was exceeded. */
  if (st.loads != 2 || st.evictions != 0 || st.over_budget != 1) { printf("1: %u %u %u\n", st.loads, st.evictions, st.over_budget); return 1; }
  if (!run()) { puts("second call"); return 1; }
  recomp_cache_stats(&st);
  if (st.loads != 2) { puts("resident routines reloaded"); return 1; }
  /* Both idle now: a third routine needs the room, so the idle ones go,
   * least recently used (the caller) first. */
  {
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.ram = g_ram; cpu.S = 0x01FF; cpu.m_flag = 1; cpu.x_flag = 0; cpu.P = 0x20;
    g_tc_valid = 0; g_recomp_stack_top = 0;
    cpu_push_jsr_return_frame(&cpu);
    bank_00_8010_M1X0(&cpu);
  }
  recomp_cache_stats(&st);
  if (st.loads != 3 || st.evictions != 2 || CallerA_M1X1__rt.fn || CalleeB_M1X1__rt.fn ||
      !bank_00_8010_M1X0__rt.fn || st.resident != 1) {
    printf("3: loads %u evictions %u resident %u\n", st.loads, st.evictions, st.resident);
    return 1;
  }
  puts("ok");
  return 0;
}
'''


def test_running_routines_are_never_evicted(cache_mode, tmp_path):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    harness = harness.replace('#define CPU_LEAN_ACCESS_DEFINE\n#include "cpu_lean_access.h"\n', '')
    repo = pathlib.Path(__file__).resolve().parents[2]
    c_file = tmp_path / 'cc.c'
    c_file.write_text(harness + _dispatch() + _bodies() + _MAIN, encoding='utf-8')
    exe = tmp_path / 'cc'
    build = subprocess.run([compiler, '-std=c11', '-O1', '-I', str(repo / 'runner' / 'src'),
                            str(c_file), str(repo / 'runner' / 'src' / 'recomp_cache.c'),
                            '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout[-3000:] + build.stderr[-3000:]
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.strip() == 'ok'
