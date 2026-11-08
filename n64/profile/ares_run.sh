#!/bin/bash
# Run an N64 ROM headless in ares (built by the environment setup script)
# under Xvfb. Captures libdragon debugf output (IS-Viewer, mirrored to stderr
# by a local ares patch) and screenshots of the virtual screen.
#
#   n64/profile/ares_run.sh ROM OUTDIR "10 30 60"   # screenshot times in seconds
#
# Env: ARES (binary path), XDISP (X display number, default 77),
# ARES_ARGS (extra ares arguments, e.g. "--setting General/ForceInterpreter=true").
# The run ends after the last screenshot time.
set -u
ROM="$1"; OUT="$2"; TIMES="$3"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARES="${ARES:-$HERE/../../../ares/build/rundir/bin/ares}"
[ -x "$ARES" ] || { echo "ares not found at $ARES (set ARES=...)" >&2; exit 1; }
mkdir -p "$OUT"
export LD_LIBRARY_PATH=/opt/sdl3/lib SDL_AUDIODRIVER=dummy DISPLAY=":${XDISP:-77}"
Xvfb "$DISPLAY" -screen 0 1024x768x24 >/dev/null 2>&1 &
XPID=$!
for _ in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
"$ARES" --system "Nintendo 64" --no-file-prompt \
  --setting Audio/Mute=true --setting Audio/Blocking=false \
  --setting Input/Defocus=Allow --setting Nintendo64/ExpansionPak=true ${ARES_ARGS:-} \
  "$ROM" > "$OUT/stdout.log" 2> "$OUT/isviewer.log" &
APID=$!
start=$(date +%s)
for t in $TIMES; do
  while [ $(( $(date +%s) - start )) -lt "$t" ]; do sleep 1; done
  import -window root "$OUT/shot_${t}s.png" 2>/dev/null
done
kill $APID 2>/dev/null; wait $APID 2>/dev/null
kill $XPID 2>/dev/null
echo "logs and screenshots in $OUT"
