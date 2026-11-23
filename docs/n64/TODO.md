# N64 TODO

Open items, by area. Details and measurements live in the design notes
linked from each item.

## APU (audio split, S-DSP microcode)

See `RED_DESIGN.md` ("The audio split, as built", "The voice-major overlay
for pitch modulation", "The paging slot", "Pitch modulation as HLE").

1. **Make the audio split the default build.** `RSPDSP_STREAM` defaults to
   0. Run it on the EverDrive (correctness and frame time against the
   default build), then flip the default.
2. **Echo prefetch on hardware.** A/B the voice-major variant with and
   without the prefetch (`RSPDSP_VM_ECHO_SYNC=1`) on the EverDrive, using
   the RSP test ROM with `RSPDSP_SCREEN` (every voice playing; ares: 1,083
   with, 1,058 without, SM 878 RSP cycles a sample). Keep whichever wins.
3. **Echo-conflict voices are inexact.** A voice that reads echo bytes the
   S-DSP has not written yet is silenced (the stopgap). Rare; an exact
   answer is still to design.
4. **Echo buffers at $0000 or wrapping past $FFFF.** The microcode comments
   say such batches go to the C fallback; check how stream mode handles
   them (there is no C fallback on Blue there).
5. **Listen to the pitch-modulation HLE.** Render a passage with modulated
   voices both ways (`RSPDSP_PMOD_HLE=1` and the exact default) and
   compare by ear.
6. **SPC idle-loop skipping (exact)**, for CPU time on Blue.

Parked: the exact sample-major pitch-modulation variant (branch
`pm-variant-wip`; does not fit IMEM, see RED_DESIGN.md).

## The whole stack together (2026-11-22, ares)

`JIT816=1 SPCDBT=1 RSPDSP_STREAM=1 SCENE=1`, ms a frame, exact against the
truth build on all three: SMW 88.1 (frames 360-2400), Joe & Mac 2 64.5
(360-1300; 217 without the scene: its big tiles sent every line to the
planes), Pitfall 66.3 (760-1400). At frame 1020, CPU phase / line build /
present: Joe & Mac 2 23 / 6 / 7 ms, Pitfall 40 / 9 / 9.5 ms.

- **Memory is now the limit in SMW.** With everything on, the heap ran out
  with 866 KB of compiled routines resident (the budget says 1,536 KB) and
  the game stopped. The code cache now evicts and lowers its capacity when
  a load cannot get memory (`recomp_cache_make_room`): SMW runs on, at
  about 900 KB resident and one reload every five frames or so. Static
  buffers to look at when the memory budget is taken up: `s_jit_code` 384
  KB, the two 256 KB stacks, the SPC translator's `s_code` 256 KB and
  `s_blocks` 102 KB, `s_jit_ent` 128 KB.
- **Echo writes are off in every scene measured** (FLG bit 5 set: SMW's
  and Joe & Mac 2's attract sequences, Pitfall's first level; Pitfall's
  ring is 4 bytes at $FF00). The split's echo paths with the translator --
  read bits on the ring, diverted stores, conflicts -- have not run in a
  game here. Find a scene with echo on (a later SMW level, or another
  game) before the split becomes the default.
- Joe & Mac 2, CPU phase: translated SPC code 20%, `spc_dbt_valid` 6.5%
  (the "code" refusals), `spc_runOpcode` 5%. Pitfall, CPU phase: the 65816
  is over a third (translated code 13%, lean loop 10%, stores 6%).

## Audio: which configuration, and what each costs (2026-11-22, ares)

ms a frame, same frames, `JIT816=1`; all exact against the truth build.

| | SMW | Joe & Mac 2 | Pitfall |
|---|---|---|---|
| batches + SPC translator (`SPCDBT=1`) | 87.5 | 222.4 | 88.4 |
| audio split (`RSPDSP_STREAM=1`), SPC interpreted | 112.8 | 233.5 | 87.8 |
| batches, SPC interpreted | 90.1 | 226.8 | 103.1 |
| **audio split + SPC translator** (since 2026-11-22) | 93.5 | **217.3** | **73.8** |

The split and the translator now build together (`SPCDBT=1 RSPDSP_STREAM=1`):
the split's "may leave the fast path" bits live in `Spc.dspWatchLog` and
`dspWatchSync`, the tables the translator already checks; a new read bit
is news to the translator (`apu_streamSetSlow`); while a voice is
untracked every store takes the slow path (the ledger). Exact on the three
games against the truth build and against the split with the SPC
interpreted. Not covered: `SPCDBT_TEST` (it runs two APUs; the split's
S-DSP is one per system), and the page-bit check build compares the
interpreter's accesses only. A block's validity check asks the exact read
test, which clears stale bits (nothing else does for a page only blocks
read): "watched read" refusals went from about 290 a frame to none. The
"code" refusals (Joe & Mac 2: about 390 block runs a frame) were indexed
stores: the driver's per-voice tables lie just below its code (`$0511+X`
and the like, code from `$0600`), a block's store range for `abs+X` is the
256 bytes the index could reach, and a block whose range holds a line of
translated code never runs. Such a store now looks at the line it really
goes to (the code bitmap, nine instructions) and leaves before the
instruction if that holds code, as a store through a pointer already did;
it is emitted only where the range holds code lines when the block is made,
and a block refused later for such a range is dropped from the map and
made again (`SPC_DBT_INDEXED_STORE_CHECK`, 2026-11-23). Refusals: none in
the three games. Joe & Mac 2's SPC bucket 20.4 -> 15.7 ms a frame (total
57.4 -> 52.5), SMW 24.3 -> 23.2 (88.3 -> 87.4), Pitfall unchanged (57.1);
exact on all three, and the translator's random test passes its 32
programs. `-DSPC_DBT_REFUSED_LOG` lists the blocks and ranges refused.

What the profiles say:

- The split removes the batch bookkeeping (log, launch, join, decode) but
  runs the front end on Blue. In SMW that is about 630 BRR blocks a frame
  at some 25 us each (16 ms; `dsp_fe_run`, `apu_feRead`, `apu_feEcho`,
  the envelope), against 3.5 ms of batch bookkeeping; in Joe & Mac 2 and
  Pitfall the front end is small (about 35-40 blocks a frame) and the SPC
  interpreter is what is left (60% of Joe & Mac 2's CPU phase).
- Front-end runs are short (1.2 samples with work each, 300-400 a frame in
  SMW) and are started by the SPC's writes of the registers the front end
  follows (pitch, key on and off, ADSR), not by ARAM stores or echo
  accesses (0 off the fast path). Tried and dropped, exact but no gain:
  bringing it up to date only for stores at or above the lowest page a
  pending read can be on, and only for the registers it follows.
- Kept (small, exact): the envelope catch-up jumps between changes, and a
  bit per page decides the SPC's fast ARAM path.

So, in this order: (1) the SPC translator under the split (the one thing
that helps every game there); (2) queue the followed register writes with
their sample and run the front end once a batch, so its 630 blocks are
done in one warm pass; (3) hardware: all of this is ares, and the split's
case rests on the RSP running beside the CPU.

## Where the CPU phase goes (profile, 2026-11-21, ares, frames 900-1020)

PC samples of the CPU phase, `JIT816=1`, with and without `SPCDBT=1`.

- **Joe & Mac 2**: 35 ms a frame without the SPC translator, 31.5 with.
  About 80% is audio either way. With the translator: translated SPC code
  20%; the S-DSP hand-off on Blue about 35% (`apu_dspLogRam` 8%,
  `snesdsp_rsp_apu_launch` 6%, `decode` 6%, `snesdsp_rsp_apu_join` 6%,
  `apu_dspMarkDirEntryAt` 3%, sync, pack, unpack); `spc_dbt_valid` 4%,
  `spc_dbt_slow_write` 3%. The 65816 is about 3%.
- **Pitfall**: 59 ms a frame either way. The SPC translator gives nothing
  there because it keeps translating: `translate` 6%, `emit_insn` 1.5%,
  the cache flushes 2%. Find what invalidates its blocks. Audio in all is
  about 40%, the 65816 about 25% (translated code 9%, lean loop 6.6%, big
  loop 1.8%, `jit_translate` 1%: the 65816 buffer flushes too).

**(1) is done** (2026-11-21, `SPC_DBT_VOLATILE_LINES`): Pitfall's driver
stores into its own code at $1A94 (and once at $1901), and each store
dropped every translation. A 32-byte line that is stored into while it
holds translated code is left to the interpreter from then on. Pitfall,
frames 760-1400: flushes 10-60 per 60 frames -> 2 in all, SPC bucket 30.6
-> 21.4 ms, frame 96.2 -> 88.4 ms. Next in the SPC translator: Joe & Mac 2
refuses about 360 block runs a frame ("refused: code": a block that could
store into a line of translated code).

Order this suggests: (1) the SPC translator's retranslation in Pitfall;
(2) the S-DSP hand-off's bookkeeping on Blue; (3) 65816 code density.
The 65816's remaining opcodes are a small part of these scenes.

## 65816 JIT (`JIT816=1`, `runner/src/snes/jit816.inc`)

Exact on Super Mario World, Joe & Mac 2 and Pitfall against the
interpret-only truth build. In Pitfall's gameplay about 87% of formerly
interpreted instructions run translated, at roughly 1.6 us each against
7.3 us in the interpreter's core (one PC-sample profile). Owner direction
(2026-11-20): the interpreter is the fallback and the reference; anything
that runs in steady state should be translated.

1. **The remaining opcodes.** Done 2026-11-23 for all but 17: 239 of 256
   have code. Without: BRK, COP, RTI, WDM, STP, WAI, XCE (they leave the
   run or halt), MVN and MVP (the lean loop steps them), the indirect
   jumps and JSR (a,X) (the lean loop interprets them after a peek), BRL,
   PER, PEI and PEA (the interpreter's PEA has a special case for one
   operand value, `0xC2AE`, that clears Y: find out what it is for before
   translating it). `[jit816-end]` (translation), `[lean-interp-op]`
   (`N64_JIT_EXITS`, what the lean loop interprets) and `[jit816-xlat]`
   (which opcodes a run translated, a 256-bit map) rank what a scene uses.
   Added this round: the direct page indexed modes, shifts and bit tests of
   memory, absolute long, (dp), (dp),Y, (dp,X), [dp], sr,S and (sr,S),Y,
   the stack and status operations (PHB PHK PHD PHP PLB PLD PLP), the flag
   operations, and the transfers with S and D. A block ends after an
   instruction that may change what its key or its entry checks hold
   (`jit_op_ends`: the widths, D, the decimal and interrupt flags).
   Whole stack, ms a frame, previous commit -> now: Joe & Mac 2 62.2 ->
   57.4 (65816 buckets 15.1 -> 10.4), SMW 90.2 -> 88.3, Pitfall 56.9 ->
   57.3 (unchanged within the build-to-build wobble); exact on all three.
   **A bug found on the way:** the table of blocks seen once
   (`s_jit_seen`) was indexed by the address XOR the widths, so a REP or
   SEP of one width bit and the instruction two bytes after it shared an
   entry and put each other's first visit out for good; neither was ever
   translated. Joe & Mac 2 interpreted some 90 instructions a frame for
   it. The widths now sit seven bits up.
   **Only 32 of the 113 new opcodes are translated by the three games'
   measured scenes.** The rest is covered by the torture test below.

   **Differential test** (`n64/tools/jit816_torture.sh [seed]`): a
   generated LoROM image of random 65816 programs and nothing else
   (`jit816_torture.py`; no game code or data) runs in ares under the
   interpreter (`JIT816=0`) and under the JIT, and the WRAM hash of every
   second frame must agree. Each test sets the stack, D, the data bank, the
   widths, the flags and the registers, runs 6-28 random instructions (all
   documented opcodes except those that leave the test; forward branches,
   counted loops, calls, jumps, block moves), then stores A, X, Y, P, S
   and D. Tests run in sets of 16, each set 8 times, because a block is
   translated on its second visit; a seed that every result changes is
   mixed into the next test, so the passes differ. The frame's end cuts
   the run at a master-clock deadline, so a wrong cycle count shows as
   well: a planted error of one cycle in TXY made every sampled frame
   differ, the first included. The generator keeps every access in RAM or
   ROM (data bank $7E, direct page at most $1C00, an index register used
   with a direct-page mode only while known small, long pointers written
   just before use), which is what lets RAM alone be compared. Seeds 2 to
   5: 634, 1,002, 1,094 and 1,130 frames, no difference; about two thirds of the
   instructions run translated. Run it after any change to the translator.
   Not covered: I/O registers from translated code (the games cover
   those), emulation mode and code in RAM (the JIT does not run there).
   Also fixed: `JIT816=0` builds again (a type the plain loop uses was
   declared inside the lean loop's conditional).
2. **Calls, returns and jumps in translated code** (JSR, JSL, RTS, RTL,
   JMP, JSR (a,X)): they are most block ends in Pitfall's gameplay, and
   each goes round the big loop, which is what keeps the 9 KB
   `_interp_run_core` in the instruction cache.
   Measured 2026-11-21 (`N64_JIT_EXITS`, `[lean-break]`): a trip round the
   big loop costs about 50 us. Transfers that are not forward were the
   larger share and now stay in the lean loop while A, X or Y change
   (`SNESRECOMP_LEAN_BACK_LAPS`; Pitfall 196.9 -> 171.3 ms a frame over
   frames 360-900, Joe & Mac 2 -3 ms, SMW neutral, all exact), as do block
   moves (`SNESRECOMP_LEAN_MOVES`). What remains is the opcodes themselves
   (Pitfall: about 160 a frame). What the big loop does for them, which a
   translation has to keep:
   - a call whose target has a compiled body is run compiled ("bounce"):
     ask `cpu_dispatch_has_entry` before the call is executed (for
     JSR (a,X), peek the pointer first) and leave the block if it has;
   - a return past the run's entry depth (`s_enter`, unless `yield_pc` or
     `stop_on_rti`) ends the run: compare S before pulling, leave if so;
   - `tail_frames` (calls recorded for tail jumps into compiled routines):
     clear them (`tail_n = 0`) after any translated call or return;
   - a jump back is a loop head for the wait detector: as branches.
   **Done 2026-11-21 for JMP, JML, JSR, JSL, RTS, RTL** (`SNESRECOMP_JIT_XFER`,
   direct targets in ROM without a compiled body; a return is a block of
   its own; the call records are kept by `jit_tail_call`/`jit_tail_ret`).
   Exact on the three games. Lesson: which routines run compiled shows in
   the state hashes (a first version that dropped the call records changed
   which jumps ran compiled, and SMW's WRAM hash differed for a few sync
   points at a time), so a change may not alter that. Gain is small so far
   (SMW 90.7 -> 89.9 ms, Joe & Mac 2 228.1 -> 227.3; Pitfall's loading
   frames 2% fewer profile samples). Still to do: JSR (a,X), JMP (a),
   JMP (a,X), JML [a] (Pitfall: JSR (a,X) is the most frequent one left),
   and calls whose target has a compiled body.
   **The code buffer now fills** in Pitfall's loading scene (4 flushes
   against 1): a translated instruction is some 30-60 words. Make the code
   denser (item 6; shared exit stubs) before growing the buffer.
   Fewer bounces were counted with the lean-back rule (Pitfall 16 against
   33 a frame): find which entry into compiled code the big loop made at
   those addresses and whether it is worth keeping.
3. **Translate on the first visit?** A block is made the second time its
   start is reached. Measure what a translation costs against interpreting
   the block once before changing it.
   The aim (owner, 2026-11-20): everything gets translated, and the
   interpreter and the translator both go quiet once a scene settles.
   Storage is not the limit (384 KB buffer, flushed when full); what costs
   is the translator's own code running through the caches, so translate
   once and keep, rather than translate and discard.
   **The code buffer keeps what runs (2026-11-24, `SNESRECOMP_JIT_EVICT`).**
   A full buffer used to be flushed whole. Measured with smaller buffers
   (`SNESRECOMP_JIT_CODE_KB`), whole stack, ms a frame:

   | buffer | policy | Joe & Mac 2 | Pitfall, play | Pitfall, loading (frames 400-1300) |
   |---|---|---|---|---|
   | 384 KB | flush | 52.5 | 57.1 | 117.3 |
   | 192 KB | collect | 52.4 | 56.3 | 115.8 |
   | 96 KB | flush | 53.5 | (106 flushes; did not reach play in the run) | 169.8 |
   | 96 KB | collect | 52.6 | 67.5 | 129.9 |
   | 48 KB | flush | 74.2 | (216 flushes) | - |
   | 48 KB | collect | 66.2 | 72.1 | 134.8 |

   Now: blocks are placed one after another, each with a header; a block
   is made in the buffer's last 12 KB and copied to its place (its code is
   position independent but for one address). When there is no room, a
   collection drops the blocks that have not run since the last one and
   moves the rest down together. If that frees less than an eighth, or
   comes within 25,000 instructions of the last, the buffer is full of
   code that runs: nothing is made for 100,000 instructions and what does
   not fit is interpreted. The reason is the price of a block: making one
   costs about five times interpreting it once, so evicting code that runs
   to make code that will be evicted in turn is the worst thing to do. Two
   versions that did that were measured and dropped on the torture ROM at
   48 KB (one the buffer cannot hold a set of): a hand that evicted in
   address order, 510 ms a frame against the flush's 348; the collector,
   198. A third, letting the coldest quarter leave under pressure by a
   count of instructions run, took Pitfall at 96 KB from 67 to 90 ms: the
   newcomers were no hotter.
   The default buffer is now 192 KB and its table of entries 64 KB (was
   384 and 128): 264 KB for the JIT in all, against 520.
   At that default: exact on the three games (3 to 4 collections each in
   Pitfall and Joe & Mac 2, none in SMW) and on the torture ROM (922
   frames, 193 collections; also at 48 KB, where it collects constantly).
   Pitfall 55.6 ms a frame, Joe & Mac 2 52.5, SMW 89.4 (87.4 before; SMW
   never fills the buffer and its 65816 buckets are unchanged, the
   difference is in the sound and "other" buckets, as between other
   builds).
   `-DN64_JIT_BUFLOG` logs the turnover every 60 frames (`[jit816-buffer]`:
   blocks made, runs, collections, dropped, kept) in any build.
   Left: a game whose running code is larger than the buffer interprets
   the overflow first come, first kept; nothing ranks blocks by how much
   they run (the heat count above did not pay with Pitfall's flat
   profile, but a game with a few very hot loops among much lukewarm code
   may differ). The same question for the SPC translator's buffer, which
   still flushes whole.
4. **What is "not worth translating" is decided by counting, not by kind.**
   Decimal-mode ADC/SBC, emulation mode, an instruction across an 8 KB page
   boundary and code in RAM stay with the interpreter today. If a game runs
   one of them every frame (a decimal score counter), it pulls the
   interpreter back into the caches each time and belongs in the JIT.
   **Density audit, 2026-11-22** (`[jit816-words]`, `N64_JIT_DUMP=n` logs
   blocks' code for `objdump -b binary -m mips:4300`). Pitfall before: 54
   MIPS words per 65816 instruction -- the instructions' own work 30%
   (about 20 words each), the step end 21% (14 each), exit stubs 15% (about
   10 each), branches 22% (about 90 each), transfers 7%, tails 5%.
   Stage 1, done, exact on the three games: no `nop` after `mfc1`/`dmfc1`
   (the VR4300 interlocks; the project's assembler emits none either), one
   base register (s7) for the globals a block uses, a store's write-log
   bookkeeping once instead of twice: **44 words**. Pitfall 66.3 -> 62.5
   ms a frame (65816 bucket 18.2 -> 13.9), SMW 88.1 -> 85.9, Joe & Mac 2
   64.5 -> 66.0 (its translation counts improved; the difference is in
   buckets this does not touch).
   A block translated before a flush is made again on its first visit
   (a state of `s_jit_seen`; a separate 8 KB table for it cost Joe & Mac 2
   3 ms a frame in data-cache misses: no new tables on these paths).
   Stage 2a, done, exact: the clock as one register, the time left before
   the event (`JR`), written out to the clock register, the coprocessors'
   clock and the SPC's pending time before a C bus function or the way out
   and taken again after; step end 12 -> 4 words. And a byte store to
   plain WRAM is made by a shared routine (`s_jit_st8`: the byte, its
   speed, the write epoch, the latch, the bus address) when nothing watches
   stores (`cpu_wram_store_plain`), instead of bridge_bus_write: 38 words
   an instruction; Pitfall 62.5 -> 59.2 ms a frame (65816 bucket 13.9 ->
   10.2), Joe & Mac 2 66.0 -> 64.8, SMW 85.9 -> 86.5. (The clock change
   alone slowed Joe & Mac 2's store-heavy scene: every store's C call had
   the clock to hand over and back. The fast store removes the call.)
   Stage 2b, done, exact: one shared way out (`s_jit_xcommon`), a thunk a
   block and a one-word table entry an instruction, the index in the step
   end's delay slot: 25 words. Stage 2c, done, exact: a store's latch and
   write-log pc go to the store routine as two registers and are used only
   on its slow path; pushes and calls the same: **23 words** (54 at the
   audit's start). Pitfall's buffer no longer flushes in the measured run.
   Whole stack, ms a frame, before the audit -> now: Pitfall 66.3 -> 60.2
   (65816 buckets 25.4 -> 15.2), Joe & Mac 2 64.5 -> 64.4 (18.0 -> 16.2),
   SMW 88.1 -> 87.9 (7.8 -> 7.6; it has 586 translated instructions).
   Stage 2d, done, exact (2026-11-22/23): A, X and Y in s0/s5/s6 across a
   block; the direct page's low-byte penalty in the block's key instead
   of 7 run-time words; a constant cycle cost as one `daddiu`: **21
   words**. No time gained by the last step: its denser blocks landed on
   the shared routines' cache lines (a PC profile showed the added samples
   on the entry words of the read routine and the way out), so blocks now
   keep off the word indexes, modulo the 16 KB cache, that the shared
   routines occupy (`SNESRECOMP_JIT_KEEP_SHARED`, a tenth of the buffer).
   The lean loop's own time still moves by about 1 ms with the blocks'
   positions (Pitfall): the same question for `bridge_lean_run` and
   `jit_find` belongs to the layout retrain (item 10).
   N and Z in an ordinary register: looked at and left; the lazy register
   is already one `mtc1` away, so it would save about half a word an
   instruction.
   Left: (d, done) A, X, Y in ordinary registers across the block (the "work"
   is now about 12 words an instruction, much of it mfc1/mtc1 and the lazy
   N/Z); branches about 25 words; the 384 KB buffer is not yet small
   enough to cut (Pitfall's translated set is 2,771 instructions, 254 KB).
   `N64_JIT_DUMP=n` also logs the shared routines (`[jitshared]`).
5. ~~Stores to plain RAM still call the bus function~~ (done: `s_jit_st8`).
6. **Per-instruction bookkeeping.** Each translated instruction adds its
   clocks, the SPC's pending time and compares with the next event; do it
   once per straight run.
7. **Chain blocks**: a branch out to an address that is translated jumps
   there instead of returning to the lean loop.
8. **Export the block list** (address, M/X) so the recompiler compiles
   those routines ahead of time; later, run the ROM in a dummy system
   during recompilation to find them (owner, 2026-11-20).
9. **Before making it the default:** the register test on the EverDrive
   (`n64/regtest`), and a long soak per game.
10. **Layout.** The lean loop is in `.text.unlikely`, wherever the link puts
    it; the layout was trained without the JIT, the second interpreter core
    or the SPC translator. Owner: retrain near the end.

## PPU (scene builder, `SCENE=1`)

See `PPU_DESIGN.md` ("Step 4: the scene builder", "The RSP must never wait
on the RDP mid-command").

1. **Hardware RDP and RSP time.** Measure present on the EverDrive with
   `SCENE=1` against the planes; ares models neither unit's time.
2. **Clip-to-black coverage.** Find a scene that clips main pixels to black
   through the colour window and check it with RENDER=2.
3. **Mid-frame VRAM or OAM changes.** The scene draws from one VRAM
   snapshot per frame; sprites use OAM as of the frame's first scene line
   (later lines fall back after a change). BG layers have no such check: a
   frame that writes VRAM in a mid-frame forced blank draws its earlier
   lines from the later VRAM.
4. **Double-buffered tile caches and TLUTs**, so the next frame's sync never
   waits for the RDP (`rdpq_fence`) and TLUT slots are never rewritten
   while the RDP may read them; frame pacing in Green; an RDP watchdog.
5. **Math and black TLUTs on the RSP** (260 us of CPU a frame in SMW's
   attract demo).
6. **Modes 2-7** in the scene (software lines today). Big tiles are done
   (2026-11-20): four cache tiles per tilemap entry.
   **Layer colour maths:** the fixed-colour case is done through palettes
   (`tl_fix`, `N64PPU_SCENE_TL_FIXED`), checked by RENDER=2 only on Joe &
   Mac 2's white flash (add, no halving, all layers). Subtract, halving, a
   colour window and the OBJ palette 0-3 exemption are coded but no test
   game shows them: write a generated-scene check. Layer maths with a
   composed subscreen still falls back (needs two colours per pixel; see
   rdptest page 5 on hardware).
7. **Sprite fallback lines.** Lines with range or time over, or a front
   sprite of lower priority overlapping one of higher priority, use the
   planes. An exact per-pixel answer on the RDP is still to design.
