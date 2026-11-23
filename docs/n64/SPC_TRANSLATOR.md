# SPC700 translator (n64/src/spc_dbt.c, `SPCDBT=1`)

Runs the SPC700 sound driver as MIPS code generated at run time, exactly:
the state hashes (WRAM, ARAM, S-DSP output) match the interpreter's.

## Where it stands (2026-11-18, SMW attract, ares)

**SPC catch-up time per frame** (`APUTIME=1`, frames 360-1000, builds run
side by side):

| Build | SPC time | Run phase |
|---|---|---|
| Interpreter (B wait skip + A run loop) | 35.5 ms | 76.8 ms |
| Translator, end of steps 1-5 + wait loops | 27.6 ms | 72.1 ms |
| + epoch follows P (6a), dp,dp / dp,#imm (7), exactness fixes | **25.7 ms** | 75.0 ms |

(Frames 360-1650 for the earlier rows: 32.75 / 24.73 / 24.25 ms; the
window matters because the attract demo's music changes.)

The run phase alone is not a reliable measure between translator builds:
builds with identical SPC behaviour moved by up to 4 ms (65.3 vs 69.0 ms)
because the I-cache layout (trained without the translator) no longer
matches the code. The history, by run phase (frames 360-2700, one build
each, so read with that in mind): interpreter 76.1 ms; step 1 73.0;
steps 2-4 75.6; blocks only at targets 74.7; superblocks 68.7; full
store-range table ends the block 65.3.

- Exact: S-DSP output marks (a hash at every 65536 samples, independent
  of how far the SPC has run at a checkpoint) and WRAM/ARAM hashes match
  the interpreter.
- Interpreted SPC cycles: ~0.58M per 300 frames (~6%); the rest is
  translated or wait-skipped.
- Pitfall and Joe & Mac 2: S-DSP marks and states match the interpreter
  too (7 marks / 14 states, 24 marks / 28 states). No translation in steady state (0 blocks made per 60
  frames, no flushes).
- Lessons from the bugs found on the way: DIV used t8 (the budget) as a
  temporary; the shared routines were never written back to memory (it
  worked while later block flushes covered them); branches left the
  instruction length at 1, harmless until fall-through continued after
  them; a refused block made the interpreter step one instruction and
  every suffix of the block got translated (~450 blocks hot).
- An opt-in self-check (`EXTRA_CFLAGS=-DSPC_DBT_SELFCHECK`) checks that
  every run handles exactly the sample edges in its cycles.

## What other translators do (read for ideas only; all GPL)

- **gpSP** (ARM to MIPS, PSP): one host frame for the whole run (s-registers
  saved once, guest registers pinned, no per-block prologue); every direct
  exit is a countdown check (`bltzal` to a trampoline) plus a `j` patched to
  the target block, ~4 instructions; blocks continue through conditional
  branches, and branches back into the block are direct jumps; links die
  with a whole-cache flush; stores check a code tag in a shadow copy of RAM
  with the store in the delay slot.
- **beebjit** (6502 to x64): countdown checked per run of straight-line
  code (block entry and after each conditional branch) for the worst case;
  when it would go negative nothing in the run has executed, so the
  interpreter steps to the exact event; dead-flag and constant passes;
  collapses polling loops; per-instruction invalidation (more than we need:
  SMW had no code-store flushes).
- **sodium64** (SNES on N64): also an SPC700-to-MIPS translator, in
  assembly; registers pinned and written back only when changed, lazy N/Z,
  timers caught up lazily. But blocks never chain (back to the dispatcher
  after each block), and it trades accuracy for speed (SPC underclocked 2x,
  cycles approximated, no H flag, simplified S-DSP): not acceptable here.

## Plan

Each step: state hashes against the interpreter (SMW, then Pitfall and Joe
& Mac 2), ares timing against the same build without the translator, and
the chain-end counters. The I-cache layout retrain (scheduled separately)
makes later timings comparable.

1. *(done; the worklist half was reverted: it doubled the blocks)* **Stale epochs** (largest chain breaker). apu_dspSyncFor/Join bump the
   epoch on every call; bump only when they clear marks that were set.
   **Untranslated targets**: translate a block's direct targets when it is
   translated (gpSP's worklist), so chains do not stop at first visits.
2. *(done)* **One frame per run.** Save s0-s8 once when C enters translated code;
   blocks lose their prologue/epilogue; C helpers spill only what they
   clobber.
3. *(done)* **Patched direct links.** A direct exit becomes: epoch compare, cycle
   check, `j target` patched once the target exists; links reset on flush.
   Indirect exits (RET) keep the map lookup. Replaces the shared chain
   routine (~40 instructions per hop).
4. *(done)* **Countdown budget in a register** (cycles to the end of the run or the
   next queued CPU write), replacing the limit/used bookkeeping; sample
   edges keep their exact handling before stores, I/O and exits.
5. *(done)* **Superblocks.** Continue through conditional branches (the taken side
   is an inline exit); branches back into the block become direct jumps
   behind the budget check, so driver loops stay in one block. Needs edge
   timing from a running cycle register instead of static offsets.
6. *(6a done)* **Cheaper checks and flags.** The largest translator
   overhead in a profile was block validation, run every time the
   validity epoch moved, and SMW's driver toggles the direct page ~1800
   times per 60 frames. The epoch's bit 0 now follows P (so a block
   validated under one P is current again when P flips back), P changes
   count only when P changes, and a new sync mark moves the epoch only on
   a page some translated block reads: slow-path entries 17k -> 0 per 60
   frames, SPC time 24.25 -> 21.87 ms per frame.
   *Evaluated, not done:* one per-page "store needs care" byte (saves two
   instructions per store but needs hooks wherever the S-DSP's log or
   view-fresh marks change) and dead N/Z elimination (almost every
   instruction can leave the block, and N/Z must be right there). Both
   are small next to what remains (see 7).
7. *(mostly done)* **Coverage.** Done: OR/AND/EOR/CMP/ADC/SBC dp,dp and
   dp,#imm; a pc whose instruction is not covered marks the next one as a
   block start. [dp+X] and [dp]+Y are written (targets checked at run
   time: I/O and IPL ROM leave the block, sync-marked pages read through
   the runtime, stores to translated code lines leave the block, MOV's
   dummy read syncs like the interpreter's) but **opt-in**
   (`EXTRA_CFLAGS=-DSPC_DBT_IDX=1`): exact in SMW, Joe & Mac 2 and the
   random test with a live S-DSP, yet Pitfall's ARAM diverges from frame
   ~290 with them on (same SPC cycle count, so a real difference; not
   reproduced by the random test, even with Pitfall's loop code in it).
   Computed I/O through the helpers (`SPC_DBT_DYN_IO=1`) was exact but
   neutral (21.01 vs 21.02 ms SPC time): off. Left: PSW push/pop,
   TCALL/PCALL, BRK/RETI, SETP/CLRP (blocks are kept per pc, not per
   direct page). What SMW still interprets (~0.5M cycles per 300 frames)
   is mostly its CPU handshake loop ($05A9-$05C0), which nothing made
   faster.
8. *(done except hardware)* **Validation and default.**
   - `SPCDBT_TEST=1`: an N64 test ROM (src/spc_dbt_test.c) runs random
     SPC programs (structured loops of random non-branch instructions with
     random operands, and fully random memory) on a translated and an
     interpreted APU and compares ARAM, ports, timers, registers and the
     S-DSP after every slice. It found the missing code-store check (see
     below) on its first full run.
     Result: 256 programs passed without the live S-DSP; with it (half
     the programs), 128 passed, then program 138 differs only in the echo
     buffer, and does so with both APUs interpreted too
     (`-DSPC_DBT_TEST_BOTH_INTERP`): a harness limit (two APUs share
     global S-DSP state), not the translator.
   - SMW, Pitfall, Joe & Mac 2: S-DSP output marks and WRAM/ARAM states
     match the interpreter (SMW 69 marks / 74 states, Pitfall 44 states,
     Joe & Mac 2 17 marks / 20 states). (Pitfall's own run is not reproducible between
     builds after ~frame 800, translator or not, so it compares against a
     no-translator run of the same source.)
   - Not done: EverDrive timing (the owner's hardware).
   - Default: stays off (`SPCDBT=0`) until it has run on the EverDrive:
     the translator writes code and maintains the caches itself, which
     ares checks but hardware has the last word on.

Bugs the test and the games found in this round:
- The interpreter's store path checked for translated code only in the
  audio-split build: in the translator's own configuration a store into
  translated code never dropped it (Pitfall uploads code as it plays).
- S-DSP echo writes can land on translated code (no SPC store to say so):
  a page with translated code that gets a sync mark (the S-DSP may write
  it) now drops the translations, and code on a sync-marked page is not
  translated.
- [dp+X] stores skipped the dummy read that syncs the S-DSP.
- Struct initializers broke when a field was added mid-struct.
- Test harness only: the two APUs share the RSP (each one's S-DSP batches
  are now joined before the other runs) and the validity epoch (the
  reference's P changes flipped the bit that follows the translated
  APU's P).

Options to weigh along the way (from the same projects):

- **Interpreter to the exact event** (beebjit): a budget check that fails
  before a run of code means nothing in it has executed, so the interpreter
  steps to the exact event; could replace part of the per-block edge
  handling in step 5.
- **Self-patching memory calls** (gpSP): an access whose region is not known
  at translation calls a patcher that rewrites the call to the right fast
  path (plain ARAM or the I/O page).
- **Constant propagation** (beebjit): known register and carry values within
  a block (ADC with C known 0 becomes an add; stores of known values).

Not adopted: sodium64's accuracy shortcuts; beebjit's fixed host slot per
guest byte (64 KB x 64 bytes would not fit) and per-instruction
invalidation (whole-cache flush is enough so far).

## I-cache layout retrain (2026-11-19)

`n64/icache_order.ld` was trained without the translator. Retrained from a
profile of the `SPCDBT=1` build (SMW attract, frames 400-520, 23,606
samples, 277 functions placed) and measured against the committed layout,
translator off and on, four builds side by side (frames 360-1650,
`APUTIME=1`):

| Layout | Translator | Run phase | SPC time |
|---|---|---|---|
| Committed | off | 74.0 ms | 32.51 ms |
| Committed | on | 71.0 ms | 21.81 ms |
| Retrained | off | 79.1 ms | 34.86 ms |
| Retrained | on | 72.5 ms | 21.58 ms |

Not adopted. The rule was: better SPC time and run phase with the
translator on, and no regression with it off. The retrained layout gains
0.23 ms of SPC time with the translator on but its run phase is 1.5 ms
longer, and with the translator off it is 5.1 ms slower.

The committed layout is itself out of date: with today's code 40 of its
529 placed functions no longer land at their planned addresses (the first
is `dma_doDma.part.0`; the function before it outgrew its gap), which the
build reports. The comparison above is therefore between a stale layout
and a fresh one trained on a single configuration, and the stale one still
wins. A retrain worth adopting should profile both configurations, and is
better done once the 65816 changes in progress have settled, since they
move which code is hot.

## Buffer use and code size (measured 2026-11-24, whole stack, ares)

`[spcdbt-flush]`, `[spcdbt-words]` and `[spcdbt-stubs]` (every 600 frames,
totals so far) say why translations were dropped and what translation emits.

| | Joe & Mac 2 | SMW | Pitfall |
|---|---|---|---|
| flushes: SPC reset / memory replaced | 2 | 2 | 2 |
| flushes: a store into translated code | 0 | 0 | 5 |
| flushes: no room, no block records, S-DSP page | 0 | 0 | 0 |
| blocks / SPC instructions translated | 270 / 2,292 | 278 / 1,924 | 837 / 6,328 (over its flushes) |
| words, main paths | 35,415 | 31,498 | 101,491 |
| words, cold paths (stubs) | 24,953 | 23,987 | 69,530 |
| words an instruction | 26.3 | 28.8 | 27.0 |
| of the 65,536-word buffer | 92% | 85% | at most a third at a flush |

- **No flush is for room**, so a collector (as the 65816 JIT's buffer has)
  would save nothing today. But one scene of Joe & Mac 2 fills 92% of the
  256 KB: a driver a little larger flushes for room, and then everything
  is made again.
- **Two fifths of the code is cold paths.** By kind, Joe & Mac 2: a store
  to a watched page 757 stubs of 18 words (13,626: 23% of all code); the
  sample edges before a point 1,177 of 6 (6,950); the front end before a
  store 644 of 4; leaving before an instruction 380 of 4. The store stub
  is the same code each time but for an address, a register and a cycle
  count: as one shared routine and a few words a site it would give back
  about a sixth of the buffer.
- The main paths are 15 words an instruction, the 13-word link stub of
  every exit included. Not yet read instruction by instruction, as the
  65816 JIT's were (54 -> 21 words there).
- Pitfall's five flushes are stores into its own code; each drops
  everything (about 22,000 words, 110 blocks), though the store touches
  one 32-byte line. Dropping only the blocks on that line needs their
  links undone, which nothing records yet.

Order this suggests: the shared store stub and the edge stub (size, no
change to what runs); then the main paths' audit; a smaller buffer after
that; a collector or per-line dropping only if a game shows the need.

**Stubs shared (2026-11-24).** The watched-store stub is one routine
(`s_wstore`; a site passes the address, the byte and the cycle offset: 6
words, was 18), and the edge and leave stubs put half of their constant in
the jump's delay slot (5 and 3 words, were 6 and 4). Nothing on a block's
main path changed. Words an instruction: Joe & Mac 2 26.3 -> 21.7 (76% of
the buffer, was 92%), SMW 28.8 -> 23.4 (69%, was 85%), Pitfall 27.0 ->
22.5. Exact on the three games and the translator's random test; frame
times as before (Joe & Mac 2 52.8 ms against 52.9, Pitfall 55.5 / 55.7,
SMW 89.8 / 89.1, its SPC bucket 22.6 against 21.8: within what two builds
of the same code differ by, but the store routine is two jumps longer than
the stub was, so look at it again if SMW's watched stores turn out hot).
Cold paths are now 29% of the code. Next: the main paths (15 words an
instruction), then the buffer's size.

**Main paths, read by what the words are for (2026-11-24).** `[spcdbt-main]`
counts them. Joe & Mac 2 before: 15.5 words an instruction on the main
path -- link stubs 18%, stores 19%, the edge test and budget at exits 10%,
edge tests before stores and I/O 8%, front-end tests 6%, Z and N 6%, the
segments' time tests 3%; the instructions' own work 30%, under 5 words
each. So the work was done on what surrounds the instructions:

- **An exit pays its budget first** (`exit_edges`): `addiu t8; subu; bltz;
  sw t8` in the delay slot, 4 words for 6, and the edge stub's offset is 0
  (one word less there, and a computed exit loses three more).
- **A store asks about the edges and the front end at once**
  (`fe_check`): one test of the two conditions and one stub (`s_edge_fe`:
  the edges if one is due, then the front end if it is behind -- the order
  the two tests had), 6 words and a 5-word stub for 7 and two stubs.
- **A store's watch test reads one bitmap under the audio split**: the
  batch host's "page is current" bitmap is never set there.
- **Links: 3 words an exit and 10 a block** for 13 an exit. The epoch and
  time checks moved from every link stub into the target block's linked
  entry, eight words before its code, which a stub's `jal` lands on; a
  failed check goes to `s_link` with `ra` still the stub's, so the stub is
  linked again when its target was retranslated. (The entry's `slti` is
  patched with the block's longest path before the caches are flushed:
  after, and the stale word ran -- ares models the instruction cache.)

Words an instruction, at the start of the audit -> after the stubs ->
now: Joe & Mac 2 26.3 -> 21.7 -> 17.8 (62% of the buffer, was 92%), SMW
28.8 -> 23.4 -> 19.0 (56%, was 85%), Pitfall 27.0 -> 22.5 -> 18.3. Exact on
the three games and the random test at each step. Frame times did not
move beyond what two builds differ by (SPC bucket, before the audit ->
now: Joe & Mac 2 17.1 -> 17.8 ms, SMW 21.8 -> 21.2, Pitfall 11.9 -> 11.4):
the gain is room, not time.

Left, by size (Joe & Mac 2, of 40,726 words): stores 5,021 (6.4 each: a
static store is load, mask, branch, nop, store); the watched-store stubs
4,542; link stubs and entries 4,249; edge and front-end tests 4,126; nops
3,305, most in the delay slots of those tests; Z and N 2,244 (two
registers, two moves). The buffer stays 256 KB for now: at 62% a smaller
one would bring back the room question it just lost.

## Where the sound time goes (PC samples, 2026-11-24, whole stack, ares)

`n64/profile/sound_split.py` splits a `PROFILE=1` log. 120 frames of play
each; ms a frame from the share of samples and the frame's time.

| | Pitfall | Joe & Mac 2 | SMW |
|---|---|---|---|
| sound, all of it on the CPU | 11.0 | 15.2 | 22.6 |
| translated SPC code | 3.9 | 5.7 | 6.7 |
| `dsp_fe_run` (the front end's tracker) | 0.5 | 0.4 | 6.3 |
| `apu_cpuWrite` + `apu_cpuRead` (the 65816's port accesses) | 1.3 | 2.1 | 1.0 |
| `apu_runCycles_native` | 0.6 | 0.6 | 1.5 |
| `spc_runOpcode` + `spc_read` (interpreted) | 0.6 | 0.2 | 1.5 |
| `spc_dbt_edge` + `spc_dbt_fe` (sample edges, front end calls) | 0.6 | 1.0 | 1.3 |
| `dsp_fe_write`, `dsp_write`, `apu_feRead`, `apu_feNow` | 0.7 | 1.6 | 1.4 |
| `spc_dbt_valid`, `_link`, `_run`, `_may_run` | 1.2 | 0.5 | 0.6 |
| `apu_native_waitSkip`, `apu_yellow_post`, timers, the rest | 1.6 | 3.1 | 2.3 |

- Translated code is the largest part everywhere, a third of the sound
  time, and it is slow for what it is: Joe & Mac 2 runs some 8,000 SPC
  instructions a frame in 5.7 ms, about 65 CPU cycles each for under 20
  MIPS instructions. That is cache misses: a frame of the driver walks
  through far more translated code than the 16 KB instruction cache holds.
  Fewer SPC instructions run (waits), or the same ones in fewer words.
- SMW's tracker is 6.3 ms on its own.
- Everything else is a tail of parts under 1 ms each.
