# newlib in SuperDuperFX

The N64 builds link newlib, the C library that comes prebuilt with libdragon's
GCC toolchain. SuperDuperFX rebuilds it from source so that it follows the
same FPU register reservation as the rest of the program (`n64/fpu_reserve.mk`,
and "The FPU register split" in `docs/n64/BRINGUP_NOTES.md`).

## Origin

newlib 4.4.0.20231231, the version libdragon's `tools/build-toolchain.sh`
uses, from `https://sourceware.org/pub/newlib/newlib-4.4.0.20231231.tar.gz`.
`newlib-4.4.0.20231231.tar.gz.sha512` is the SHA-512 sourceware publishes
for that file (`pub/newlib/sha512.sum`); `build.sh` refuses any other file.

The source is downloaded by `build.sh` rather than kept in the repository.

## Changes from the origin

- **Compiler flags.** Built with `SDFX_FPU_FIXED` from `n64/fpu_reserve.mk` added
  to the toolchain script's `CFLAGS_FOR_TARGET`, so compiled newlib code uses
  only `$f0-$f4`, `$f12` and `$f13`.
- **`setjmp-no-fpr.patch`.** `setjmp`/`longjmp` (hand-written assembly the
  flags cannot reach) save and restore no FPU registers. libdragon's o64 ABI
  had them save `$f20-$f31`, all of which are reserved; a `longjmp` would have
  rolled them back to their values at `setjmp` time.

Configure options are otherwise those of libdragon's toolchain script.

## Building

    third_party/newlib/build.sh [PREFIX]      # default build/newlib

`N64_INST` (or `N64_GCCPREFIX`) points at the GCC toolchain.

libgcc, the compiler's own support library, needs no rebuild: none of its
code touches a reserved register (checked by disassembly).
