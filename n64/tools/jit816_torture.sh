#!/bin/bash
# The 65816 JIT's differential test: a generated torture ROM (random 65816
# programs, no game code or data; jit816_torture.py) runs in ares under the
# interpreter (JIT816=0, the reference) and under the JIT, and the WRAM hash
# of every second frame must be the same in both.
#
#   n64/tools/jit816_torture.sh [SEED] [SECONDS] [EXTRA MAKE FLAGS FOR THE JIT BUILD]
#
# SECONDS is each run's wall time (default 150; the reference build is the
# slower of the two, so it decides how many frames are compared). Needs
# N64_INST and the ares of n64/profile/ares_run.sh. Work files go to
# $TORTURE_DIR (default: a new directory under ${TMPDIR:-/tmp}); the build
# is the working tree's n64/ (make clean first).
set -u
SEED=${1:-1}; SECS=${2:-150}; JITFLAGS=${3:-}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
N64="$HERE/.."
OUT=${TORTURE_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/jit816_torture.XXXXXX")}
mkdir -p "$OUT"
python3 "$HERE/jit816_torture.py" "$OUT/torture.sfc" --seed "$SEED" --set 16 --repeat 8 || exit 1

run() {   # run NAME "MAKE FLAGS"
  ( cd "$N64" && make clean >/dev/null &&
    make -j"$(nproc)" $2 APUTIME=0 "EXTRA_CFLAGS=-DN64_STATE_HASH -DN64_STATE_HASH_PERIOD=2" ROM="$OUT/torture.sfc" \
      > "$OUT/make_$1.log" 2>&1 ) || { echo "build $1 failed: $OUT/make_$1.log"; exit 1; }
  cp "$N64/snesrecomp_interp.z64" "$OUT/$1.z64"
  "$N64/profile/ares_run.sh" "$OUT/$1.z64" "$OUT/run_$1" "$SECS" >/dev/null 2>&1
  grep -a '^\[state\]' "$OUT/run_$1/isviewer.log" | awk '{print $3, $5}' > "$OUT/$1.states"
}
run interp "JIT816=0"
run jit "JIT816=1 $JITFLAGS"

n=$(wc -l < "$OUT/interp.states"); m=$(wc -l < "$OUT/jit.states"); [ "$m" -lt "$n" ] && n=$m
if [ "$n" -lt 20 ]; then echo "FAILED: only $n frames sampled (see $OUT)"; exit 1; fi
d=$(diff <(head -"$n" "$OUT/interp.states") <(head -"$n" "$OUT/jit.states") | grep -c '^<')
if [ "$d" -eq 0 ]; then
  echo "PASSED: seed $SEED, $n sampled frames (every second frame), WRAM the same in both"
else
  echo "FAILED: seed $SEED, $d of $n sampled frames differ; first: frame $(diff <(head -"$n" "$OUT/interp.states") <(head -"$n" "$OUT/jit.states") | grep '^<' | head -1 | awk '{print $2}') (see $OUT)"
  echo "Narrow it down with jit816_torture.py --only (mnemonics, modes, constructs) and -DN64_STATE_PAGES_FROM/TO."
  exit 1
fi
