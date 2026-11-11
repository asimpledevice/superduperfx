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

## Audio (2026-11-08)

Output goes through libdragon's audio interface (`n64_platform_audio_*`,
`RtlRenderAudio`). The N64 build uses the native APU runner
(`SNESRECOMP_APU_NATIVE=1`). Exact speedups, all output-identical (x86 frame
and audio hashes over 3000 frames, native and LLE APU):

- SPC plain-ARAM accesses inline in `spc.c`; only the I/O page and IPL ROM
  go through `apu_cpuRead`/`apu_cpuWrite`.
- The S-DSP skips Gaussian interpolation for a voice whose envelope is and
  stays zero (the product is zero; key-on is computed as before).
- `SNESRECOMP_AUDIO_TRACE_HOOKS=0` compiles the per-sample/register/port
  trace hooks out (statistics only; N64 sets it).
- The overlay mixer returns early when no voice plays.
- Native runner: SPC timers advance lazily in closed form, synced before
  any SPC access to `$F1`, `$FA-$FF` and at the end of every run. Cycle
  counted A/B: 60.6 -> 55 ms per frame for the APU runner.

APU runner per frame (ares, cycle counter, SMW title): ~55 ms, of which
S-DSP ~26 ms, SPC700 ~14.5 ms, runner loop ~14.5 ms over ~5200 SPC
instructions (the SMW driver polls timer 0, so syncs stay frequent).
`N64_STATE_HASH` (via `EXTRA_CFLAGS`) logs WRAM/ARAM hashes every 60 frames;
it confirmed identical emulation between builds on N64.

### S-DSP on the CPU (option 1)

Per DSP sample (ares, cycle counter, warm = rerun from restored state):
cold ~3700 cycles, warm ~2700 before tuning. Split: voices ~290 cycles
each (common path ~100 instructions plus five 5-cycle MULT stalls), echo
~575, mixing ~95, output ~135. Rewriting the channel step and BRR decoder
with locals (byte stores to `dsp->ram`/ARAM alias everything, forcing
reloads) and per-filter BRR loops: warm 2700 -> ~2510 (-7%), exact.
`SNESRECOMP_DSP_NATIVE_MIX=1` measured slower here (~2680), so it stays off.

The code is near its intrinsic cost. The larger CPU-side lever is cache
warmth (cold minus warm ~1000 cycles per sample), which batching the DSP
recovers; that is the first stage of the deferred (RSP) DSP.

Calibration build: `make EXTRA_CFLAGS="-DN64_DSP_CLOCK
-DSNESRECOMP_DSP_CYCLE_HOOK=n64_dsp_cycle_timed"` shows `dsp cold/warm`
cycles per sample on the status line (refreshed every 4096 samples), for
comparing ares with real hardware. ares at frame 731: 4756/3072.

### Deferred S-DSP (option 2, stage 1)

`SNESRECOMP_APU_DEFER_DSP=1` (native runner only; off by default, including
on N64 for now) runs the C DSP in batches of up to 256 samples behind the
SPC. SPC writes to ARAM and DSP registers are logged with the number of
samples owed; a sync undoes the logged ARAM writes and replays the owed
samples with the log reapplied in order. Forced syncs: SPC reads of ENVX,
OUTX, ENDX (other register reads are answered from the log), SPC reads of
the echo window, FLG/ESA/EDL writes, full log/batch, save/load, the end of
`run_frame` and of `RtlRunFrame`, `RtlRenderAudio`, the HLE SPC upload,
output suppression, trim and ring snapshots. Host code that reads DSP
output or writes ARAM directly must call `apu_dspSync` first.

Exact: x86 frame/audio hashes over 3000 frames, APU native contract test
(new deferred variant), port scheduling test. SMW: no forced syncs besides
host/frame boundaries; every batch is full. Its driver reads back pitch,
volume and KON/KOF it wrote (~27 reads/frame), never ENVX/OUTX/ENDX.

N64 (ares, cycle counter): DSP cost per sample including replay ~2700-2830
cycles versus ~3500 eager (cold), about -20%. But the whole sound path per
frame is unchanged (~44-45 ms both ways): logging every SPC write and the
replay cost what the warmer cache saves. On the CPU it is break-even; its
purpose is the RSP stage, where the batch leaves the CPU entirely.

### Narrow write tracking (page marks)

At batch start the DSP's reachable ARAM is marked per 256-byte page (bits
in `Spc.dspWatchLog/dspWatchSync`, beside the registers the SPC touches on
every instruction): the echo window, the directory, and per voice 585
bytes (65 BRR blocks, the most a 256-sample batch can decode at pitch
0x3fff) from its decode position and from its sample's start and loop
addresses. SRCN writes and directory writes mark newly reachable data at
that moment; DIR writes sync. Only writes to marked pages are logged
(out of line); reads of echo pages sync.

`SNESRECOMP_APU_DEFER_DSP_VERIFY=1` logs every write and replays each batch
both ways (full log vs page marks), reporting differences. SMW over 3000
frames: 0 differences. Mutation check: with marking disabled it reports
mismatches, so the check is live.

SMW still logs ~750 writes per frame: unused voices keep decoding "samples"
from $0000 (their decode pointer after reset), so the SPC's direct page is
genuinely DSP-read and must be logged.

Speed is unresolved. Timing the APU components on N64 (ares) across
variants: eager ~44-46 ms per frame; deferred with marks in Apu ~45 ms;
deferred with no logging at all (inexact, timing only) ~40-42 ms; deferred
with marks in Spc ~53 ms, where the unchanged DSP replay alone went from
~15 to ~22 ms. Padding Spc in the eager build changed nothing. Component
times swing by 20%+ with unrelated code/data placement, so 1-3 ms effects
cannot be resolved one variable at a time. Deferral stays off.

### Code layout dominates small comparisons

ares models the VR4300's direct-mapped 16 KB I-cache. Inserting only a
padding function among the runtime objects moved the same code's frame
time by ~16 ms (run) and ~7 ms (PPU stage): pad 1 KB 147/88 ms, pad 4 KB
131/81 ms. Hot code is ~80 KB (`_interp_run_core` alone 15.7 KB,
`spc_runOpcode` 16 KB), so whole-frame A/B tests under ~10% are not
meaningful without controlling layout; measure the changed component with
the cycle counter instead. Deliberate hot-function ordering (ld 2.45
`--section-ordering-file`, from profile data) is itself an exact speedup
candidate.

## Instruction-cache placement (2026-11-08)

`n64/profile/icache_order.py` turns a PC profile into an ld
`--section-ordering-file`: it measures heat per 32-byte line, then places
the hottest functions greedily at the 16 KB cache offset where their hot
lines overlap the least heat already placed (gaps are left empty; minor
functions may move at most 2 KB). `.boot` stays first. The Makefile uses
`n64/icache_order.ld` by default (`ICACHE_ORDER=` disables it); it holds
runtime code only (`--runtime-only`), so it does not depend on one game's
generated code.

Profile: 131072 samples at 250 us, SMW attract frames 400-520
(`make PROFILE=1 PROFILE_START=400 PROFILE_FRAMES=120 PROFILE_US=250`).
Modelled conflict cost: 8% of the default link order. 262 functions,
96% of samples, ~450 KB of code and gaps.

ares, same build otherwise, state hashes identical:

| frames | default order | ordered (runtime only) |
|---|---|---|
| 600 | 262 ms | 231 ms (-12%) |
| 720 | 238 ms | 212 ms (-11%) |
| 900 | 269 ms | 235 ms (-13%) |

The CPU stage drops ~20%, the PPU stage ~4%. Placed functions sit at fixed
addresses, so unrelated code changes no longer move them; whole-frame
measurements should be steadier from here. Regenerate after large runtime
changes:

    make PROFILE=1 PROFILE_START=400 PROFILE_FRAMES=120 PROFILE_US=250 ...
    profile/icache_order.py build/snesrecomp_interp.elf \
        build/snesrecomp_interp.map isviewer.log -o icache_order.ld --runtime-only

### Stage-aware ordering (2026-11-09)

The ordering above scored conflicts between all hot code. But a frame runs
its stages one after another (CPU emulation, then the PPU stage, then
present), so code of different stages can share cache sets at no cost;
what matters is conflicts inside a stage. The PPU stage interleaves the
line builder, the PPU's per-line state, HDMA and the raster replay on
every line, so conflicts among those repeat 224 times a frame.

Samples now carry the stage (`g_n64_profile_phase`, written by the host
loop in every build, so profiling builds keep the shipped layout):
"PROF <pc> <stage>". `icache_order.py` counts conflicts per stage
(`--merge-stages` for the old behaviour); `symbolize.py --phase N` shows
one stage.

Regenerated for the current code (SMW attract, frames 400-520, 500 us):
42,748 samples, 61% CPU stage, 33% PPU, 6% present. Modelled same-stage
conflict cost: 14% of the previous ordering's. ares, same source:

| frame | old ordering: run / ppu / present (ms) | stage-aware |
|---|---|---|
| 420 | 98.7 / 57.0 / 10.2 | 86.3 / 50.2 / 8.8 |
| 600 | 99.6 / 56.6 / 10.1 | 87.2 / 49.8 / 8.7 |
| 900 | 98.4 / 56.6 / 10.0 | 85.6 / 49.8 / 8.6 |

About -12% per frame across frames 360-1140.

Memory note: the default 131072-sample buffer (512 KB) no longer fits
beside everything else (the cartridge copy's malloc fails at boot), so
profile with half the buffer at 500 us:

    make PROFILE=1 PROFILE_START=400 PROFILE_FRAMES=120 PROFILE_US=500 \
        EXTRA_CFLAGS=-DN64_PROFILE_MAX=65536 ...
    profile/icache_order.py build/snesrecomp_interp.elf \
        build/snesrecomp_interp.map isviewer.log -o icache_order.ld --runtime-only

Free heap in normal builds is therefore under 1 MB; worth knowing before
adding large buffers (a decoded-tile cache would need its memory found).

## Data-cache placement (2026-11-08)

Tools:
- `n64/profile/ares-dcache-profiler.patch` (apply to the ares checkout and
  rebuild): ares counts data-cache misses per line, evicted/evicting pairs,
  missing PCs and all accesses between `DCPROF_START`/`DCPROF_STOP` lines on
  the IS-Viewer, writing `$ARES_DCPROF`; with `$ARES_DCTRACE` it also writes
  every access as a u32 trace. ares's JIT checks hits inline, so traces need
  `ARES_ARGS="--setting General/ForceInterpreter=true"` (ares_run.sh).
- Build with `EXTRA_CFLAGS=-DN64_DCPROF=400`: logs heap object addresses and
  brackets frames 400-460. The hooks are always compiled in and the define
  only sets two data words, so a profiling build has exactly the shipped
  layout (symbol tables identical). Before 2026-11-09 the hooks' strings
  shifted .rodata by 0x150 bytes in profiling builds only: the profile then
  showed a conflict (PPU state against `spriteSizes`, 16% of misses) that
  the shipped build did not have.
- `profile/dcache_report.py ELF LOG dcprof.txt`: misses by object and region,
  eviction pairs, missing functions.
- `profile/dcache_sim.c`: replays a trace through the 8 KB direct-mapped
  write-back cache, with objects moved or swept over all 512 positions.
  Miss-count or access-count heuristics mispredicted placements; replaying
  the real access order did not.

Changes (all layout only; emulated state hashes identical):
1. Apu: port-queue scalars moved ahead of the 16 KB queue. After it they
   sat exactly 16 KB from the APU control fields, the same cache line, both
   touched on every SPC instruction (12% of all misses).
2. Main loop on its own 256 KB stack whose top is placed at 0x18e0 modulo
   8 KB (`N64_STACK_COLOR`, chosen by trace sweep). libdragon's stack top
   (end of RDRAM, 0 mod 8 KB) was the worst position; the stack held 27% of
   all misses. Sweep range over positions: 1.19M to 3.15M misses per trace.
3. `g_cpu` placed in `.sbss` next to the hot runtime counters
   (`SNESRECOMP_HOT_BSS`, empty by default; the N64 build sets it).
4. PPU-build state: deferred-run fields first, not after the span array.

Misses in frames 400-460: 6.43M -> 3.05M. ares frame time (no profiling
hooks), with the I-cache ordering: 231 -> 217 ms at frame 600; against the
original layout 262 -> 217 ms (-17%), -16% to -18% across frames 360-960.

Not done: placing the large heap objects (APU, DSP, PPU, WRAM) by
allocation offset would save about 3% of misses each in the trace, but the
best offsets depend on the game's sound driver and RAM use and it needs
allocation hooks in the engine. The I-cache ordering predates these changes
(main is now a trampoline to host_main); regenerate it after larger edits.

### Pinned hot statics, and the deferred DSP re-measured

The placement above was fragile: the deferred-DSP build adds a few statics,
which moved two hot interpreter statics onto the stack's lines (49% of all
misses) and made it look 26% slower. `profile/dcache_order.py` now pins
the 47 hottest small statics (18% of static data accesses) in compact
blocks at 8 KB-aligned starts of `.sdata`, `.sbss`, `.data` and `.bss`,
taking consecutive cache positions (`n64/data_order.ld`, joined with
`icache_order.ld` into `build/cache_order.ld`). `__bss_start` still
precedes the pinned BSS blocks. The stack top was re-swept against that
layout (trace sweep): 0x11e0 modulo 8 KB.

Pinned vs the earlier tuned layout: ~1.5% slower (frame 600: 206 -> 209
ms), but no longer dependent on unrelated statics.

Deferred DSP vs eager on the pinned layout (ares, state hashes identical):
+1% to +5%, about +2% on average (frame 600: 209 -> 215 ms). On the CPU,
batching's warmer cache does not pay for write logging and replay. Its
value is as the base for the RSP DSP, which removes the eager DSP's
~18 ms per frame from the CPU.

## S-DSP on the RSP (2026-11-08)

`n64/rspdsp/`: `rsp_snesdsp.S` (rspq overlay), `snesdsp_rsp.c` (glue),
`test/` (test ROM; `make` there, run with `profile/ares_run.sh`).

One command runs a batch: the CPU packs the C `Dsp` into an RSP state block
(per-voice values as 8-lane int16 vectors, decode buffers doubled), the RSP
applies logged writes at their sample tags and runs every sample, the CPU
unpacks and appends the output samples to the ring as `dsp_cycle` would.
Register writes are decoded on the CPU by running the real `dsp_write` on a
shadow and emitting stores of every field it assigns, so `dsp_write` stays
the only definition. Exactness techniques: the Gaussian taps are
`VMUDM(2s, 32g)` (= floor(g*s/1024)), the 16-bit wrap and clamp of the sum
are `VADDC` and `VADD`, gain is `VMUDM(s, gain<<5)`; products C shifts after
32-bit multiplication (`>>6`, `>>7`, FIR taps) come from `VMUDH`+`VSAR`
and are shifted and saturated on the scalar unit in C's order. The C path
handles batches with pitch modulation, the host shadow/output filter, or an
echo buffer touching $0000-$000F or wrapping past $FFFF.

Test (ares): 2000 random batches (random consistent register state via
`dsp_write`, random dynamic state and ARAM, random logs of register and ARAM
writes, up to 1024 samples): all identical to the C DSP in state, ARAM and
every output sample. A planted error (`>>6` -> `>>5` in the mix) fails.

Speed (ares): C `dsp_cycle` 2575 CPU cycles per sample. RSP cycles per
sample with pitches like real music (`make RSPDSP_REAL_PITCH=1` in
`test/`; SMW decodes 0.47 BRR blocks per sample):

| Version | RSP cycles/sample | Share of the RSP at 32 kHz |
|---|---|---|
| First exact version (random pitches) | 3469 | 178% |
| Vector mix/echo, scheduled gather, branch-free BRR | 1651 | 85% |
| Vector envelope (merges), key off as a lane merge | 1329 | 68% |
| Voice-major interpolation (`ldv`/`llv`, paired Gaussian table) | 1136 | 58% |
| Background echo DMA + prefetch, vector FIR sums | 1069 | 55% |
| BRR only for wrapped voices, delay slots filled | 960 | 49% |
| ENVX/OUTX set by the CPU; BRR nibbles shifted by vector, filters inlined | 917 | 47% |

Phase costs are measured by omission (`RSP_DEFS=-DPROF_NO_<A|B|C|D|E|G|LOG>`).
ares's RSP model: a vector result read by the next instruction costs 3
stalls (free after four), a load used next costs 2, each taken branch
or jump 1; one scalar and one vector instruction can issue together.
VCC is not a hazard, so compare/merge pairs are cheap.

BRR: the 16 nibbles of a block are shifted together (`lpv`, then
VMUDM by 2^(shift+3) of the nibble times 4096; invalid shifts keep the
sign times 2048) and a loop per filter adds the prediction, clamps and
clips, with no call per sample. ENVX/OUTX are no longer stored by the
RSP: the CPU sets them from the final gain and output at the join
(`set_envx_outx`), except where a write logged after the last sample
replaced them; the runtime joins before the SPC can read them. IMEM:
0xfa0 of 4 KB used.

### Integration: asynchronous batches (default, `RSPDSP=1`)

The N64 build defers the DSP (`SNESRECOMP_APU_DEFER_DSP`) and hands each
batch to the RSP through `SNESRECOMP_DSP_LAUNCH_HOOK`/`_JOIN_HOOK`
(`runner/src/snes/apu_dsp_batch.h`); `RSPDSP=0` builds the C DSP.
- The RSP works on a private 64 KB view of ARAM: only the pages the batch
  can read (page marks) are copied at launch, through the uncached segment,
  then returned to their batch-start values by undoing the logged writes.
  The SPC keeps running on the live ARAM. At the join the state and output
  come back and the batch's echo bytes are copied into the live ARAM; SPC
  accesses to the echo window, DIR entries and BRR spans of a running batch
  join first (`apu_dspMarkAfterLaunch`).
- Reads of DSP registers other than ENVX/OUTX/ENDX come from a register
  shadow (`Apu.dspRegs`) and do not join.
- Full batches (256 samples) run in the background; the frame-end flushes
  (`apu_dspFrameSync`, `SNESRECOMP_DSP_HOST_ASYNC`) launch the remainder
  but never wait for a running batch: the samples stay owed. Joining there
  slept through the batch launched by the frame's last SPC catch-up
  (6-7 ms per frame). Output reaches the ring when a batch is joined, up to
  about a frame later than the eager DSP.
- The register-write decoding copies only the `Dsp` fields before the
  output ring (the whole struct copy was 9 ms per frame).
- The heap objects sit at other cache positions in this build, so the main
  stack's colour was re-swept for it (`N64_STACK_COLOR` 0x1b40; 0x11e0 for
  `RSPDSP=0`): trace misses 1.65M -> 1.04M.

Exactness: SMW's in-emulator WRAM/ARAM/DSP-output hashes are identical to
the eager C DSP every 60 frames; all samples ran on the RSP.

Speed (ares, frame times run+PPU+present, frames 300-660): 4.5-8.7%
faster than the C DSP build (frame 600: 209 -> 193 ms; run stage 120 ->
103 ms). CPU cost left per frame: launch 1.9 ms (page copies 0.9, decode
0.5, pack 0.4), join 1.5 ms of which 0.9 ms waiting for the RSP.

## CPU-side speed round (2026-11-09)

All exact (state hashes, RDP-vs-software diff 0 px); ares frame times, SMW.

| Change | Frame time |
|---|---|
| Interpreter bridge: diagnostics compiled out (`SNESRECOMP_INTERP_DIAG=0`), bus read fast path a leaf, per-step tests hoisted/reordered, opcode class table | -0.5% to -1.4% (native host instructions -5.2%) |
| RDP renderer sprites: per-line sprite masks rebuilt only on OAM change, 8-pixel decode | -5.4% to -6.2% |
| RDP renderer BG rows: whole rows claimed in the cache, direct unaligned stores | -1.1% |
| Software PPU: counts-only sprite evaluation on lines drawn without pixels | -0.6% |

Frame 600: 193 -> 179 ms. Flag elimination in generated code was measured
first and dropped for now: generated code is under 0.1% of the time
(N/Z writes are 78-80% dead, C/V 40-44%, but rare).

Where interpreted work comes from (1200 frames, 3.78M interpreted
instructions, by the routine and CPU mode containing each PC):

| | Share |
|---|---|
| Inside routines that have a native body for that mode (mostly boot) | 40% |
| Routines with no native body in this selection | 37% |
| No routine at all in the manifest | 13.5% |
| A routine, but not for this mode | 10% |

### Guest fibers (`SNESRECOMP_GUEST_FIBER`, off by default)

Generated code's deadline checks now call `interp_bridge_lle_deadline_pause(cpu, pc)`
(same result as `..._master_deadline_reached` without fibers; all hashes
unchanged). With `SNESRECOMP_GUEST_FIBER=1` the frame driver runs the guest
on its own stack (`runner/src/guest_fiber.c`; MIPS context switch on the
N64, ucontext elsewhere): at a deadline the native code pauses instead of
unwinding, the driver sees exactly what an unwind leaves (resume PC, APU
flush, beam catch-up, bridge nesting state and shadow stack at their
run-start values), and a run at the same PC/S resumes the native code.

Measured on SMW: every pause resumed (125), none unwound, but all 125 are
during boot (decompression). After boot no native code spans a deadline,
interpreted work is unchanged (2,880 instructions per frame), and ares
frame time is +1% (with the guest stack at the tuned cache colour; +15%
without). Off by default; kept as an engine mechanism for titles with long
native stretches. Hashes differ with it on, as they do between any two
native selections: interpreted and native code already disagree on cycle
timing (an interpreter-only build differs from the normal build by
200-2,000 WRAM bytes per frame), so this class of change needs gameplay
validation, not hash equality.

Where interpreted work comes from after boot (frames 300-1200): routines
with no native body in this selection 52%, routines entered by `JMP`
(tail jumps: never bounced to native, no return frame) 29% (e.g.
`$00A2F0: JMP $8494`), code outside any routine 18%.

### Tail jumps into compiled routines

The bridge records the calls it interprets (S after the push, frame size,
return PC). A `JMP`/`JML` (direct or indirect) that lands on a compiled
routine for the live (m,x) runs it compiled when the stack top is a
recorded call's frame holding exactly that call's return address (the
innermost such frame: code like `PLA / PLA / JMP` pops its own data
first). The routine then returns through that call's paired contract, as
a JSR bounce would. Unbalanced returns clear the record.

SMW: 4,117 of 5,119 jumps onto compiled routines (frames 0-1200) now run
compiled; the rest start in a scheduler run whose calls happened in
compiled code (not recorded). Interpreted work after boot: 2,877 -> 2,569
instructions per frame; native host instructions -2.8%; ares frame time
about -1% after the title. No bridge warnings; RDP-vs-software diff 0 px;
the attract demo plays correctly. (Hashes move, as with any change in
what runs compiled.)

### PPU stage: sprite evaluation from a line table

The PPU stage (ares, after the title) measured: build 25 ms, final flush
of BG runs 25 ms, software PPU per-line state 15 ms, HDMA 6 ms. The 15 ms
was the PPU's own sprite evaluation (STAT77 range/time-over flags and
"line has sprites"), which tests all 128 sprites on every line.

`ppu_evaluateSpritesFromLines` (runner/src/snes/ppu.c) answers lines
evaluated without pixels from a table of in-range sprites and visible
8-pixel slivers for all 256 line values, built once per OAM content and
sprite size/interlace setting. Without widescreen margins those two
counts decide everything such a line leaves behind; a line with more than
32 sprites in range (where the rotation start matters) and widescreen
margins use the full evaluation. The table is validated by a copy of OAM
by default; a host whose OAM only changes through the ports can name its
PPU with `ppu_set_oam_write_tracking` to validate by `oamWriteCount`
instead (the N64 host does).

Two N64 traps found on the way: newlib's `memcmp` is byte-wise, and the
data cache is direct-mapped, so the OAM copy has to sit half the cache
away from the live OAM (it is placed at run time) or every compare load
evicts its partner. Even then, reading 544 bytes per line costs ~70
misses because the line builder evicts OAM between lines, hence the
counter opt-in.

Result: per-line state 15.4 -> 3.8 ms; PPU stage about 73 -> 57 ms per
frame. Native state hashes are identical to the full evaluation (it is
exact, not a timing change); host instructions -8.6%.
`tests/ppu/ppu_sprite_lines_test.c` builds ppu.c with
`SNESRECOMP_SPR_LINES_CHECK` (both paths on every line, abort on any
difference) over random and crowded sprite tables in both validation
modes; it catches deliberately injected off-by-one errors.
`SNESRECOMP_SPR_LINES=0` restores the old path.

### Window masks on the RDP; heap colour anchor

Window masks and per-line layer enables are no longer written into the
planes: each span carries `hide`/`hide_sub` layer bits and each line its
`main_layers`, and the compositor (RDP and `profile/rdp_reference.c`)
skips a hidden layer in that span. A BG row is now only ever written whole
by its expansion, and nothing clears BG rows (a row of a layer that is off
on its line is never drawn). This is what lets the RSP own the BG planes.
Sprite rows keep their clears (sprites write rows sparsely). Native
compare over 3,000 frames and ares `RENDER=2`: 0 px; PPU stage -1.3 ms.

That change grew the frame description by 3 KB, which moved every engine
heap object (allocated after it) to a new data-cache colour: the CPU stage
went 87 -> 104 ms at frame 600 with no code change in it. The host now
pads the heap top to `N64_HEAP_COLOR` modulo 8 KB just before `SnesInit`
(`heap_anchor` in n64_host.c; `-DN64_HEAP_COLOR=-1` only logs the
colour). The engine's large objects come from the top; small ones may
fill holes, so the anchor is the top, probed with a 32 KB block that is
freed back into it. 0x1b58 reproduces the previous good layout exactly
(PPU, APU and ROM copy at the same addresses, same frame times). Host
allocations before `SnesInit` no longer move the engine's objects; engine
struct size changes still can (re-tune the colour if a stage jumps).

### Background layers on the RSP (`RSPBG=1`, default)

`n64/rspbg/rsp_bg.S` expands a BG run (one layer, consecutive lines with
the same registers) into plane rows, exactly as `flush_run` does on the
CPU; `RSPBG=0` keeps the CPU expansion. The CPU queues one job per run as
runs close and merges the per-row "has low/high-priority pixels" flags
after waiting at the end of the PPU stage. The RSP owns the BG planes:
nothing on the CPU writes them (window masks and layer enables moved to
the compositor first), the planes' boot-time contents are written back and
dropped from the cache, and a run the CPU must expand (jobs used up) is
written back at once.

Decode: 8 pixels per vector; VMUDN by per-lane powers of two moves each
pixel's bit to bit 15, VAND 0x8000, VMUDL/VMADL by 2^(9+p) assemble the
index in the high byte, SPV stores it. Full tile rows (8 lines) load the
tile's eight row words at once and broadcast one per line, fully unrolled;
the next tile's tilemap entry, attributes and DMA are interleaved with
that decode (`gen_full.py` generates it) so scalar and vector instructions
issue in pairs. Fine scroll is a 16-bit-lane shift through a carry buffer.

Two traps: the VRAM image is the Ppu struct's array, at 4 mod 8 in RDRAM,
and RSP DMA drops the low 3 address bits (reads fetch 8 more bytes and use
the adjusted DMEM pointer); and every DMEM buffer used by DMA or LQV needs
explicit alignment (two bugs here were alignment that had held by luck).

Validation: `n64/rspbg/test` (ROM-free test ROM: random VRAM at the same
misalignment, random runs incl. flips, 2/4 bpp, fine scroll, the SMW
title's sub-BG3 case; 18,312 rows and flags exact) plus timing; in SMW a
check build (`-DN64_RSPBG_CHECK`, CPU re-expands every row) reports 0
differing rows and `RENDER=2` 0 px through frame 1200.

Cost (ares): 224 rows take 3.5 ms (4bpp), 4.4 ms (4bpp with fine
scroll), 2.7 ms (2bpp) of RSP time. SMW attract: PPU stage 49 -> 37 ms,
of which ~11 ms is the CPU waiting for the RSP at the end of the stage.

Pipelined (`N64_PIPELINE`, default with `RSPBG=1`): a frame is drawn after
the next frame's emulation, so the RSP expands its backgrounds while the
CPU emulates. The jobs read a VRAM snapshot that an RSP copy command takes
at the first job of the frame (64 KB through DMEM, ~1 ms of RSP time); the
CPU waits for that copy before emulation may write VRAM again, and VRAM
changing within the PPU stage waits for the queued jobs and snapshots
again. The snapshot sits at the PPU array's address modulo 8, so a plain
DMA copy keeps every offset. Planes and the frame description have one
owner at a time: the frame is drawn (and the RDP finishes) before the next
frame's PPU stage reuses them. Emulation is unchanged; display latency
grows by one frame. SMW attract (ares): PPU stage 37 -> 26 ms, CPU stage
and present unchanged (no stall on audio batches queued behind the jobs),
`RENDER=2` 0 px through frame 1320. Whole frame about 123 ms (was 177 ms
at the start of 2026-11-09).

Open: the CPU stage is ~2 ms slower with `RSPBG=1` than with `RSPBG=0`
(88 vs 86 ms), also unpipelined, so it is layout, not RDRAM contention:
the microcode blob and new statics move 365 static objects to new cache
colours, among them large buffers too big for dcache_order.py's pinning
(the DSP's ARAM view and op log, the builder state). Pinning those six at
their old colours by hand made it worse (95 ms: everything else moved).
A regenerated I-cache ordering models worse than the current one. Needs
the measured data-cache placement pass (profiler and trace simulation)
over the current build, with large objects included.

Done the same day: `data_order.ld` regenerated from a profile of the
current build (145 statics, 51% of static-data accesses; it now pins
`cyclesPerOpcode`, which the stack top had been evicting: 9% of misses),
then the heap anchor and stack colour swept over a recorded trace with
`dcache_sim` (heap +0xd0 -> `N64_HEAP_COLOR` 0x1998; stack 0x1820). The
heap anchor had silently stopped working: its 32 KB probe allocation was
served from a free hole, so it measured the wrong place. It now reads the
top chunk from `mallinfo().keepcost` below `sbrk(0)`, and grows its pad in
8 KB steps until the pad really comes from the top (a small pad also fit
a hole). ares, SMW attract: CPU stage 88 -> 86 ms (as with `RSPBG=0`),
PPU stage 26 -> 23 ms; frame about 118 ms.

### Per-line palette and window-span caches

Measured inside the line builder (ares, SMW attract): palettes and
backdrop colours 7.8 ms, window spans 4.3 ms, sprites 4.0 ms, BG queueing
2.8 ms per frame, plus HDMA 3.0 and the PPU's per-line state 1.6. The
first two are pure functions of a few registers that rarely change from
one line to the next, so `line_palettes` reuses the previous line's
palettes and fills when CGRAM (write counter), brightness, backdrop and
fixed colours and the colour-math settings are unchanged, and
`line_spans` reuses the previous spans when the window registers are.
Palettes 7.8 -> 2.5 ms, spans 4.3 -> 2.9 ms; PPU stage 23 -> 17 ms, frame
about 112 ms. Native compare (3,000 frames) and ares `RENDER=2`: 0 px.

Layout trap, twice in one change: (1) `n64_ppu_build_line` grew past its
slot in `icache_order.ld` (`. = MAX(., addr)` then pushes every later
placed function: here `bridge_bus_read` and `interp816_runOpcode`, CPU
stage +5 ms), so the new code lives in out-of-line helpers the ordering
does not list; (2) the caches as new statics moved every later static by
0x90, so they live in space the builder state already had (the per-plane
dirty bits now cover only sprite planes; a union keeps the struct's size,
with a static assert). Check `nm` positions modulo 16 KB / 8 KB against
the tuned build before timing a change.

### Reserved I-cache slices: measured, not adopted; placement check

`icache_order.py --slice N` reserves the first N bytes of every 16 KB
window of the placed region for the densest hot functions (nothing else
placed maps there). Profile of the current build: the hottest 2 KB of
runtime code lines take 40% of samples, 4 KB 56%. Modelled conflict cost:
plain regeneration 61% of the current ordering, 2 KB slice 210%, 4 KB
330%. ares (frame 540, CPU/PPU/present ms): current 86/17/9, regenerated
84/18/9 (a wash, not adopted), 2 KB slice 88/20/9 (about +6 ms per frame).
Each stage's hot code is hundreds of KB, far beyond the 16 KB cache, so
walling off 2 KB for a few functions makes everything else conflict more.
Off by default; kept for other workloads (a small hot core).

What the slices were meant to fix, that a growing function shifts the
tuned positions of the hottest ones, is now caught instead:
`profile/check_placement.py` runs after every link (Makefile) and warns
when any function the ordering places is not at its planned address,
naming the first one. It found one more in the committed build
(`n64_ppu_draw_and_show` had outgrown its slot by 0x40; its RENDER=2-only
compare routine is now out of line).

### PPU stage: tried, no measurable value (not adopted)

With the line caches in, both hit on 223 of 224 lines. PC samples of the
PPU stage then put `memset` first (16%) and palette building (`get_tlut`,
`math_color`) at 11%, from the first line of each frame. Tried, each
exact (native compare 0 px), each reverted because ares frame averages
over frames 420-1140 did not move (PPU stage 18.2 ms before and after):
- palettes kept across frames (the pool restarted only when half used);
- sprite scratch buffers and sprite-plane rows cleared only over the x
  range the sprites covered (a `--wrap=memset` census of the PPU stage:
  ~50 KB cleared per frame, 27 KB of it here).
The sample shares overstate these: the writes miss in the cache whether
or not a memset precedes them. Use stage timings, not sample shares, to
pick PPU-stage work.

Heap trap found on the way: a small host allocation made before
`SnesInit` can take a free hole that an engine object (the `Snes` struct)
would otherwise use; that object then comes from the top and every engine
object after it moves (CPU stage 86 -> 96 ms). The anchor fixes the top,
not the holes: allocate host buffers before `SnesInit` only if they are
large, else lazily after it.

SPC700 timer-wait skip (2026-11-10, `SNESRECOMP_SPC_WAIT_SKIP`, on unless
SPC diagnostics are built; `=0` restores the plain loop): a sound driver
polling a timer counter (`MOV A/X/Y, $FD-$FF` then `BEQ` back to it) is
skipped in closed form up to the pass that sees the counter change, capped
by the next queued port write and the requested clocks, with the DSP's
32-cycle edges replayed. SMW's `$0549` wait was 31% of SPC instructions.
Native state hashes and audio over 600 frames are identical to the plain
loop; native instructions -2.6% (SPC opcodes 200M -> 135M Ir). On the N64
the gain is inside layout noise (about +-2 ms per change).

Rejected (2026-11-10, no value): packing the quiescent detector's record
(88 -> 64 bytes, two-multiply hash, no memset) and skipping the dense-only
`idle_cost` ring writes under `SNESRECOMP_QUIESCENT_BACKEDGE`. Hashes were
identical and native instructions -0.4%, but the N64 samples in the record
block did not move (848 -> 831 of ~4,300 in `_interp_run_core`). The samples
fall on the first use of loaded values and on the ring store, so they are
D-cache misses caused by the 4 KB rotating ring's footprint, not by its
record size. `_interp_run_core` is ~19% of the CPU stage on the N64 but ~10%
of native instructions: its cost is cache misses spread over the step, not
any single statement.

See [LESSONS.md](LESSONS.md) for ideas collected from other projects
(sodium64) and design notes, with how they apply here.

## ROM and compiled-code working sets (2026-11-10)

Measured natively over 3,000 frames of SMW's attract demo with
`n64/profile/romtrace/` (build.sh, then analyze_rom.py / analyze_code.py).
The ROM copy is moved to an mmap'd region and made unreadable, so every
reader (interpreter, generated code, fast map, DMA) faults once per 4 KB
page per frame and is recorded; generated code is built with
`-finstrument-functions` to record the compiled functions entered per frame,
sized from the MIPS objects. Audio hashes are unchanged by the tracing.

ROM (512 KB):

| | |
|---|---|
| Code bytes (recompiler manifest ranges) | 139 KB (27%) |
| Never read in 3,000 frames | 256 KB (half) |
| Pages read per frame | median 21 (84 KB), max 26 |
| Pages read per 60-frame window | max 33 |
| LRU page cache of 96 / 128 / 192 KB, loads after boot | 140-1,113 / 47-49 / 10-11 pages |

No DMA reads ROM directly: SMW decompresses graphics into WRAM, and its
assets are read by the CPU (`cpu_read8` in the decompressor) and by the SPC
upload. Page readers: generated code through `cpu_read8/16`, the
interpreter, and `RtlUploadSpcImageFromDpInternal` (sound).

Compiled code (MIPS -Os):

| | Current native set (gen_fib) | Everything compilable (gen_full) |
|---|---|---|
| Compiled code in the build | 2.46 MB | 14.65 MB (with LLE stubs) |
| Functions run in 3,000 frames | 277 = 1.14 MB | 376 = 2.15 MB |
| Distinct code run per frame | median 308 KB, max 397 KB | median 721 KB, max 912 KB |
| Per 60-frame window | max 479 KB | max 1,093 KB |
| LRU cache 512 KB: loads after boot / worst frame | 474 (1.96 MB) / 109 KB | thrashes |
| LRU cache 1 MB / 1.5 MB / 2 MB | | 686 / 426 / 304 loads (3.8 / 2.4 / 1.7 MB), worst frame 226 / 124 / 124 KB |

With lean emission (`SNESRECOMP_EMIT_LEAN`, 2026-11-10), the same 3,000
frames run the same routines (all 2,971 traced frames have identical routine
sets), so only sizes change:

| Everything compilable (gen_full) | Default emission | Lean emission |
|---|---|---|
| Compiled code (with LLE stubs) | 14.89 MB | 7.34 MB |
| Routines run in 3,000 frames | 2.15 MB | 1.01 MB |
| Distinct code run per frame | median 721 KB, max 912 KB | median 344 KB, max 428 KB |
| Per 60-frame window | max 1,093 KB | max 508 KB |
| LRU 512 KB: loads after boot / worst frame | thrashes | 618 (1.6 MB) / 106 KB |
| LRU 768 KB | thrashes | 402 (1.07 MB) / 57 KB |
| LRU 1 MB | 686 (3.8 MB) / 226 KB | 184 (0.48 MB) / 57 KB |
| LRU 1.5 MB | 426 (2.4 MB) / 124 KB | 184 (0.48 MB) / 57 KB |

At 1 MB the lean cache only takes compulsory loads (each routine the demo
uses, once). A 768 KB lean cache does better than a 1.5 MB default one.

Real gameplay (2026-11-10). TASVideos movie #6738 ("all 96 exits, Lunar
Dragon", BizHawk 2.9.1, bsnes v115 core, same ROM SHA-1) replayed in a
local, unmodified-except-as-noted bsnes v115 libretro build (scratch only:
memory accessors, a per-opcode trace hook, and raw D-pad, because bsnes
clears Up+Down/Left+Right and BizHawk's fork does not; the movie presses
Left+Right on 5,449 frames). It stays in sync for 84,000 frames (23 min,
29 exits, 9 saves) and then drifts in translevel $31. bsnes reported, per
frame and in order, which compiled-routine entry points it executed; sizes
are the lean MIPS objects:

| 23 min of play, everything compilable, lean | |
|---|---|
| Routines used | 1,382 of 3,432 (3.38 MB of 6.86 MB) |
| Code run per frame | median 399 KB, max 676 KB |
| Per 1 s / 10 s / 1 min window, max | 843 KB / 1.29 MB / 1.68 MB |

| LRU cache | Loads in 23 min | Frames with loads | Worst frame |
|---|---|---|---|
| 768 KB | 18,631 (50 MB) | 4,148 | 350 KB |
| 1 MB | 11,748 (31 MB) | 3,107 | 303 KB |
| 1.5 MB | 4,495 (11 MB) | 1,673 | 89 KB |
| 2 MB | 1,943 (4.5 MB) | 838 | 70 KB |

So the attract demo (1.01 MB used) under-sizes the cache by 3x: real play
wants about 1.5 MB, still well under today's 2.46 MB resident selection
(default emission) while covering every compilable routine.

Code cache design options, simulated (2026-11-10) on the full routine-entry
sequence bsnes executed over two 100 s stretches of the TAS (frames
30,000-36,000 and 60,000-66,000; 267 and 197 compiled-routine entries per
frame), caches warmed with the preceding play, lean MIPS sizes. A: software
cache at routine granularity (calls through a routine table, relocating
loads). B: compiled code linked at fixed virtual addresses and paged by
TLB misses (no relocation, no call table). Assumed costs: refill 600
cycles through libdragon's exception path or 50 with a dedicated refill
vector, cartridge DMA 5 MB/s, 100 us fixed per A load.

| 1.5 MB of code in RAM | Loads, avg per frame | Worst frame | Refills per frame |
|---|---|---|---|
| A, LRU | 0.03-0.04 ms | 8.5-9 ms | - (table calls ~0.01 ms) |
| A, FIFO | 0.03-0.05 ms | 16-18 ms | - |
| B, address order, 4 KB pages, 16-32 TLB entries | 0.08-0.22 ms | 25-83 ms | 81-132: 0.5-0.85 ms slow, ~0.05 ms fast |
| B, hot-first (profile) order, 16 KB pages | 0.14-0.32 ms | 36-59 ms | 1-20: up to 0.13 ms slow |
| B, 64 KB pages | 2.4-360 ms | up to 700 ms | ~0 |

B's refill traffic is modest (a few hundred entries per frame, ~100 pages)
but page-granular residency drags cold neighbours into RAM, so it moves
several times A's bytes and its worst frames are 3-9x A's; it gets close to
A only with a profile-ordered layout. It also competes with ROM paging for
the 32 TLB entries. The simulation counts routine entries only (returns
into a caller touch its page again, so B's refills are a lower bound) and
assumes an entered routine touches all its pages (an overestimate for large
routines).

Our timing against bsnes: generated with `--no-hle` (the SPC upload runs
as game code) the title screen accepts input at frame 334 against bsnes's
330; with the HLE upload, ~75 frames earlier. Replaying the movie needs
`SNESRECOMP_ALLOW_OPPOSING_DIRECTIONS=1` (profile host) and the same raw
D-pad in any reference.

Reading: a 128 KB ROM page cache holds SMW's ROM working set (the full
copy only matters for ROMs larger than RAM), and a 1.5 MB code cache in
RDRAM backed by all compiled code in cartridge space holds the full
compiled set's working set, about 1 MB less RAM than today's 2.46 MB
selection while covering every compilable routine without a per-ROM
capture. A miss need not stall: the interpreter already runs any routine
exactly, so it can run the routine while the DMA completes (selection then
varies at run time, the same validation class as guest fibers). Function
granularity is coarse: a frame enters 700 KB of compiled functions but runs
far less of each.

## Lean emitter (`SNESRECOMP_EMIT_LEAN`, 2026-11-10)

Where the generated MIPS code goes (SMW's native set, 2.58 MB of `.text`),
measured by rewriting the generated C and compiling with the N64 flags:

| Change | `.text` | Exact |
|---|---|---|
| Per-block watchdog + deadline as one call, one shared yield exit per function | -6.0% | yes |
| Void-alias leak check: shared fatal routine instead of fprintf/abort | -1.3% | yes |
| Mirrored-body code speed from `g_code_speed[PB >> 7]` | -2.5% | yes |
| Speed as a constant 8 (ceiling; needs the speed known statically) | -10.3% | no |
| All N/Z flag writes removed (ceiling for dead-flag elimination) | -3.1% | no |

`SNESRECOMP_EMIT_LEAN=1` (v2_emit environment, off by default, in the
generator digest via `EMIT_SWITCHES`) emits the three exact rows:

- `recomp_block_enter(cpu, pc16)` = `WatchdogCheck()` then
  `interp_bridge_lle_deadline_pause(cpu, PB:pc16)`, same order; a pause
  stores the PC and jumps to the function's one `_lle_yield` exit
  (`RecompStackPopYield` + `interp_bridge_lle_yield_unwind(PB:pc)`). SA-1
  builds keep the watchdog inline (their mapping check sits between).
- `g_code_speed[2]` (cpu_state.h) is kept by common_rtl.c at both places
  `g_memsel` changes ($420D write, snapshot resync).
- `recomp_void_alias_leak(r, name)` prints the same message and aborts.

SMW, current native set: 2,577,336 -> 2,342,866 bytes (-9.1%, -229 KB).
Native state and audio hashes over 1,200 frames identical to the default
emission. tests/v2 passes in both modes (one failure, test_disconnect_
diagnostics, predates this); tests/v2/test_emit_lean.py runs a lean
function with the deadline forced at each block, and
test_continuation_live_pb runs the table for bank $80 with MEMSEL off and on.
ares frame time unchanged within layout noise (generated code is under 1%
of the frame): the gain is RAM, and a smaller working set for a code cache.

Dead-flag elimination is capped at about 3% (2.5% at the measured 80% dead
rate), far below the 25% the per-block sequence was first guessed at.

Memory-access sites (same switch, `recompiler/v2/lean_rewrite.py` and
`runner/src/cpu_lean_access.h`): about 15,000 calls to `cpu_read8/16` and
`cpu_write8/16` each spelled out their addressing mode. In lean mode a
rewrite pass turns the emitter's exact text for each mode into a helper
that makes the same bus call with the same arithmetic, so a site passes
only its constant: direct page (`cpu_read_dp8`), dp,X, absolute,X/,Y with
bank carry, long,X, push/pull (`cpu_push8`/`cpu_pull8`), and the prologue's
return-frame read (`cpu_host_return_pc24`, emitted directly). Left inline:
writes whose value contains a call, and the two pointer reads of
`(dp)`/`(dp),Y`, since C leaves the order of calls in one expression open.
Absolute (DB, k) was measured at -0.2% and left out.

| SMW native set, MIPS `.text` | bytes | vs default |
|---|---|---|
| Default emission | 2,577,336 | |
| Lean, block checks / speed / leak | 2,342,866 | -9.1% |
| Lean, plus memory-access helpers | 2,097,386 | -18.6% |

Native state and audio hashes over 1,200 frames identical to the default
emission; test_emit_lean.py runs one routine through every rewritten mode
emitted both ways and compares RAM and the whole CpuState; tests/v2 and the
runtime C tests pass as before. ares frame time unchanged within noise
(90.7 / 17.8 / 9.7 ms).

Fixed sequences (same switch; the remaining lean output attributed per
generated-C line, then each sequence moved into cpu_lean_access.h in the
order the emitter writes it):

- block entry: with no entry hook, `recomp_block_enter_charge(cpu, pc, n)`
  is the watchdog, the deadline test, then (no yield) the coprocessor clock
  store and the block's static charge (`_c` form for fixed-speed regions);
- `cpu_charge(cpu, n)` for branch-taken and other dynamic charges,
  `cpu_charge_dl` (D.l != 0 penalty), `cpu_charge_page` (index page cross);
- `cpu_sync_p_nz` (P's N/Z bits after a flag update);
- prologue: `recomp_enter(name)` and `recomp_frame(...)` (entry S, tail-call
  context, host return frame, entry-S record); the debug deny gate keeps the
  written-out prologue.

| SMW native set, MIPS `.text` | bytes | vs default |
|---|---|---|
| Lean, plus memory-access helpers | 2,097,386 | -18.6% |
| Lean, plus fixed-sequence helpers | 1,549,754 | -39.9% |

Same function set; native state and audio hashes identical to default
emission; the differential test now runs PB $00 and the $80 mirror with
MEMSEL off and on, through D.l, page-cross and branch-taken charges and
PHP/PLP; tests/v2 and the runtime C tests as before; ares 87.9 / 18.4 /
9.8 ms.

RTS/RTL returns (same switch): each site pops its frame and tests the
paired, balanced host return inline; every other outcome (rewritten return
address, return to an ancestor, callee-cleaned frame, interpreter-owned
caller, paired bounce, computed return through a pushed frame, dispatch)
is `recomp_return_slow` in cpu_lean_access.h, the default tree test for
test with the same recomp-stack pop before each exit. The routine-specific
HLE return forms and RTI are unchanged. On SMW (600 frames) the slow path
ran 54 times (52 returns to an ancestor, 2 dispatches); every other return
stayed inline. test_emit_lean.py sweeps nine RTS/RTL stack shapes against
every runtime answer (1,152 cases, every exit taken) and compares the exit
called, its arguments, S and the pop kind; it catches a swapped pop kind or
an RTS frame size on an RTL.

| SMW native set, MIPS `.text` | bytes | vs default |
|---|---|---|
| Lean, plus fixed-sequence helpers | 1,549,754 | -39.9% |
| Lean, plus out-of-line return tree | 1,285,270 | -50.1% |

Hashes identical; tests/v2 and the runtime C tests as before; ares 90.1 /
18.7 / 9.8 ms. Still to consider: decimal-mode ADC/SBC (about 4.5% before
this step).

## ROM paging (`ROMPAGING=1`, 2026-11-10)

The cartridge no longer needs the SNES ROM in RAM. `n64_platform_rom_window`
returns a read-only window at `0xC0000000` (kseg2, always TLB-translated in
kernel mode) over the ROM file in the cartridge filesystem. Each of
`ROMPAGE_SLOTS` 8 KB-aligned RAM slots (default 24 = 192 KB) is one TLB
entry (an even/odd 4 KB pair); a load miss in the window reaches the
handler registered with libdragon's `register_exception_handler`, which
DMAs the page from the cartridge into the oldest slot (FIFO), rewrites that
entry and returns to the faulting instruction. Other exceptions go to the
previous handler; a write into the window (entries are not writable) is a
crash, as it should be. Slots and window pages share data-cache index bits
12:4, so invalidating the slot through kseg0 before the DMA clears the old
page's lines. All 32 entries start invalid at distinct kseg0 addresses
(two matching entries would shut the TLB down). libdragon uses no TLB
entries itself (only its crash inspector reads them).

Runtime side (shared, game-agnostic): `cart_set_rom_provider` lets a host
supply the cartridge ROM. `snes_loadRom` offers it power-of-two images
(the bus image is then the bytes themselves) and `cart_load_external` uses
the pointer as is, never copying or freeing it; other sizes keep the copy.
The window works for any reader that holds a pointer, enhancement-chip
emulation included.

The profile host emulates the window with mmap and SIGSEGV (same page size,
slot count and FIFO), so exactness and miss counts are checked natively:
state and audio hashes over 1,200 frames identical to plain loading at 8,
16, 24 and 32 slots. Pages copied in over 3,000 frames of the attract demo
(64-page ROM, the boot-time identity hash reads all of it once): 8 slots
32,147, 16 slots 760, 24 slots 132, 32 slots 103.

ares, SMW, lean native set: heap in use 2.43 -> 2.09 MB, free 2.77 ->
3.09 MB, and free memory trapped in holes 1.05 MB -> 600 bytes (the three
boot-time ROM copies -- file buffer, power-of-two copy, cartridge copy --
are gone), so the free heap is one contiguous 3.09 MB block. 102 pages
copied in by frame 1,200. Frame time with a regenerated I-cache order
93.2 / 18.1 / 10.8 ms against 90.1 / 18.7 / 9.8 without paging (+3.5 ms in
one sample, run stage; misses are too rare to account for it, so either
layout or a per-access cost of mapped reads -- to be re-measured). Off by
default until it has run on hardware.

## Code cache, steps 1-2: routine records and trampolines (2026-11-10)

`SNESRECOMP_EMIT_CODE_CACHE=1` (with `SNESRECOMP_EMIT_LEAN=1`; in the
generator digest) emits every compiled routine F as `F__body`, and
dispatch_v2.c defines F as a resident trampoline,
`RecompReturn F(CpuState *cpu) { return recomp_call(&F__rt, cpu); }`, plus
`g_recomp_routines[]`. Callers, the dispatch table and host wrappers keep
calling F, so nothing else changes. runner/src/recomp_cache.[ch] manage
residency: a call to an absent routine loads it, evicting the least recently
used routines that are not running (a per-routine count of calls in progress
pins them) until it fits the budget; if only running routines are left, the
load goes over budget and is counted. The loader is a host hook; by default
the body is linked in and loading only makes it reachable, which is how the
profile host emulates the cache (`N64_PROFILE_CODE_CACHE=<bytes>`,
`N64_PROFILE_CODE_SIZES=<name size file>`, stats every 600 frames).

SMW, every compilable routine (lean): native state and audio hashes over
1,200 frames identical to the same set without the cache, unlimited and at
1.5 MB, 512 KB and 128 KB (27,529 evictions in 600 frames). The attract
demo loads 281 routines (791 KB) and evicts none at 1.5 MB. Step 3 is the
N64 loader: bodies packed outside the main image and loaded from the
cartridge on a miss.

## Code cache, step 3: routine modules on the N64 (`CODECACHE=1`, 2026-11-10)

`make CODECACHE=1 GEN_DIR=<tree generated with SNESRECOMP_EMIT_LEAN=1
SNESRECOMP_EMIT_CODE_CACHE=1>` runs tools/n64/split_code_cache.py at parse
time: each compiled routine body becomes rom:/cc/<name>.dso (a libdragon
DSO, -G 0), and the main program keeps the runtime, the host wrappers and
dispatch_v2.c (records built with RECOMP_CACHE_EXTERNAL_BODIES, so they do
not refer to bodies). The main ELF links against the modules' .externs and
ships the .msym that dlopen resolves imports with; tools/n64/code_cache_
sizes.py turns the module objects into the size table the cache budgets
with. The host loader (n64_host.c) dlopens the module, looks up F__body and
dlcloses on eviction; budget `CODECACHE_BYTES` (default 1.5 MB). Bodies take
their diagnostic name from the resident record (`recomp_enter_rt`), so no
pointer into an evicted module outlives it.

SMW in ares, all 3,432 compilable routines, ROM paging on: main ELF text
1.19 MB (2.12 MB with the 709-routine selection linked in); the attract
demo loads 372 routines (1,067 KB) by frame 1,800 and evicts none; free
heap 2.14 MB contiguous; run stage 85.6-88.7 ms (no regenerated I-cache
order). With a 256 KB budget: 27,192 loads and 27,122 evictions in 900
frames, no crash, never over budget (running routines are never evicted).

The cost is the load: 2.6 ms on average, 56 ms worst (first load, when the
main symbol table is read). dlopen does two filesystem lookups (~235 us
each: the module and its .sym) and reads modules that average 10 KB
uncompressed for ~2.2 KB of code -- the rest is symbol and relocation
tables, because every runtime call and global access in a body is an
import. Uncompressed modules load slower (2.85 ms): the cartridge read is
the cost. Next: a custom module format with imports resolved at build time
against the linked main program (jal reaches the whole kseg0 segment), only
module-internal relocations kept, one packed file with an index read at
boot -- a load becomes one DMA of ~2 KB plus a short relocation pass.

### Code cache: own module format (2026-11-10)

dlopen replaced. tools/n64/code_cache_externs.py lists every symbol the
module objects import as EXTERN() for the main link; tools/n64/
pack_code_cache.py links each module alone at 0x80000000 (n64/cc_module.ld;
the base is written sign-extended, as the 64-bit linker sees the runtime's
kseg0 symbols, or jal to them does not fit) with --just-symbols=<main ELF>
and --emit-relocs, so every import is already its absolute address and only
module-internal relocations remain (R_MIPS_32, 26, HI16 with its paired
LO16, LO16; GPREL16 references resolve against the runtime's _gp, which $gp
holds at run time). All modules go into rom:/cc.pack in g_recomp_routines
order with an index the host reads once at boot for the sizes. A load
(n64_platform_codepack_load) is: read the 24-byte index entry, allocate,
one DMA of the image, zero the bss, add (base - 0x80000000) at each
relocation, write back the data cache and invalidate the instruction cache.

SMW: 3,432 modules, 7.16 MB in memory, 40,884 relocations in all (about 12
per module, at most 242); pack 7.57 MB (the DSOs were 15 MB compressed,
34 MB not); ROM 8.8 MB. ares: loads 0.73 ms on average, 4.6 ms at worst
(2.6 / 56 ms with dlopen); the attract demo loads 372 routines (1,022 KB);
run stage 83-89 ms. A 256 KB budget: 27,167 loads and 27,093 evictions,
no crash, run stage 200 ms (376 with dlopen). At ~2.7 KB per load the time
is now the cartridge DMA (~3.7 MB/s in ares).

### Code cache: background loading, experimental (2026-11-10)

`make CODECACHE_ASYNC=1` (default 0). A call to an absent routine starts its
DMA (n64_platform_codepack_begin: index entry, relocations, then
dma_read_async of the image) and runs the routine in the interpreter this
time; recomp_cache_poll() each frame, or the next absent call, finishes the
load (relocate, write back, invalidate) and sets rt->fn. One load in flight;
it is pinned while it loads. The fallback (runner/src/recomp_cache_interp.c)
runs the body's prologue -- frame, entry S with the tail-call context --
pops the frame as the AOT deny gate does and interprets from the entry PC
(tools/n64/split_code_cache.py writes each routine's entry PC and entry-S
offset into g_recomp_fallback). A guest return past the routine's own frame
resolves into the SKIP_N its RTS/RTL would return
(cpu_resolve_callee_post_return_skip, interp_tier_dispatch_routine).

ares, attract demo: the CPU cost per load falls from 0.73 ms to 0.27 ms (the
DMA overlaps emulation), 372 routines interpreted once while loading, run
stage unchanged (85 ms). Image lengths are rounded to whole cache lines:
libdragon's data_cache_hit_invalidate asserts on a partial line.

Why it is not the default: SMW timing changes (boot reaches its first game
mode 66 frames earlier, much as with the HLE SPC upload), so the TAS replay
cannot validate it; stress runs of the attract demo on the native host
(N64_PROFILE_CODE_ASYNC=<frames in flight>, N64_PROFILE_CODE_ASYNC_IDS=lo:hi
to bisect, -DN64_PROFILE_CC_ASYNC=<split_code_cache.py --fallback-only table>)
found three return-contract cases. Fixed: a pointer-table call that returns
to its caller's caller (CheckWhichControllersArePluggedIn: the interpreter
returned NORMAL where the body returns SKIP_1), and GetDrawInfo's PLA/PLA/RTS
inside an interpreted SubSprGfx2Entry1 (keeping the routine's frame during
interpretation counted the interpreter's scope frame in the unwind depth).
Open: GameMode08_FileSelect interpreted, its compiled callee
HandleMenuCursor_Main_ReturnsTwice returns out of FileSelect; the bridge
hands that return to the outermost interpreter (interp_bridge_return_
targets_owner), which unwinds through compiled callers (LLE unwind) instead
of returning from FileSelect. The deny gate has the same gap.

I-cache order regenerated for the code-cache main program (1.15 MB text):
modelled conflict cost 27% lower, measured run stage 85.8 ms against
85.0 ms with the current order, so the current order stays.

### Hot kernel, hot/cold splitting, and libdragon's auto-var-init (2026-11-10)

Hot kernel per stage (attract demo, distinct 32-byte lines holding 90% of PC
samples): 65816 emulation 17.9 KB, PPU 3.2 KB, present 2.2 KB. Whole
functions holding them: 68 KB, 21 KB, 10 KB. The emulation stage's hot lines
sit in three ~14 KB functions (_interp_run_core, the SPC700 and 65816 opcode
switches) that keep 90% of their own samples in 10-13% of their bytes.
Generated code is only 5.6% of that stage's samples.

Rejected, no value:
- Module block reordering (-freorder-blocks-algorithm=stc, with and without
  partitioning, modules only): 82.26 vs 82.47 ms.
- Cold labels on the never-sampled opcode cases (196 SPC700, 201 65816):
  time in both switches unchanged (1657 vs 1627, 393 vs 396 samples); run
  stage moves only with layout. Moving cold cases does not reduce how many
  hot lines a switch touches. (GCC accepts `cold` on an ordinary label after
  a case label; partitioning does not split functions on this target.)

The real cost: memset was 9% of the emulation stage. -DN64_PROFILE_RA (the
interrupted ra next to each PC sample; libdragon's interrupt_exception_frame
is the sp, the register block sits 32 bytes above it) traced it to
_interp_run_core's entry: libdragon's n64.mk compiles everything with
-ftrivial-auto-var-init=pattern, ~7 KB of fills per interpreter entry (the
quiescent detector's 64-state history, trace rings), ~100 entries a frame,
plus every generated body with a stack array. The Makefile now passes
-ftrivial-auto-var-init=uninitialized (AUTOVARINIT=pattern restores it) and
the interpreter no longer clears the detector history it never reads before
writing (qring; its buckets only for quiescent runs). Each alone showed
nothing, the other's clear still standing: run stage 82.5 -> 69.7 ms (-15%),
code-cache modules 7.16 -> 7.10 MB. SNES state hashes (WRAM, ARAM, DSP out)
identical in ares over frames 0-1800; native TAS replay identical over
90,000 frames.

### I-cache: measured misses and trace-driven placement (2026-11-11)

Layout work is now judged in I-cache misses, not milliseconds (layout moves
frame time by +-2-3 ms either way). Tools, n64/profile:
- ares-icache-profiler.patch: ares counts I-cache misses per 32-byte code
  line and frame stage, which line evicted which, and (ARES_ICTRACE) records
  the sequence of code lines entered, windowed by the host's
  DCPROF_START/STOP markers. ares models the cache in its JIT too (48 cycles
  a miss); the patch hooks the JIT's inline refill and line checks.
- icsim/icsim.c replays a trace through the 16 KB direct-mapped cache, under
  any layout (a remap of function addresses): SMW 2,492,765 emulation-stage
  misses against ares's 2,492,753, in 0.1 s. icfa.c gives the fully
  associative floor; icreport.py ranks functions and eviction pairs.
- icache_trg.py: placement from a temporal relationship graph over 256-byte
  chunks (Gloy & Smith 1999), built by icsim/trg.c from the trace; graphs
  of several builds (games) merge by function name, each weighted equally;
  --gap-cost trades RAM for misses; --seed restarts the greedy order;
  --remap writes the planned layout for icsim, so candidates are scored
  offline. The greedy placer is noisy between restarts, so: generate
  several, score each game in icsim, build the best, confirm in ares.

g_n64_profile_phase is now set through n64_platform_set_phase, which writes
its line back: the profiler reads RDRAM, and the write-back D-cache made
stage attribution depend on when that line happened to be flushed.

SMW attract demo, frames 400-460, before: 47,000 misses a frame (emulation
41,400, about 21 ms of stalls at 48 cycles; PPU 4,600; present 1,300).
Fully associative floor: emulation 1,234,068 vs 2,492,765 (60 frames) --
about half are conflict misses, the rest is capacity. Mutual eviction
pairs (apu_runCycles_native/spc_readOpcodeWord 2,570 a frame,
_interp_run_core/interp816_doOpcode, dma_doHdma/memset, ...) and
code-cache modules evicting each other (6,300 a frame over 6,900 lines)
dominated.

New icache_order.ld, from SMW and Pitfall (interpreter-only beyond its
static coverage) traces, --gap-cost 0.5 --seed 4, chosen among restarts by
simulated misses on both games, confirmed in ares:
- SMW: 2,881,714 -> 1,879,685 misses (-35%; emulation -34%, PPU -59%,
  present -7%); run stage 69.7 -> 62.8 ms, PPU 16.8 -> 15.3 ms.
- Pitfall: 33,706,446 -> 9,404,642 (-72%).
- .text 1,149,572 -> 1,157,724 bytes (the old order carried gaps too).
The other restart kept (seed 2, gap cost 1) gave SMW -30%, Pitfall -81%.

Other games (static seed configs only: Pitfall 364 routines, Joe & Mac 2
52, Tetris Attack 42; most code interpreted until a coverage run):
Pitfall boots and runs, slowly. Joe & Mac 2 sets BGMODE's 16x16-tile bits,
which a stale assert in ppu_write rejected (removed; the PPU and the N64
renderer's fallback handle large tiles). Tetris Attack: an interrupt
handler's wait loop hits the interpreter step cap during boot and the guest
reaches BRK at $B6:7C09 -- natively too, so not N64-specific; open.
Also fixed: builds without CODECACHE failed to compile (recomp_cache_poll).

### Interpreter kernel: compact cores (2026-11-11)

Goal: an interpreter whose per-instruction code fits the 16 KB I-cache by
construction, so it can be given its own cache sets (no per-game layout).

65816 core (interp816.c) and SPC700 core (spc.c), both LakeSnes-style: one
switch case per opcode, each calling an addressing-mode helper and an
operation helper, all inlined -- the 65816's ORA was compiled 15 times. Now:
- opcodes that are exactly "addressing mode, then operation" (65816: 177 of
  256, 20 modes x 24 operations; SPC700: 127, single and two-operand forms)
  dispatch through generated 256-entry tables and two small switches; the
  other cases are unchanged, as are the helpers and the order they run in.
- SNESRECOMP_COMPACT_INTERP=1 (the N64 Makefile sets it) compiles both files
  for size (#pragma GCC optimize "Os"); other hosts keep their flags.
Sizes (N64): interp816.c 25,324 -> 9,976 bytes (decoder 13,952 -> 6,564);
spc.c 27,140 -> 6,224 (decoder 15,080 -> 4,692). The tables alone, at -O2:
interp816.c 18,272, spc.c 19,236 -- -Os matters because the memory accessors
and helpers were inlined into every case (spc_adrIdy alone was 468 bytes:
three full copies of spc_read).

Verification: differential tests running the original and rewritten cores
side by side on random states, comparing registers, flags, cycles and the
exact sequence of bus accesses (65816: every opcode, all 16 M/X/E/D
combinations, with and without the word-bus handlers, 41 million runs; SPC700:
every opcode, I/O, DSP-pending and watch-log paths, the whole ARAM, 2 million
runs per configuration): no mismatch; a deliberately corrupted table entry is
caught. Native SMW TAS replay identical over 90,000 frames with both settings;
interp816 89/89, interp_bridge 432/432, SA-1, DSP-1 and APU C tests pass.

_interp_run_core (13.8 KB): 8 blocks that never ran in SMW or Pitfall
(diagnostics, step-cap reports, idle-poll replay, cooperative polling, and
title-specific special cases keyed on guest PCs) are marked cold
(BRIDGE_COLD_PATH, layout only). Its frequently entered code (lines entered
>= 0.1% of the hottest) shrinks from 4,320 to 2,784 bytes but still spans
10 KB of the function: GCC moves only part of the cold code to the end. A
compact per-instruction loop needs the function split.

Hot runtime code in the emulation stage (lines entered >= 0.1% of the
hottest), new build: Pitfall (interpreter-bound) 9.9 KB -- the interpreter
kernel fits; SMW 32.3 KB -- the same kernel plus the hardware-access layer
compiled game code calls (ppu_write, snes_writeReg, DMA).

Not yet re-placed: icache_order.ld predates these changes (SMW 1.88M ->
1.91M misses, Pitfall 9.4M -> 19.3M with the stale layout).

### Interpreter kernel: _interp_run_core split (2026-11-11)

The interpreter loop's rare paths are now out-of-line functions marked
noinline + cold (BRIDGE_COLD; GCC places them in .text.unlikely.* sections):
the Star Ocean $00D9 wait and vblank fast-forward (the fast-forward's
30-address PC list is now checked only after a bank pre-filter, (bank & 0xF1)
== 0xC0, a superset of its banks), idle-poll replay, the stable-poll detector,
nested scheduler hand-off, non-local return exit, the automatic-joypad wait,
the yield point, the stop-PC intercept, pre-opcode hooks, and the step-cap,
bad-bank and IBRWATCH diagnostics. Exits come back as result codes
(BRIDGE_COLD_FALL / _CONTINUE / _RETURN_1 / _STEP_CAP or the run's return
value). Bodies were moved by script (in. -> in->, returns -> codes), not
retyped; the detector's struct types moved to file scope.

_interp_run_core 13,784 -> 7,348 bytes (N64); its hot code (lines entered
>= 0.1% of the hottest) is unchanged, 4.0 KB in SMW and 2.0 KB in Pitfall,
but now within the first 6.6 / 4.4 KB instead of 11.4 / 7.5 KB. Verified:
interp_bridge 432/432 (incl. stable-poll S17 and idle-replay S18), interp816
89/89, bridge pytest suites, native SMW TAS replay identical over 90,000
frames with and without SNESRECOMP_COMPACT_INTERP.

Kernel by role (loop, both cores, APU loop, bus accessors) as whole
functions: 49 functions, 32 KB -- hot within it about 11 KB per game. The
decoders are the gap: a game-agnostic kernel holds the whole decoder (games
use different opcodes): 65816 8.7 KB (hot 0.8-2.1 KB), SPC700 6.9 KB (hot
0.5-2.3 KB). A naive kernel-first layout (no traces) measured worse than the
old default (SMW 3.34M misses vs 2.88M; PPU stage 505K vs 96K): placing the
kernel without a rule for the rest moves every other hot function. Next:
consolidate the decoders' special cases, then a complete deterministic
layout (kernel sets + Idea 1 colouring for the rest).

### Interpreter kernel: decoder consolidation (2026-11-11)

65816 core: opcode families that differ only in the register or flag they
use are one case each (8 branches -- flag by bits 7-6, taken when it equals
bit 5; 7 flag set/clear; X/Y transfers; X/Y increment/decrement; A transfers
and INC/DEC A; register push and pull; PHP/PHK/PHB): 37 opcodes. The
per-operation functions for the read-operand, store and read-modify-write
shapes (22 functions, each with an 8-bit and a 16-bit copy) are now three
width-generic blocks in the decoder (M, or X for the index registers, selects
the width; m is its mask); ADC/SBC keep theirs (decimal mode). SPC700 core:
the same treatment for 39 opcodes (branches -- same encoding as the 65816 --,
flag set/clear, push/pop, register INC/DEC and moves, the abs.bit carry
operations); its other cases were already one-liners -Os merges well.

Title-specific code in the shared 65816 core, preserved as is (the tests
compare against the original): PEA ($F4) sets Y to 0 when its operand is
$C2AE (`if (w == 0xC2AE) cpu->y = 0;`) -- any game executing PEA #$C2AE
gets that, which hardware does not do. Like the guest-PC special cases in
_interp_run_core, it belongs in a title adapter.

Verified against the original (pre-table) cores by the differential tests:
65816 24.6M runs per configuration, SPC700 1.0M, no mismatch; native SMW TAS
replay identical over 90,000 frames; C and pytest suites pass.

Sizes (N64 build): interp816_runOpcode 8,744 -> 6,276; spc_runOpcode
6,896 -> 5,008 (the SPC700 decoder is 3.8 KB with its helpers out of line).
Kernel by role: 39.2 KB (49 functions) -> 28.4 KB (48).

The two cores cannot share cache sets: they alternate 316 times a frame in
SMW and 744 in Pitfall, in short SPC700 batches (median 123 and 51 code-line
entries); each switch would refill the other's working set. The kernel's
hot code per game is ~11 KB (SMW) / ~5.5 KB (Pitfall); the whole kernel,
which a game-agnostic layout must hold, is 28 KB, and SMW's emulation stage
also keeps ~20 KB of hardware-access code hot. 16 KB cannot hold the
emulation stage's hot set in SMW whatever the layout.

### Interpreter kernel: batching the SPC700 (2026-11-11, not adopted)

Idea: if the 65816 and the SPC700 alternated less often, the two cores would
stop evicting each other and could share cache sets. Measured on the ares
icprof traces (SMW attract window from frame 400; Pitfall from frame 100).

Ceiling. `icsim/batchtrace.py` rewrites a recorded trace so each frame's
APU-side lines (`spc.o`, `dsp.o`, `apu.o`) run after its 65816-side lines.
On SMW, emulation-stage misses fall from 1,611,442 to 1,341,478 (-16.8%).
This is an upper bound: no exact scheme can run the SPC700 only once per
frame, because port reads must see it caught up.

Where the alternations come from (`icsim/switches.py` and native counts).
On SMW, the window has about 39,000 alternations, an average of about 630
cache lines between them. Native counts over 90 attract frames, about 165
APU catch-ups per frame:

| Source | Count | Share |
|---|---|---|
| `_interp_run_core`: flush at every progress checkpoint (any interpreted write or dynamic-read change) | 11,185 | 75% |
| Bridge exit on return | 1,996 | 13% |
| Pending-cycle threshold (1,024 master cycles) | 757 | 5% |
| APU port accesses | ~360 | 2% |

Raising the threshold therefore barely matters. With 65,536: SMW total
misses -1.6% (1.880M to 1.850M), Pitfall -0.06%.

Dropping the per-checkpoint flush (port accesses, the threshold and bridge
exits still catch the SPC up):

| | Before | Without the flush | Change |
|---|---|---|---|
| SMW total misses | 1,880,077 | 1,748,751 | -7.0% |
| Pitfall total misses | 22,237,765 | 23,102,294 | +3.9% |

Not adopted, for two reasons:

- Pitfall regresses.
- The audio changes. WRAM and ARAM stay identical at every 60-frame
  checkpoint, but the DSP output stream differs from sample 13,891 on
  (largest difference 348 against a peak of 7,134).

The cause is the bridge's relative catch-up. `bridge_apu_flush` adds the
pending time as a delta (`apuCatchupCycles`, `snes_catchupApu`) to wherever
the SPC is. It then syncs to the absolute guest clock
(`rtl_sync_apu_to_cpu_locked`), which only moves the SPC forward. When the
absolute sync has already pushed the SPC ahead, the next delta adds to that.
So the SPC's position depends on how many flushes there were, not only on
guest time. The SPC core itself is slice-invariant: `apu_runCycles_native`
carries partial-opcode cycles across calls.

Batching can only be exact once the bridge catches up to an absolute target
(for example, to the greater of the relative and absolute positions, without
stacking). That changes SPC timing in every title, so it needs its own
validation.

### I-cache layout and SPC catch-up, round 2 (2026-11-11)

**Rule-based layout: measured, not adopted.** The interpreter kernel (the
48-function role list) goes first; the hardware layer goes next, ordered
by how many code-cache modules import each function (14.7 KB); the
module allocator is coloured away from the kernel's sets.
`icsim/layout_eval.py` estimates such orders on recorded traces without
relinking. Emulation-stage misses in the trace window:

| Layout | SMW | Pitfall |
|---|---|---|
| Trained layout (old) | 1.64M | 1.15M |
| Best rule-based | 2.13M | 1.60M |
| Fully associative floor | 0.91M | 0.007M |

The kernel is 28 KB, so it collides with itself. Role lists also can't say
which lines of the hardware layer and libdragon are hot. Static
reachability from the interpreter loop is no help either: it pulls in
printf and every coprocessor. Colouring modules on top of the trained
layout changes misses by about 1%, because the trained layout already uses
every set.

**Engine layout: trained on a fixed suite, checked on a held-out game.**
Trained on SMW and Pitfall; Joe & Mac 2 is never in training. One training
is noisy: across 8 seeds of `icache_trg.py`, SMW's PPU-stage misses ranged
from 134K to 470K. The rule is therefore best of seeds 0-7, scored on the
training games only (geometric mean of misses relative to the best seen
for each game).

**SPC catch-up by guest time** (`SNESRECOMP_LLE_APU_GUEST_TARGET`, runner
default off). The bridge's relative catch-up stacked on the absolute sync,
so the SPC's position depended on how often the bridge flushed. Now every
caller targets one function of guest time:

- That time runs on past the end of an overrunning frame instead of
  stopping at the boundary, as the SPC does on hardware.
- Frame boundaries and HLE SPC uploads start from that time.
- Time owed across an APU reset is dropped.
- Boot, before the port clock is anchored, keeps the relative path.

With the option on, `SNESRECOMP_LLE_APU_PROGRESS_FLUSH=0` (drop the
per-checkpoint catch-up) and a 64x threshold leave WRAM, ARAM, DSP output
and rendered audio identical. Checked on SMW (attract 1,200 frames, TAS
3,000), Pitfall, Joe & Mac 2 and Tetris Attack (900 each). SMW's
90,000-frame TAS progress is identical with the option on and off, also
under code-cache emulation.

Against the old catch-up, audio pacing shifts at loads. Samples relative
to a nominal 32 kHz by frame 840:

| Game | Old | New |
|---|---|---|
| Joe & Mac 2 | -37,340 | -29,621 (recovering) |
| Pitfall | -2,385 | +4,932 (bounded) |
| Tetris Attack | +6,619 | +7,247 |

Tetris Attack's existing BRK remains, at a different address.

**Adopted on the N64** (`APUBATCH=1`, the default), with the seed-7 layout
trained on that configuration. ares totals:

| Game | Catch-up kept, best layout | APUBATCH=1, seed 7 | Change |
|---|---|---|---|
| SMW | 1,844,030 | 1,705,494 | -7.5% |
| Pitfall | 4,818,294 | 2,907,037 | -40% |
| Joe & Mac 2 (held out) | 9,720,656 | 8,737,702 | -10% |

Default link order for reference: 3.53M, 48.3M, 22.9M. With a single
shared layout, batching alone moved SMW -6% to -10%, and Pitfall and
Joe & Mac 2 by 0% to +18% depending on the layout. Layout noise is larger
than the batching effect, which is why each configuration gets its own
best-seed layout.

**SPC700 on the RSP: assessment, no code yet.**

The case for it:

- The RSP runs the DSP at about 7.8 ms per emulated frame and background
  expansion at 3-4 ms, against 70-80 ms frames, so it is about 80% idle.
- The SPC700 runs about 5,000 instructions per frame: roughly 4 ms on the
  RSP at an estimated 50 cycles per instruction.
- It is about 27% of SMW's emulation-stage fetches, and about 7 KB of hot
  code that would leave the VR4300 I-cache.

The design that keeps results exact: the SPC trails the 65816. The CPU
publishes its guest time; the RSP runs the SPC up to it, never past it; a
port access waits for the SPC to reach the access time. No rollback is
needed.

The hard parts:

- an SPC700 interpreter in the 4 KB IMEM, next to the DSP microcode (as an
  overlay);
- a DMEM cache for the 64 KB of ARAM;
- MUL and DIV;
- an exactness harness like the DSP's.

## Next steps

1. Audio: check the RSP DSP by ear on hardware (output arrives later than
   with the eager DSP). Cheaper launches (fewer marked pages, decode).
   The serial BRR filter recurrence is now the largest RSP cost.
2. Placing the large heap objects by allocation offset, so builds stop
   needing their own stack colour.
3. Interpreter share: promote more code (analysis of SMW's `$0086DF`
   dispatcher idiom), and gameplay captures for the native set.
4. PPU stage: decoded-tile cache for BG expansion, then BG line building
   on the RSP overlapped with emulation (see LESSONS.md).
5. RDP coverage: layer colour math, mosaic, Mode 7 (a textured plane).
