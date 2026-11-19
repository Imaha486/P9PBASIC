#!/bin/sh
# NOT SUPPORTED in P8PBASIC -- this script is inherited unchanged
# from P9PBASIC, where it builds the PC-98 MS-DOS interpreter
# (P9PBASIC.EXE) with Open Watcom. P8PBASIC's interpreter sources
# call the Z80 vessel throughout (XDE/XDD/XDF relays in exec.c,
# BLOAD,R in fileio.c, vessel_pump in the statement loop), and the
# vessel + ymfm OPNA renderer + D88 layer have no 16-bit DOS
# target, so this build cannot link. It stops here with an
# explanation rather than failing obscurely at the linker.
# Use ./build-host.sh (host CLI) or ./build-embed.sh (engine).
# See docs/APPROXIMATIONS.md, audit item G-5.
echo "build.sh: the DOS build is a P9PBASIC target and is not" >&2
echo "supported in P8PBASIC (the vessel has no DOS target)." >&2
echo "Use ./build-host.sh or ./build-embed.sh instead." >&2
exit 1
# Build P9PBASIC.EXE with Open Watcom (16-bit DOS, large model, 8086, no FPU required)
set -e
export WATCOM=${WATCOM:-/opt/watcom}
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH
export INCLUDE=$WATCOM/h
mkdir -p obj
CF="-za99 -q -bt=dos -ml -0 -fpc -s -ox -w3 -zq"
for f in main exec eval var num token print screen graph fileio; do
  wcc $CF -fo=obj/$f.obj src/$f.c
done
wcc $CF -zu -fo=obj/hal98.obj src/hal98.c
wlink system dos option quiet option stack=16384 option map=obj/p9pbasic.map name P9PBASIC.EXE \
  file { obj/main.obj obj/exec.obj obj/eval.obj obj/var.obj obj/num.obj obj/token.obj obj/print.obj obj/screen.obj obj/graph.obj obj/fileio.obj obj/hal98.obj }
ls -l P9PBASIC.EXE
