#!/bin/sh
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
