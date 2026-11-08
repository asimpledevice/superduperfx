#!/usr/bin/env bash
# Build the N64 host's runtime configuration natively for profiling.
#   n64/profile/build.sh [extra CFLAGS...]   ->  n64/profile/build/n64host_linux
# Run: N64_PROFILE_ROM=game.sfc N64_PROFILE_FRAMES=300 n64/profile/build/n64host_linux
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
N64="$HERE/.."
RUNNER="$N64/../runner/src"
OUT="$HERE/build"
mkdir -p "$OUT"
SRCS=$(sed -n '/^RUNNER_SRCS = /,/^$/p' "$N64/Makefile" | sed 's/RUNNER_SRCS = //; s/\\//' | tr -s ' \t\n' ' ')
DIAG=$(sed -n '/^N64_CFLAGS += -DSNESRECOMP_AUDIO_TRACE_HISTORY/,/^$/p' "$N64/Makefile" | sed 's/N64_CFLAGS += //; s/\\//' | tr -s ' \t\n' ' ')
CFLAGS=(-std=gnu17 -O2 -g -ffunction-sections -fdata-sections "-I$N64/include" "-I$RUNNER" "-I$RUNNER/snes" "-I$RUNNER/desktop"
  -DSNESRECOMP_TRACE=0 -DSNESRECOMP_EXECUTION_DEFAULT_MODE=SNESRECOMP_EXECUTION_MODE_OFF
  '-DN64_ROM_PATH=getenv("N64_PROFILE_ROM")' -include stdlib.h $DIAG "$@")
OBJS=()
for s in $SRCS; do
  o="$OUT/$(echo "$s" | tr / _).o"
  gcc "${CFLAGS[@]}" -w -c "$RUNNER/$s" -o "$o" &
  OBJS+=("$o")
  while [ "$(jobs -r | wc -l)" -ge "$(nproc)" ]; do wait -n; done
done
for s in n64_host.c n64_stubs.c n64_ppu_build.c; do
  o="$OUT/$s.o"; gcc "${CFLAGS[@]}" -w -c "$N64/src/$s" -o "$o"; OBJS+=("$o")
done
gcc "${CFLAGS[@]}" -w -c "$HERE/linux_platform.c" -o "$OUT/linux_platform.o"; OBJS+=("$OUT/linux_platform.o")
gcc "${CFLAGS[@]}" -w -c "$HERE/rdp_reference.c" -o "$OUT/rdp_reference.o"; OBJS+=("$OUT/rdp_reference.o")
wait
gcc "${CFLAGS[@]}" -Wl,--gc-sections -o "$OUT/n64host_linux" "${OBJS[@]}" -lm -lpthread
echo "built $OUT/n64host_linux"
