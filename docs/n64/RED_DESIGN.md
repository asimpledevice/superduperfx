# Red: communication model and the APU on the RSP

Status: design (2026-11-13). Owner direction from the discussion in
`BRINGUP_NOTES.md` ("Red step 1"). Built so far: the one-word mailbox, the
doorbell and the run flags (step 1), the Phase 0 counters, and Phase 1's
DMEM command ring with Red spinning instead of halting ("Red phase 1").

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
| Command ring | 32 B (built; beside rspq's 256 B fetch buffer, which stays for the RDRAM stream) | CPU | control commands to Red and the overlays |
| Ring indices, saved stream position, idle counter | 16 B | CPU head, Red the rest | lock-free single producer, single consumer |
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
kernel). Built as a second lane rather than a replacement: Phase 0 showed
rdpq's stream is nearly all the traffic, so the RDRAM queue and its fetch
buffer stay as they are (rdpq untouched), and a small ring carries control.
- `rspq_ring_write` / `rspq_ring_wait`; Red takes ring commands between any
  two RDRAM commands, ahead of queued work; the ring's end is padded with
  no-ops, so a command never wraps.
- Red spins on the ring and the RDRAM queue instead of halting (the highpri
  queue still halts), counting idle turns for the idle share.
- First user: the S-DSP launch.
- Budget found: libdragon's overlays were packed beside stock rspq; ULC and
  H.264 are no longer built, and rdpq has 36 bytes of IMEM left, the limit
  for Phase 2. Phase 2's kernel code may have to move out of the common
  part (e.g. into a Red overlay that rdpq never shares IMEM with).

**Phase 2 scheduling: options kept for later** (owner, 2026-11-14: decide
once the RSP is loaded and its idle time measured). All three share the
overlay-side contract (bounded slices, a preempt check macro in the
overlay, state in rspq's saved-state section, one overlay switch per
resume):
- A: a parked command word Red runs when idle, chosen by the CPU (about 4
  kernel instructions; the CPU picks; no Red-side chaining).
- B: a task table in the kernel, Red picks (30-50 kernel instructions;
  chains SPC -> S-DSP by itself; needs libdragon's GL left unbuilt).
- C: A plus a scheduler overlay that writes the parked word (policy without
  kernel cost; one extra switch per decision).
Two words, one writer each, for the parked command (CPU "requested", Red
"current/done").

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

**Phase G: Red feeds the RDP** (graphics Yellow lane; after or between the
APU phases).
- Log the renderer's exact RDP command stream; its modes are few and static,
  so the fixups rdpq's overlay applies (other modes, combiners, auto-TMEM,
  exclusive bounds, scissor) become constant blocks built at start.
- A Red command runs an RDP list (DP_START/DP_END), replacing rdpq's
  dynamic buffers; the BG overlay writes its lines straight into the list
  as texture rectangles, so the PPU stage and the RDP feed are one RSP
  pipeline and the CPU never touches those commands.
- rdpq's RSP overlay is no longer loaded (rdpq stays on the CPU side at most,
  for attach). Step taken already: rdpq without its RSP triangle engine
  (1,360 bytes of IMEM free instead of 36).

## Independence: where the CPU waits on the RSP today (2026-11-14)

The CPU posts work to the RSP and soon waits for it, everywhere:

| Lane | Where the CPU waits | Why |
|---|---|---|
| BG lines | `n64_bgrsp_wait` mid-frame | the RSP's flags are needed to build the RDP commands |
| BG lines | `n64_bgrsp_sync_vram` (`rspq_wait`) | queued jobs must finish with the old VRAM snapshot |
| BG lines | `n64_bgrsp_release_vram` | the snapshot's DMA before VRAM changes |
| rdpq | `rdpq_detach_wait` at frame end | the RDP finishes before the frame is shown |
| rspq | queue buffer full | 50-60 us a frame in SMW |
| S-DSP | before every RSP SPC run | the run may read what the batch changes |
| S-DSP | `apu_dspSync` (audio pump, frame end, saves) | samples up to now |
| S-DSP | log full; ENVX/OUTX/ENDX reads | the DSP must be current |
| SPC | every APU lock (ports, audio pump) | the state lives on the CPU |
| SPC | frame boundaries, uploads | the SPC brought exactly to guest time |
| SPC | traps, DSP batch edges | one instruction or cycle runs in C |

Three habits behind them: the CPU owns the state and the RSP borrows it per
job; results are consumed right after they are posted; and waits are global
(`rspq_wait` waits for every lane's work, so lanes block each other).

The plan:
1. Specific waits only: a per-job syncpoint or mailbox message, never
   `rspq_wait` (quick, anywhere).
2. Each Yellow lane owns its state on the RSP side (SPC, S-DSP; BG lines and
   RDP lists in Phase G); the CPU sends timestamped inputs (port writes,
   register writes, a horizon) and reads small outputs (ports, progress,
   finished samples).
3. Results consumed later: the audio pump takes the samples that are done
   (the output buffer holds frames of slack) and never forces a sync;
   graphics pipelines frames (the RSP and RDP draw frame N while the CPU
   emulates N+1, from a PPU snapshot at the frame boundary).
4. Syncs that must stay exact move off the CPU: ENVX/OUTX/ENDX and echo
   reads become RSP-internal (the S-DSP runs up to now); a port read waits
   only until the SPC's progress passes its cycle, with Blue yielding.

Order: B2; global waits out; non-blocking audio pump; opcode paging; B3/B4
(the two-overlay design below); frame pipelining and Phase G.

## No traps to the CPU: two audio overlays on Red (2026-11-14)

Every trap can go; what stays exact becomes a sync on the RSP:
- Missing opcodes: rare handlers paged into a slot of the SPC overlay
  (DMA'd on demand) instead of run in C.
- IPL ROM reads: the 64-byte ROM in the SPC's DMEM. Opcode fetches from a
  timer register: handled directly once no other trap needs undoing.
- DSP register writes (FLG, ESA, EDL, DIR, SRCN, ENDX, writes with no batch
  open) and sample-directory writes: the SPC logs every ARAM and DSP write
  with its cycle; the S-DSP keeps a private ARAM copy and applies the log in
  order, so it sees ARAM as it was at each sample. No page marks, no view
  recopies (a few thousand 8-byte entries a frame).
- ENVX/OUTX/ENDX reads and echo-area reads: a sync on the RSP: switch to the
  S-DSP, run it to now, switch back.
The shape: the SPC overlay (horizon, port queue, full log, paged handlers)
and the S-DSP overlay (resident state and ARAM copy, echo into the shared
ARAM) hand off through Red ("run this next", Phase 2's parked word); the
CPU sets the horizon, queues port writes and reads ports and progress, and
takes the full state only for saves. Costs: an overlay switch per batch and
per sync (to be measured on hardware), the SPC's borrowed IMEM/DMEM returned
before each handoff, IMEM care.

**Step B (owner, 2026-11-14): the CPU out of the audio loop.** Measured
after step A (the asynchronous SPC, `BRINGUP_NOTES.md`): S-DSP batches wait
behind SPC runs, the S-DSP's ARAM view is recopied after every run, and
each batch edge stops the RSP for C work. In stages, each hash-identical
against the C core:
- B1: the RSP run reports the pages it wrote (its line tags and the dirty
  lines it evicted), so only those leave the S-DSP's view.
- B2: the S-DSP keeps its state on the RSP side between batches; the CPU
  unpacks it only where it reads DSP state (ENVX/OUTX/ENDX, saves).
- B3: the SPC writes its log in the S-DSP's operation format (sample tags,
  watched pages), so the CPU no longer filters and decodes it.
- B4: batch edges on the RSP: the SPC run continues past an edge and the
  S-DSP batch follows it on the RSP without a CPU step; the batch-start page
  marks (`apu_dspStartBatch`) move with it. This is where Red needs an
  overlay to queue a follow-on command (Phase 2's parked word, or the
  SPC overlay handing over directly).

**Phase 5: the S-DSP chained behind the SPC.**
- S-DSP batches as queued commands consuming the SPC's log; the CPU joins
  only where it needs DSP state. The S-DSP overlay keeps its state between
  batches (the Yellow-lane rewrite).
