# libdragon in SuperDuperFX

This is SuperDuperFX's own copy of libdragon. libdragon is public domain (see
`LICENSE.md`), and this copy is owned and changed here like any other source.
Nothing is pulled from upstream.

## Origin

`git archive` of DragonMinded/libdragon, branch `preview`, commit
`39d0d6096130836a65710da6730290701d060272` (2026-09-15). That is the commit the
N64 builds used from `/opt/libdragon` before this copy existed.

Kept: `src/`, `include/`, `boot/`, the top-level `Makefile`, `n64.mk`, the
linker scripts (`n64.ld`, `dso.ld`, `rsp.ld`), `libdragon.version`,
`LICENSE.md` and `README.md`.

From `tools/`, only what the ROM builds run, plus a few debugging aids:
`n64tool`, `n64sym`, `n64symdump`, `n64elfcompress`, `ed64romconfig`,
`n64metadata`, `mkdfs`, `dumpdfs`, `rdpvalidate`, their `common/` code, and
`build-toolchain.sh` (to rebuild the GCC toolchain).

Left out: `examples/`, `tests/`, `website/`, `scripts/`, the Docker and
`build.sh` install scripts, and the asset converters SuperDuperFX does not use
(`audioconv64`, `videoconv64`, `mksprite`, `mkfont`, `mkmodel`, `mkmaterial`,
`mkasset`, `n64dso`, `cpaktool`).

## Changes from the origin

- `tools/Makefile`: `TOOLS` lists only the tools kept.

Record every further change here.

## Building

The N64 Makefiles include `n64/libdragon.mk`. It builds this copy and installs
it into `build/libdragon`, which becomes `N64_INST`. Only the GCC toolchain is
external: `N64_INST` (or `N64_GCCPREFIX`) in the environment points at it.
