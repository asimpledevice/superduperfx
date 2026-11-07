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

## Interpreter-only host (`n64/`)

`make -C n64 ROM=/path/to/game.sfc` builds `n64/snesrecomp_interp.z64`: the
shared runtime with no generated code, so every guest instruction runs in the
interpreter tier. It is game-agnostic. The ROM is packed into the cartridge
filesystem (`rom:/game.sfc`) at build time and never committed.

- `n64/src/n64_host.c`: host contract symbols, the framework's beam-aligned
  frame driver, the boot sequence (`RtlRegisterGame`, `SnesInit`) and the
  frame loop with timing.
- `n64/src/n64_platform.c`: the only libdragon-facing unit (display, pad,
  filesystem, logging). libdragon and the runtime both define `dma_read`,
  `dma_write` and `cart_init`, so the two never share a translation unit,
  and the runtime's copies are renamed with `-D` in the Makefile.
- `n64/src/n64_stubs.c`: empty dispatch tables (interpreter-only) and a
  disabled MSU-1 (`msu1.c` needs `<dirent.h>`).
- `n64/include/config.h`: wraps the desktop `config.h` without SDL.

Controls: stick or D-pad, N64 A = SNES B, B = Y, C-right = A, C-up = X,
Z = Select, Start, L, R. The bottom line of the screen shows the frame
number and the milliseconds spent in the guest frame and the PPU.

### Memory

| | text | data | bss |
|---|---|---|---|
| First link | 519,492 | 5,052 | 6,473,352 |
| Diagnostics shrunk | 519,300 | 5,052 | 395,976 |

The diagnostic ring sizes are now `#ifndef`-overridable in the shared
runtime, and `SNESRECOMP_INTERP_PAGE_MAP=0` drops the interpreter page map.
Desktop defaults are unchanged. Heap use at boot is roughly 1.5 MB (ROM copy,
PPU 205 KB, APU 82 KB, DSP 34 KB, the 256x224 XRGB frame and two
320x240 16-bit display buffers).

### Byte order

The N64 is big-endian; the runtime had only ever run little-endian.
`GET_WORD`, `SET_WORD` and `IndirPtr` in `common_rtl.h` read guest memory
through host-order pointer casts. They now assemble bytes explicitly, which is
identical on little-endian hosts. A full big-endian audit has not been done;
running the build is the next check.

### First run (ares v148, 2026-11-07)

The interpreter-only build boots SMW in ares and reaches the title screen
with correct graphics and colours, so the byte-order fixes hold for
everything SMW exercises up to that point.

Speed (N64 time per SNES frame, from the on-screen counters and log):

| Frames | Guest CPU + APU (`RtlRunFrame`) | PPU | Present (CPU pixel copy) |
|---|---|---|---|
| 1-60 (boot) | 1,464 ms | 18 ms | 20 ms |
| 300-360 (title) | ~1,275 ms | ~107 ms | 17 ms |

The frame budget is 16.7 ms, so this is roughly **80-90x too slow**.

The same configuration built for x86 (`n64/profile/build.sh`) takes about
2.7 ms per frame for ~13,700 SNES instructions: about 200 ns, or ~800 x86
cycles, per instruction. On the VR4300 that becomes ~10,000 cycles per
instruction.

N64 sampling profile, frames 3-13 (`make PROFILE=1`, 15,952 samples at 1 ms):

| Share | Function |
|---|---|
| 18.6% | `_interp_run_core` |
| 12.5% | `cpu_read8` |
| 8.6% | `bridge_bus_read` |
| 8.5% | `cart_getRomPtr` (+ `.part.0`) |
| 5.4% | `RomPtr` |
| 4.4% | `cpu_region_speed` |
| 3.6% | `interp816_runOpcode` |
| 2.6% | `cpu_sram_offset` |
| 2.5% | `bridge_continuous_read` |
| 2.1% | `cart_sync_coprocessors` |
| ~1-2% each | APU sync, beam advance, `snes_refresh_charge`, master clock sync |

No soft-float or 64-bit helper calls appear in the binary; the profile
matches the x86 one in shape. The cost is general per-instruction overhead,
not one pathology. About 40% is the memory path: every opcode byte goes
through the full bus decode (`cpu_read8` -> `bridge_bus_read` ->
`cart_getRomPtr` -> `RomPtr`, plus region-speed and SRAM checks).

### Tools

- `n64/profile/build.sh`: builds the N64 host's exact runtime configuration
  natively, with a stub platform (`linux_platform.c`) that runs N frames and
  can dump the last frame as PPM. Pass `-pg` for gprof.
- `make PROFILE=1`: N64 sampling profiler (COP0 EPC on a 1 ms timer) for
  frames 3-13, dumped as `PROF` lines over IS-Viewer;
  `n64/profile/symbolize.py <elf> <log>` maps them to functions.
- `n64/profile/ares_run.sh ROM OUTDIR "secs..."`: headless ares run with
  debug log and screenshots.
- ares v148 built locally with SDL3 3.2.30 (Ubuntu 24.04 has no SDL3
  package) and a one-line patch mirroring IS-Viewer output to stderr. The
  development snapshot at the time crashed in RDRAM writes; v148 works.

## Next steps

1. Fast memory path for the interpreter: page-table reads for ROM and WRAM
   instead of the full bus decode per byte.
2. Measure the generated (AOT) code's speed on N64, then prototype selective
   AOT with a native-code budget.
3. Cut per-instruction bookkeeping (state syncs, beam, APU catch-up) to
   per-block or per-scanline where correctness allows.
4. Replace the CPU frame conversion and software PPU output with an RDP path.
