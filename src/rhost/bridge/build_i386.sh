#!/bin/bash
# build_i386.sh - regenerate the native bridge from r2native.spec and build its i386 side,
# libr2native.dylib (and the r2test check program), on Mac OS X 10.6 Snow Leopard. The host side
# (hostcall_gen.c) is compiled into rhost by ../build.sh on Catalina. Copy libr2native.dylib to bin/
# and run tools/binstamp.sh --update libr2native.dylib.
cd "$(dirname "$0")" || exit 1
python gen.py r2native.spec . || exit 1
gcc -arch i386 -O1 -std=gnu99 -dynamiclib -o libr2native.dylib r2native.c r2native_gen.c \
    -install_name @loader_path/libr2native.dylib -framework CoreFoundation || exit 1
gcc -arch i386 -O1 -std=gnu99 -o r2test r2test.c -L. -lr2native -framework CoreFoundation || exit 1
echo built
