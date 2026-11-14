#!/usr/bin/python
# -*- coding: utf-8 -*-
"""export_from_mojave.py - make the Mojave export for Rosettus for Catalina (run on macOS 10.14 Mojave).

Catalina cannot run 32-bit Intel code and has no 32-bit system libraries. Rosettus for Catalina runs
Rosetta's translator (itself a 32-bit Intel program) inside its own host process, on the 32-bit Intel
libraries of a Mojave installation. This script copies exactly those files from this Mac:

  * the i386 dyld shared cache (/var/db/dyld/dyld_shared_cache_i386 and its .map),
  * the i386 slice of dyld and of every library, framework and plug-in in mojave_i386_files.txt
    (the ones the translator and the shim library load that are not in the cache),
  * a dump of this Mac's 32-bit commpage (commpage32.bin, written by the helper tool commdump),

into one archive, Rosettus-Mojave-export.tar.gz, for install.sh or the Rosettus for Catalina
package. Nothing is changed on this Mac. No developer tools are needed (Mach-O files are thinned here,
in Python). The archive holds Apple software from your own Mojave installation: it is for use on your
own Macs only.

usage: export_from_mojave.py [--root DIR] [--no-cache] [OUTPUT.tar.gz]
                                     (default: ~/Desktop/Rosettus-Mojave-export.tar.gz)
  --root DIR    take the files from a Mojave system in DIR instead of this Mac's (another disk, or a
                system unpacked from Apple's "Install macOS Mojave" app: import/mojave/from_installer.sh)
  --no-cache    do not copy a shared cache: copy the libraries in it as files (mojave_i386_cache_files.txt)
                and build the i386 shared cache from them with that Mojave's own update_dyld_shared_cache
                (an installer has no shared cache: macOS builds it while installing). This works on
                Catalina too. Without a shared cache PowerPC Java crashes.
"""
import os, sys, struct, shutil, subprocess, tarfile, tempfile, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, 'mojave_i386_files.txt')
CACHELIST = os.path.join(HERE, 'mojave_i386_cache_files.txt')
EXPECTED = os.path.join(HERE, 'mojave_i386_expected.sha1')
CPU_I386 = 7

def say(m):
    sys.stdout.write(m + '\n'); sys.stdout.flush()

def i386_slice(path):
    """the bytes of the i386 slice of a Mach-O file (thin i386 or fat), or None"""
    with open(path, 'rb') as f:
        head = f.read(8)
        if len(head) < 8:
            return None
        magic = struct.unpack('>I', head[:4])[0]
        if magic == 0xcafebabe:                        # fat (big-endian header)
            n = struct.unpack('>I', head[4:8])[0]
            if n == 0 or n > 32:
                return None
            arch = f.read(20 * n)
            for i in range(n):
                cpu, sub, off, size, align = struct.unpack('>iiIII', arch[20 * i:20 * i + 20])
                if cpu == CPU_I386:
                    f.seek(off)
                    return f.read(size)
            return None
        if magic == 0xcefaedfe:                        # thin, little-endian 32-bit
            cpu = struct.unpack('<i', head[4:8])[0]
            if cpu == CPU_I386:
                f.seek(0)
                return f.read()
        return None

def sw_vers():
    try:
        out = subprocess.check_output(['/usr/bin/sw_vers'])
        return out.decode('utf-8', 'replace')
    except Exception:
        return ''

def main():
    args = sys.argv[1:]
    sysroot, nocache, out = '/', False, os.path.expanduser('~/Desktop/Rosettus-Mojave-export.tar.gz')
    while args:
        a = args.pop(0)
        if a == '--root' and args: sysroot = args.pop(0)
        elif a == '--no-cache': nocache = True
        else: out = a
    sysroot = os.path.abspath(sysroot)
    if sysroot == '/':
        v = sw_vers()
    else:
        v = ''
        pl = os.path.join(sysroot, 'System/Library/CoreServices/SystemVersion.plist')
        if os.path.isfile(pl):
            import plistlib
            d = plistlib.readPlist(pl)
            v = 'ProductName:\t%s\nProductVersion:\t%s\nBuildVersion:\t%s\n' % (d.get('ProductName'), d.get('ProductVersion'), d.get('ProductBuildVersion'))
    if 'ProductVersion:\t10.14' not in v:
        say('This needs macOS 10.14 Mojave files (%s:\n%s).' % ('this Mac' if sysroot == '/' else sysroot, v or 'no SystemVersion.plist'))
        say('Only Mojave has the 32-bit Intel libraries Rosettus for Catalina needs.')
        return 1
    cache = os.path.join(sysroot, 'var/db/dyld/dyld_shared_cache_i386')
    if not nocache and not os.path.isfile(cache):
        say('%s is missing: this Mojave system has no 32-bit shared cache.' % cache)
        say('Rebuild it with  sudo update_dyld_shared_cache -force  or run this with --no-cache.')
        return 1
    files = [l.rstrip('\n') for l in open(LIST) if l.strip() and not l.startswith('#')]
    if nocache:
        files += [l.rstrip('\n') for l in open(CACHELIST) if l.strip() and not l.startswith('#')]
    tmp = tempfile.mkdtemp(prefix='rosettus-mojave-export.')
    root = os.path.join(tmp, 'i386')
    missing, noi386, n = [], [], 0
    try:
        os.makedirs(os.path.join(root, 'var/db/dyld'))
        if not nocache:
            say('Copying the i386 shared cache (%d MB)...' % (os.path.getsize(cache) >> 20))
            for f in (cache, cache + '.map'):
                shutil.copyfile(f, os.path.join(root, 'var/db/dyld', os.path.basename(f)))
        say('Copying the i386 slices of %d libraries and plug-ins...' % len(files))
        for rel in files:
            src = os.path.join(sysroot, rel)
            if rel.startswith('var/db/dyld/'):
                continue
            if not os.path.isfile(src):
                missing.append(rel); continue
            data = i386_slice(src)
            if data is None:
                noi386.append(rel); continue
            dst = os.path.join(root, rel)
            d = os.path.dirname(dst)
            if not os.path.isdir(d):
                os.makedirs(d)
            with open(dst, 'wb') as w:
                w.write(data)
            os.chmod(dst, 0o755 if (os.stat(src).st_mode & 0o111) else 0o644)
            n += 1
        if nocache:
            udsc = os.path.join(sysroot, 'usr/bin/update_dyld_shared_cache')
            say('Building the i386 shared cache with %s (a few minutes)...' % udsc)
            rc = 1
            if os.path.isfile(udsc):
                rc = subprocess.call([udsc, '-root', root, '-arch', 'i386', '-cache_dir',
                                      os.path.join(root, 'var/db/dyld'), '-force', '-search_disk'])
            if rc != 0 or not os.path.isfile(os.path.join(root, 'var/db/dyld/dyld_shared_cache_i386')):
                say('warning: no shared cache could be built: most apps work without it, PowerPC Java does not.')
        if sysroot == '/':
            say('Dumping the 32-bit commpage...')
            p = subprocess.Popen([os.path.join(HERE, 'commdump')], cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            cdo = p.communicate()[0]
            if p.returncode != 0 or not os.path.isfile(os.path.join(tmp, 'commpage32.bin')):
                say('commdump failed: ' + cdo.decode('utf-8', 'replace'))
                return 1
        else:
            # no running Mojave to dump: an empty data page (rhost keeps every field of it current
            # from Catalina's own commpage; Mojave's i386 libraries call no commpage code)
            with open(os.path.join(tmp, 'commpage32.bin'), 'wb') as w:
                w.write(struct.pack('<II', 0xffff0000, 4096) + b'\0' * 4096)
        with open(os.path.join(tmp, 'mojave_version.txt'), 'w') as w:
            w.write(v)
        # compare with the Mojave build Rosettus for Catalina was tested with (informational)
        diff = 0
        if os.path.isfile(EXPECTED):
            for l in open(EXPECTED):
                h, f = l.rstrip('\n').split('  ', 1)
                p2 = os.path.join(root, f)
                if os.path.isfile(p2) and hashlib.sha1(open(p2, 'rb').read()).hexdigest() != h:
                    diff += 1
        say('Writing %s (a few minutes)...' % out)
        with tarfile.open(out, 'w:gz') as t:
            t.add(root, arcname='i386')
            t.add(os.path.join(tmp, 'commpage32.bin'), arcname='commpage32.bin')
            t.add(os.path.join(tmp, 'mojave_version.txt'), arcname='mojave_version.txt')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    say('Done: %d files in %s (%d MB).' % (n + 2, out, os.path.getsize(out) >> 20))
    if missing:
        say('note: %d listed files are not on this Mac: %s%s'
            % (len(missing), ', '.join(missing[:5]), ' ...' if len(missing) > 5 else ''))
    if noi386:
        say('note: %d listed files have no i386 code: %s' % (len(noi386), ', '.join(noi386[:5])))
    if diff:
        say('note: %d files differ from the Mojave build Rosettus for Catalina was tested with '
            '(10.14.6 18G103): another Mojave update. It should work, but that build was not tested.' % diff)
    say('Copy this file to the Catalina Mac and give it to the Rosettus for Catalina installer.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
