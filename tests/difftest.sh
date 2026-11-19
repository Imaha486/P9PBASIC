#!/bin/sh
# Differential test against the genuine machine, emulated by
# QUASI88 (winterheart fork). NOT part of run_regress.sh: it needs
# the user's own PC-88 ROM dump in ~/.quasi88/rom, an X server
# (Xvfb is started if :99 is absent), and ~3 minutes.
#
#   tests/difftest.sh
#
# Captures 80 s of QUASI88 audio (SDL disk driver) with the two
# bundled disk images on separate drives, runs the embed engine
# for 75 s, and compares the audio phase landmarks with
# tests/envelope.py. Screenshots for human review land in the
# output dir when xwd is available.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
DISK=${P8P_TEST_DISK:-$HOME/workspace/p88test/ma2026/MA2026A.D88}
Q88=${QUASI88:-$HOME/workspace/quasi88/src/build/quasi88.sdl}
OUT=$(mktemp -d)
echo "output dir: $OUT"
[ -x "$Q88" ] || { echo "QUASI88 not found at $Q88"; exit 1; }
[ -f "$DISK" ] || { echo "no game disk at $DISK"; exit 1; }
./build-embed.sh >/dev/null 2>&1 || { echo "embed build failed"; exit 1; }

python3 tests/split_d88.py "$DISK" "$OUT" >/dev/null || exit 1

XVFB_STARTED=0
if [ ! -e /tmp/.X11-unix/X99 ]; then
  Xvfb :99 -screen 0 1024x800x24 >/dev/null 2>&1 &
  XVFB_STARTED=1
  sleep 2
fi

echo "QUASI88: 80 s capture..."
(cd "$OUT" && DISPLAY=:99 SDL_AUDIODRIVER=disk timeout 90 \
   "$Q88" -sd2 "$OUT/DISK_A.D88" "$OUT/DISK_B.D88" >/dev/null 2>&1) &
QPID=$!
if command -v xwd >/dev/null 2>&1; then
  sleep 15
  DISPLAY=:99 xwd -root -silent -out "$OUT/q88_15s.xwd" 2>/dev/null
  sleep 25
  DISPLAY=:99 xwd -root -silent -out "$OUT/q88_40s.xwd" 2>/dev/null
  sleep 40
else
  echo "(xwd unavailable -- screenshots skipped)"
  sleep 80
fi
wait $QPID 2>/dev/null
pgrep -x quasi88.sdl >/dev/null && pkill -x quasi88.sdl
[ $XVFB_STARTED -eq 1 ] && pkill -f "Xvfb :99" 2>/dev/null
[ -f "$OUT/sdlaudio.raw" ] || { echo "FAIL: no QUASI88 audio captured"; exit 1; }

echo "embed: 75 s run..."
rm -rf /tmp/p8p??????
(cd /tmp && "$ROOT/build-embed/embed_test" "$DISK" 75 > "$OUT/embed75.log" 2>&1)
D=$(ls -td /tmp/p8p* 2>/dev/null | head -1)
[ -n "$D" ] && [ -f "$D/embed_audio.wav" ] || { echo "FAIL: no embed audio"; exit 1; }
cp "$D/embed_audio.wav" "$OUT/embed_audio.wav"
[ -f "$D/embed_gfx.ppm" ] && cp "$D/embed_gfx.ppm" "$OUT/embed_gfx.ppm"

python3 tests/envelope.py "$OUT/sdlaudio.raw" "$OUT/embed_audio.wav"
RC=$?
echo "artifacts in $OUT"
exit $RC
