/* sys.c - BSD system calls of the 32-bit world.
   Arguments are 32-bit words on the 32-bit stack. Most calls take ints and pointers and the kernel
   structures they touch have the same layout for i386 and x86_64: those are passed through. Calls
   with 64-bit arguments split in two words, 32-bit/64-bit structure differences, paths into the
   Snow Leopard tree, memory mapping (kept below 4 GB) and signals are handled here. */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include "kinfo32.h"
#include <sys/resource.h>
#include <dirent.h>
#include <libkern/OSByteOrder.h>

#define A(i) rd32(argp + 4 * (i))


/* ---- paths: /usr/lib, /System/Library, ... come from the Snow Leopard tree when it has the file ---- */
static const char *g_map_prefixes[] = { "/usr/lib/", "/System/Library/", "/usr/libexec/oah/", "/usr/share/icu/",
                                        "/usr/share/zoneinfo/", "/Library/Frameworks/", "/usr/bin/", "/bin/", "/var/db/dyld/", 0 };
int vol_alias_map(const char *p, char *buf, size_t n);   /* sysconv.c */
const char *map_path(const char *p, char *buf, size_t n) {
    if (!p || p[0] != '/' || !g_root32[0]) return p;
    if (p[1] == '.' && vol_alias_map(p, buf, n)) return buf;  /* /.vol/<dev>/<alias of a system volume object> */
    if (!strncmp(p, "/Network", 8) && (p[8] == 0 || p[8] == '/')) {
        /* Catalina has no /Network: the Folder Manager's network domain root. Mojave's CarbonCore
           (FSFindFolder for the network domain, when the audio components are scanned) logged the
           failure with a backtrace that crashed. The root's empty /Network stands in. */
        static int host_has = -1;
        if (host_has < 0) { struct stat st; host_has = stat("/Network", &st) == 0; }
        if (!host_has) { snprintf(buf, n, "%s%s", g_root32, p); return buf; }
    }
    {   /* The Component Manager's caches (com.apple.Components2.*Cache.*) are written by this Mac's
           CarbonCore; the root's (Mojave) CarbonCore misread Catalina's system cache and crashed in
           searchForComponents (G4 demos, qt_play: a size read from the text "jscr"). For the 32-bit
           world they are the root's, i.e. absent: it scans the component folders itself.
           R2_HOST_COMPCACHE=1 lets it read this Mac's. */
        const char *b = strrchr(p, '/');
        static int hc = -1;
        if (hc < 0) hc = getenv("R2_HOST_COMPCACHE") != 0;
        if (b && !hc && !strncmp(b + 1, "com.apple.Components2.", 22)) { snprintf(buf, n, "%s%s", g_root32, p); return buf; }
    }
    if (!strncmp(p, "/private/var/db/dyld/", 21)) {           /* the i386 shared cache of the root */
        struct stat st;
        snprintf(buf, n, "%s%s", g_root32, p + 8);
        if (!lstat(buf, &st)) return buf;
        return p;
    }
    for (int i = 0; g_map_prefixes[i]; i++) {
        size_t l = strlen(g_map_prefixes[i]);
        if (!strncmp(p, g_map_prefixes[i], l)) {
            struct stat st;
            snprintf(buf, n, "%s%s", g_root32, p);
            if (!lstat(buf, &st)) {
                /* The root holds only the i386 binaries (and the folders leading to them). A folder
                   this Mac also has is this Mac's: a path built from it (Office 2004 looked for
                   ApplicationServices.framework/Frameworks/QD.framework) must not end in the root's
                   sparse copy. Files, and folders this Mac lacks (QuickTime), come from the root. */
                struct stat hs;
                if (S_ISDIR(st.st_mode) && !lstat(p, &hs) && !getenv("R2_ROOT_DIRS")) return p;
                return buf;
            }
            /* not in the root: this Mac's file, unless it is code. Catalina still has i386 slices of a
               few libraries (libSystem, libgcc_s): those must not mix with the root's userland. */
            extern int g_cp_modern;
            if (g_cp_modern) {
                /* Only code with an i386 part is kept from the 32-bit world: a 64-bit-only program
                   (/bin/sh, /usr/bin/...) is this Mac's and is started natively (spawn.c). Sending it
                   to the root's missing copy failed every fork + exec of /bin/sh with ENOENT (xACT's
                   "do shell script": the 10.6 translator stats the program before its execve).
                   R2_ROOT_ALLCODE=1 restores the old rule. */
                int fd = open(p, O_RDONLY);
                if (fd >= 0) {
                    unsigned char h[512]; ssize_t k = read(fd, h, sizeof h); close(fd);
                    uint32_t mg = 0; if (k >= 4) memcpy(&mg, h, 4);
                    int code = k >= 4 && (mg == 0xfeedface || mg == 0xfeedfacf || mg == 0xbebafeca || mg == 0xcafebabe);
                    int has32 = mg == 0xfeedface;
                    if (mg == 0xbebafeca && k >= 8) {         /* fat: big-endian header */
                        uint32_t na = OSSwapBigToHostInt32(*(uint32_t *)(h + 4));
                        for (uint32_t i = 0; i < na && 8 + (i + 1) * 20 <= (uint32_t)k; i++)
                            if (OSSwapBigToHostInt32(*(uint32_t *)(h + 8 + 20 * i)) == 7) has32 = 1;   /* CPU_TYPE_I386 */
                    }
                    if (code && (has32 || mg == 0xcafebabe || getenv("R2_ROOT_ALLCODE"))) return buf;
                }
            }
            return p;
        }
    }
    return p;
}
#define MAPPED(i, b) map_path((const char *)P32(A(i)), b, sizeof b)

/* ---- POSIX aio for i386 callers. The kernel reads the caller's struct aiocb in its own (64-bit)
   layout, so the i386 one (48 bytes: fildes, offset at 4, buf 12, nbytes 16, reqprio 20, sigevent
   24 {notify, signo, value, function, attributes}, lio_opcode 44) went in as garbage: EAGAIN.
   QuickTime's file data handler reads with aio_read: every graphics importer failed (-2003) and
   Mactracker quit at startup. Requests are done synchronously here; aio_error / aio_return /
   aio_suspend report them, SIGEV_SIGNAL is raised on completion. */
#include <aio.h>
typedef struct { u32 cb; long ret; int err; int used; } r2aio_t;
static r2aio_t g_aio[256];
static pthread_mutex_t g_aio_mu = PTHREAD_MUTEX_INITIALIZER;
static r2aio_t *aio_slot(u32 cb, int make) {
    r2aio_t *fr = 0;
    for (int i = 0; i < 256; i++) {
        if (g_aio[i].used && g_aio[i].cb == cb) return &g_aio[i];
        if (!g_aio[i].used && !fr) fr = &g_aio[i];
    }
    if (make && fr) { fr->used = 1; fr->cb = cb; fr->ret = 0; fr->err = EINPROGRESS; return fr; }
    return 0;
}
static int aio_do(u32 cb, int write_op) {   /* returns 0 / errno for the submission */
    int fd = (int)rd32(cb), notify = (int)rd32(cb + 24), signo = (int)rd32(cb + 28);
    off_t off = (off_t)((uint64_t)rd32(cb + 4) | ((uint64_t)rd32(cb + 8) << 32));
    void *buf = P32(rd32(cb + 12)); size_t n = rd32(cb + 16);
    pthread_mutex_lock(&g_aio_mu);
    r2aio_t *a = aio_slot(cb, 1);
    if (!a) { pthread_mutex_unlock(&g_aio_mu); return EAGAIN; }
    pthread_mutex_unlock(&g_aio_mu);
    ssize_t r = write_op ? pwrite(fd, buf, n, off) : pread(fd, buf, n, off);
    int e = r < 0 ? errno : 0;
    pthread_mutex_lock(&g_aio_mu); a->ret = r; a->err = e; pthread_mutex_unlock(&g_aio_mu);
    if (g_trace) rlog("[aio] %s fd %d off %lld n %zu -> %zd (%d)", write_op ? "write" : "read", fd, (long long)off, n, r, e);
    if (notify == SIGEV_SIGNAL && signo > 0 && signo < 32) kill(getpid(), signo);
    return 0;
}
static int sys_aio(ss64_t *s, u32 num, u32 argp) {   /* 1 if handled */
    switch (num) {
    case 318: case 319: {                        /* aio_read / aio_write */
        int e = aio_do(A(0), num == 319);
        if (e) ret_err(s, e); else ret_ok(s, 0);
        return 1;
    }
    case 317: {                                  /* aio_error */
        pthread_mutex_lock(&g_aio_mu); r2aio_t *a = aio_slot(A(0), 0); int e = a ? a->err : -1; pthread_mutex_unlock(&g_aio_mu);
        if (e < 0) ret_err(s, EINVAL); else ret_ok(s, (u32)e);
        return 1;
    }
    case 314: {                                  /* aio_return: the result, once */
        pthread_mutex_lock(&g_aio_mu); r2aio_t *a = aio_slot(A(0), 0);
        long r = a ? a->ret : 0; int e = a ? a->err : EINVAL; if (a) a->used = 0;
        pthread_mutex_unlock(&g_aio_mu);
        if (!a) ret_err(s, EINVAL); else if (r < 0) ret_err(s, e); else ret_ok(s, (u32)r);
        return 1;
    }
    case 315: ret_ok(s, 0); return 1;            /* aio_suspend: everything is done already */
    case 316: ret_ok(s, AIO_ALLDONE); return 1;  /* aio_cancel */
    case 320: {                                  /* lio_listio(mode, list, n, sigevent) */
        u32 list = A(1), n = A(2);
        for (u32 i = 0; i < n && i < 256; i++) {
            u32 cb = rd32(list + 4 * i); if (!cb) continue;
            int op = (int)rd32(cb + 44);
            if (op == LIO_READ || op == LIO_WRITE) aio_do(cb, op == LIO_WRITE);
        }
        ret_ok(s, 0);
        return 1;
    }
    }
    return 0;
}

static int g_cp_modern_sys(void) { extern int g_cp_modern; return g_cp_modern; }
/* The i386 shared cache of the root (Mojave's): dyld takes a library from it only if the file on
   disk has the inode and modification time recorded in the cache (else it is a "root" and loaded
   from disk). The copies in the root cannot match, so stat reports the recorded values for them. */
#include <sys/mman.h>
typedef struct { char *path; uint64_t mtime, inode; } cacheimg_t;
static cacheimg_t *g_cimg; static int g_ncimg = -1;
static void cache_init(void) {
    g_ncimg = 0;
    char p[1100]; snprintf(p, sizeof p, "%s/var/db/dyld/dyld_shared_cache_i386", g_root32);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return;
    unsigned char h[0x40];
    if (pread(fd, h, sizeof h, 0) != sizeof h || memcmp(h, "dyld_v1    i386", 15)) { close(fd); return; }
    u32 ioff = *(u32 *)(h + 0x18), icount = *(u32 *)(h + 0x1c);
    struct stat st; fstat(fd, &st);
    unsigned char *m = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    g_cimg = calloc(icount, sizeof *g_cimg);
    for (u32 i = 0; i < icount; i++) {
        const unsigned char *e = m + ioff + 32 * i;
        u32 po = *(const u32 *)(e + 24);
        if (po >= st.st_size) continue;
        g_cimg[g_ncimg].path = strdup((const char *)m + po);
        g_cimg[g_ncimg].mtime = *(const uint64_t *)(e + 8);
        g_cimg[g_ncimg].inode = *(const uint64_t *)(e + 16);
        g_ncimg++;
    }
    munmap(m, (size_t)st.st_size);
    if (g_trace) rlog("[cache] %d images in the root's shared cache", g_ncimg);
}
/* a library that is only in the cache (no file in the root): stat as dyld expects it */
static int cache_stat_fake(const char *guestpath, struct stat *st) {
    if (!g_cp_modern_sys()) return 0;
    if (g_ncimg < 0) cache_init();
    char rp[1024];      /* a framework's top-level symlink: the path in the cache is the one behind it */
    const char *alt = realpath(guestpath, rp) ? rp : guestpath;
    for (int i = 0; i < g_ncimg; i++) if (!strcmp(g_cimg[i].path, guestpath) || !strcmp(g_cimg[i].path, alt)) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFREG | 0755; st->st_nlink = 1; st->st_size = 4096;
        st->st_ino = g_cimg[i].inode; st->st_mtimespec.tv_sec = (long)g_cimg[i].mtime;
        return 1;
    }
    return 0;
}
static void cache_stat_fix(const char *guestpath, const char *mapped, struct stat *st) {
    (void)mapped; if (!g_cp_modern_sys()) return;
    if (g_ncimg < 0) cache_init();
    for (int i = 0; i < g_ncimg; i++) if (!strcmp(g_cimg[i].path, guestpath)) {
        st->st_ino = g_cimg[i].inode; st->st_mtimespec.tv_sec = (long)g_cimg[i].mtime; st->st_mtimespec.tv_nsec = 0;
        return;
    }
}

/* ---- stat structures ---- */
struct ts32 { int32_t sec, nsec; };
struct stat32 {            /* i386 struct stat (32-bit inode) */
    int32_t dev; uint32_t ino; uint16_t mode, nlink; uint32_t uid, gid; int32_t rdev;
    struct ts32 at, mt, ct; int64_t size, blocks; int32_t blksize; uint32_t flags, gen; int32_t lspare; int64_t qspare[2];
} __attribute__((packed));
struct stat64_32 {         /* i386 struct stat64 */
    int32_t dev; uint16_t mode, nlink; uint64_t ino; uint32_t uid, gid; int32_t rdev;
    struct ts32 at, mt, ct, bt; int64_t size, blocks; int32_t blksize; uint32_t flags, gen; int32_t lspare; int64_t qspare[2];
} __attribute__((packed));

static void put_stat(u32 addr, const struct stat *h, int is64) {
    if (is64) {
        struct stat64_32 o; memset(&o, 0, sizeof o);
        o.dev = h->st_dev; o.mode = h->st_mode; o.nlink = h->st_nlink; o.ino = h->st_ino; o.uid = h->st_uid; o.gid = h->st_gid; o.rdev = h->st_rdev;
        o.at.sec = (int32_t)h->st_atimespec.tv_sec; o.at.nsec = (int32_t)h->st_atimespec.tv_nsec;
        o.mt.sec = (int32_t)h->st_mtimespec.tv_sec; o.mt.nsec = (int32_t)h->st_mtimespec.tv_nsec;
        o.ct.sec = (int32_t)h->st_ctimespec.tv_sec; o.ct.nsec = (int32_t)h->st_ctimespec.tv_nsec;
        o.bt.sec = (int32_t)h->st_birthtimespec.tv_sec; o.bt.nsec = (int32_t)h->st_birthtimespec.tv_nsec;
        o.size = h->st_size; o.blocks = h->st_blocks; o.blksize = h->st_blksize; o.flags = h->st_flags; o.gen = h->st_gen;
        memcpy(P32(addr), &o, sizeof o);
    } else {
        struct stat32 o; memset(&o, 0, sizeof o);
        o.dev = h->st_dev; o.ino = (uint32_t)h->st_ino; o.mode = h->st_mode; o.nlink = h->st_nlink; o.uid = h->st_uid; o.gid = h->st_gid; o.rdev = h->st_rdev;
        o.at.sec = (int32_t)h->st_atimespec.tv_sec; o.at.nsec = (int32_t)h->st_atimespec.tv_nsec;
        o.mt.sec = (int32_t)h->st_mtimespec.tv_sec; o.mt.nsec = (int32_t)h->st_mtimespec.tv_nsec;
        o.ct.sec = (int32_t)h->st_ctimespec.tv_sec; o.ct.nsec = (int32_t)h->st_ctimespec.tv_nsec;
        o.size = h->st_size; o.blocks = h->st_blocks; o.blksize = h->st_blksize; o.flags = h->st_flags; o.gen = h->st_gen;
        memcpy(P32(addr), &o, sizeof o);
    }
}

/* ---- memory ---- */
volatile u32 g_mw_base;                 /* R2_MEMWATCH=IMAGE:...: that image's base (trap.c) */
static __thread int t_mw_pending;
static void sys_mmap(ss64_t *s, u32 argp) {
    u32 addr = A(0), len = A(1), flags = A(3);
    int prot = (int)A(2), fd = (int)A(4);
    off_t off = (off_t)((uint64_t)A(5) | ((uint64_t)A(6) << 32));
    /* A zero-length mapping succeeded with address 0 up to Mojave's kernel (xnu: "user_size == 0 ->
       error = 0"); Catalina's fails it with EINVAL. Adobe CoolType maps a font file's (empty) data
       fork before reading its resource fork: on EINVAL it marked Illustrator's LWFN fonts
       (Reqrd/Base/Myria*) Invalid, so a reopened document had a "Font Problems" alert.
       R2_MMAP0_EINVAL=1: the Catalina answer. */
    if (!len) { static int ei = -1; if (ei < 0) ei = getenv("R2_MMAP0_EINVAL") != 0;
        if (ei) ret_err(s, EINVAL); else ret_ok(s, 0);
        return; }
    {   /* R2_MEMWATCH=IMAGE:...: note where the named image's first executable mapping lands */
        extern volatile u32 g_mw_base; static int mw = -1; static char mwname[128];
        if (mw < 0) { const char *e = getenv("R2_MEMWATCH"), *c = e ? strchr(e, ':') : 0;
                      mw = c && c - e < 100; if (mw) snprintf(mwname, sizeof mwname, "/%.*s", (int)(c - e), e); }
        if (mw && !g_mw_base && fd >= 0 && !(flags & MAP_ANON)) {
            char fp[1024];
            if (!fcntl(fd, F_GETPATH, fp) && strstr(fp, mwname) && strlen(strstr(fp, mwname)) == strlen(mwname)) {
                rlog("[memwatch] %s: mmap(%#x, %#x, prot %d, flags %#x, off %#llx)", fp, addr, len, prot, flags, (long long)off);
                if (prot & PROT_EXEC) t_mw_pending = 1;
            }
        }
    }
    {   /* the translator maps PowerPC images read+execute but never runs them natively: a file
           mapping asked for execute makes Catalina check a quarantined file with syspolicyd (a
           Gatekeeper prompt; mmap waits for it), so the translator's file mappings are not executable */
        extern u32 g_xlat_lo, g_xlat_hi;
        if (fd >= 0 && !(flags & MAP_ANON) && (prot & PROT_EXEC) && EIP(s) >= g_xlat_lo && EIP(s) < g_xlat_hi &&
            !getenv("R2_XLAT_EXEC_MAPS")) prot &= ~PROT_EXEC;
    }
    u32 rlen = (len + R32_PAGE - 1) & ~(R32_PAGE - 1);
    u32 at;
    if (flags & MAP_FIXED) {
        if ((uint64_t)addr + rlen > R32_TOP || addr < R32_LOW) { ret_err(s, ENOMEM); return; }
        at = addr; low_mark(at, rlen, 1);
    } else {
        at = 0;
        if (addr && low_is_free(addr & ~(R32_PAGE - 1), rlen)) { at = addr & ~(R32_PAGE - 1); low_mark(at, rlen, 1); }
        if (!at) at = low_alloc(rlen, 0, 0);
        { extern void r2_vm_log(ss64_t *, u32, u32, int); r2_vm_log(s, rlen, at, at ? 0 : 3); }
        if (!at) { ret_err(s, ENOMEM); return; }
    }
    int hflags = (int)(flags | MAP_FIXED);
    /* old Darwin took a file mapping with neither MAP_SHARED nor MAP_PRIVATE (flags 0, MAP_FILE) as
       shared; Catalina's kernel refuses it (EINVAL). Office 2004 maps its font cache that way: the
       failed read made Word rebuild the cache with FontCacheTool on every launch (~15 s). */
    if (fd >= 0 && !(flags & (MAP_ANON | MAP_SHARED | MAP_PRIVATE))) hflags |= MAP_SHARED;
    /* the same for anonymous memory: old Darwin took MAP_ANON alone as private; Catalina refuses it
       (EINVAL). PowerPC Java 1.5 maps its safepoint polling page that way: the failed mmap (-1) went
       into every compiled method's poll ("li r10,-1; lwz r10,0(r10)"), and the first loop compiled
       crashed the JVM (JTest, javac). R2_NO_ANONPRIV=1: the Catalina answer. */
    {   static int ap = -1; if (ap < 0) ap = getenv("R2_NO_ANONPRIV") ? 0 : 1;
        if (ap && (flags & MAP_ANON) && !(flags & (MAP_SHARED | MAP_PRIVATE))) hflags |= MAP_PRIVATE; }
    int want_patch = (prot & PROT_EXEC) && !(flags & MAP_ANON) && fd >= 0;
    /* file and memory-object mappings keep the asked protection: a shared memory object refuses
       execute, and on Catalina mapping a quarantined file executable makes the kernel ask
       syspolicyd, which shows a Gatekeeper prompt and blocks mmap (the translator reading a
       downloaded PowerPC app hung). Anonymous memory that is readable is also executable, as on i386. */
    int hprot = (fd >= 0 && !(flags & MAP_ANON) && !(prot & PROT_EXEC)) ? prot : PROT32(prot);
    void *r = mmap(P32(at), len, hprot | (want_patch ? PROT_WRITE : 0), hflags, fd, off);
    {   static int shmtr = -1; if (shmtr < 0) shmtr = getenv("R2_SHMTRACE") != 0;
        if (shmtr && (flags & MAP_SHARED) && fd >= 0) rlog("[shm] mmap(%#x, %#x, prot %d, flags %#x, fd %d, off %#llx) at %#x -> %s", addr, len, prot, flags, fd, (long long)off, at, r == MAP_FAILED ? strerror(errno) : "ok"); }
    if (r == MAP_FAILED && errno == EINVAL && fd >= 0 && (flags & MAP_SHARED)) {
        /* POSIX shared memory (notifyd's, for libnotify) refuses a fixed address inside rhost's
           reserved low region: map it anywhere, then move the mapping (still shared) down */
        void *t = mmap(0, len, hprot, (int)(flags & ~MAP_FIXED), fd, off);
        if (t != MAP_FAILED) {
            mach_vm_address_t dst = at; vm_prot_t cur, max;
            kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, rlen, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                             mach_task_self(), (mach_vm_address_t)(uintptr_t)t, FALSE, &cur, &max, VM_INHERIT_SHARE);
            munmap(t, len);
            if (kr == KERN_SUCCESS) r = P32(at);
            else errno = EINVAL;
            if (g_trace) rlog("[mmap] shared fd %d remapped to %#x: %d", fd, at, kr);
        }
    }
    if (r == MAP_FAILED) {
        int e = errno;
        if (!(flags & MAP_FIXED)) low_free(at, rlen);
        ret_err(s, e); return;
    }
    if (want_patch) {
        if (!(flags & MAP_SHARED)) patch_sysenter(at, len);
        mprotect(P32(at), len, PROT32(prot));
    }
    if (g_trace > 1) rlog("[mmap] %#x len %#x prot %d flags %#x fd %d off %#llx", at, len, prot, flags, fd, (long long)off);
    if (t_mw_pending) { t_mw_pending = 0; if (!g_mw_base) g_mw_base = at; }
    {   /* R2_MMAPLOG=LO-HI (hex): log the mappings that land in that range, and who asked */
        static int ml = -1; static u32 mlo, mhi;
        if (ml < 0) { const char *e = getenv("R2_MMAPLOG"); ml = e != 0; if (e) { mlo = (u32)strtoul(e, 0, 16); const char *d = strchr(e, '-'); mhi = d ? (u32)strtoul(d + 1, 0, 16) : mlo + 1; } }
        if (ml && at < mhi && at + rlen > mlo) {
            char fp[1024] = ""; if (fd >= 0) fcntl(fd, F_GETPATH, fp);
            char fr[300] = ""; int fn = 0; u32 bp = EBP(s);
            for (int k = 0; k < 10 && bp > R32_LOW && bp < R32_TOP && !(bp & 3) && !low_is_free(bp & ~0xfffu, 8); k++, bp = rd32(bp))
                fn += snprintf(fr + fn, sizeof fr - fn, " %#x", rd32(bp + 4));
            rlog("[mmaplog] %#x len %#x prot %d flags %#x fd %d %s off %#llx eip %#x ret %#x frames%s", at, len, prot, flags, fd, fp, (long long)off, EIP(s), rd32(ESP(s)), fr);
        }
    }
    ret_ok(s, at);
}

static void sys_munmap(ss64_t *s, u32 argp) {
    u32 a = A(0), len = A(1);
    if ((uint64_t)a + len > R32_TOP || a < R32_LOW) { ret_err(s, EINVAL); return; }
    free32_user(a & ~(R32_PAGE - 1), len + (a & (R32_PAGE - 1)));
    ret_ok(s, 0);
}

/* ---- System V shared memory: shmat must attach below 4 GB. Passed through, the kernel chose an
   address in the 64-bit space and the 32-bit side kept its low half: GTK's X11 backend (MIT-SHM
   images, Wireshark) copied into that and crashed in memmove. The segment goes into the 32-bit
   space (the reservation's pages there give way to it); shmdt gives them back. ---- */
#include <sys/ipc.h>
#include <sys/shm.h>
static struct { u32 at, len; } g_shm[64];
static pthread_mutex_t g_shm_lock = PTHREAD_MUTEX_INITIALIZER;
static void sys_shmat(ss64_t *s, u32 argp) {
    int id = (int)A(0), fl = (int)A(2); u32 want = A(1), at, len;
    struct shmid_ds ds;
    if (shmctl(id, IPC_STAT, &ds)) { ret_err(s, errno); return; }
    len = (u32)((ds.shm_segsz + R32_PAGE - 1) & ~(uint64_t)(R32_PAGE - 1));
    if (!len) { ret_err(s, EINVAL); return; }
    if (want) {
        at = (fl & SHM_RND) ? want & ~(u32)(SHMLBA - 1) : want;
        if ((at & (R32_PAGE - 1)) || at < R32_LOW || (uint64_t)at + len > R32_TOP || !low_is_free(at, len)) { ret_err(s, EINVAL); return; }
        low_mark(at, len, 1);
    } else if (!(at = low_alloc(len, 0, 0))) { ret_err(s, ENOMEM); return; }
    munmap(P32(at), len);
    if (shmat(id, P32(at), fl & ~SHM_RND) == (void *)-1) {
        int e = errno;
        low_free(at, len);
        ret_err(s, e); return;
    }
    pthread_mutex_lock(&g_shm_lock);
    for (int k = 0; k < 64; k++) if (!g_shm[k].at) { g_shm[k].at = at; g_shm[k].len = len; break; }
    pthread_mutex_unlock(&g_shm_lock);
    if (g_trace || getenv("R2_SHMTRACE")) rlog("[shm] shmat(%d, %#x, %#x) -> %#x (%u KB)", id, want, fl, at, len >> 10);
    ret_ok(s, at);
}
static void sys_shmdt(ss64_t *s, u32 argp) {
    u32 a = A(0), len = 0;
    pthread_mutex_lock(&g_shm_lock);
    for (int k = 0; k < 64; k++) if (g_shm[k].at == a) { len = g_shm[k].len; g_shm[k].at = 0; break; }
    pthread_mutex_unlock(&g_shm_lock);
    if (a < R32_LOW || a >= R32_TOP) { ret_err(s, EINVAL); return; }
    if (shmdt(P32(a))) { ret_err(s, errno); return; }
    if (len) low_free(a, len);
    ret_ok(s, 0);
}
/* shmctl(IPC_STAT): the i386 struct shmid_ds (packed to 4: 32-bit size_t, time_t and pointer) */
static void sys_shmstat(ss64_t *s, u32 argp) {
    struct shmid_ds ds; u32 *o = P32(A(2));
    if (shmctl((int)A(0), IPC_STAT, &ds)) { ret_err(s, errno); return; }
    memcpy(o, &ds.shm_perm, 24);                       /* struct ipc_perm: the same 24 bytes */
    o[6] = (u32)ds.shm_segsz; o[7] = (u32)ds.shm_lpid; o[8] = (u32)ds.shm_cpid;
    o[9] = (u32)ds.shm_nattch;                         /* unsigned short, then 2 bytes of padding */
    o[10] = (u32)ds.shm_atime; o[11] = (u32)ds.shm_dtime; o[12] = (u32)ds.shm_ctime; o[13] = 0;
    ret_ok(s, 0);
}

/* ---- signals: remembered for the 32-bit world; the host keeps its trap handlers ---- */
struct sigaction32 { u32 handler, tramp, mask, flags; };
sigact32_t g_sig32[32];
__thread u32 g_sigaltstack32[3];          /* per thread, as sigaltstack is */
static int host_owns(int sig) { return sig == SIGILL || sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGTRAP; }

static void sys_sigaction(ss64_t *s, u32 argp) {
    int sig = (int)A(0); u32 act = A(1), oact = A(2);
    if (sig <= 0 || sig >= NSIG) { ret_err(s, EINVAL); return; }
    if (oact) { u32 *o = P32(oact); o[0] = g_sig32[sig].handler; o[1] = g_sig32[sig].mask; o[2] = g_sig32[sig].flags; }
    if (act) {
        struct sigaction32 *a = P32(act);
        g_sig32[sig].handler = a->handler; g_sig32[sig].tramp = a->tramp; g_sig32[sig].mask = a->mask; g_sig32[sig].flags = a->flags;
        if (!host_owns(sig) && sig != SIGKILL && sig != SIGSTOP) {
            struct sigaction h; memset(&h, 0, sizeof h);
            if (a->handler == 0 || a->handler == 1) h.sa_handler = a->handler ? SIG_IGN : SIG_DFL;  /* pass through */
            else {                                     /* caught: by the host, delivered by sig.c */
                extern void r2_sigentry(int, siginfo_t *, void *);
                h.sa_sigaction = r2_sigentry; h.sa_flags = SA_SIGINFO | SA_NODEFER | (a->flags & SA_RESTART);
            }
            sigaction(sig, &h, 0);
        }
        if (g_trace || (r2_siglog() && (sig == 10 || sig == 11 || sig == 5))) rlog("[sig] sigaction(%d) handler %#x flags %#x (eip %#x)", sig, a->handler, a->flags, EIP(s));
    }
    ret_ok(s, 0);
}

/* the mask lives in the host frame the thread returns through (a mask set here directly would be
   replaced by that frame's on return) */
static void sys_sigprocmask(ss64_t *s, u32 argp) {
    int how = (int)A(0); u32 set = A(1), oset = A(2);
    ucontext_t *uc = cur_uc();
    if (!uc) { ret_err(s, EINVAL); return; }
    sigset_t cur = uc->uc_sigmask, n = cur;
    if (set) {
        sigset_t hs = (sigset_t)rd32(set);
        if (how == SIG_BLOCK) n = cur | hs;
        else if (how == SIG_UNBLOCK) n = cur & ~hs;
        else if (how == SIG_SETMASK) n = hs;
        else { ret_err(s, EINVAL); return; }
        sigdelset(&n, SIGILL); sigdelset(&n, SIGSEGV); sigdelset(&n, SIGBUS); sigdelset(&n, SIGFPE); sigdelset(&n, SIGTRAP);
        sigdelset(&n, SIGKILL); sigdelset(&n, SIGSTOP);
        uc->uc_sigmask = n;
    }
    if (oset) wr32(oset, (u32)cur);
    ret_ok(s, 0);
}

/* ---- sysctl: sizes and a few 32-bit answers ---- */
static void sys_sysctl(ss64_t *s, u32 argp) {
    int *name = P32(A(0)); u32 namelen = A(1), oldp = A(2), oldlenp = A(3), newp = A(4), newlen = A(5);
    size_t ol = oldlenp ? rd32(oldlenp) : 0;
    if (g_trace) rlog("[sysctl] name %d.%d.%d.%d (len %u) oldlen %zu newp %#x", name[0], namelen > 1 ? name[1] : -1,
                      namelen > 2 ? name[2] : -1, namelen > 3 ? name[3] : -1, namelen, ol, newp);
    if (g_trace && namelen == 2 && name[0] == 0 && name[1] == 3 && newp) rlog("[sysctl]   name2oid %.*s", (int)newlen, (const char *)P32(newp));
    /* names Snow Leopard had and this system has not: a private OID each */
    static const struct { const char *name; int oid; const char *sval; } fake[] = {
        { "kern.exec.archhandler.powerpc", 0x7f00, "/usr/libexec/oah/translate" },
        { 0, 0, 0 }
    };
    if (namelen == 2 && name[0] == 0 && name[1] == 3 && newp) {          /* name2oid */
        for (int i = 0; fake[i].name; i++)
            if (newlen == strlen(fake[i].name) && !memcmp(P32(newp), fake[i].name, newlen)) {
                if (oldp && ol >= 8) { wr32(oldp, CTL_KERN); wr32(oldp + 4, fake[i].oid); }
                if (oldlenp) wr32(oldlenp, 8);
                ret_ok(s, 0); return;
            }
    }
    if (namelen == 2 && name[0] == CTL_KERN && name[1] >= 0x7f00) {
        for (int i = 0; fake[i].name; i++)
            if (fake[i].oid == name[1]) {
                size_t l = strlen(fake[i].sval) + 1;
                if (oldp) { if (ol < l) { ret_err(s, ENOMEM); return; } memcpy(P32(oldp), fake[i].sval, l); }
                if (oldlenp) wr32(oldlenp, (u32)l);
                ret_ok(s, 0); return;
            }
    }
    if (namelen >= 2 && name[0] == CTL_KERN && name[1] == KERN_USRSTACK32) {
        if (oldp && ol >= 4) wr32(oldp, 0xc0000000u);
        if (oldlenp) wr32(oldlenp, 4);
        ret_ok(s, 0); return;
    }
    /* KERN_PROC: the kernel answers rhost with the 64-bit kinfo_proc; the guest gets the 32-bit
       one (kinfo32.h). R2_KPROC64=1: pass the 64-bit records through as before. */
    if (namelen >= 3 && name[0] == CTL_KERN && name[1] == KERN_PROC && !getenv("R2_KPROC64")) {
        const size_t S64 = sizeof(struct kinfo_proc), S32 = sizeof(struct k32_kinfo_proc);
        size_t nl = 0;
        if (sysctl(name, namelen, 0, &nl, 0, 0)) { ret_err(s, errno); return; }
        size_t want = nl / S64 * S32;
        if (!oldp) { if (oldlenp) wr32(oldlenp, (u32)want); ret_ok(s, 0); return; }
        size_t cap = ol / S32, hl64 = (cap + 16) * S64;
        if (hl64 < nl) hl64 = nl + 16 * S64;
        struct kinfo_proc *kb = malloc(hl64);
        if (!kb) { ret_err(s, ENOMEM); return; }
        int r = sysctl(name, namelen, kb, &hl64, 0, 0);
        if (r) { int e = errno; free(kb); ret_err(s, e); return; }
        size_t n = hl64 / S64, i, put = n < cap ? n : cap;
        for (i = 0; i < put; i++) kinfo_to32((struct k32_kinfo_proc *)P32(oldp + (u32)(i * S32)), &kb[i]);
        free(kb);
        if (oldlenp) wr32(oldlenp, (u32)(n * S32));
        if (put < n) { ret_err(s, ENOMEM); return; }
        ret_ok(s, 0); return;
    }
    unsigned char tmp[4096]; size_t hl = sizeof tmp;
    void *ob = oldp ? (void *)tmp : 0;
    if (oldp && ol > sizeof tmp) { ob = P32(oldp); hl = ol; }
    int r = sysctl(name, namelen, ob, oldlenp ? &hl : 0, newp ? P32(newp) : 0, newlen);
    if (r && errno != ENOMEM) { ret_err(s, errno); return; }
    if (oldp && ob == tmp) {
        if (hl == 8 && ol == 4) { uint64_t v; memcpy(&v, tmp, 8); wr32(oldp, (u32)v); hl = 4; }    /* 64-bit value into an int */
        else if (hl > ol) { memcpy(P32(oldp), tmp, ol); if (oldlenp) wr32(oldlenp, (u32)hl); ret_err(s, ENOMEM); return; }
        else memcpy(P32(oldp), tmp, hl);
    }
    if (oldlenp) wr32(oldlenp, (u32)hl);
    if (r) ret_err(s, errno); else ret_ok(s, 0);
}

/* ---- iovec ---- */
static struct iovec *iov_in(u32 a, int n) {
    if (n < 0 || n > 1024) return 0;
    struct iovec *v = calloc(n ? n : 1, sizeof *v);
    for (int i = 0; i < n; i++) { v[i].iov_base = P32(rd32(a + 8 * i)); v[i].iov_len = rd32(a + 8 * i + 4); }
    return v;
}

/* the 32-bit world's thread_start / wqthread entry points (bsdthread_register) */
u32 g_thread_start32, g_wqthread32, g_pthsize32;
__thread int t_sys_newctx;          /* the call replaced the whole context (sigreturn, workqueue re-entry) */
u32 thread_create32(u32 func, u32 arg, u32 stack, u32 pthread, u32 flags, int *err);
void thread_terminate32(u32 stackaddr, u32 freesize, u32 port, u32 sem) __attribute__((noreturn));

static const char *sysname(u32 n) {
    switch (n) {
    case 1: return "exit"; case 3: return "read"; case 4: return "write"; case 5: return "open"; case 6: return "close";
    case 20: return "getpid"; case 33: return "access"; case 46: return "sigaction"; case 48: return "sigprocmask";
    case 54: return "ioctl"; case 58: return "readlink"; case 73: return "munmap"; case 74: return "mprotect"; case 92: return "fcntl";
    case 153: return "pread"; case 169: return "csops"; case 188: return "stat"; case 189: return "fstat"; case 190: return "lstat";
    case 194: return "getrlimit"; case 197: return "mmap"; case 199: return "lseek"; case 202: return "sysctl";
    case 294: return "shared_region_check_np"; case 327: return "issetugid"; case 336: return "proc_info";
    case 338: return "stat64"; case 339: return "fstat64"; case 340: return "lstat64"; case 366: return "bsdthread_register";
    case 372: return "thread_selfid";
    }
    return "?";
}

void bsd_syscall(ss64_t *s, u32 num, u32 argp, int sysenter_style) {
    if ((int32_t)num < 0) {                                   /* Mach trap through the sysenter stub */
        mach_trap(s, (int)num, argp);
        { extern void heapchk(const char *, long); heapchk("mach trap", (long)(int)num); }
        if (sysenter_style) s->__rip = EDX(s);
        return;
    }
    num &= 0xffff;                       /* sysenter stubs put the argument byte count in bits 16-23 */
    u32 ret_eip = sysenter_style ? EDX(s) : 0;
    char pb[2048], pb2[2048];
    long r;
    struct stat st;
    if (g_trace) rlog("[sys] %u %s(%#x, %#x, %#x, %#x) eip %#x", num, sysname(num), A(0), A(1), A(2), A(3), EIP(s));
    else if ((num == 328 || num == 37) && r2_siglog()) {
        mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
        rlog("[sig] %s(%#x, %d) by thread %#x eip %#x", num == 328 ? "__pthread_kill" : "kill", A(0), (int)A(1), me, EIP(s));
    }
    if (num > 600) {
        unsigned char *ip = P32(EIP(s));
        rlog("[sys] bad system call number %u at eip %#x (code %02x %02x %02x %02x %02x %02x | %02x %02x)", num, EIP(s),
             ip[-6], ip[-5], ip[-4], ip[-3], ip[-2], ip[-1], ip[0], ip[1]);
        if (A(0) > 0x1000 && A(0) < R32_TOP) rlog("[sys]   arg0 as text: %.200s", (const char *)P32(A(0)));
        ret_err(s, ENOSYS);
        if (sysenter_style) s->__rip = EDX(s);
        return;
    }
    /* the _nocancel variants (396..423) behave like their plain calls here */
    static const short nocancel[] = { 3, 4, 5, 6, 7, 27, 28, 29, 30, 65, 92, 93, 95, 98, 111, 120, 121, 133, 153, 154, 173, 230, 0, 0, 0, 0, 0, 334 };
    if (num >= 396 && num <= 423 && nocancel[num - 396]) num = (u32)nocancel[num - 396];
    switch (num) {
    case 1:
        if (g_trace && A(0)) {                                 /* who exits with an error: the 32-bit frame chain */
            u32 bp = EBP(s); char m[512]; int n = snprintf(m, sizeof m, "[sys] exit(%u) from", A(0));
            for (int i = 0; i < 12 && bp > R32_LOW && bp < R32_TOP && !(bp & 3); i++, bp = rd32(bp)) {
                n += snprintf(m + n, sizeof m - n, " %#x", rd32(bp + 4));
                for (int k = 0; k < 6 && i < 4; k++) {            /* string arguments of the inner frames */
                    u32 v = rd32(bp + 8 + 4 * k);
                    const char *str = P32(v);
                    if (v > 0x1000 && v < R32_TOP && !low_is_free(v & ~0xfffu, 1) && str[0] >= 0x20 && str[0] < 0x7f)
                        rlog("[sys]   frame %d arg %d %#x: \"%.160s\"", i, k, v, str);
                    else if (v > 0x1000 && v < R32_TOP && !low_is_free(v & ~0xfffu, 1)) {
                        const unsigned char *u = P32(v);
                        rlog("[sys]   frame %d arg %d %#x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | %.60s", i, k, v,
                             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], (const char *)u + 4);
                    } else rlog("[sys]   frame %d arg %d %#x", i, k, v);
                }
            }
            rlog("%s", m);
        }
        _exit((int)A(0));
    case 3: ret_host(s, read((int)A(0), P32(A(1)), A(2))); break;
    case 4: ret_host(s, write((int)A(0), P32(A(1)), A(2))); break;
    case 5: {
        const char *mp = MAPPED(0, pb);
        if (g_trace > 1) rlog("[sys]   open %s", mp);
        /* DTrace helper registration: its ioctl cannot be carried across from a PowerPC guest,
           and USDT probes of the 32-bit world are of no use to the host */
        if (!strcmp(mp, "/dev/dtracehelper")) { ret_err(s, ENOENT); break; }
        ret_host(s, open(mp, (int)A(1), (int)A(2))); break;
    }
    case 6: {   /* close: not rhost's own log (a forked Java child closes every descriptor before its exec) */
        extern int g_rlog_fd;
        if ((int)A(0) >= 0 && (int)A(0) == g_rlog_fd) { ret_host(s, 0); break; }
        ret_host(s, close((int)A(0))); break; }
    case 33: ret_host(s, access(MAPPED(0, pb), (int)A(1))); break;
    case 46: sys_sigaction(s, argp); break;
    case 48: case 329: sys_sigprocmask(s, argp); break;     /* sigprocmask, __pthread_sigmask */
    case 184: sig32_return(s, cur_uc(), A(0), A(1)); t_sys_newctx = 1; break; /* sigreturn: no return value */
    case 53: {                                                 /* sigaltstack: remembered for 32-bit signal delivery */
        u32 ss = A(0), oss = A(1);
        if (oss) { wr32(oss, g_sigaltstack32[0]); wr32(oss + 4, g_sigaltstack32[1]); wr32(oss + 8, g_sigaltstack32[2]); }
        if (ss) { g_sigaltstack32[0] = rd32(ss); g_sigaltstack32[1] = rd32(ss + 4); g_sigaltstack32[2] = rd32(ss + 8); }
        ret_ok(s, 0); break;
    }
    case 58: ret_host(s, readlink(MAPPED(0, pb), P32(A(1)), A(2))); break;
    case 73: sys_munmap(s, argp); break;
    case 74: ret_host(s, mprotect(P32(A(0)), A(1), PROT32((int)A(2)))); break;
    case 2: case 66: sys_fork(s); break;                       /* fork, vfork (spawn.c) */
    case 59: sys_execve(s, argp); break;
    case 244: sys_posix_spawn(s, argp); break;
    case 116: {                                                /* gettimeofday: seconds in eax, microseconds in edx */
        struct timeval tv; gettimeofday(&tv, 0);
        if (A(0)) { wr32(A(0), (u32)tv.tv_sec); wr32(A(0) + 4, (u32)tv.tv_usec); }
        ret_ok2(s, (u32)tv.tv_sec, (u32)tv.tv_usec); break;
    }
    case 120: { struct iovec *v = iov_in(A(1), (int)A(2)); if (!v) { ret_err(s, EINVAL); break; } ret_host(s, readv((int)A(0), v, (int)A(2))); free(v); break; }
    case 121: { struct iovec *v = iov_in(A(1), (int)A(2)); if (!v) { ret_err(s, EINVAL); break; } ret_host(s, writev((int)A(0), v, (int)A(2))); free(v); break; }
    case 153: ret_host(s, pread((int)A(0), P32(A(1)), A(2), (off_t)((uint64_t)A(3) | ((uint64_t)A(4) << 32)))); break;
    case 154: ret_host(s, pwrite((int)A(0), P32(A(1)), A(2), (off_t)((uint64_t)A(3) | ((uint64_t)A(4) << 32)))); break;
    /* the *at calls: an absolute path (or one relative to the current directory) is mapped like the
       plain calls; a path relative to a directory descriptor is used as it is */
    case 463: case 464: {                                      /* openat(fd, path, flags, mode) */
        const char *pp = (const char *)P32(A(1));
        if (g_trace > 1) rlog("[sys]   openat %d %s", (int)A(0), pp);
        if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
        ret_host(s, openat((int)A(0), pp, (int)A(2), (int)A(3)));
        break;
    }
    case 466: { const char *pp = (const char *)P32(A(1)); if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
                ret_host(s, faccessat((int)A(0), pp, (int)A(2), (int)A(3))); break; }
    case 469: case 470: {                                      /* fstatat / fstatat64(fd, path, stat, flag) */
        const char *pp = (const char *)P32(A(1));
        if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
        if (fstatat((int)A(0), pp, &st, (int)A(3)) && !(pp[0] == '/' && cache_stat_fake((const char *)P32(A(1)), &st))) ret_err(s, errno);
        else { if (pp[0] == '/') cache_stat_fix((const char *)P32(A(1)), pb, &st); put_stat(A(2), &st, num == 470); ret_ok(s, 0); }
        break;
    }
    case 472: { const char *pp = (const char *)P32(A(1)); if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
                ret_host(s, unlinkat((int)A(0), pp, (int)A(2))); break; }
    case 473: { const char *pp = (const char *)P32(A(1)); if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
                ret_host(s, readlinkat((int)A(0), pp, P32(A(2)), A(3))); break; }
    case 475: { const char *pp = (const char *)P32(A(1)); if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
                ret_host(s, mkdirat((int)A(0), pp, (mode_t)A(2))); break; }
    case 467: { const char *pp = (const char *)P32(A(1)); if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
                ret_host(s, fchmodat((int)A(0), pp, (mode_t)A(2), (int)A(3))); break; }
    case 468: { const char *pp = (const char *)P32(A(1)); if (pp[0] == '/') pp = map_path(pp, pb, sizeof pb);
                ret_host(s, fchownat((int)A(0), pp, (uid_t)A(2), (gid_t)A(3), (int)A(4))); break; }
    case 188: case 338: if (stat(MAPPED(0, pb), &st) && !cache_stat_fake((const char *)P32(A(0)), &st)) ret_err(s, errno); else {
            cache_stat_fix((const char *)P32(A(0)), pb, &st);
            put_stat(A(1), &st, num == 338); ret_ok(s, 0);
            if (g_trace > 1) rlog("[sys]   stat size %lld -> 32-bit size field %lld", (long long)st.st_size, *(long long *)P32(A(1) + (num == 338 ? 60 : 48)));
        } break;
    case 190: case 340: if (lstat(MAPPED(0, pb), &st)) ret_err(s, errno); else { cache_stat_fix((const char *)P32(A(0)), pb, &st); put_stat(A(1), &st, num == 340); ret_ok(s, 0); } break;
    case 189: case 339: if (fstat((int)A(0), &st)) ret_err(s, errno); else { put_stat(A(1), &st, num == 339); ret_ok(s, 0); } break;
    /* stat_extended / lstat_extended / fstat_extended (279-281) and their 64-bit-inode forms (341-343):
       (path or fd, struct stat *, void *xsecurity, size_t *xsecurity_size). They used to go to the kernel
       unchanged, which wrote the x86_64 struct stat (16-byte timespecs) and a 64-bit size_t: the i386
       caller read st_size from the wrong place. copyfile() starts with stat64_extended, so every copy a
       32-bit library made (liboah's FSpExchangeFiles: Photoshop's "Color Settings.csf" became 0 bytes)
       read nothing. The host call is always the 64-bit-inode form, the struct stat this program has. */
    case 279: case 280: case 281: case 341: case 342: case 343: {
        u32 *szp = A(3) ? (u32 *)P32(A(3)) : 0;
        size_t xs = szp ? *szp : 0;
        void *xsec = A(2) ? P32(A(2)) : 0;
        int hn = num == 279 || num == 341 ? 341 : num == 280 || num == 342 ? 342 : 343;
        long r = hn == 343 ? syscall(343, (long)(int)A(0), (long)&st, (long)xsec, (long)(szp ? &xs : 0))
                           : syscall(hn, (long)MAPPED(0, pb), (long)&st, (long)xsec, (long)(szp ? &xs : 0));
        if (r) ret_err(s, errno);
        else { put_stat(A(1), &st, num >= 341); if (szp) *szp = (u32)xs; ret_ok(s, 0); }
        break;
    }
    case 197: sys_mmap(s, argp); break;
    case 199: {
        off_t o = lseek((int)A(0), (off_t)((uint64_t)A(1) | ((uint64_t)A(2) << 32)), (int)A(3));
        if (o == -1) ret_err(s, errno); else ret_ok2(s, (u32)o, (u32)((uint64_t)o >> 32));
        break;
    }
    case 201: ret_host(s, ftruncate((int)A(0), (off_t)((uint64_t)A(1) | ((uint64_t)A(2) << 32)))); break;
    case 200: ret_host(s, truncate(MAPPED(0, pb), (off_t)((uint64_t)A(1) | ((uint64_t)A(2) << 32)))); break;
    case 336: {   /* proc_info(callnum, pid, flavor, uint64 arg, buffer, size): the 64-bit argument takes two
                     words; the region and vnode structures have the same layout in both worlds */
        long r = syscall(336, (long)(int)A(0), (long)(int)A(1), (long)(int)A(2),
                         (long)((uint64_t)A(3) | ((uint64_t)A(4) << 32)), (long)A(5), (long)(int)A(6));
        ret_host(s, (int)r);
        break;
    }
    case 202: sys_sysctl(s, argp); break;
    case 294: ret_err(s, ENOSYS); break;                       /* no shared cache: dyld maps each library */
    case 169:                                                  /* csops: status queries only; never mark this process */
        if (A(1) == 0 || A(1) >= 4) ret_host(s, syscall(169, (long)A(0), (long)A(1), (long)A(2), (long)A(3)));
        else ret_ok(s, 0);
        break;
    case 366: {   /* bsdthread_register(thread_start, wqthread, pthsize[, registration data, size, dq offset]) */
        extern u32 g_tsd_offset32; extern int g_cp_modern;
        g_thread_start32 = A(0); g_wqthread32 = A(1); g_pthsize32 = A(2);
        if (!g_cp_modern) { ret_ok(s, 0); break; }
        /* 10.9+ libpthread: the registration data gives the TSD offset in the pthread (the kernel then
           sets new threads' %gs base); the result is the kernel's feature bits */
        u32 d = A(3), dsz = A(4);
        extern u32 g_mts_offset32;
        if (d && dsz >= 28) g_tsd_offset32 = rd32(d + 24);
        if (d && dsz >= 36) g_mts_offset32 = rd32(d + 32);   /* mach_thread_self slot in the TSD */
        if (d && dsz >= 48) { wr32(d + 16, 0); wr32(d + 20, 0); wr32(d + 36, 0); wr32(d + 40, 0); wr32(d + 44, 0); }
        if (g_trace) rlog("[thread] registered: start %#x wq %#x pthsize %#x tsd offset %#x", A(0), A(1), A(2), g_tsd_offset32);
        { static long feat = -1;
          if (feat < 0) { const char *v = getenv("R2_WQ_FEATURES"); feat = v ? strtol(v, 0, 0) : 0x400000df; }   /* + WORKLOOP (wq.c) */
          ret_ok(s, (u32)feat); }   
        break;
    }
    case 367: ret_ok(s, 0); break;                             /* workq_open */
    case 368: wq_kernreturn(s, argp); break;                   /* workq_kernreturn */
    case 478: ret_ok(s, 0); break;                             /* bsdthread_ctl (QoS overrides): accepted */
    case 360: {                                                /* bsdthread_create */
        int e = 0;
        u32 r = thread_create32(A(0), A(1), A(2), A(3), A(4), &e);
        if (!r) ret_err(s, e); else ret_ok(s, r);
        break;
    }
    case 361: thread_terminate32(A(0), A(1), A(2), A(3)); break;
    case 42: {                                                 /* pipe(): the two fds in eax and edx */
        /* (the passthrough returned only rax: the write end came back as stack garbage. Office 2004's
           ShMem forks a child that dup2s it onto stdout; the child exited 1 and Word stopped) */
        int fds[2];
        if (pipe(fds)) ret_err(s, errno); else ret_ok2(s, (u32)fds[0], (u32)fds[1]);
        break;
    }
    case 372: { uint64_t tid = 0; pthread_threadid_np(0, &tid); ret_ok2(s, (u32)tid, (u32)(tid >> 32)); break; }
    case 128: ret_host(s, rename(MAPPED(0, pb), map_path((const char *)P32(A(1)), pb2, sizeof pb2))); break;
    case 10: ret_host(s, unlink(MAPPED(0, pb))); break;
    case 12: ret_host(s, chdir(MAPPED(0, pb))); break;
    case 136: ret_host(s, mkdir(MAPPED(0, pb), (mode_t)A(1))); break;
    case 262: sys_shmat(s, argp); break;
    case 264: sys_shmdt(s, argp); break;
    case 263: if (A(1) == IPC_STAT && A(2)) { sys_shmstat(s, argp); break; } goto passthrough;
    default:
    passthrough:
        if (num >= 314 && num <= 320 && sys_aio(s, num, argp)) break;   /* POSIX aio (i386 aiocb) */
        if (sys_conv(s, num, argp)) break;                      /* structures that differ (sysconv.c) */
        /* everything else: ints and pointers, same layout */
        /* zero-extended: the kernel takes the low 32 bits of int arguments, and pointers are below 4 GB */
        {   extern long sys8(long, long, long, long, long, long, long, long, long);
            r = sys8(num, (long)A(0), (long)A(1), (long)A(2), (long)A(3), (long)A(4), (long)A(5), (long)A(6), (long)A(7)); }
        if (g_trace && r == -1) rlog("[sys] %u passthrough -> errno %d", num, errno);
        {   static int shmtr = -1;                              /* R2_SHMTRACE: POSIX shm / semaphores */
            if (shmtr < 0) shmtr = getenv("R2_SHMTRACE") != 0;
            if (shmtr && (num == 266 || num == 267 || num == 268 || num == 270))
                rlog("[shm] %u(\"%s\", %#x, %#o) -> %ld%s%s", num, A(0) ? (const char *)P32(A(0)) : "", A(1), A(2), r,
                     r == -1 ? " errno " : "", r == -1 ? strerror(errno) : "");
            else if (shmtr && (num == 201 || num == 189))
                rlog("[shm] %u(fd %u, %#x) -> %ld%s", num, A(0), A(1), r, r == -1 ? strerror(errno) : "");
        }
        ret_host(s, r);
        break;
    }
    if (g_trace > 1) rlog("[sys]   -> %#x%s", EAX(s), (s->__rflags & CF) ? " (error)" : "");
    { extern void heapchk(const char *, long); heapchk("syscall", (long)num); }
    if (sysenter_style && !t_sys_newctx) s->__rip = ret_eip;
    t_sys_newctx = 0;
}

void machdep_trap(ss64_t *s, u32 num, u32 argp) {
    switch (num) {
    case 3: {                                                  /* thread_fast_set_cthread_self(self): %gs base */
        u32 self = A(0);
        if (g_trace) rlog("[machdep] cthread_self %#x", self);
        unsigned short sel = tsd32_set(self);              /* the %gs base restored from the signal frame */
        s->__rax = sel;
        /* the kernel keeps the hardware %gs selector as it is; a null one faults in 32-bit code,
           so the thread loads its own (an LDT segment with the same base) once */
        set_gs_via_stub(s, sel);
        break;
    }
    case 0x5232: {                                             /* the hostcall bridge (bridge/hostcall.c) */
        extern void hostcall(ss64_t *s, u32 argp);
        hostcall(s, argp);
        break;
    }
    default:
        rlog("[machdep] unsupported call %u", num);
        s->__rax = (u32)-1;
        break;
    }
}
