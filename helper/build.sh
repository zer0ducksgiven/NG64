#!/bin/bash
# Builds ng64helper.exe with MSYS2 MinGW-w64 (run from any shell; uses C:\msys64).
# libsm64 is compiled from ../libsm64-master (its own Makefile fetches Mario's geometry from the sm64 decomp).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
export MSYSTEM=MINGW64
/c/msys64/usr/bin/bash.exe -lc "
set -e
cd '$HERE/../libsm64-master' && make lib CC=gcc -j8 >/dev/null
cd '$HERE' && mkdir -p dist
gcc -O2 -Wall -Wno-unused-function -DGBI_FLOATS -DSM64_LIB_EXPORT -I../libsm64-master/src -o dist/ng64helper.exe \
  src/main.c src/png.c src/audio.c \$(ls ../libsm64-master/build/src/*.o ../libsm64-master/build/src/decomp/*/*.o ../libsm64-master/build/src/decomp/*.o ../libsm64-master/build/src/decomp/*/*/*.o 2>/dev/null) \
  -static -lws2_32 -lwinmm -lm
"
echo built "$HERE/dist/ng64helper.exe"
