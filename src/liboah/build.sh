#!/bin/bash
# src/liboah/build.sh - build liboah_r2.dylib, the shim library of Rosettus for Catalina.
#
# It is i386 code and has to be built on Mac OS X 10.6 Snow Leopard (gcc 4.2, the 10.6 SDK); the
# Rosettus for Mojave build of the same sources leaves out -DOAH_R2. The result goes to
# build/liboah_r2.dylib; copy it to bin/ and run tools/binstamp.sh --update liboah_r2.dylib.
cd "$(dirname "$0")" || exit 1
mkdir -p build
gcc -std=gnu99 -O2 -arch i386 -dynamiclib -DOAH_R2 \
    -framework IOKit -framework CoreFoundation -framework AudioUnit -framework CoreAudio -lobjc \
    -framework Security -framework SystemConfiguration -framework CoreServices \
    -o build/liboah_r2.dylib liboah_mojave.c 2>&1 | grep -v "warning:\|In function\|In file included\|from liboah_mojave.c\|^ *from "
[ -f build/liboah_r2.dylib ] && echo "built src/liboah/build/liboah_r2.dylib"
