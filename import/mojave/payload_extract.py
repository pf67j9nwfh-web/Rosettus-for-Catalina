#!/usr/bin/python
# -*- coding: utf-8 -*-
"""payload_extract.py PAYLOAD DEST LIST... : copy chosen files out of a macOS installer package's
Payload (a pbzx stream of xz chunks holding an odc cpio archive), without unpacking the rest.

  PAYLOAD  the Payload file of Core.pkg (xar -xf Core.pkg Payload)
  DEST     folder to write into (made if needed)
  LIST     text files of paths without the leading / ('#' lines are comments); a line ending in /
           takes everything below that folder. Symbolic links below System/Library and usr are
           always taken (install names often go through them).

The xz chunks are decoded with the system's libcompression (macOS 10.11 and later), so this runs
with the Python 2.7 that comes with macOS: no tools need to be installed. Unlike pkgutil
--expand-full it needs no administrator rights and writes only the files asked for.
"""
import os, sys, struct, ctypes, errno

def die(m):
    sys.stderr.write('payload_extract: %s\n' % m); sys.exit(1)

_lib = ctypes.CDLL('/usr/lib/libcompression.dylib')
_dec = _lib.compression_decode_buffer
_dec.restype = ctypes.c_size_t
_dec.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
COMPRESSION_LZMA = 0x306

def chunks(f):
    """the decompressed pieces of a pbzx stream"""
    if f.read(4) != b'pbzx': die('not a pbzx payload')
    struct.unpack('>Q', f.read(8))
    out = None
    while True:
        h = f.read(16)
        if len(h) < 16: break
        usize, csize = struct.unpack('>QQ', h)
        data = f.read(csize)
        if len(data) != csize: die('truncated payload')
        if data[:6] == b'\xfd7zXZ\x00':
            if out is None or len(out) < usize: out = ctypes.create_string_buffer(usize)
            n = _dec(out, usize, data, csize, None, COMPRESSION_LZMA)
            if n != usize: die('xz chunk decoded to %d bytes, expected %d' % (n, usize))
            yield out.raw[:n]
        else:
            yield data

class Stream(object):
    """a file-like reader over the decompressed chunks"""
    def __init__(self, it):
        self.it = it; self.buf = b''; self.pos = 0
    def read(self, n):
        parts = []
        while n > 0:
            if self.pos >= len(self.buf):
                try: self.buf = next(self.it)
                except StopIteration: break
                self.pos = 0
            take = self.buf[self.pos:self.pos + n]
            self.pos += len(take); n -= len(take); parts.append(take)
        return b''.join(parts)
    def skip(self, n):
        while n > 0:
            if self.pos >= len(self.buf):
                try: self.buf = next(self.it)
                except StopIteration: return
                self.pos = 0
            k = min(n, len(self.buf) - self.pos)
            self.pos += k; n -= k

def wanted_sets(lists):
    files, dirs = set(), []
    for l in lists:
        for line in open(l):
            p = line.strip()
            if not p or p.startswith('#'): continue
            p = p.lstrip('/')
            if p.endswith('/'): dirs.append(p)
            else: files.add(p)
    return files, tuple(dirs)

def mkdirs(d):
    try: os.makedirs(d)
    except OSError as e:
        if e.errno != errno.EEXIST: raise

def main():
    if len(sys.argv) < 4: die(__doc__)
    payload, dest = sys.argv[1], sys.argv[2]
    files, dirs = wanted_sets(sys.argv[3:])
    s = Stream(chunks(open(payload, 'rb')))
    nfile = nlink = 0; found = set()
    while True:
        h = s.read(76)
        if len(h) < 76: break
        if h[:6] != b'070707': die('not an odc cpio archive (header %r)' % h[:6])
        mode = int(h[18:24], 8); namesize = int(h[59:65], 8); size = int(h[65:76], 8)
        name = s.read(namesize)[:-1].decode('utf-8', 'replace')
        if name == 'TRAILER!!!': break
        p = name[2:] if name.startswith('./') else name.lstrip('/')
        kind = mode & 0o170000
        take = p in files or (dirs and p.startswith(dirs))
        if kind == 0o120000 and (p.startswith('System/Library/') or p.startswith('usr/')): take = True
        if not take or kind not in (0o100000, 0o120000):
            s.skip(size); continue
        out = os.path.join(dest, p)
        mkdirs(os.path.dirname(out))
        if kind == 0o120000:
            target = s.read(size)
            if os.path.lexists(out): os.remove(out)
            os.symlink(target.decode('utf-8'), out); nlink += 1; found.add(p)
        else:
            if os.path.lexists(out): os.remove(out)
            with open(out, 'wb') as o:
                left = size
                while left > 0:
                    b = s.read(min(left, 1 << 20))
                    if not b: die('truncated file ' + p)
                    o.write(b); left -= len(b)
            os.chmod(out, (mode & 0o777) | 0o200)
            nfile += 1; found.add(p)
    missing = sorted(files - found)
    sys.stderr.write('payload_extract: %d files, %d links%s\n' % (nfile, nlink, (', %d listed files not in the payload' % len(missing)) if missing else ''))
    for m in missing[:20]: sys.stderr.write('  missing: %s\n' % m)

if __name__ == '__main__':
    main()
