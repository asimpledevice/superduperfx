#!/usr/bin/env bash
# Native build of the N64 runtime configuration plus generated code, with the
# ROM/code working-set tracer (romtrace.c) linked in.
#   OUT=dir GEN_DIR=gen GEN_INC=cfgdir n64/profile/romtrace/build.sh [extra CFLAGS...]
# Run: RT_OUT=trace.txt N64_PROFILE_ROM=game.sfc N64_PROFILE_FRAMES=3000 dir/n64host_linux
# Then: analyze_rom.py . trace.txt dir/n64host_linux GEN_DIR/program_manifest.json
#       analyze_code.py trace.txt dir/n64host_linux <MIPS .o dir or elf> [cache KB list]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
N64="$HERE/.."
RUNNER="$N64/../runner/src"
OUT="${OUT:-$HERE/build}"
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
for s in n64_host.c n64_ppu_build.c; do
  o="$OUT/$s.o"; gcc "${CFLAGS[@]}" -w -c "$N64/src/$s" -o "$o"; OBJS+=("$o")
done
gcc "${CFLAGS[@]}" -DSNESRECOMP_N64_HAS_GENERATED_CODE -w -c "$N64/src/n64_stubs.c" -o "$OUT/n64_stubs.o"; OBJS+=("$OUT/n64_stubs.o")
for g in $GEN_DIR/*.c; do
  o="$OUT/gen_$(basename $g .c).o"
  gcc "${CFLAGS[@]}" -w -finstrument-functions -I$GEN_INC -I$GEN_DIR -c "$g" -o "$o" &
  OBJS+=("$o")
  while [ "$(jobs -r | wc -l)" -ge "$(nproc)" ]; do wait -n; done
done
gcc "${CFLAGS[@]}" -w -c "$HERE/linux_platform.c" -o "$OUT/linux_platform.o"; OBJS+=("$OUT/linux_platform.o")
gcc "${CFLAGS[@]}" -w -c "$HERE/rdp_reference.c" -o "$OUT/rdp_reference.o"; OBJS+=("$OUT/rdp_reference.o")
gcc "${CFLAGS[@]}" -w -c "$HERE/romtrace/romtrace.c" -o "$OUT/romtrace.o"; OBJS+=("$OUT/romtrace.o")
wait
gcc "${CFLAGS[@]}" -Wl,--gc-sections -o "$OUT/n64host_linux" "${OBJS[@]}" -lm -lpthread
echo "built $OUT/n64host_linux"
