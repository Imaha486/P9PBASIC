#!/bin/sh
# Build the host (macOS / Linux) headless CLI version of P8PBASIC.
# Uses the host HAL (src/halhost.c) instead of the PC-98 HAL (src/hal98.c):
# the text screen is kept in memory and dumped to stdout at exit, keyboard
# input comes from a script, and graphics can be dumped to a PPM file.
# The DOS build (build.sh / build.bat, Open Watcom) is unaffected.
set -e
cd "$(dirname "$0")"
mkdir -p build-host
CC=${CC:-cc}
$CC -O1 -Ithird_party/z80 -o build-host/p8pbasic \
  src/main.c src/exec.c src/eval.c src/var.c src/num.c src/token.c \
  src/print.c src/screen.c src/graph.c src/fileio.c src/halhost.c \
  src/vessel.c third_party/z80/z80.c -lm
ls -l build-host/p8pbasic
