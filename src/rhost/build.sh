#!/bin/bash
# build.sh - build rhost (macOS 10.15, Command Line Tools). Its image goes above 4 GB, the page-zero
# segment is one page, and a zero-fill segment (reserve.s + fixseg.py) holds 0x1000-0xfffec000 for
# the 32-bit world from exec on.
set -e
cd "$(dirname "$0")"
rm -f rhost.new
clang -std=gnu11 -O1 -g -Wall -Wno-deprecated-declarations -Wno-unused-function \
  -o rhost.new rhost.c lowmem.c loader.c commpage.c trap.c sig.c exc.c sys.c sysconv.c mach.c thread.c wq.c spawn.c snap.c heapchk.c bridge/hostcall.c bridge/hostcall_gen.c reserve.s sigentry.s enter32.s gate.s -framework CoreFoundation -framework CoreGraphics -framework ImageIO -framework CoreText -lobjc \
  -Wl,-pagezero_size,0x1000 -Wl,-segalign,0x1000 -Wl,-segaddr,__LOW32,0x1000 -Wl,-segaddr,__GUARD,0x100000000 \
  -Wl,-image_base,0x110000000 -Wl,-no_pie 2>&1 | grep -v "ld: warning: -no_pie" || true
# built aside and renamed into place: running processes keep the old file (an in-place write could crash them)
[ -f rhost.new ] && /usr/bin/python fixseg.py rhost.new && mv -f rhost.new rhost && echo "built rhost"
# debug symbols stay out of the source folder (bin/SOURCES.sha1 hashes it)
rm -rf rhost.new.dSYM rhost.dSYM
