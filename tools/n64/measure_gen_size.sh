#!/usr/bin/env bash
# Compile a directory of generated bank C with the libdragon N64 toolchain and
# report total section sizes. Measurement only: nothing is linked.
#
# Usage:
#   tools/n64/measure_gen_size.sh --gen-dir DIR --include DIR [--include DIR ...]
#                                 [--opt -Os] [--jobs 4] [--out DIR]
#
# --gen-dir   generated bank C (e.g. <game>/src/gen)
# --include   extra include dirs the generated code needs (the game's recomp/
#             for funcs.h, and its src/ for config.h / variables.h)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
N64_INST="${N64_INST:-/opt/libdragon}"
GCC="$N64_INST/bin/mips64-elf-gcc"
SIZE="$N64_INST/bin/mips64-elf-size"
GEN_DIR=""
OPT="-Os"
JOBS="$(nproc)"
OUT=""
INCS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --gen-dir) GEN_DIR="$2"; shift 2 ;;
    --include) INCS+=("-I$2"); shift 2 ;;
    --opt)     OPT="$2"; shift 2 ;;
    --jobs)    JOBS="$2"; shift 2 ;;
    --out)     OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$GEN_DIR" ] || { echo "--gen-dir is required" >&2; exit 2; }
[ -x "$GCC" ] || { echo "N64 toolchain not found at $N64_INST" >&2; exit 1; }
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

CFLAGS=(-march=vr4300 -mtune=vr4300 -mabi=o64
  "-I$N64_INST/mips64-elf/include/newlib_overrides" "-I$N64_INST/mips64-elf/include"
  -include ktls.h -std=gnu17 -G0 "$OPT"
  "-I$ROOT/runner/src" "-I$ROOT/runner/src/snes" "${INCS[@]}"
  -DSNESRECOMP_TRACE=0)

export GCC OUT
printf '%s\0' "${CFLAGS[@]}" > "$OUT/cflags"
find "$GEN_DIR" -maxdepth 1 -name '*.c' -print0 | sort -z |
  xargs -0 -P "$JOBS" -I{} sh -c '
    f="$1"; o="$OUT/$(basename "$f").o"
    xargs -0 "$GCC" -c "$f" -o "$o" < "$OUT/cflags" 2> "$o.err" || echo "FAIL $f (see $o.err)"
  ' _ {}

echo "Section totals for $GEN_DIR ($OPT):"
"$SIZE" -A "$OUT"/*.o | awk '$1 ~ /^\./ { s[$1] += $2 } END { for (k in s) if (s[k] > 0) printf "  %-18s %12d\n", k, s[k] }' | sort -k2 -n -r
echo "Objects in: $OUT"
