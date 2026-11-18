# SPC700 translator (n64/src/spc_dbt.c, `SPCDBT=1`)

Runs the SPC700 sound driver as MIPS code generated at run time, exactly:
the state hashes (WRAM, ARAM, S-DSP output) match the interpreter's.

## Where it stands (2026-11-18, SMW attract, ares)

| | Run phase per frame |
|---|---|
| Interpreter (B wait skip + A run loop) | 74.2 ms |
| Translator, blocks chained | 70.4 ms |

- Translated code covers about 90% of SPC cycles; opcodes not covered are
  2-4% of cycles in SMW, Pitfall and Joe & Mac 2.
- Per 60 frames: ~100k chained block ends, ~48k stale epoch, ~7k next block
  not translated, ~3.5k exits that never chain, ~1.2k out of cycles.
- Half of the time in translated code is in the shared exit/chain/prologue
  routines; blocks average ~3 SPC instructions.

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

1. **Stale epochs** (largest chain breaker). apu_dspSyncFor/Join bump the
   epoch on every call; bump only when they clear marks that were set.
   **Untranslated targets**: translate a block's direct targets when it is
   translated (gpSP's worklist), so chains do not stop at first visits.
2. **One frame per run.** Save s0-s8 once when C enters translated code;
   blocks lose their prologue/epilogue; C helpers spill only what they
   clobber.
3. **Patched direct links.** A direct exit becomes: epoch compare, cycle
   check, `j target` patched once the target exists; links reset on flush.
   Indirect exits (RET) keep the map lookup. Replaces the shared chain
   routine (~40 instructions per hop).
4. **Countdown budget in a register** (cycles to the end of the run or the
   next queued CPU write), replacing the limit/used bookkeeping; sample
   edges keep their exact handling before stores, I/O and exits.
5. **Superblocks.** Continue through conditional branches (the taken side
   is an inline exit); branches back into the block become direct jumps
   behind the budget check, so driver loops stay in one block. Needs edge
   timing from a running cycle register instead of static offsets.
6. **Cheaper checks and flags.** One per-page "store needs care" byte
   (S-DSP log marks, view-fresh pages, translated code) instead of two
   loads per store; dead N/Z/C write elimination within a block.
7. **Coverage.** The remaining opcodes ([dp+X], [dp]+Y, dp,dp and dp,#imm
   ALU forms, PSW push/pop, TCALL/PCALL, BRK/RETI).
8. **Validation and default.** An SPC test ROM for the N64 (the native
   core's random-program contract, apu_native_test, run against the
   translator); all three games' hashes; EverDrive timing; then decide
   whether SPCDBT becomes the default.

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
