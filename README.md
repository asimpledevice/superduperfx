# SuperDuperFX

SuperDuperFX turns Super Nintendo games into Nintendo 64 programs. It is a
static recompiler: the SNES game's 65816 code becomes C, compiled for the
N64's VR4300 CPU, with the rest of the SNES (PPU, SPC700, S-DSP, DMA and the
cartridge coprocessors) emulated on the N64's CPU, RSP and RDP.

It is a fork of [SNESRecomp](https://github.com/RetroPortingToolKit/snesrecomp)
that targets only the N64 (Expansion Pak required). Everything here builds an
N64 ROM, or is a tool run on a developer's machine to make one.

**Status:** in development. Super Mario World, Pitfall: The Mayan Adventure
and Joe & Mac 2 are the test games. Builds are checked in the
[ares](https://github.com/ares-emulator/ares) emulator and on real hardware
(EverDrive 64). No ROMs or game data are included: you supply your own dump.

## How it works

- **Recompiler** (`recompiler/`, `recompiler-rs/`, `tools/`): analyzes a ROM
  and emits C for its code. Code it cannot place statically runs through an
  exact interpreter at run time.
- **Runtime** (`runner/src/`): the SNES hardware model the generated code runs
  against: memory map, PPU, APU, DMA, coprocessors (Super FX, SA-1, Cx4, DSP-1,
  S-DD1), and the interpreter.
- **N64 host** (`n64/`): boot, video, audio and input on libdragon; the RDP
  renderer; RSP microcode for the S-DSP (`rspdsp/`), BG line building
  (`rspbg/`) and the SPC700 (`rspspc/`); ROM paging and a native code cache.
- **libdragon** (`third_party/libdragon/`): SuperDuperFX's own copy of the N64
  library, built by the N64 Makefiles.

## Building

You need the `mips64-elf` GCC toolchain (libdragon's toolchain package) and a
host C compiler. Point `N64_INST` at the toolchain:

```bash
export N64_INST=/opt/libdragon
```

Generate C for a game with the recompiler, then build the ROM:

```bash
make -C n64 GEN_DIR=<generated C> GEN_INC=<game config> ROM=<your .sfc>
```

The first build also builds libdragon into `build/libdragon` (about a
minute). See [`docs/n64/BRINGUP_NOTES.md`](docs/n64/BRINGUP_NOTES.md) for
the toolchain, the build options (`RENDER`, `CODECACHE`, `ROMPAGING`,
`RSPDSP`, `RSPSPC`, ...) and the measurements behind each design choice.

## Tests

```bash
tests/run_c_tests.sh
```

```bash
python3 -m pytest
```

The first runs the emulation core's C tests on your PC in seconds; the second
runs the recompiler's tests. The test ROMs in `n64/rspspc/test`,
`n64/rspdsp/test`, `n64/rspbg/test` and `n64/hwtest` check the RSP microcode
and CPU/RSP signalling in ares or on hardware.

## Acknowledgements

SNESRecomp did not start from scratch. Its runtime and tooling stand on prior
reverse-engineering and emulation work:

- **[snesrev](https://github.com/snesrev)** (`snesrev/zelda3`,
  `snesrev/smw`) — the C runner and surrounding ecosystem were heavily based
  on the snesrev reverse-engineered ports. The model of recompiling or porting
  CPU code to C, modeling the surrounding silicon, and validating against a
  reference emulator comes from that work. Runtime utilities, ROM verification,
  function-boundary conventions, and the default input layout were also
  adapted from it.
- The C SNES hardware core under
  [`runner/src/snes/`](runner/src/snes/) derives from
  **[LakeSnes](https://github.com/elzo-d/LakeSnes)** by elzo-d, as vendored by
  snesrev, with individual algorithms credited inline to **snes9x**.
- **[IsoFrieze/SMWDisX](https://github.com/IsoFrieze/SMWDisX)** — the Super
  Mario World disassembly used as the symbol and RAM-map basis and as a
  conformance reference. SMWDisX in turn credits mikeyk's original 2013
  disassembly and loveemu's SPC700 work.
- **[libdragon](https://github.com/DragonMinded/libdragon)** — the open-source
  N64 development library, which SuperDuperFX carries as its own copy in
  [`third_party/libdragon`](third_party/libdragon/).
- **[ares](https://github.com/ares-emulator/ares)** — the coprocessor cores
  credited in [`THIRD_PARTY_ATTRIBUTION.md`](THIRD_PARTY_ATTRIBUTION.md), and
  the N64 emulator used to test every build.

## License

For this project's code covered by the [PolyForm Noncommercial License](LICENSE),
[PERMISSIONS.md](PERMISSIONS.md) grants standing permission for voluntary
donations, sponsorships, and advertising revenue, including support for broader
development initiatives. Payment must not buy access to the software, games
built with it, or their updates, features, or game content, including early access.

Matthew Stanley has authority from the relevant copyright holders to grant
these permissions for the project's PolyForm-covered code. Third-party
components and code explicitly licensed under other terms retain those terms.

SNESRecomp's original code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). Copyright © 2026 Matthew
Stanley; commercial licensing inquiries go to him at <https://1379.tech>.
Third-party components retain their own licenses as documented in
[Acknowledgements](#acknowledgements) and
[`THIRD_PARTY_ATTRIBUTION.md`](THIRD_PARTY_ATTRIBUTION.md).

**If you distribute a build:** the runner statically links code whose licenses
require the notice to travel with binaries — snesrev's zelda3/smw ports and
LakeSnes (MIT) and the ares-derived coprocessor cores (ISC). Ship
[`THIRD_PARTY_ATTRIBUTION.md`](THIRD_PARTY_ATTRIBUTION.md) with any package
that contains the runner; it carries every required notice text.
