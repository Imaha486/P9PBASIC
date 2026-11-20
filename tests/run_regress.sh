#!/bin/sh
# P8PBASIC regression suite. See tests/README.md for the tier design.
#
#   tests/run_regress.sh           build, run all tiers, report PASS/FAIL
#   tests/run_regress.sh --bless   regenerate the Tier B snapshot baselines
#
# The game disk is located through P8P_TEST_DISK (default:
# ~/workspace/p88test/ma2026/MA2026A.D88). Without it the probe tiers
# cannot run (the probes BLOAD NEOMSC from the disk) and are skipped
# with a notice; the build tier still runs.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BLESS=0
[ "${1:-}" = "--bless" ] && BLESS=1
DISK=${P8P_TEST_DISK:-$HOME/workspace/p88test/ma2026/MA2026A.D88}
FAIL=0
note() { echo "== $*"; }
bad() { echo "FAIL: $*"; FAIL=1; }

note "build"
./build-host.sh >/dev/null 2>&1 || { bad "build-host.sh"; echo "SUITE FAILED"; exit 1; }
./build-embed.sh >/dev/null 2>&1 || { bad "build-embed.sh"; echo "SUITE FAILED"; exit 1; }
# unit checks (text-screen merge; RAM window on the Z80 bus);
# they link the embed objects minus every test main.
EMBED_OBJS=$(ls build-embed/*.o | grep -v embed_test \
             | grep -v textmerge_test | grep -v window_test \
             | grep -v subsys_test | grep -v dramload_test | grep -v pcmb_ports_test \
             | grep -v kanjifont_test)
unit_test() { # name [args...]
  _t=$1; shift
  cc -O1 -std=gnu11 -Isrc -Ithird_party/z80 -c "tools/$_t.c" \
     -o "build-embed/$_t.o" 2>/dev/null \
    && c++ -o "build-embed/$_t" $EMBED_OBJS "build-embed/$_t.o" \
         -lm -lpthread 2>/dev/null \
    && "./build-embed/$_t" "$@" >/dev/null \
    || bad "$_t"
}
unit_test textmerge_test
unit_test window_test
unit_test kanjifont_test
unit_test pcmb_ports_test
echo "build ok"

if [ ! -f "$DISK" ]; then
  note "no game disk at $DISK -- probe/embed tiers skipped"
  [ $FAIL -eq 0 ] && echo "SUITE PASSED (build only)" || echo "SUITE FAILED"
  exit $FAIL
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK" /tmp/p8p??????' EXIT
cp tests/probes/*.BAS "$WORK/"
# With P8P_DISK set the host CLI materializes the disk's files into
# a scratch dir itself (src/materialize.c, shared with the embedded
# engine) and runs there, so the probes find the disk's own NEOMSC
# without any extracted plain files in $WORK.

# ---- subsystem write path (cmd 01H) against the real disk image:
# writes land in the in-memory image only; the file stays untouched.
unit_test subsys_test "$DISK"
echo "subsys ok"

# ---- FFDE service 1 (vessel_dram_load) must fill the OPNA's
# ADPCM-B DRAM with the disk's sectors and leave ADPCM-A untouched.
# (No unconditional "ok" echo: unit_test records failures itself.)
unit_test dramload_test "$DISK"

run_probe() { # name [env assignments...]
  _n=$1; shift
  (cd "$WORK" && env P8P_DISK="$DISK" "$@" timeout 60 \
     "$ROOT/build-host/p8pbasic" "$_n.BAS" 2>/dev/null)
}

# ---- Tier A: semantic assertions (values verified against the genuine
# machine / the NEOMSC contract, not just "what the code prints today").
note "Tier A assertions"
out=$(run_probe PROBE2) || bad "PROBE2 exit"
printf '%s\n' "$out" | grep -q '^00| 0  202$' || bad "PROBE2: (AA0F/AA10) != 0,202"
printf '%s\n' "$out" | grep -q '^01| 0  192$' || bad "PROBE2: (AA13/AA14) != 0,192"
out=$(run_probe PROBE18) || bad "PROBE18 exit"
printf '%s\n' "$out" | grep -q '60848' || bad "PROBE18: song-DB pointer 60848 missing"
printf '%s\n' "$out" | grep -q '9081' || bad "PROBE18: song-DB pointer 9081 missing"
out=$(run_probe PROBE19) || bad "PROBE19 exit"
printf '%s\n' "$out" | grep -q 'done8' || bad "PROBE19: XDD 8 did not complete"
out=$(run_probe PROBE42) || bad "PROBE42 exit"
printf '%s\n' "$out" | grep -q 'waited' || bad "PROBE42: playback wait loop broken"
out=$(run_probe PROBE44) || bad "PROBE44 exit"
printf '%s\n' "$out" | grep -q 'ok44' || bad "PROBE44: CG load/draw path broken"
# PROBE: *label GOTO (upstream Ver.261119 label support; on the
# pre-merge base this errored at line 10, which the old Tier B
# baseline honestly captured)
out=$(run_probe PROBE) || bad "PROBE exit"
printf '%s\n' "$out" | grep -q 'oklab' || bad "PROBE: label GOTO *FOO did not reach the label"
# PROBE53: the FFDE service-3 staff roll renders into the guest text
# image at 0xF3C8 as semi-graphic bitmap codes (font at 0xCA00,
# 8 bytes per character; the scroll shifts the dot plane). With the
# probe's fixed staff data the image ends with exactly 384 nonzero
# bytes. Control runs: no service-3 calls -> all zero; different
# staff text -> a different image (348). Before the ROM string
# evaluator (0x11D3) was emulated, the draw path read a stale
# numeric descriptor and produced a fixed 1728-byte pattern
# regardless of the text. The embed tier below checks that the
# engine's merged text surface displays the image (staff roll
# rows, attribute-pair recoloring) alongside interpreter text.
out=$(run_probe PROBE53) || bad "PROBE53 exit"
printf '%s\n' "$out" | grep -q 'rolldone' || bad "PROBE53: staff roll did not complete"
img=$(printf '%s\n' "$out" | grep -o 'img *[0-9]*' | grep -o '[0-9]*$')
if [ "$img" != "384" ]; then
  bad "PROBE53: text image count ${img:-none} != 384 (staff roll render changed)"
fi
[ $FAIL -eq 0 ] && echo "Tier A ok"

note "Tier A audio (standalone songs must be loud from the start)"
for p in PROBE5 PROBE51 PROBE52; do
  (cd "$WORK" && env P8P_DISK="$DISK" P8PWAV="$WORK/$p.wav" timeout 120 \
     "$ROOT/build-host/p8pbasic" "$p.BAS" >/dev/null 2>&1) \
    || { bad "$p exit"; continue; }
  python3 tests/wavprofile.py probe "$WORK/$p.wav" >/dev/null \
    || bad "$p audio profile"
done
[ $FAIL -eq 0 ] && echo "Tier A audio ok"

# ---- Tier B: characterization snapshots of every probe's final screen.
# A diff means behaviour changed; a human decides whether the change is
# the intended one and re-blesses. These are NOT correctness proofs.
note "Tier B snapshots"
for f in tests/probes/*.BAS; do
  n=$(basename "$f" .BAS)
  out=$(run_probe "$n")
  rc=$?
  [ $rc -ne 0 ] && { bad "$n exit rc=$rc"; continue; }
  if [ $BLESS -eq 1 ]; then
    printf '%s\n' "$out" > "tests/expected/$n.txt"
  else
    printf '%s\n' "$out" > "$WORK/$n.out"
    if ! diff -q "tests/expected/$n.txt" "$WORK/$n.out" >/dev/null 2>&1; then
      bad "$n screen differs from baseline (diff below)"
      diff -u "tests/expected/$n.txt" "$WORK/$n.out" | head -20
    fi
  fi
done
[ $BLESS -eq 1 ] && echo "baselines regenerated"
[ $FAIL -eq 0 ] && echo "Tier B ok"

# ---- Embed tier: the whole MA2026 flow in the embedded engine.
note "embed: title CG (40 s run)"
rm -rf /tmp/p8p??????
(cd /tmp && P8P_VRAMDUMP=1 "$ROOT/build-embed/embed_test" "$DISK" 40 \
   > "$WORK/embed40.log" 2>&1)
D=$(ls -td /tmp/p8p* 2>/dev/null | head -1)
grep -q 'running=1' "$WORK/embed40.log" || bad "embed40: engine not running at exit"
DR=$(sed -n 's/.*dropped=\([0-9]*\).*/\1/p' "$WORK/embed40.log")
[ -n "$DR" ] && [ "$DR" -le 20000 ] || bad "embed40: dropped=$DR > 20000"
if [ -n "$D" ] && [ -f "$D/vram0.bin" ]; then
  python3 - "$D" <<'EOF' || bad "embed40: VRAM plane counts changed"
import sys
want = [4555, 4770, 5394]
got = []
for b in range(3):
    data = open("%s/vram%d.bin" % (sys.argv[1], b), "rb").read()
    got.append(sum(1 for x in data if x))
if got != want:
    print("VRAM counts", got, "want", want)
    sys.exit(1)
EOF
else
  bad "embed40: no VRAM dump produced"
fi
[ $FAIL -eq 0 ] && echo "embed CG ok"

note "embed: audio phases (55 s run)"
rm -rf /tmp/p8p??????
(cd /tmp && "$ROOT/build-embed/embed_test" "$DISK" 55 > "$WORK/embed55.log" 2>&1)
D=$(ls -td /tmp/p8p* 2>/dev/null | head -1)
DR=$(sed -n 's/.*dropped=\([0-9]*\).*/\1/p' "$WORK/embed55.log")
[ -n "$DR" ] && [ "$DR" -le 20000 ] || bad "embed55: dropped=$DR > 20000"
if [ -n "$D" ] && [ -f "$D/embed_audio.wav" ]; then
  python3 tests/wavprofile.py embed "$D/embed_audio.wav" \
    || bad "embed55: audio phase order differs from the genuine machine"
else
  bad "embed55: no audio captured"
fi
[ $FAIL -eq 0 ] && echo "embed audio ok"

# The merged text surface: hold RETURN during attract to enter
# *SELECT (SCREEN 0,2, list + help printed by the interpreter),
# then check the list survived the merge (no image shadowing) and
# that the guest image's attribute pairs recolored merged cells
# (the pair build paints the two song columns green/cyan: merged
# attr bytes a0 appear, the interpreter's own plane has none).
note "embed: text merge (select screen)"
KEYS=$(printf '\\w%.0s' $(seq 50))
(cd /tmp && "$ROOT/build-embed/embed_test" "$DISK" 5 \
   "${KEYS}\\t\\w\\w\\w\\T\\w" > "$WORK/embedsel.log" 2>&1)
sed -n '/----- SCREEN -----/,/----- ATTRS (raw)/p' "$WORK/embedsel.log" \
  | grep -q 'MEGAFEPPS MUSIC ARENA 2026' || bad "embedsel: header missing from merged text"
sed -n '/----- SCREEN -----/,/----- ATTRS (raw)/p' "$WORK/embedsel.log" \
  | grep -q 'Move\[ASDW' || bad "embedsel: song list / help missing from merged text"
sed -n '/----- ATTRS (merged) -----/,$p' "$WORK/embedsel.log" \
  | grep -q 'a0a0' || bad "embedsel: no pair-driven recoloring in merged attrs"
[ $FAIL -eq 0 ] && echo "embed text ok"

echo
[ $FAIL -eq 0 ] && echo "SUITE PASSED" || echo "SUITE FAILED"
exit $FAIL
