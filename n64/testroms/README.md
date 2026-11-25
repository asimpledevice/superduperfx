# Hardware test ROMs (EverDrive)

Built from this repository's own test programs and libdragon. They contain
no game code or data. Rebuild with `make` in the source directory.

| ROM | Source | What to photograph |
|---|---|---|
| `regtest.z64` | `n64/regtest` | The one result screen. Lines 1, 3 and 5 matter most (MTC1 high half, FPU registers across interrupts, ErrorEPC). |
| `rdptest.z64` | `n64/rdptest` | Seven pages, A to step. Page 3 (scissor left edge), pages 4-5 (colour maths counts), page 6 (timing of one layer three ways) and page 7 (the cost of a CPU cache miss in cycles, I and D: idle, with the RDP drawing, with RSP DMA running, with the cartridge being read; and of an uncached word). |
| `scenetest.z64` | `n64/rsptile/scenetest` | Optional. The final PASS/FAIL line and pass count. |

## Built on request (not kept here)

| ROM | Build | What to photograph |
|---|---|---|
| `ppu_test.z64` | `tools/ppu_test_rom.py` for the SNES image, then `make JIT816=1 SCENE=1 RENDER=2 "EXTRA_CFLAGS=-DN64_COMPOSE_RSPTIME" ROM=...` | The status line once frame `f` has passed 380 (one pass of the 31 scenes; it loops). `bad` is the frames whose RDP lines differed from the software PPU's (want 0), `s` the frame's software lines, `R a/b` and `C a/b` a composed line's time in microseconds on the RSP and on the CPU: the last full-screen band and the slowest (frames alternate between the two). |
| a game with the RDP's time | `make ... "EXTRA_CFLAGS=-DN64_PRESENT_PROF -DN64_PRESENT_RDPWAIT"` with the game's options | Both status lines in a level: `cpu`, `ppu` in ms; `c` commands, `b` BG wait, `a` audio, `p` RDP pipe busy, `t` TMEM busy in tenths of a ms. |
