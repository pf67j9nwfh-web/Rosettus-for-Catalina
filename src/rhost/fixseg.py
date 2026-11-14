#!/usr/bin/python
# fixseg.py BINARY - give rhost's empty __LOW32 segment (linked at 0x1000) the size of the whole 32-bit
# world and its commpage (0x1000 .. 4 GB), as zero-fill. The kernel then maps it at exec, before libSystem's
# start-up can put anything below 4 GB. (ld cannot make a zero-fill segment that large itself.)
import struct, sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, res = struct.unpack_from('<IiiIIIII', d, 0)
assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
off = 32
done = False
for i in range(ncmds):
    cmd, cmdsize = struct.unpack_from('<II', d, off)
    if cmd == 0x19:  # LC_SEGMENT_64
        segname = bytes(d[off + 8:off + 24]).rstrip(b'\0')
        if segname == b'__LOW32':
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot = struct.unpack_from('<QQQQii', d, off + 24)
            assert vmaddr == 0x1000 and filesize == 0, (hex(vmaddr), filesize)
            struct.pack_into('<QQQQii', d, off + 24, 0x1000, 0xfffff000, 0, 0, 7, 3)
            done = True
        if segname == b'__GUARD':
            vmaddr = struct.unpack_from('<Q', d, off + 24)[0]
            assert vmaddr == 0x100000000, hex(vmaddr)
            struct.pack_into('<QQQQii', d, off + 24, 0x100000000, 0x10000000, 0, 0, 0, 0)
            print('__GUARD: 0x100000000-0x110000000 no access')
    off += cmdsize
assert done, 'no __LOW32 segment'
open(p, 'wb').write(d)
print('__LOW32: 0x1000-0x100000000 zero-fill')
