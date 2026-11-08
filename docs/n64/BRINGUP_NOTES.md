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

## Speedups (2026-11-07, second pass)

All runtime changes are opt-in compile flags (desktop defaults unchanged) and
keep SMW's frame hashes identical over 3000 frames
(`n64/profile/check_hashes.sh`), except where a generated tree is linked,
which has its own reference (the native and interpreter tiers already differ
upstream).

| Flag | What it does |
|---|---|
| `SNESRECOMP_FAST_READ_MAP` | `cpu_fast_map`: direct page pointers for plain WRAM/ROM; the interpreter bus read uses them |
| `SNESRECOMP_QUIESCENT_BACKEDGE` | stable-wait detector records state only at loop heads |
| `SNESRECOMP_LAZY_BEAM` | beam walked at beam events, before MMIO, and on bridge return, not every instruction |
| `SNESRECOMP_REVERSE_DEBUG=0` | existing switch; drops the per-instruction register round trip |
| `SNESRECOMP_INTERP_EDGE_TRACE=0` | drops per-instruction diagnostic rings |
| `SNESRECOMP_WRAM_POLL_QUIESCENT` | low-WRAM reads on plain carts do not defeat the stable-wait detector |
| `SNESRECOMP_PARK_TO_VBLANK` | beam frame driver parks a wait straight to the field boundary when no IRQ source exists |
| `SNESRECOMP_FRAME_FINGERPRINTS=0`, `SNESRECOMP_PPU_DMA_HISTORY=0` | existing switches; drop per-frame WRAM hash and PPU/DMA snapshot |

Also: `snes_refresh_charge` skips a 64-bit divide; coprocessor presence and
the APU flush threshold are hoisted out of the per-instruction loop.

The biggest single win: in attract mode 82% of interpreted instructions were
SMW's wait-for-NMI loop (`LDA $10 / BEQ` at `$00806B`). The two idle-wait
flags let the runtime skip it exactly.

### N64 results (ares), steady state (frames 360-600)

| Build | Guest CPU + APU | PPU |
|---|---|---|
| First run | ~1,275 ms | ~105-154 ms |
| + fast map, detector, lazy beam | ~594 ms | |
| + reverse-debug off, hoisting | ~441 ms | ~140 ms |
| + idle-wait skip | ~250 ms | ~145 ms |
| + selective native code (below) | **~185 ms** | ~150 ms |

### Native code, selectively

`tools/n64/select_native_set.py` reads a coverage capture
(`SNESRECOMP_TIER2_CAPTURE=1`), attributes interpreted instructions to the
generated function containing them, keeps the hottest native up to a
coverage target or size budget, and writes `force_lle` lines for the rest.
Append them to a copy of the title's bank cfg and regenerate.

SMW attract mode (capture from the interpreter-only build): 88.8% of
interpreted work lies in functions with native bodies.

| Native set | Coverage of interpreted work | MIPS code |
|---|---|---|
| 25 functions | 50% | 76 KB |
| 117 functions | 81% | 512 KB |
| 471 entry PCs (all captured) | 88.8% | 1.6 MB |
| everything generated | | 8.2 MB (11.6 MB with LLE stubs) |

`make GEN_DIR=... GEN_INC=...` links a generated tree. The full captured set
gives a 2.3 MB `.text` ROM that runs SMW's attract mode correctly in ares.

N64 profile of that build (frames 400-460):

| Component | Share |
|---|---|
| Software PPU | 41% |
| APU / SPC700 / DSP | 23% |
| Interpreter (code still interpreted) | 17% |
| CPU pixel copy to the display | 4.7% |
| DMA | 4.0% |
| Bus helpers | 3.6% |
| **Generated code** | **0.9%** |

Native code is already the cheapest part of the frame, so a leaner emitter
does not pay for itself yet. Its value would be code size; selection
already makes the native set fit.

## RDP backend (2026-11-08)

`n64/src/n64_ppu_build.c` + `n64_ppu_draw.c` replace the software PPU on
the N64 (`make RENDER=1`, the default; `RENDER=0` keeps the software PPU,
`RENDER=2` runs both and logs per-frame pixel differences).

- The beam driver's raster replay runs unchanged; each line calls
  `ppu_runLineState` (state, no pixels) and the RDP builder instead of
  `ppu_runLine`.
- CPU: each BG layer becomes one 8-bit palette-indexed plane
  (`index = priority << 7 | palette index`); sprites become four planes, one
  per priority, using `ppu_evaluateSprites`' rules. Bitplanes are converted
  with lookup tables. Layers are expanded tile-major over runs of lines that
  share their registers, so a tile's data is read once per 8 lines. Rows are
  claimed in the cache with Create Dirty Exclusive before being rewritten.
- Windows: up to five spans per line with the software PPU's rules, applied
  by clearing spans and drawn as scissor rectangles.
- Colour math: exact for math on backdrop pixels only (add/subtract/half,
  subscreen or fixed colour): backdrop (+/-) subscreen pixel is a palette
  lookup, so the subscreen is drawn under the main screen with a math
  palette. Brightness is applied in the palettes.
- RDP: per band of identical lines and per span, backdrop fill, subscreen
  planes, then main planes in SNES priority order (each BG plane twice, low
  and high half), in 8-line CI8 strips.
- Fallback per line to the software PPU: layer colour math, mosaic, modes
  2-7, hi-res/interlace, big tiles, subscreen sprites with math.

Verification: the x86 reference compositor (`n64/profile/rdp_reference.c`)
matches the software PPU on all 3000 SMW frames compared (boot, title,
attract demo), and `RENDER=2` in ares matches on every compared frame
through 1170. ares needs software Vulkan (`mesa-vulkan-drivers`) for its
RDP (paraLLEl-RDP); without it RDP commands are silently dropped.

Known limit: a VRAM write by HDMA during the visible frame would reach
background lines already deferred in a run.

### Speed (ares, SMW attract, frames 420-660, hot set native)

| | Guest CPU + APU | PPU stage | Present |
|---|---|---|---|
| Software PPU | ~185 ms | ~150 ms | ~17 ms |
| RDP backend | ~175 ms | ~81 ms | ~10 ms |

The PPU stage is now plane building (~28 ms), sprite expansion (~12 ms),
the PPU's own per-line state/sprite evaluation (~12 ms), palettes and HDMA.
Remaining frame cost is led by the APU (~25%) and the interpreter (~15%).

## Next steps

1. Audio: the SPC700/DSP path is now the largest cost (~25%).
2. Interpreter share: promote more code (analysis of SMW's `$0086DF`
   dispatcher idiom), and gameplay captures for the native set.
3. PPU stage: skip the PPU's duplicate per-line sprite evaluation when the
   RDP path evaluates sprites itself (keep STAT77 flags exact).
4. RDP coverage: layer colour math, mosaic, Mode 7 (a textured plane).
