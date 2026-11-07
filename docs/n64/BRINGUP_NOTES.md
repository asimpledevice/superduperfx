# N64 bring-up notes

Working notes for running snesrecomp output on the Nintendo 64 with libdragon.
Target: N64 with Expansion Pak (8 MB RDRAM), VR4300 at 93.75 MHz.

## Toolchain

- `mips64-elf-gcc` 16.2.0 from libdragon's prebuilt `gcc-toolchain-mips64`
  package, installed to `/opt/libdragon` (`N64_INST`).
- libdragon `preview` branch, pinned to `39d0d6096130836a65710da6730290701d060272`.
- Flags follow libdragon's `n64.mk`: `-march=vr4300 -mtune=vr4300 -mabi=o64`,
  plus `-G0` for large objects.

## Measurement 1: generated code size (2026-11-07)

Test input: Super Mario World (USA), SHA-1 `6b47bb75d16514b6a476aa0c73a683a2a4c18765`,
analysis config from SuperMarioWorldRecomp `2c52fbc`, framework commit `ccfc129`.

Generation on Linux with the stock pipeline (`build_native_analyzer.py`,
`v2_sync_funcs_h.py`, `v2_emit.py`) succeeded in about 45 s:

- 8,068 roots, 8,268 exact width variants
- 3,432 compiled ahead of time (AOT), 4,836 left to the interpreter (LLE)
- 17 banks, 100 translation units, 72 MB of C, about 1.33 M lines

All 100 units compile for N64 without errors. Reproduce with
`tools/n64/measure_gen_size.sh`.

| Build | `.text` |
|---|---|
| `-Os` | 11,581,992 B (11.0 MiB) |

`-O2` is roughly 10-15% larger than `-Os` on the sampled units.

**This does not fit in 8 MB of RAM**, before counting the runtime, the ROM
image, framebuffers or audio. N64 code executes from RDRAM.

### Where the size comes from

Removing per-block bookkeeping from two of the largest units (a size
experiment only; the result is not semantically valid):

| Unit | Baseline | Without `WatchdogCheck` + LLE deadline checks | Also without cycle accounting + name tracking |
|---|---|---|---|
| `bank01_part03` | 259,588 | 244,604 (-6%) | 228,244 (-13%) |
| `bank00_part0d` | 236,592 | 221,232 (-7%) | 190,936 (-20%) |

Per-block bookkeeping is 13-20%. Most of the size is the instruction bodies:
every guest register and flag lives in `CpuState` memory, and every guest
memory access is a call to `cpu_read*`/`cpu_write*`. Counts in the generated
SMW code: 26,125 `WatchdogCheck()` sites, 29,559 deadline checks and 16,214
interpreter fallback call sites.

### Options

1. **Selective AOT (profile-guided).** Compile only hot functions natively and
   leave the rest to the interpreter, which already exists as the fallback
   tier. The budget becomes a tunable: e.g. "best 3 MB of native code". Needs
   a coverage profile (the framework already has coverage tooling).
2. **Leaner emitter.** Cache guest registers in C locals, skip dead flag
   writes, inline fast paths for WRAM access, and drop diagnostics in an N64
   build mode. Helps both size and speed.
3. **Overlays.** Load banks of generated code from cartridge on demand.
   libdragon has DSO/overlay support (`examples/overlays`, `n64dso`).

These combine; 1 and 2 are probably needed regardless.

## Measurement 2: runtime compile check

Core runtime units compiled with the N64 flags and `-DSNESRECOMP_TRACE=0`:

- Compile cleanly: `cpu_state.c`, `common_cpu_infra.c`, `snes/interp_bridge.c`,
  `snes/ppu.c`, `snes/dma.c`, `snes/spc.c`, `snes/dsp.c`, `snes/apu.c`,
  `snes/cart.c`, `snes/joypad.c`, `snes/snes_other.c`, `snes/color_lut.c`,
  `beam_frame_driver.c`, `beam_frame_driver_state.c`,
  `generic_frame_driver.c`, `cpu_trace.c`, `util.c`.
- Need game-project headers (`config.h`, `variables.h`): `common_rtl.c`,
  `snes/snes.c`, `snes/cpu.c`. An N64 host needs its own minimal versions.
- Needs a timing shim: `snes/interp816.c` (`clock_gettime`/`CLOCK_MONOTONIC`).
- Desktop-only and left out: debug server, Lua bridge, render workers,
  host report, MSU-1, benchmark, audio trace, netplay.

### Static memory that has to go

Largest `.bss` objects in the units above, all diagnostics:

| Object | Unit | Size |
|---|---|---|
| `s_wlog` | `ppu.c` | 1,572,864 |
| `g_interp_edges` | `interp_bridge.c` | 393,216 |
| `s_spc_trace_pc` + `s_spc_trace_op` | `spc.c` | 196,608 |
| `g_itrace_recent` | `interp_bridge.c` | 98,304 |
| `s_interp_scope_names` | `interp_bridge.c` | 81,920 |

These need a compile-time switch for an N64 build.

## Next steps

1. Minimal N64 host: `config.h`/`variables.h` stand-ins, timing shim, libdragon
   `main`, stub video/audio output. Link interpreter-only first (no generated
   code), boot to the first frame, and measure interpreter speed.
2. Add a diagnostics-off switch for the buffers above.
3. Prototype selective AOT with a native-code budget.
