# N64 port: lessons from other projects and notes

Ideas collected for later work, with how they apply here. Nothing below is
copied code. sodium64 (github.com/Hydr8gon/sodium64, GPL-3.0, MIPS
assembly; surveyed 2026-11-09 at commit a4c75d3) is a reference for ideas
only: its code cannot be used in this repository.

sodium64 aims at "playable", not exact. Many of its speed wins are
accuracy shortcuts this project does not take (listed at the end so they
are not mistaken for techniques).

## Graphics (highest value for us)

- **Rendering on the RSP + RDP, one frame ahead.** The CPU only records
  PPU state; the RSP turns SNES tiles into CI8 textures and emits RDP
  commands while the CPU emulates the next frame. Our PPU line building
  runs serially after emulation on the CPU (about 69 of 177 ms per frame in
  ares, Nov 2026); moving it to the RSP and overlapping it with emulation
  is the largest remaining CPU saving. Keep our plane output, exact
  software fallback and per-line sprite rules.
- **Vector bitplane-to-CI8 decode** (sodium64 `rsp_main.S` `shared_decode`):
  `lpv` spreads 8 bitplane bytes into lanes, AND with a per-lane bit mask
  (`0x8000 >> lane`), `vmudl` by per-lane powers of two works as a
  per-lane variable shift, OR the planes; palette added to non-zero pixels
  with `veq`/`vmrg`; `spv` stores 8 pixels. 2/4/8 bpp share one routine.
- **Decoded-tile cache** keyed by (character, palette) in RDRAM, with
  per-64-byte VRAM dirty bits per layer set by the CPU on VRAM writes and
  consumed by the RSP; empty tiles marked and skipped. Most frames decode
  almost nothing. (A CPU-side variant could also help our current builder.)
- **Section snapshots:** PPU register writes that change a value mark the
  state dirty; a compact (~64-byte) register block is snapshotted only
  then, with an end-line field. HDMA comes for free because it calls the
  same register handlers. Our raster journal already records per-line
  state; the lesson is to hand the RSP compact, DMA-friendly blocks.
- **Let the RDP do flips** (texture-rectangle gradient sign), priority
  (painter's order with low/high-priority passes, skipping a high pass
  when no high tiles were seen) and window clipping (scissor segments).
- **Pitfalls seen there:** one blocking RDP command packet per tile and
  synchronous DMA everywhere (overlap DMA with work; batch commands);
  DMEM is 4 KB (stream strips to RDRAM; tile cache in RDRAM); a 64 KB VRAM
  snapshot at frame start stalls the CPU's V-blank uploads (prefer dirty
  regions); patching command templates right after DP_END can race the
  RDP's fetch (use separate command slots).
- **RSP budget:** our audio microcode already uses about 47% of the RSP at
  32 kHz; graphics work has to fit beside it.

## Memory and paging

- **TLB demand paging works on hardware:** sodium64 maps the SNES address
  space through the TLB and pages ROM from the cartridge in 8 KB blocks by
  PI DMA into a 2 MB ring on TLB misses (`memory.S`). That is the mechanism
  for paging translated code (option 2). It owns all exception vectors; we
  run under libdragon's handler, so hooking TLB refills there is the open
  question.
- **The CPU cannot execute from the cartridge with caching:** cache-line
  fills from the PI domain are not supported (ares freezes with "cached
  access to non-RDRAM area", matching hardware). Uncached execution is
  about 100 cycles per instruction. Measured 2026-11-09.
- TLB write-protect (no Dirty bit) traps writes to ROM; mirrors are
  aliased mappings. Memory-mapped I/O through TLB faults is elegant but the
  exception cost is high for frequent register traffic; a table is better
  for us.
- A software page table of 8 KB granules is 8 KB at 4 bytes per entry,
  the whole D-cache: our fast map is that size plus a 2 KB kind table.
  Misses measured small (about 0.3%); a coarser or two-level table is a
  cheap experiment.
- KSEG0 pointers are negative as signed values: make special entries
  non-negative and test with `bgez`.

## Interpreter (option 3)

- Budget: about 26 host cycles per guest cycle (93.75 MHz vs 3.58 MHz).
  sodium64's interpreter costs about 25-30 host instructions per guest
  instruction; ours about 300 (mostly bridge bookkeeping, Nov 2026).
- Opcode tables specialised per mode (E, M/X widths, and decimal-mode
  copies), switched only on REP/SEP/XCE/PLP/RTI/SED/CLD: no mode tests in
  handlers, BCD free when D is clear.
- Zero the X/Y high bytes once when X becomes 8-bit, so indexed
  addressing never masks.
- Guest registers in locals (computed-goto dispatch in C) instead of a
  struct passed by pointer; cache host pointers for the direct page, the
  stack and the current bank.
- One down-counter per scanline; all timing counters relative to the
  current line, rebased once per line, so each check is one compare.
- Per-region access speed stored in the page table entry (today
  `cpu_region_speed` is a branch chain on every bus access).
- Flat one-entry-per-address I/O handler tables (0x2100-0x437F).
- Two-stage dispatch (addressing-mode routine, then operation routine)
  keeps code small across many tables.
- Lazy flags: keep the last result left-shifted so N/Z branches are
  `bltz`/`bgez`/`beq`/`bne` against one value with no width check; wide
  registers give carry as bit 8/16. PLP/RTI can set N=1 and Z=1 together,
  which one value cannot represent: needs a fallback. (Also applies to
  dead-flag work in generated code.)
- Translated code can thrash the 16 KB I-cache where a compact
  interpreter stays resident: measure option 2's paging with that in mind.

## SPC700 and audio

- sodium64 JIT-compiles the SPC700: blocks of at most 16 bytes ending at
  any control flow, a flat 64K-entry PC-to-block table, a version counter
  per 64-byte ARAM page bumped on every write and checked (start and end
  page) at block entry, the whole buffer flushed when full, code written
  uncached plus per-line cache ops, lazy register load/store and lazy N/Z,
  cycles baked into each block. In a C interpreter the page-version idea
  works as a decoded-instruction cache.
- SPC700 "underclock": charge about 2x cycles per SPC instruction while
  timers and the DSP stay on master time; tempo is unchanged and most
  drivers only busy-wait. A possible opt-in speed setting (off by default
  under the accuracy policy; needs an LLE reference per docs/HLE policy).
- Timer reads: compare against a precomputed next-overflow time and return
  the cached count (we already have closed-form timers).
- Envelope steps scheduled as "next global counter value" instead of a
  per-sample test; lazy BRR decode only when the pitch counter crosses a
  4-sample chunk.
- IPL ROM overlay through a per-page offset table (ROM stored right after
  ARAM), writes always to RAM: no branch on reads.

## Not techniques: sodium64's accuracy shortcuts

Internal CPU cycles and FastROM timing not counted; IRQ/NMI/HDMA only at
line boundaries; no H-IRQ; DMA instant inside the register write; instant
multiply/divide; one hard-coded idle-loop pattern; PPU state per section
(may lag several lines), VRAM/CGRAM once per frame, no sprite line limits,
wrong OBJ-vs-OBJ priority, colour math only on the backdrop, one window,
no mosaic/hi-res/offset-per-tile; DSP without interpolation, echo, FIR,
pitch modulation or release; SPC700 at half speed by default.
