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

See [LESSONS.md](LESSONS.md) for ideas collected from other projects
(sodium64) and design notes, with how they apply here.

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
