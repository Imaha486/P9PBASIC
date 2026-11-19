#!/bin/sh
# Build the embedded-engine test binary (tools/embed_test.c) with the
# embedded HAL (src/halp8.c) and engine (src/engine.c) instead of the
# host CLI's main.c/halhost.c. The iOS app links the same objects.
set -e
cd "$(dirname "$0")"
mkdir -p build-embed
CC=${CC:-cc}
CXX=${CXX:-c++}
CSRC="src/exec.c src/eval.c src/var.c src/num.c src/token.c
      src/print.c src/screen.c src/graph.c src/fileio.c src/halp8.c
      src/vessel.c src/p88disk.c src/p88conv.c src/materialize.c
      src/engine.c
      tools/embed_test.c third_party/z80/z80.c"
CXXSRC="src/opna_ymfm.cpp
        third_party/ymfm/src/ymfm_opn.cpp
        third_party/ymfm/src/ymfm_ssg.cpp
        third_party/ymfm/src/ymfm_adpcm.cpp"
OBJS=""
for f in $CSRC; do
  o="build-embed/$(echo "$f" | tr / _).o"
  $CC -O1 -std=gnu11 -Isrc -Ithird_party/z80 -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
for f in $CXXSRC; do
  o="build-embed/$(echo "$f" | tr / _).o"
  $CXX -O1 -Ithird_party/ymfm/src -Isrc -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
$CXX -o build-embed/embed_test $OBJS -lm -lpthread
ls -l build-embed/embed_test
