# Hardware test ROMs (EverDrive)

Built from this repository's own test programs and libdragon. They contain
no game code or data. Rebuild with `make` in the source directory.

| ROM | Source | What to photograph |
|---|---|---|
| `regtest.z64` | `n64/regtest` | The one result screen. Lines 1, 3 and 5 matter most (MTC1 high half, FPU registers across interrupts, ErrorEPC). |
| `rdptest.z64` | `n64/rdptest` | Six pages, A to step. Page 3 (scissor left edge), pages 4-5 (colour maths counts) and page 6 (timing of one layer three ways). |
| `scenetest.z64` | `n64/rsptile/scenetest` | Optional. The final PASS/FAIL line and pass count. |
