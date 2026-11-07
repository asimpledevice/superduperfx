#!/usr/bin/env bash
# Rebuild the native profile host and compare its frame hashes to a reference.
#   n64/profile/check_hashes.sh ROM REFERENCE [FRAMES] [extra CFLAGS...]
# Writes the reference first if it does not exist.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROM="$1"; REF="$2"; FRAMES="${3:-3000}"; shift 3 || shift $#
rm -rf "$HERE/build"
"$HERE/build.sh" "$@" >/dev/null
OUT="$(mktemp)"
start=$(date +%s.%N)
N64_PROFILE_ROM="$ROM" N64_PROFILE_FRAMES="$FRAMES" "$HERE/build/n64host_linux" 2>/dev/null > "$OUT"
end=$(date +%s.%N)
printf 'run: %.2fs for %s frames\n' "$(echo "$end - $start" | bc)" "$FRAMES"
if [ ! -f "$REF" ]; then cp "$OUT" "$REF"; echo "wrote reference $REF"; exit 0; fi
if diff -q "$REF" "$OUT" >/dev/null; then echo "MATCH ($(wc -l < "$REF") hashes)"; else
  echo "MISMATCH"; diff "$REF" "$OUT" | head -6; exit 1; fi
