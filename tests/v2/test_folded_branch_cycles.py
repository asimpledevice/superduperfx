"""Execute default emitted constant-Z branches against the real 65816 core."""
from pathlib import Path
import os
import subprocess

import pytest
from _helpers import make_lorom_bank0
from snes65816 import ROM_MAP_LOROM, get_rom_mapping, set_rom_mapping
from v2.emit_function import emit_function


def run_folded_branch_cases(tmp_path, **emit_options):
    import test_hle_func_seam as seam
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    # All bytes are hand-authored. Include identical jump/fall PCs: those
    # edges alone cannot tell whether an offset-zero branch was taken.
    cases = [
        ('BneTaken', 'A9 01 D0 01 EA 60', 1, 1, 'jump'),
        ('BeqTaken', 'A9 00 F0 01 EA 60', 1, 1, 'jump'),
        ('BneFall', 'A9 00 D0 01 EA 60', 1, 1, 'fall'),
        ('BeqFall', 'A9 01 F0 01 EA 60', 1, 1, 'fall'),
        ('BneZeroTaken', 'A9 01 D0 00 60', 1, 1, 'jump'),
        ('BneZeroFall', 'A9 00 D0 00 60', 1, 1, 'fall'),
        ('BeqZeroTaken', 'A9 00 F0 00 60', 1, 1, 'jump'),
        ('BeqZeroFall', 'A9 01 F0 00 60', 1, 1, 'fall'),
        ('WideTaken', 'A9 01 00 D0 01 EA 60', 0, 0, 'jump'),
        ('WideFall', 'A9 00 00 D0 01 EA 60', 0, 0, 'fall'),
        ('IndexTaken', 'A2 00 01 D0 01 EA 60', 1, 0, 'jump'),
        ('IndexFall', 'A0 00 00 D0 00 60', 1, 0, 'fall'),
    ]
    generated = []
    previous = get_rom_mapping()
    try:
        set_rom_mapping(ROM_MAP_LOROM)
        for name, code, m, x, kind in cases:
            folds = []
            generated.append(emit_function(
                make_lorom_bank0({0x8000: bytes.fromhex(code)}),
                bank=0, start=0x8000, entry_m=m, entry_x=x, func_name=name,
                const_z_fold_collector=folds, **emit_options))
            assert len(folds) == 1 and folds[0].taken_kind == kind
    finally:
        set_rom_mapping(previous)
    repo = Path(__file__).resolve().parents[2]
    harness = seam._HARNESS.split('/* The C helpers.')[0]
    harness += r'''
#include "snes/interp816.h"
int interp816_opcode_hook(uint32_t pc) { (void)pc; return 0; }
static uint8_t read_reference(void *memory, uint32_t address) {
  return cpu_read8(memory, (uint8)(address >> 16), (uint16)address);
}
static void write_reference(void *memory, uint32_t address, uint8_t value) {
  cpu_write8(memory, (uint8)(address >> 16), (uint16)address, value);
}
static void prepare(CpuState *cpu, const uint8 *code, unsigned length,
                    unsigned m, unsigned x) {
  memset(cpu, 0, sizeof *cpu); memset(g_ram, 0, sizeof g_ram);
  memcpy(g_ram + 0x8000, code, length);
  cpu_set_S(0x01ff);
  cpu_set_P((m << 5) | (x << 4));
  cpu_push_jsr_return_frame(cpu);
  g_other_exits = 0; g_tc_valid = 0; g_recomp_stack_top = 0;
}
static int check(const char *name, RecompEntryFn fn,
                 const uint8 *code, unsigned length, unsigned m, unsigned x) {
  /* One CPU state (g_cpu): run, keep a copy, prepare again for the reference. */
  CpuState native, memory;
  prepare(&g_cpu, code, length, m, x);
  fn(&g_cpu, 2);
  native = g_cpu; cpu_regs_store(&native);
  if (g_other_exits || g_recomp_stack_top) return 1;
  prepare(&g_cpu, code, length, m, x);
  memory = g_cpu; cpu_regs_store(&memory);
  Interp816 reference = {0}; reference.mem = &memory;
  reference.read = read_reference; reference.write = write_reference;
  reference.sp = memory.reg_S; reference.pc = 0x8000;
  interp816_setFlags(&reference, memory.reg_P); reference.e = false;
  uint64 cycles = 0;
  for (unsigned n = 0; n < 32 && reference.pc != 1; ++n)
    cycles += interp816_runOpcode(&reference);
  if (reference.pc != 1 || native.reg_master_cycles != cycles * 8 || native.reg_S != reference.sp ||
      native.reg_A != reference.a || native.reg_X != reference.x || native.reg_Y != reference.y ||
      native.reg_P != interp816_getFlags(&reference)) {
    /* Bank $00 code: 8 master clocks per CPU cycle. */
    fprintf(stderr, "%s CPU cycles native=%llu interpreter=%llu S=%04x/%04x\n",
            name, (unsigned long long)(native.reg_master_cycles / 8), (unsigned long long)cycles,
            native.reg_S, reference.sp);
    return 1;
  }
  return 0;
}
'''
    main = ['int main(void) { int failures = 0;']
    for name, code, m, x, kind in cases:
        data = ','.join(f'0x{value:02x}' for value in bytes.fromhex(code))
        main.append(f'{{ const uint8 code[] = {{{data}}}; failures += check("{name}", '
                    f'{name}_M{m}X{x}, code, sizeof code, {m}, {x}); }}')
    main.append('return failures != 0; }')
    source = tmp_path / 'folded_branch.c'
    source.write_text(harness + '\n'.join(generated) + '\n'.join(main))
    exe = tmp_path / ('folded_branch.exe' if os.name == 'nt' else 'folded_branch')
    build = subprocess.run([compiler, '-std=c11', '-O1', '-I', str(repo / 'runner/src'),
                            str(source), str(repo / 'runner/src/snes/interp816.c'),
                            '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    executed = subprocess.run([str(exe)], capture_output=True, text=True)
    assert executed.returncode == 0, executed.stdout + executed.stderr


def test_default_folded_branch_cpu_cycles_match_interpreter(tmp_path):
    run_folded_branch_cases(tmp_path)
