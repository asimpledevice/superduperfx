# Red: communication model and the APU on the RSP

Status: design (2026-11-13). Owner direction from the discussion in
`BRINGUP_NOTES.md` ("Red step 1"). Nothing here is built yet except the
one-word mailbox, the doorbell and the run flags (step 1).

Colours: **Blue** and **Green** are the CPU's two threads (the 65816;
everything else). **Red** is the RSP kernel (rspq, extended). **Yellow** are
the RSP overlays Red runs.

## Principle

DMEM for realtime coordination (small, frequent, latency-sensitive: commands,
doorbells, progress, statuses); RDRAM for bulk data and long command streams.
Hardware basis (`n64/hwtest` v1): the CPU may read and write DMEM while the
RSP runs; a word is atomic; a CPU DMEM write costs 10 cycles (rspq's
uncached RDRAM queue writes cost 17), a read 34; a round trip is under
1.3 us; the SP interrupt doorbell about 3.3 us. CPU DMEM traffic slows the
RSP 2-9%, so the CPU polls SP_STATUS or takes the interrupt rather than
spinning on DMEM (v3 measures the register-polling cost).

## Red's resident DMEM

Everything below lives in Red's own data, so it survives overlay loads and
is never borrowed by an overlay.

| Region | Size | Writer | Purpose |
|---|---|---|---|
| Command ring | 256 B (today's DMEM command buffer) | CPU | commands to Red and the overlays |
| Ring indices | 8 B | CPU head, Red tail | lock-free single producer, single consumer |
| Port ring (APU) | 16-32 entries x 8 B | CPU | timestamped 65816 -> SPC port writes |
| Port ring indices | 8 B | CPU head, SPC tail | |
| Horizon | 4 B | CPU | how far in emulated time the SPC may run |
| Down word | 4 B | CPU | one-word messages to a running overlay |
| Up word | 4 B | RSP | one-word messages to the CPU (SIG0 + interrupt) |
| Progress pair | 8 B | RSP | SPC cycle reached, SPC output ports |
| Block boxes | 2 x 32 B (later) | one side each | multi-word messages, semaphore try-lock |

Every byte here comes out of every overlay's DMEM budget. The SPC overlay
was already full (the 16-byte mailbox overflowed it), so its rewrite must
free room. Sizes are to be set from the measurements below.

## Protocols

- **Rings** (commands, ports): the producer writes the entry's words, then
  **publishes** them by writing its head index (one word, written last):
  until the head moves past them the consumer ignores those slots, so it
  never sees a half-written entry. The consumer reads the head, then the
  entries, then writes its tail. One producer, one consumer, atomic words:
  no lock. The CPU's DMEM stores are uncached and performed in program
  order (only the compiler must be kept from reordering them). Several
  entries can be published with one head write, like rspq's flush.
- **One-word boxes:** a status word where the latest value wins needs no
  sequence bits; a message that must not be lost is one in flight, flagged by
  a SIG bit where one is spare or a toggle bit, acknowledged through the
  other direction's box.
- **Progress pair** (cycle, ports): read as a consistent pair. Either a block
  box under the semaphore, or the RSP posts the pair only when the ports
  change, so the up word carries the news and the second word is context.
  To be settled with the v3 hardware results.
- **Block boxes:** the semaphore with a try-lock that never waits on the RSP
  side; the CPU holds it with interrupts off, briefly.

## Red's responsibilities

1. **Dispatch from the DMEM ring.** Red reads commands directly from the
   ring (no DMA fetch). Command format stays rspq's: top byte overlay and
   command, arguments in the following words (first word's low 24 bits too).
2. **Bulk through RDRAM lists.** A ring entry "run the list at RDRAM address
   A" (rspq's Jump/Call) carries long streams: an rdpq frame, a prerecorded
   block. When the ring is full, the CPU spills the rest into an RDRAM list
   and posts one jump instead of waiting.
3. **Overlay loads,** as rspq does (implicit from the command's overlay id).
   With a view of the ring, Red may later group same-overlay work.
4. **Background tasks.** With the ring empty, Red runs background work in
   slices instead of halting; between slices it checks the ring head, and
   commands pre-empt at slice boundaries. A small table lists the tasks: the
   overlay, a cheap data-driven "has work?" test (e.g. horizon > progress,
   both words in DMEM), a priority and the slice entry. Red runs the highest
   priority task with work, preferring the overlay already loaded (each
   switch costs an overlay load, so slices must amortise it). The SPC is the
   first task; candidates later: S-DSP batches as soon as the SPC's log
   allows, PPU lines as they become final, prefetch. Choice of task changes
   only the order of work, never results (every limit is in emulated time).
   With nothing to do, Red spins on the ring head in DMEM rather than
   halting: no RDRAM traffic, and a new command starts within a few cycles
   without SIG_MORE or an unhalt. Further idle-work candidates: loading and
   relocating compiled-code modules for the CPU's code cache (the CPU keeps
   only the I-cache invalidation), state hashing and checks in debug builds,
   staging the next jobs' inputs (the BRR pages of the next S-DSP batch).
8. **Plug-ins (in reserve).** Where switching overlays costs too much,
   small code plug-ins paged into a slot of a base overlay (as the SPC
   overlay pages its cold code) cut a switch to a few hundred bytes.
5. **Borrowing as a service.** Red defines what is resident (the regions
   above, the return path) and what may be borrowed (its code and command
   handling, which run only between commands), owns the RDRAM save area, and
   provides the borrow/return macros. They expand inside the overlay (the
   restore must run from IMEM that is not borrowed).
6. **Time slices.** A borrowing or background overlay returns to Red within a
   bounded slice, so commands (rdpq's RDP work, BG lines, S-DSP batches) are
   never starved.
7. **Messages to the CPU** through the up word and SIG0; syncpoints become
   messages.

## The APU on Red

- **The SPC700 is Red's first background task.** It runs from where it
  stopped up to the horizon, in slices. Its state (registers, the cycle count, the zero page,
  the dirty lines of its ARAM cache) persists between slices: kept in DMEM
  when nothing else needs the space, otherwise written back.
- **65816 -> SPC port writes** go into the port ring with their APU-cycle
  timestamps, from `apu.c`'s port-write path; the SPC applies each when its
  clock reaches it. A full ring makes the CPU wait (measure how often).
- **The horizon** is the APU cycle up to which every port write is known: the
  CPU's current time, published at its sync points. The SPC never runs past
  it.
- **SPC -> 65816:** the output ports and the cycle reached, in the progress
  pair. A 65816 port read waits only until the SPC has passed the cycle it
  needs; Blue yields to Green meanwhile (step 1's machinery).
- **The S-DSP stays a queued job.** The SPC writes the DSP log into RDRAM
  already in the S-DSP overlay's operation format (today it writes raw
  entries that the CPU filters and decodes: 2.7 ms a frame in JM2), and an
  S-DSP batch command consumes it, ordered after the SPC slice that produced
  it by Red. No CPU work and no CPU join between the two.
- **ARAM:** the RSP's copy is the ARAM while the SPC runs on the RSP. The
  CPU takes a view only when it needs one (state saves, C fallbacks), and the
  SPC reports the pages it wrote instead of invalidating the S-DSP view.
- **Traps** (rare opcodes, IPL ROM reads) become messages: the SPC posts
  "trap at PC", stops, and the CPU runs that instruction in C and resumes it.
  FLG/ESA/EDL/DIR writes no longer trap (the C path logs them now).
- **Determinism:** every exchange is timestamped in emulated time, so the
  result never depends on when a slice runs. Validation: state hashes
  identical to the C core.

## Measurements first

Phase 0 results (ares, `N64_PHASE0=1`, per frame; ares stretches frame time,
so idle shares are high and need a hardware run):

| | SMW (title demo) | Pitfall | JM2 |
|---|---|---|---|
| rspq words | 3,000-3,400 (peaks ~10,800) | 430-530 | ~600 |
| CPU wait for a free queue buffer | 50-60 us (up to 190) | 7-9 us | 10-13 us |
| RSP idle | 72-86% | 99% | 96-98% |
| APU port writes | ~3 | ~0.1 | bursts to ~10 |
| most port writes between two SPC runs | 4 | 2 | 2 |

So: rdpq's stream (nearly all of SMW's words; S-DSP and BG queue about 4
commands a frame each) travels as RDRAM lists, and the DMEM command ring
carries control; 256 bytes is ample. Port bursts are small: 32 entries
leave a wide margin even with horizons far rarer than today's SPC runs.


1. rspq commands per frame by source (rdpq, S-DSP, BG): sizes the command
   ring and tells how much goes through RDRAM lists.
2. SPC port writes per frame, and the peak burst between two horizons: sizes
   the port ring.
3. How long the RSP is idle per frame: the SPC's budget as an idle task.
4. The v3 and Red test ROM results on hardware.

## Risks

- **rdpq** is built on rspq's internals (its write macros, buffer state,
  blocks, the validator's use of SIG0). Red must keep rdpq working; changing
  the queue's transport is the largest compatibility change.
- **DMEM budget:** every resident byte shrinks every overlay.
- **Slice overhead:** saving and restoring the SPC's state per slice must
  stay small against the slice.
- **IMEM:** Red's code grows (ring dispatch, idle task, slices); the
  borrowable part shrinks accordingly.

## What to build, in order

Each phase is opt-in at build time until it is hash-identical against the
current path and measured; the current path stays as the reference.

**Phase 0: measure.** Counters for rspq commands by source, SPC port writes
per frame and per horizon, RSP idle time. Hardware photos of `hwtest` v3 and
`redtest` v1.

**Phase 1: Red's DMEM command ring** (`third_party/libdragon` rspq, the
kernel).
- Ring and indices in Red's resident DMEM, replacing the DMEM fetch buffer.
- CPU side: rspq's write path (`rspq_write` and the macros rdpq uses) writes
  the ring and publishes the head; on full, spill to an RDRAM list and post
  one jump; wake Red if halted.
- RSP side: dispatch from the ring, tail index, wrap-around; Jump/Call to
  RDRAM lists unchanged; blocks unchanged.
- rdpq keeps working: its command stream, its RDP buffers, its blocks.
- Test: `n64/redtest` grows ring tests (wrap, spill, interleaved overlays,
  rdpq traffic); the emulator's hashes unchanged.

**Phase 2: Red services.**
- The borrow/return macros and the resident/borrowable split; the SPC
  overlay's own borrowing moves onto them (and its DMEM overflow is fixed by
  keeping the resident regions out of the borrow).
- The idle hook and the time-slice contract: with the ring empty, call the
  registered background task for one slice; check the ring between slices.
- Syncpoints as messages.

**Phase 3: the APU's channels.**
- Port ring (CPU writer in `apu.c`'s port-write path), horizon word,
  progress pair; CPU-side waits for port reads on the progress pair, with
  Blue yielding to Green.
- Reference first: the C SPC core consumes the same ring and horizon (so the
  protocol is validated before the RSP core uses it).

**Phase 4: the SPC overlay as Red's idle task.**
- Rewrite of `n64/rspspc`: slices from the horizon, state kept between
  slices, port ring consumption, progress posts, traps as messages, the DSP
  log written to RDRAM, written-page reports.
- Free DMEM for the resident regions; reuse the existing core, the vector
  DIV/MUL and the random test harness (`n64/rspspc/test`), extended for
  slices.
- Validation: the 10,000-case random comparison against `spc.c`, then state
  hashes identical to the C core in SMW, Pitfall and JM2.

**Phase 5: the S-DSP chained behind the SPC.**
- S-DSP batches as queued commands consuming the SPC's log; the CPU joins
  only where it needs DSP state. The S-DSP overlay keeps its state between
  batches (the Yellow-lane rewrite).
