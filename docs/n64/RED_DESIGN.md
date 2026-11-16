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

## The SPC belongs on the CPU (owner, 2026-11-15; supersedes the model below)

The 65816 and the SPC talk constantly (port reads: about 5,200 a frame on
JM2's loading screen); the SPC and the S-DSP talk rarely (register writes,
a few catch-ups for echo, ENVX and the pages the S-DSP still has to read).
The chatty pair goes on one processor. The RSP SPC (`n64/rspspc`, the
async lane, its runner hooks) is retired; its history stays in
BRINGUP_NOTES and git. The CPU makes room for the SPC by moving other work
to the RSP and RDP instead (the PPU first).

- **Blue** owns the 65816, the SPC (`spc.c`, the C core, also the LLE
  reference) and ARAM. The SPC is caught up where the 65816's time is
  known: port accesses and the frame boundary (at least once a frame, so
  the S-DSP always has register writes to render).
- **Red/Yellow** own the S-DSP: batches, decode (prep, step 2a), and the
  output samples.
- **Green** drives frames, the PPU and audio out, and never touches the
  SPC. (The N64 CPU has one core: Blue and Green are fibers. Running the
  SPC on Green would add a fiber switch to every port read and buy no
  parallelism.)

Next, in order: make the C SPC cheaper (exact idle-loop skipping for timer
polls; the 65816's port-poll loops fast-forwarded by running the SPC until
the port changes; the SPC's hot code laid out to fit the 16 KB I-cache), then
the SPC → S-DSP link as a time-sliced queue of register writes with one
shared ARAM and hot pages (the S-DSP's footprint: BRR spans of the playing
voices, directory entries in use, the echo window), which removes the
CPU's batch bookkeeping (marks, view, undo, pack/unpack, launch/join).

## The audio ownership model (owner, 2026-11-15) (superseded above)

What the hardware couples: the 65816 and the SPC through the four ports each
way; the SPC and the DSP through DSP register writes and ARAM (samples, the
directory) forward, and ENVX/OUTX/ENDX and the echo buffer back; the DSP
feeds audio out. One clock (a DSP sample every 32 SPC cycles). Data flows
forward with timestamps, so each stage may trail the one before it; only
the backward reads force a wait: a port read needs the SPC at that cycle,
an ENVX/OUTX/ENDX or echo read needs the DSP at that cycle. In SMW and JM2
those are 0-2 a frame.

Everything else that waits today is ours: the CPU runs parts of the APU
(packing state per run, C steps at batch edges, batch bookkeeping, traps),
the frame boundary and the audio pump force the SPC and DSP current, Blue
and Green both touch APU state, and `rspq_wait` couples lanes.

The rule: one owner per piece.
- **Blue** runs the 65816 and never touches APU state: it posts timestamped
  port writes and the horizon; a port read waits only for the SPC's
  published progress to pass its cycle, then reads the published ports
  (yielding to Green meanwhile).
- **Red/Yellow** own the SPC and the DSP on the RSP: state, batch
  bookkeeping, the handoff between the two overlays, ENVX/echo syncs.
- **Green** drives frames, the PPU and audio out; it takes finished samples
  and never forces a sync.
The CPU runs no APU code (save states excepted).

Steps, each hash-identical to the C core:
1. The SPC's state owned by the RSP: (a) lazy state on the CPU side (no
   packing or unpacking per run); (b) an opcode handler cache (every handler
   swappable, a handful of IMEM slots, FIFO eviction, the dispatch table
   patched so hits cost nothing), which frees the IMEM for (c) the port
   queue, the horizon and the published progress on the RSP.
2. DSP bookkeeping on the RSP: the SPC logs every write (no page marks), the
   S-DSP keeps its own ARAM copy and applies the log, Red hands off between
   the two overlays, ENVX/echo syncs on the RSP.
3. The audio pump takes finished samples only; the frame boundary stops
   forcing a catch-up.

## Audio output on the N64, and the latency it allows (2026-11-15)

The N64 has no sound chip. The Audio Interface (AI) DMAs 16-bit stereo PCM
from RDRAM to the DAC at a programmed rate, with two buffers queued (one
playing, one pending) and an interrupt as each finishes. Native games
render ahead in chunks: the CPU's sequencer builds a command list per
output buffer (about a frame), an RSP audio microcode renders it, and the
AI plays it a buffer or two later. libdragon wraps the AI as a ring of
buffers (`audio_init(rate, buffers)`, `audio_can_write`,
`audio_write_begin`/`end`); its mixer is an RSP microcode in the same
style. We do not use the mixer: `n64_platform_audio_init(32000)` runs the
AI at the S-DSP's 32 kHz with 4 buffers, and Green's
`n64_platform_audio_pump` fills free buffers from the S-DSP's sample ring.
Our S-DSP overlay plays the part of a native audio microcode.

For the SPC/S-DSP split this means the S-DSP may trail the SPC by about a
frame without starving the output: the AI buffers are the latency budget.
How far the S-DSP is allowed to trail is then a choice about the hot set
(a longer lag means more pages to protect) and the overlay switches (a
shorter lag means more), not about audio timing.

## Step 2 in stages (2026-11-15)

Measured before step 2 (ares, per frame, gameplay): the CPU's S-DSP work is
4.6 ms in SMW and 6.6 ms in JM2. It splits into decode 1.3/1.8 ms, the view
(page copies and undo) 0.6/0.8, pack and submit 0.6, and the join 1.4/2.0
(0.3-1.5 of it waiting). The S-DSP microcode fills IMEM to the last word,
so new RSP work goes in a separate overlay ("prep") that Red runs on its
ring just before each batch. Each stage is a build option, the CPU code
stays as the reference, and each is hash-identical to the C core.

- **2a. Decode on the RSP.** Prep turns the write log into the S-DSP's
  operations. Decode is a function of the written byte plus a few values at
  the batch's start (per-voice pitch, GAIN value, gain mode, rate 3, the
  USEGAIN/DIRECTGAIN masks), which the CPU passes. The CPU still decides
  whether a batch runs on the RSP (pitch modulation, the echo window,
  operation count) with a scan that applies decode's rules to the few
  registers they read. `RSPDSP_PREP=1`; `n64/rspdsp/preptest` compares the
  two decoders.
- **2b. The view on the RSP: tried and dropped (2026-11-15).** Prep copied
  the marked stale pages by DMA and undid the batch's writes. The random
  test passed (300 of 300 against the CPU algorithm), but SMW and JM2
  hashes differed from frame 300 (S-DSP output only). The CPU copies the
  pages at launch; prep copies them when Red reaches the command, and by
  then the SPC has written ARAM again (it runs on while the batch is in
  flight). The undo reverts only the batch's own writes, so the later ones
  leak into the view. A correct view on the RSP must be kept current by
  applying every write in order, which is 2e. The CPU's view work is only
  0.5 ms a frame (0.26 with the attempt), so it stays on the CPU until 2e.
- **2c-2e as first written (superseded below):** 2c, the S-DSP state stays
  on the RSP; 2d, the join on the RSP (samples to the output ring, echo
  bytes back to ARAM); 2e, prep reads the SPC's raw log, batch edges on the
  RSP. Written while the SPC still ran on the RSP.

## The SPC → S-DSP link, fire and forget (owner, 2026-11-16)

Supersedes 2c-2e above. The SPC is on Blue (the CPU), the S-DSP on Red (the
RSP). The goal: lockstep results (the reference: the C core with the
deferred S-DSP off, one S-DSP sample every 32 SPC cycles on one shared
ARAM), chunked execution, and nothing coming back to the CPU unless the
SPC itself reads something only the S-DSP knows.

**One queue in RDRAM, a doorbell in the mailbox.**
- Blue's SPC appends every event the S-DSP depends on to a ring in RDRAM,
  each with its sample time: "at sample t, DSP register r = v" and "at
  sample t, ARAM[a] = v". Register and ARAM writes share the ring because
  the S-DSP must apply them in order. The ring holds a couple of frames,
  so the SPC never waits for room.
- At natural points (every N samples, the frame end) Blue writes back the
  new entries from its data cache and rings the doorbell: one mailbox word,
  "valid up to entry k, time T". It does not wait. (CPU stores into DMEM
  are read back: see the lost-store finding in BRINGUP_NOTES. One per post
  is cheap; the entries themselves never go through DMEM.)
- Red, between other jobs, reads the new entries by DMA, applies the ARAM
  writes to its own 64 KB copy of ARAM in time order, renders samples up to
  T into the output ring, and posts "caught up to sample T" in the mailbox
  (the up word and its interrupt). The S-DSP's state (voices, envelopes,
  echo position) stays on the RSP side between batches: no pack, unpack or
  join on the CPU.
- Green feeds the AI from finished samples, using the published progress;
  it never forces a sync.

**Only the backward direction waits.** The S-DSP can never be ahead of the
SPC (it runs only to times the SPC has posted), so "run to t" is always
possible. When the SPC reads ENVX, OUTX or ENDX at sample t, or reads ARAM
inside the echo window (the CPU knows the window from the SPC's own ESA and
EDL writes), Blue rings the doorbell with "run to t and publish"; Red
renders to exactly t, writes the readable registers (17 bytes) or the
requested echo bytes into a small RDRAM block, and posts "caught up to t";
Blue reads the values. Earlier counts: 0-2 such reads a frame in SMW and
JM2. SPC writes into the echo window are ordinary queue entries.

**What this deletes on the CPU:** page marks, the ARAM view and its undo,
pack and unpack, the join before every launch, the C fallback for batches
the RSP rejected (the RSP must accept every batch: wrapping echo windows,
pitch modulation, long logs split), and direct register writes outside a
batch.

**Open questions, to measure before building:**
- SPC ARAM writes per frame (the queue's size and the CPU's append cost);
  if logging every write is too dear, log only writes to the S-DSP's hot
  pages (directory, BRR spans of playing voices, echo window), which needs
  the footprint published by Red.
- Backward reads per frame (ENVX/OUTX/ENDX, echo window) per game.
- How often an ARAM write lands where the S-DSP reads later in the same
  batch (Red must apply such writes at their sample, splitting the batch:
  corner case or common?). The S-DSP microcode fills IMEM, so applying
  ARAM writes is prep's job or needs IMEM freed.
- Batches the RSP rejects today, and why.
- How long a backward read waits: Red should take a "run to t" request
  ahead of graphics work (libdragon's high-priority queue, which needs RSP
  jobs split small enough to give it a turn), and should keep the S-DSP
  caught up eagerly as entries arrive, so a read waits for a few samples,
  not a batch.

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

## The S-DSP microcode's IMEM, and splitting overlays (owner, 2026-11-16)

Overlays may be split into smaller stages, at the cost of an overlay switch
(IMEM and data, tens of microseconds) and intermediate buffers, when one is
too constrained. The S-DSP microcode (`rsp_snesdsp.S`) fills IMEM to the
last word (4,096 bytes, of which the rspq kernel is 704 and the S-DSP
3,392):

| Part | Bytes | Runs |
|---|---|---|
| rspq kernel (in every overlay) | 704 | per command |
| Batch loop, log apply, ARAM access (`Aram_Rmw`, `Dir_Entry`) | 516 | per batch / per logged write |
| A: pitch counters | 188 | per sample |
| B: Gaussian interpolation | 268 | per sample |
| C-H: key on/off, envelopes, gain, volumes, mix, master, output | 824 | per sample |
| Out_Flush, Noise | 148 | per 32 samples / with noise |
| BRR decode (four filter loops) | 776 | per 16 samples per voice |
| Key on | 116 | on key-on |
| Echo (FIR, feedback, echo buffer) | 556 | per sample with echo |

Already there for the fire-and-forget link: the microcode applies logged
ARAM writes at their sample (`Log_aram` -> `Aram_Rmw`, an 8-byte
read-modify-write by DMA into the ARAM image it reads). What feeds it today
is the CPU's per-batch view (marked pages copied, the batch's writes
undone). With an RSP-owned ARAM copy and every SPC write logged, the view,
marks and undo go away; but one read-modify-write per write would be costly
at thousands of writes a frame, so the writes should be applied a line or a
page at a time, with per-sample order kept only where the S-DSP reads.

First split candidate: **echo as its own stage** (556 bytes). Echo never
feeds back into the voices, and for EDL >= 1 its delay (>= 512 samples)
exceeds a batch (256), so a second pass can take each sample's echo input
and dry total from the voice stage and finish the batch (EDL = 0 keeps a
one-sample loop inside the echo stage). It needs the echo registers'
logged writes at their samples, and a fallback when the echo window
overlaps sample data the voices read in the same batch. That frees room in
the voice stage for publishing ENVX/OUTX/ENDX, "run to sample t", and
eventually pitch modulation. Next candidate: BRR decode as a pre-pass.

## RSP dual issue (owner note, 2026-11-16)

Excluding pipeline stalls, the RSP issues one scalar-unit (SU) and one
vector-unit (VU) instruction in the same cycle, so microcode should
interleave them. Vector loads and stores (LQV, SSV, ...) run in the SU's
load/store path and pair with VU arithmetic. First candidate: the tile
cache conversion (`n64/rsptile/gen_conv.py`) emits each word's VAND/VMADN
run and then its eight SSV stores back to back; interleaving each word's
stores with the next word's vector work should roughly halve that section.
Others: the scene command's scalar address math beside vector work, and
the S-DSP microcode's mixing and echo sections.

## Variants and a paging slot (owner decision, 2026-11-16)

Instead of splitting echo into a separate stage (exactness hazards: SPC
writes into the echo window and voices reading inside it), the S-DSP gets
**variants** built from the same source and **a paging slot** for rare code.

**No-echo variant (in).** `rsp_snesdsp_ne.S` builds `rsp_snesdsp.S` with
`RSPDSP_NO_ECHO`: 3,776 bytes of IMEM against the full variant's 4,096
(320 free). The CPU picks it per batch (`CHOOSE_OVERLAY`,
`snesdsp_rsp.c`) when echo writes are off and both echo volumes are 0 at
the batch's start and no logged write turns them on (FLG bit 5 clear,
EVOLL/EVOLR non-zero): echo then changes neither the output nor ARAM, so
the variant only advances the echo position and reads the echo bytes of
the batch's last eight samples, which are all the FIR history keeps. The
rule is a property of the S-DSP, not of any game. Tests: `n64/rspdsp/test`
2,000 random batches identical to the C S-DSP (515 on the variant; a third
of the random states have echo idle); `RSPDSP_FORCE_NE` (every batch on
the variant) fails 1,093 of 2,000, so the test sees the difference. SMW in
ares (`N64_STATE_HASH`): WRAM, ARAM and every output sample identical to
the C S-DSP build at all 26 checkpoints to frame 1500.

**Paging slot (next).** Rare code (key on, directory entries, ARAM reads,
and the new publish / run-to-t / pitch-modulation / wrapping-echo paths)
DMA'd into a fixed IMEM slot when used, so the full variant has room too.

## ARAM between the SPC and the S-DSP: the agreed design (owner, 2026-11-16)

The problem is time, not size: the SPC (Blue) runs ahead of the S-DSP
(Red) in emulated time, yet each S-DSP read must see ARAM as it was at
that sample, and ARAM is random access (looping samples, shared
instruments, the directory and the echo ring are read again and again).
Snapshots (copy at the batch's start, undo later writes) and replay logs
(the S-DSP keeps its own copy and re-applies every SPC write) both spend
work twice. Two designs replace them.

### Main design: the S-DSP front end on Blue, merged with the SPC

The S-DSP's control half runs on Blue inside the SPC's run loop, one step
at every sample edge (every 32 SPC cycles: lockstep, like the hardware and
the C reference): per voice, the pitch counter, block crossings, key on
and key off, the BRR header's end and loop flags (and the directory entry
at a loop or key on). At a block crossing it reads the 9-byte block from
ARAM at that moment, the exact moment the S-DSP would, and puts it in the
queue to Red with its sample time, beside the register writes. Envelopes
are not stepped on Blue: they are computed lazily, from the last known
state, when the SPC reads ENVX.

Red is then a pure function of the queue: decode the shipped blocks,
interpolate, envelope, mix, echo. It never reads the SPC's ARAM. The echo
buffer is the one ARAM region Red keeps, as its own copy of the echo
window: SPC writes into the window go into the queue; SPC reads of the
window wait for Red (backward read). What this removes: the ledger, the
snapshot, the marks, the view and its undo, shared-memory cache hazards,
and the S-DSP microcode's ARAM read, directory and write paths (IMEM).

Register reads: ENDX and ENVX are answered on Blue with no round trip
(ENDX from the headers and timing, ENVX from the lazy envelope). OUTX needs
real audio, so it still syncs with Red (rare).

Exact by construction (the same order of events as the C S-DSP); tested
like the S-DSP, against `dsp_cycle` on random states. Game-independent: it
lives in the shared runtime (`apu.c`), and the full C S-DSP stays as the
reference. Cost: a fixed step per sample (estimated 100-200 CPU cycles, to
be measured). If that shows, or once idle-loop skipping fast-forwards the
SPC, the front end becomes event-driven: the next block crossing per voice
is a closed-form time, so it works only at crossings.

Not predictable on Blue: pitch modulation (PMON), whose pitch depends on
the previous voice's audio output. PMON voices use the fallback below.

### Fallback (PMON voices): one shared ARAM plus an undo ledger

The S-DSP reads the SPC's ARAM directly. Every SPC write also appends
(address, old value, time) to a ledger. A read at S-DSP time t takes the
oldest ledger entry for that address later than t (its old value is the
byte at t), else ARAM's value; entries the S-DSP has passed expire. Nothing
is applied twice. Subtleties: the SPC's ARAM sits in the CPU's data cache,
so a post writes back the lines written since the last post, and ledger
entries are appended uncached so an entry reaches RDRAM before its data can
(an evicted line must never show a future value without its entry; the
S-DSP fetches data first, then checks the ledger). The S-DSP's own echo
writes go "into the past" when the SPC wrote that byte later: they update
the ledger entry's old value instead of ARAM. The CPU must not hold dirty
echo-window lines (the SPC's echo-window accesses take the uncached slow
path). Cost on Red: a per-page bitmap check per read, rarely a short scan.

### Refinements (owner, 2026-11-16)

**The front end short-circuits.** Inline in the SPC loop does not mean
work every sample. The front end keeps one number, the next sample at
which anything can happen (the earliest block crossing over the voices, a
pending key on or off, a pending reset); at each sample edge it compares
and returns. A voice's crossing sample follows from its counter and pitch
(the counter advances by a fixed step each sample), so counters are
brought current only when something happens, and the next-event sample is
recomputed only when an event fires or the SPC writes something that
changes timing (pitch, key on/off, source number, directory, reset). The
per-sample cost is one comparison; work happens at block crossings. This
also fits idle-loop skipping: a skip jumps time and the events in between
are handled in order.

**Small urgent results go through the mailbox / outbox.** Red posts
progress ("caught up to sample T") in the mailbox's up word, as it posts
"batch done" today, and a small outbox holds the latest readable values as
of T (the eight OUTX). Blue reads them without waiting; a sequence counter
(odd while Red writes) makes the read consistent. Resident DMEM comes out
of every overlay, so the outbox stays small; larger results go to an RDRAM
block Red DMAs, with only the progress word in the mailbox.

Stale reads are an HLE option, not the default: the exact build makes an
OUTX read wait for Red to reach its sample; an HLE build may take the
outbox's latest OUTX (selectable at build time, documented, validated per
game). ENDX and ENVX need neither: the front end answers them exactly.
Pitch modulation is not fed from the outbox: it is not a register read but
pitch driven by the previous voice's output, and a stale value would put
Blue's block positions out of step with the audio, so PMON voices stay on
Red with the ledger fallback.

### The front end, first version (2026-11-16)

`runner/src/snes/dsp_frontend.c` (`dsp_frontend.h`): per voice the pitch
counter, block decodes (the previous block's loop/end flags, the
directory's loop address, ENDX) and key on (even samples, after the decode,
KOF holding the latch), event-driven: one comparison against the next
event sample, work only at decodes and key ons. Reports each decoded block
(voice, sample, start, 9 bytes) as read at that sample. Voices with pitch
modulation are dropped from tracking (their ENDX bits too). Envelopes not
yet. `tests/audio/dsp_frontend_test.c` (in `tests/run_c_tests.sh`) steps it
beside `dsp_cycle` on 3,000 random states with random register and ARAM
writes: 1.7 million samples, 863,000 blocks, identical (counters, offsets,
flags, latches, ENDX, every block's start and bytes); deliberately broken
copies (a crossing one sample late, a wrong loop rule) fail. Random states
had work on 39% of samples; real music, with lower pitches, fewer.

**The echo window.** Within a sample the S-DSP decodes before it writes
echo, and its echo writes land in ARAM. In the agreed design Blue's ARAM
does not receive Red's echo writes, so a block or directory entry inside
the echo window (echo writes on) must not be read from Blue's ARAM: such a
block is shipped as "read at Red" and Red fills it from its own echo copy
(exact: Red works in sample order); a directory entry there makes the voice
untracked (Red's fallback). Both are rare and detectable on Blue, which
knows the window from ESA/EDL and FLG.

### Results back to Blue (owner, 2026-11-16)

Consistency of what Red publishes: Red must never wait on Blue, so a
sequence counter (Red never blocks, Blue retries rarely) or two slots with
an index suit it better than the SP semaphore (with which Red could find
it held). Large results go to RDRAM as Red produces them; Blue checks a
flag in DMEM and takes what is there.

**Register history in RDRAM.** Red appends readable register state indexed
by sample time (OUTX for the eight voices is 8 bytes a sample, about 4 KB
a frame), DMA'd a chunk at a time; a ring Blue discards behind its own
time. A backward read at sample t then needs only Red's progress to have
passed t, not Red to stop at t: Blue waits for the progress word and reads
the entry for t (exact). An HLE build may take the newest entry instead of
waiting.
