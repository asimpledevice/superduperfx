#!/usr/bin/env bash
# Build SuperDuperFX's newlib: the C library the N64 builds link, compiled with
# the reserved-FPU-register flags (n64/fpu_reserve.mk) and with setjmp/longjmp
# patched to leave the reserved registers alone. See SUPERDUPERFX.md.
#
#   third_party/newlib/build.sh [PREFIX]     (default: build/newlib)
#
# Needs the mips64-elf GCC toolchain: N64_GCCPREFIX, else N64_INST.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/third_party/newlib"
PREFIX="$(realpath -m "${1:-$ROOT/build/newlib}")"
TOOLCHAIN="${N64_GCCPREFIX:-${N64_INST:-/opt/libdragon}}"
VERSION=4.4.0.20231231
SHA512=$(cat "$HERE/newlib-$VERSION.tar.gz.sha512")
URL="https://sourceware.org/pub/newlib/newlib-$VERSION.tar.gz"
CACHE="$ROOT/build/newlib-src"   # the verified tarball, kept between runs
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

[ -x "$TOOLCHAIN/bin/mips64-elf-gcc" ] || { echo "mips64-elf-gcc not found in $TOOLCHAIN/bin" >&2; exit 1; }
FIXED="$(make -s -f "$ROOT/n64/fpu_reserve.mk" print-fixed)"

mkdir -p "$CACHE"
TARBALL="$CACHE/newlib-$VERSION.tar.gz"
if [ ! -f "$TARBALL" ] || ! echo "$SHA512  $TARBALL" | sha512sum -c --quiet - 2>/dev/null; then
  curl -fsSL --retry 3 -o "$TARBALL.tmp" "$URL"
  echo "$SHA512  $TARBALL.tmp" | sha512sum -c --quiet - || { echo "newlib checksum mismatch" >&2; exit 1; }
  mv "$TARBALL.tmp" "$TARBALL"
fi

# A fresh work directory per run: nothing is deleted or reused.
WORK="$(mktemp -d "$ROOT/build/newlib-work.XXXXXX")"
SRC="$WORK/newlib-$VERSION"
tar -xzf "$TARBALL" -C "$WORK"
for p in "$HERE"/*.patch; do patch -s -d "$SRC" -p1 < "$p"; done

mkdir -p "$WORK/build"
cd "$WORK/build"
# The same configuration as libdragon's tools/build-toolchain.sh, plus FIXED.
PATH="$TOOLCHAIN/bin:$PATH" \
CC_FOR_TARGET="mips64-elf-gcc" \
CFLAGS_FOR_TARGET="-DHAVE_ASSERT_FUNC -O2 -fpermissive $FIXED" \
"$SRC/configure" \
    --prefix="$PREFIX" \
    --target=mips64-elf \
    --with-cpu=mips64vr4300 \
    --disable-libssp \
    --disable-werror \
    --enable-newlib-io-c99-formats \
    --enable-newlib-multithread \
    --enable-newlib-retargetable-locking >"$WORK/configure.log" 2>&1
PATH="$TOOLCHAIN/bin:$PATH" make MAKEINFO=true -j "$JOBS" >"$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }
PATH="$TOOLCHAIN/bin:$PATH" make MAKEINFO=true install >"$WORK/install.log" 2>&1 || { tail -30 "$WORK/install.log"; exit 1; }
echo "newlib $VERSION installed in $PREFIX"
