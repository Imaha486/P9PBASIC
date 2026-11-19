#!/bin/sh
# Build the host (macOS / Linux) headless CLI version of P8PBASIC.
# Uses the host HAL (src/halhost.c) instead of the PC-98 HAL (src/hal98.c):
# the text screen is kept in memory and dumped to stdout at exit, keyboard
# input comes from a script, and graphics can be dumped to a PPM file.
# The DOS build (build.sh / build.bat, Open Watcom) is unaffected.
# C sources are compiled as C; the ymfm OPNA renderer (src/opna_ymfm.cpp
# + third_party/ymfm) is C++ and linked in with the C++ linker.
set -e
cd "$(dirname "$0")"
mkdir -p build-host
CC=${CC:-cc}
CXX=${CXX:-c++}
CSRC="src/main.c src/exec.c src/eval.c src/var.c src/num.c src/token.c
      src/print.c src/screen.c src/graph.c src/fileio.c src/halhost.c
      src/vessel.c src/p88disk.c src/p88conv.c src/materialize.c
      third_party/z80/z80.c"
CXXSRC="src/opna_ymfm.cpp
        third_party/ymfm/src/ymfm_opn.cpp
        third_party/ymfm/src/ymfm_ssg.cpp
        third_party/ymfm/src/ymfm_adpcm.cpp"
OBJS=""
for f in $CSRC; do
  o="build-host/$(echo "$f" | tr / _).o"
  $CC -O1 -DP8_HOST_MATERIALIZE -Ithird_party/z80 -Isrc -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
for f in $CXXSRC; do
  o="build-host/$(echo "$f" | tr / _).o"
  $CXX -O1 -Ithird_party/ymfm/src -Isrc -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
$CXX -o build-host/p8pbasic $OBJS -lm
ls -l build-host/p8pbasic
