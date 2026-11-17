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
6. **Modes 2-7 and big tiles** in the scene (planes today).
7. **Sprite fallback lines.** Lines with range or time over, or a front
   sprite of lower priority overlapping one of higher priority, use the
   planes. An exact per-pixel answer on the RDP is still to design.
