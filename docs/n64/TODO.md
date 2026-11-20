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

## 65816 JIT (`JIT816=1`, `runner/src/snes/jit816.inc`)

Exact on Super Mario World, Joe & Mac 2 and Pitfall against the
interpret-only truth build. In Pitfall's gameplay about 87% of formerly
interpreted instructions run translated, at roughly 1.6 us each against
7.3 us in the interpreter's core (one PC-sample profile). Owner direction
(2026-11-20): the interpreter is the fallback and the reference; anything
that runs in steady state should be translated.

1. **The remaining opcodes.** About 90 of 256 have code; a block ends at
   the first one without. `[jit816-end]` (translation) and
   `[jit816-exit-op]` (`N64_JIT_EXITS`, run time) rank what is missing.
2. **Calls, returns and jumps in translated code** (JSR, JSL, RTS, RTL,
   JMP, JSR (a,X)): they are most block ends in Pitfall's gameplay, and
   each goes round the big loop, which is what keeps the 9 KB
   `_interp_run_core` in the instruction cache.
3. **Translate on the first visit?** A block is made the second time its
   start is reached. Measure what a translation costs against interpreting
   the block once before changing it.
4. **What is "not worth translating" is decided by counting, not by kind.**
   Decimal-mode ADC/SBC, emulation mode, an instruction across an 8 KB page
   boundary and code in RAM stay with the interpreter today. If a game runs
   one of them every frame (a decimal score counter), it pulls the
   interpreter back into the caches each time and belongs in the JIT.
5. **Stores to plain RAM** still call the bus function (reads do not).
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
7. **Sprite fallback lines.** Lines with range or time over, or a front
   sprite of lower priority overlapping one of higher priority, use the
   planes. An exact per-pixel answer on the RDP is still to design.
