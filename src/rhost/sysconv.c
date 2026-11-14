/* sysconv.c - system calls whose structures differ between i386 and x86_64 (long, pointer and
   timeval/timespec members), or whose 64-bit arguments take two words on i386. */
#include "rhost.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/attr.h>
#include <fcntl.h>
#include <pthread.h>

#define A(i) rd32(argp + 4 * (i))
static uint64_t rd64(u32 a) { return (uint64_t)rd32(a) | ((uint64_t)rd32(a + 4) << 32); }

struct tv32 { int32_t sec, usec; };
struct ts32 { int32_t sec, nsec; };
struct kev32 { u32 ident; int16_t filter; uint16_t flags; u32 fflags; int32_t data; u32 udata; };
struct ru32 {
    struct tv32 utime, stime;
    int32_t maxrss, ixrss, idrss, isrss, minflt, majflt, nswap, inblock, oublock, msgsnd, msgrcv, nsignals, nvcsw, nivcsw;
};
struct msghdr32 { u32 name, namelen, iov; int32_t iovlen; u32 control, controllen; int32_t flags; };

static int tv_in(u32 a, struct timeval *tv) { if (!a) return 0; tv->tv_sec = (int32_t)rd32(a); tv->tv_usec = (int32_t)rd32(a + 4); return 1; }
static void tv_out(u32 a, const struct timeval *tv) { if (a) { wr32(a, (u32)tv->tv_sec); wr32(a + 4, (u32)tv->tv_usec); } }
static void ru_out(u32 a, const struct rusage *r) {
    if (!a) return;
    struct ru32 *o = P32(a);
    o->utime.sec = (int32_t)r->ru_utime.tv_sec; o->utime.usec = r->ru_utime.tv_usec;
    o->stime.sec = (int32_t)r->ru_stime.tv_sec; o->stime.usec = r->ru_stime.tv_usec;
    o->maxrss = (int32_t)r->ru_maxrss; o->ixrss = (int32_t)r->ru_ixrss; o->idrss = (int32_t)r->ru_idrss; o->isrss = (int32_t)r->ru_isrss;
    o->minflt = (int32_t)r->ru_minflt; o->majflt = (int32_t)r->ru_majflt; o->nswap = (int32_t)r->ru_nswap; o->inblock = (int32_t)r->ru_inblock;
    o->oublock = (int32_t)r->ru_oublock; o->msgsnd = (int32_t)r->ru_msgsnd; o->msgrcv = (int32_t)r->ru_msgrcv;
    o->nsignals = (int32_t)r->ru_nsignals; o->nvcsw = (int32_t)r->ru_nvcsw; o->nivcsw = (int32_t)r->ru_nivcsw;
}
static struct iovec *iov_conv(u32 a, int n) {
    if (n < 0 || n > 1024) return 0;
    struct iovec *v = calloc((size_t)n + 1, sizeof *v);
    for (int i = 0; i < n; i++) { v[i].iov_base = P32(rd32(a + 8 * i)); v[i].iov_len = rd32(a + 8 * i + 4); }
    return v;
}


/* ---- one volume for "/": Catalina's system volume (read-only, mounted on /) and its data volume
   (mounted nobrowse on /System/Volumes/Data, joined to / by firmlinks) form one tree, and every
   object reports the data volume's device. Mojave's and Snow Leopard's File Managers know nothing of
   that: an object's device (data) did not match the fsid of the volume mounted on / (system), so
   paths came out as /System/Volumes/Data/<garbage> and FSPathMakeRef("/") failed. The 32-bit world
   is shown a single volume: the system volume's statfs takes the data volume's fsid and is not
   read-only, the data volume is "mounted on /" for a path under it and left out of volume lists.
   R2_NO_VOLGROUP=1 shows the volumes as they are. */
static fsid_t g_sysfsid, g_datafsid; static int g_vg = -1;
/* Only in an i386 program's process. A PowerPC program's (the translator's) keeps the two volumes as
   they are: its 10.6 File Manager copes with them, and with one volume its font database (fontd) could
   not revive its file tokens and listed no system fonts. Native code in that process (liboah, the
   bridged Mojave frameworks) must see the same volumes as the guest, or FSRefs and aliases break.
   R2_VOLGROUP_PPC=1 shows a PowerPC process the single volume too. */
static int vg_on(void) {
    static int ppc = -1;
    extern u32 g_xlat_hi;
    /* Nov 6: PowerPC apps now see the single volume too (not the helper daemons, OAH_AGENT: fontd
       is the one whose database needed two). With two, Navigation Services' save dialogs opened on
       the computer level ("Where: <Mac name>", Save disabled: TextWrangler, Illustrator, GoLive,
       Photoshop) instead of Documents. R2_VOLGROUP_PPC=0: two volumes for PowerPC apps again. */
    if (ppc < 0) { const char *e = getenv("R2_VOLGROUP_PPC"); ppc = e ? atoi(e) != 0 : !getenv("OAH_AGENT"); }
    if (g_xlat_hi && !ppc) return 0;
    if (g_vg < 0) {
        struct statfs a, b;
        g_vg = 0;
        if (!getenv("R2_NO_VOLGROUP") && !statfs("/", &a) && !statfs("/System/Volumes/Data", &b) &&
            !strcmp(b.f_mntonname, "/System/Volumes/Data") && memcmp(&a.f_fsid, &b.f_fsid, sizeof a.f_fsid)) {
            g_sysfsid = a.f_fsid; g_datafsid = b.f_fsid; g_vg = 1;
        }
    }
    return g_vg;
}
/* No volume group (PowerPC processes): "/" is still the read-only system volume, which 10.6's
   File Manager reports as a locked startup disk (vcbAtrb 0x8080). Mactracker checks
   Volume(0).Locked and refused to run ("needs Mac OS X 10.4"). Writes land on the Data volume
   through the firmlinks anyway. Not for the helper daemons (OAH_AGENT): the PowerPC fontd then
   regenerates its read-only font database, finds no fonts on this macOS's volumes and gives
   clients fonts with no data (CoreText crashed in TASCIIEncoder::Encode). R2_ROOT_RDONLY=1 keeps
   the flag. */
static void root_rw_fix(uint32_t *flags) {
    static int on = -1;
    if (on < 0) on = !getenv("R2_ROOT_RDONLY") && !getenv("OAH_AGENT") && access("/System/Volumes/Data", F_OK) == 0;
    if (on && (*flags & MNT_ROOTFS)) *flags &= ~(uint32_t)MNT_RDONLY;
}
/* adjust a host struct statfs for the 32-bit world; 0: leave it out of a volume list */
static int vg_fix(struct statfs *h, int inlist) {
    if (!vg_on()) { root_rw_fix(&h->f_flags); return 1; }
    if (!strcmp(h->f_mntonname, "/System/Volumes/Data")) {
        if (inlist) return 0;
        strcpy(h->f_mntonname, "/");
        h->f_flags &= ~(uint32_t)(MNT_DONTBROWSE | MNT_RDONLY);
    } else if (!memcmp(&h->f_fsid, &g_sysfsid, sizeof g_sysfsid)) {
        h->f_fsid = g_datafsid;
        h->f_flags &= ~(uint32_t)(MNT_DONTBROWSE | MNT_RDONLY);
    }
    return 1;
}

/* i386 struct statfs of the 32-bit inode interface (statfs, fstatfs, getfsstat): longs are 4 bytes */
#pragma pack(push, 4)
struct statfs32 {
    int16_t otype, oflags; int32_t bsize, iosize, blocks, bfree, bavail, files, ffree; int32_t fsid[2];
    u32 owner; int16_t reserved1, type; int32_t flags, reserved2[2];
    char fstypename[15], mntonname[90], mntfromname[90], reserved3; int32_t reserved4[4];
};
#pragma pack(pop)
static int32_t clamp32(uint64_t v) { return v > 0x7fffffffull ? 0x7fffffff : (int32_t)v; }
static void statfs_out(struct statfs32 *o, const struct statfs *h0) {
    struct statfs hc = *h0, *h = &hc;
    vg_fix(h, 0);
    memset(o, 0, sizeof *o);
    /* block counts in units that fit: scale up the block size like the 10.6 kernel did for large volumes */
    uint64_t bs = h->f_bsize, blocks = h->f_blocks, bfree = h->f_bfree, bavail = h->f_bavail;
    while (blocks > 0x7fffffffull && bs < 0x40000000ull) { bs <<= 1; blocks >>= 1; bfree >>= 1; bavail >>= 1; }
    o->bsize = (int32_t)bs; o->iosize = h->f_iosize; o->blocks = clamp32(blocks); o->bfree = clamp32(bfree); o->bavail = clamp32(bavail);
    o->files = clamp32(h->f_files); o->ffree = clamp32(h->f_ffree);
    o->fsid[0] = h->f_fsid.val[0]; o->fsid[1] = h->f_fsid.val[1]; o->owner = h->f_owner;
    o->type = (int16_t)h->f_type; o->flags = (int32_t)h->f_flags;
    if (!strcmp(h->f_mntonname, "/System/Volumes/Data")) o->flags &= ~MNT_DONTBROWSE;   /* see data_browse */
    strncpy(o->fstypename, h->f_fstypename, sizeof o->fstypename - 1);
    strncpy(o->mntonname, h->f_mntonname, sizeof o->mntonname - 1);
    strncpy(o->mntfromname, h->f_mntfromname, sizeof o->mntfromname - 1);
}

/* ---- getattrlist & co: the attribute buffer of a 64-bit caller has 16-byte timespecs, an i386
   caller's 8-byte ones; everything else (4-byte packing, attrreference offsets relative to the
   reference) is the same. The buffer is rebuilt in the i386 layout. */
#define AK_FIX 0
#define AK_TS 1
#define AK_REF 2
typedef struct { u32 bit; unsigned char size, kind; } attrdef_t;
static const attrdef_t g_cmn[] = {
    {0x1,8,AK_REF},{0x2,4,0},{0x4,8,0},{0x8,4,0},{0x10,4,0},{0x20,8,0},{0x40,8,0},{0x80,8,0},{0x100,4,0},
    {0x200,16,AK_TS},{0x400,16,AK_TS},{0x800,16,AK_TS},{0x1000,16,AK_TS},{0x2000,16,AK_TS},{0x4000,32,0},
    {0x8000,4,0},{0x10000,4,0},{0x20000,4,0},{0x40000,4,0},{0x80000,4,0},{0x100000,4,0},{0x200000,4,0},
    {0x400000,8,AK_REF},{0x800000,16,0},{0x1000000,16,0},{0x2000000,8,0},{0x4000000,8,0},{0x8000000,8,AK_REF},
    {0x10000000,16,AK_TS},{0x20000000,4,0},{0x40000000,4,0},{0,0,0}};
static const attrdef_t g_vol[] = {
    {0x1,4,0},{0x2,4,0},{0x4,8,0},{0x8,8,0},{0x10,8,0},{0x20,8,0},{0x40,8,0},{0x80,4,0},{0x100,4,0},{0x200,4,0},
    {0x400,4,0},{0x800,4,0},{0x1000,8,AK_REF},{0x2000,8,AK_REF},{0x4000,4,0},{0x8000,8,AK_REF},{0x10000,8,0},
    {0x20000,32,0},{0x40000,16,0},{0x800000,8,0},{0x10000000,8,0},{0x20000000,8,0},{0x40000000,40,0},{0,0,0}};
static const attrdef_t g_dir[] = { {0x1,4,0},{0x2,4,0},{0x4,4,0},{0x8,8,0},{0x10,4,0},{0x20,8,0},{0,0,0} };
static const attrdef_t g_file[] = {
    {0x1,4,0},{0x2,8,0},{0x4,8,0},{0x8,4,0},{0x10,4,0},{0x20,4,0},{0x40,4,0},{0x80,4,0},{0x100,8,AK_REF},
    {0x200,8,0},{0x400,8,0},{0x800,64,0},{0x1000,8,0},{0x2000,8,0},{0x4000,64,0},{0,0,0}};
static const attrdef_t g_fork[] = { {0x1,8,0},{0x2,8,0},{0,0,0} };

struct attrlist32 { uint16_t bitmapcount, reserved; u32 commonattr, volattr, dirattr, fileattr, forkattr; };

/* ---- 32-bit object IDs for Catalina's system volume.
   Since 10.15 the read-only system volume and the data volume form one file system whose system
   side has 64-bit object IDs (bit 60 set). Carbon's File Manager keeps 32-bit node IDs (FSRefs,
   directory IDs) and finds objects again through /.vol/<dev>/<id>: with the kernel's truncated IDs
   the system side (/System/Library/...) was unreachable (FSFindFolder(kSystemDomain, ...) -43).
   Such IDs are given to the 32-bit world as aliases (0xF0000000 + n) with the object's path, and
   /.vol/<dev>/<alias>[/...] paths lead back to it (vol_alias_map, used by map_path). */
#define ALIAS_BASE 0xF0000000u
typedef struct { uint64_t id; char *path; } alias_t;
static alias_t *g_al; static u32 g_nal, g_alcap;
static u32 *g_alhash; static u32 g_alhcap;              /* open addressing: index + 1 */
static pthread_mutex_t g_almu = PTHREAD_MUTEX_INITIALIZER;
static u32 alias_of(uint64_t id, const char *path) {
    if (id < 0x100000000ull) return (u32)id;
    pthread_mutex_lock(&g_almu);
    if (!g_alhcap) { g_alhcap = 1u << 16; g_alhash = calloc(g_alhcap, sizeof *g_alhash); }
    u32 h = (u32)((id * 0x9E3779B97F4A7C15ull) >> 48) & (g_alhcap - 1), k;
    while ((k = g_alhash[h]) && g_al[k - 1].id != id) h = (h + 1) & (g_alhcap - 1);
    if (!k) {
        if (g_nal == g_alcap) { g_alcap = g_alcap ? g_alcap * 2 : 1024; g_al = realloc(g_al, g_alcap * sizeof *g_al); }
        if (g_nal >= g_alhcap / 2 || g_nal >= 0x0fffffff) { pthread_mutex_unlock(&g_almu); return (u32)id; }  /* full */
        g_al[g_nal].id = id; g_al[g_nal].path = path ? strdup(path) : 0;
        g_alhash[h] = k = ++g_nal;
    } else if (path && !g_al[k - 1].path) g_al[k - 1].path = strdup(path);
    pthread_mutex_unlock(&g_almu);
    return ALIAS_BASE + k - 1;
}
/* /.vol/<dev>/<alias>[/rest] -> the object's path [/rest]; 1 if mapped */
int vol_alias_map(const char *p, char *buf, size_t n) {
    if (strncmp(p, "/.vol/", 6)) return 0;
    const char *q = strchr(p + 6, '/');
    if (!q) return 0;
    char *e; unsigned long long id = strtoull(q + 1, &e, 10);
    if (e == q + 1 || (*e && *e != '/') || id < ALIAS_BASE || id >= 0x100000000ull) return 0;
    pthread_mutex_lock(&g_almu);
    const char *path = id - ALIAS_BASE < g_nal ? g_al[id - ALIAS_BASE].path : 0;
    if (path) snprintf(buf, n, "%s%s", path, e);
    pthread_mutex_unlock(&g_almu);
    return path != 0;
}

/* 1 if p is exactly /.vol/<dev>/<alias> (an alias of a system volume object, no further components) */
static int vol_alias_leaf(const char *p) {
    if (strncmp(p, "/.vol/", 6)) return 0;
    const char *q = strchr(p + 6, '/');
    if (!q) return 0;
    char *e; unsigned long long id = strtoull(q + 1, &e, 10);
    return e != q + 1 && !*e && id >= ALIAS_BASE && id < 0x100000000ull;
}

/* where an attribute buffer's objects are: their own path (getattrlist), or their directory (bulk:
   the object is <dir>/<name>), so that aliases can lead back to them */
typedef struct { const char *obj, *dir; } apath_t;
#define CMN_NAME 0x1u
#define CMN_OBJID 0x20u
#define CMN_PAROBJID 0x80u
#define CMN_FILEID 0x2000000u
#define CMN_PARENTID 0x4000000u
#define CMN_IDS (CMN_OBJID | CMN_PAROBJID)      /* the 32-bit IDs; FILEID / PARENTID (64-bit) stay as they are:
                                                   10.7+ code (Mojave's CarbonCore) uses those and checks them */

/* the attribute list given to the kernel: with the 64-bit IDs as well when an ID is asked for */
u32 g_xlat_lo, g_xlat_hi;                         /* the translator's text (rhost.c) */
static struct attrlist32 attr_hostlist(const struct attrlist32 *al, const ss64_t *s) {
    struct attrlist32 h = *al;
    static int off = -1;
    if (off < 0) off = getenv("R2_NO_IDALIAS") != 0;
    /* only for the PowerPC guest's (10.6) File Manager: Mojave's i386 CarbonCore checks the 32-bit IDs
       against the 64-bit ones and needs the kernel's */
    if (off || !(EIP(s) >= g_xlat_lo && EIP(s) < g_xlat_hi)) return h;
    if ((h.commonattr & CMN_IDS) && !h.volattr) h.commonattr |= CMN_FILEID | CMN_PARENTID;   /* not with volume attributes (EINVAL) */
    return h;
}

/* host buffer (h, hlen) for the attributes of hal -> i386 buffer (o, olen) with those of al; returns
   the i386 length */
static u32 attr_repack(const struct attrlist32 *al, const struct attrlist32 *hal, const apath_t *ap,
                       const unsigned char *h, u32 hlen, unsigned char *o, u32 olen) {
    const attrdef_t *groups[5] = { g_cmn, g_vol, g_dir, g_file, g_fork };
    u32 want[5] = { al->commonattr, al->volattr, al->dirattr, al->fileattr, al->forkattr };
    u32 masks[5] = { hal->commonattr, hal->volattr, hal->dirattr, hal->fileattr, hal->forkattr };
    unsigned char *tmp = calloc(1, hlen + 64);
    struct { u32 opos; const unsigned char *data; u32 len; int mnt; } refs[32]; int nref = 0;
    u32 hp = 4, op = 4;
    u32 hend = *(const u32 *)h; if (hend > hlen) hend = hlen;
    if (masks[0] & 0x80000000u) {                 /* ATTR_CMN_RETURNED_ATTRS: only those are in the buffer */
        for (int g = 0; g < 5; g++) {
            u32 m; memcpy(&m, h + hp + 4 * g, 4);
            masks[g] = m | (g == 0 ? 0x80000000u : 0);
            m &= want[g]; memcpy(tmp + op + 4 * g, &m, 4);   /* report only what was asked */
        }
        hp += 20; op += 20;
    }
    /* first pass: the 64-bit IDs and the name */
    uint64_t fid = 0, pid = 0; int has_fid = 0, has_pid = 0; char name[1100]; name[0] = 0;
    {   u32 p = hp;
        for (int g = 0; g < 5; g++) for (const attrdef_t *d = groups[g]; d->bit; d++) {
            if (!(masks[g] & d->bit) || (g == 0 && d->bit == 0x80000000u)) continue;
            if (p + d->size > hend) break;
            if (g == 0 && d->bit == CMN_FILEID) { memcpy(&fid, h + p, 8); has_fid = 1; }
            if (g == 0 && d->bit == CMN_PARENTID) { memcpy(&pid, h + p, 8); has_pid = 1; }
            if (g == 0 && d->bit == CMN_NAME) {
                int32_t off; u32 len; memcpy(&off, h + p, 4); memcpy(&len, h + p + 4, 4);
                const unsigned char *dp = h + p + off;
                if (dp >= h && dp + len <= h + hlen && len < sizeof name) { memcpy(name, dp, len); name[len] = 0; }
            }
            p += d->size;
        }
    }
    u32 afid = 0, apid = 0;
    if (has_fid) {
        char pb[2200]; const char *path = ap ? ap->obj : 0;
        if (!path && ap && ap->dir && name[0]) { snprintf(pb, sizeof pb, "%s/%s", ap->dir, name); path = pb; }
        afid = alias_of(fid, path);
    }
    if (has_pid) {
        char pb[2200]; const char *path = ap ? ap->dir : 0;
        if (!path && ap && ap->obj) {               /* the object's directory */
            snprintf(pb, sizeof pb, "%s", ap->obj);
            char *sl = strrchr(pb, '/'); if (sl && sl != pb) { *sl = 0; path = pb; } else if (sl) { pb[1] = 0; path = pb; }
        }
        apid = alias_of(pid, path);
    }
    for (int g = 0; g < 5; g++) for (const attrdef_t *d = groups[g]; d->bit; d++) {
        if (!(masks[g] & d->bit)) continue;
        if (g == 0 && d->bit == 0x80000000u) continue;
        if (hp + d->size > hend) break;
        if (!(want[g] & d->bit)) { hp += d->size; continue; }      /* asked for us, not by the caller */
        if (d->kind == AK_TS) {                     /* tv_sec (8) tv_nsec (8) -> 4 + 4 */
            int64_t sec, nsec; memcpy(&sec, h + hp, 8); memcpy(&nsec, h + hp + 8, 8);
            int32_t s32 = (int32_t)sec, n32 = (int32_t)nsec;
            memcpy(tmp + op, &s32, 4); memcpy(tmp + op + 4, &n32, 4); op += 8;
        } else if (d->kind == AK_REF) {
            int32_t off; u32 len; memcpy(&off, h + hp, 4); memcpy(&len, h + hp + 4, 4);
            if (nref < 32) { refs[nref].opos = op; refs[nref].data = h + hp + off; refs[nref].len = len;
                             refs[nref].mnt = g == 1 && d->bit == 0x1000; nref++; }
            memcpy(tmp + op + 4, &len, 4); op += 8;
        } else {
            memcpy(tmp + op, h + hp, d->size);
            if (!vg_on() && g == 1 && d->bit == 0x4000) { uint32_t f; memcpy(&f, tmp + op, 4); root_rw_fix(&f); memcpy(tmp + op, &f, 4); }
            if (vg_on()) {                            /* one volume for "/" (vg_fix) */
                if (g == 0 && d->bit == 0x4 && !memcmp(tmp + op, &g_sysfsid, 8)) memcpy(tmp + op, &g_datafsid, 8);
                if (g == 1 && d->bit == 0x4000) { u32 f; memcpy(&f, tmp + op, 4); f &= ~(u32)(MNT_DONTBROWSE | MNT_RDONLY); memcpy(tmp + op, &f, 4); }
            }
            if (g == 0 && d->bit == CMN_OBJID && has_fid && fid >= 0x100000000ull) memcpy(tmp + op, &afid, 4);
            if (g == 0 && d->bit == CMN_PAROBJID && has_pid && pid >= 0x100000000ull) memcpy(tmp + op, &apid, 4);
            op += d->size;
        }
        hp += d->size;
    }
    for (int k = 0; k < nref; k++) {               /* variable data after the fixed part, 4-byte padded */
        const unsigned char *dp = refs[k].data;
        u32 len = refs[k].len;
        if (dp < h || dp + len > h + hlen) len = 0;
        if (refs[k].mnt && vg_on() && len >= 21 && !memcmp(dp, "/System/Volumes/Data", 21)) { dp = (const unsigned char *)"/"; len = 2; }
        int32_t off = (int32_t)(op - refs[k].opos);
        memcpy(tmp + refs[k].opos + 4, &len, 4);
        memcpy(tmp + refs[k].opos, &off, 4);
        if (len) memcpy(tmp + op, dp, len);
        op += (len + 3) & ~3u;
    }
    memcpy(tmp, &op, 4);
    u32 n = op < olen ? op : olen;
    memcpy(o, tmp, n);
    free(tmp);
    return op;
}

/* Catalina keeps the user's files on a separate "Data" volume mounted nobrowse under
   /System/Volumes/Data. Snow Leopard's File Manager does not list nobrowse volumes, so files in the
   home folder (and the temporary folder) would belong to no volume (nsvErr): show it browsable. */
static void data_browse64(unsigned char *sf) {      /* struct statfs (64-bit inode layout) */
    if (!strcmp((const char *)sf + 88, "/System/Volumes/Data")) { u32 fl; memcpy(&fl, sf + 64, 4); fl &= ~MNT_DONTBROWSE; memcpy(sf + 64, &fl, 4); }
}

/* System calls with 64-bit arguments, which take two words on the i386 stack. Each signature
   letter is an argument: w = word (zero-extended), i = signed word, q = 64-bit (two words). */
static const struct { u32 num; const char *sig; } g_wide[] = {
    { 301, "wwwqw" },        /* psynch_mutexwait(mutex, mgen, ugen, tid, flags) */
    { 302, "wwwqw" },        /* psynch_mutexdrop */
    { 303, "wqqwwqq" },      /* psynch_cvbroad(cv, cvlsgen, cvudgen, flags, mutex, mugen, tid) */
    { 304, "wqwiwqqw" },     /* psynch_cvsignal(cv, cvlsgen, cvugen, thread_port, mutex, mugen, tid, flags) */
    { 305, "wqwwqwqw" },     /* psynch_cvwait(cv, cvlsgen, cvugen, mutex, mugen, flags, sec, nsec) */
    { 0, 0 } };
/* a BSD system call with up to 8 arguments: the kernel reads the 7th and 8th from the user stack,
   above a return address. Returns -1 with errno on error. */
long sys8(long num, long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
    long r; unsigned long cf;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    __asm__ volatile("pushq %[a7]\n\tpushq %[a6]\n\tpushq $0\n\tsyscall\n\tsetc %%cl\n\tmovzbq %%cl, %%rcx\n\taddq $24, %%rsp"
                     : "=a"(r), "=c"(cf), "+r"(r10), "+r"(r8), "+r"(r9)
                     : "a"(0x2000000 | num), "D"(a0), "S"(a1), "d"(a2), [a6] "r"(a6), [a7] "r"(a7)
                     : "r11", "memory");
    if (cf) { errno = (int)r; return -1; }
    return r;
}
static int wide_call(ss64_t *s, u32 num, u32 argp) {
    for (int i = 0; g_wide[i].num; i++) if (g_wide[i].num == num) {
        long a[8] = { 0 }; u32 p = argp; int k = 0;
        for (const char *c = g_wide[i].sig; *c && k < 8; c++, k++) {
            if (*c == 'q') { a[k] = (long)rd64(p); p += 8; }
            else if (*c == 'i') { a[k] = (long)(int32_t)rd32(p); p += 4; }
            else { a[k] = (long)rd32(p); p += 4; }
        }
        long r = sys8(num, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        ret_host(s, r);
        return 1;
    }
    return 0;
}

/* returns 1 if the call was carried out here */
int sys_conv(ss64_t *s, u32 num, u32 argp) {
    long r;
    if (wide_call(s, num, argp)) return 1;
    switch (num) {
    case 231: case 232: case 233: {
        /* watchevent(req, mask) / waitevent(req, tv) / modwatch(req, mask): the old socket event
           API, still in this kernel and used by Open Transport (BSD_waitevent). struct eventreq holds
           a pointer (er_data, the caller's own value, handed back by waitevent) and waitevent takes
           a timeval: both in the 64-bit layout here. Passed as they were, waitevent failed with
           EINVAL and Open Transport never saw a connect complete (Internet Explorer stuck at
           "Connecting to ..."). i386: type, handle, data, rcnt, wcnt, ecnt, eventbits (7 words). */
        struct { int type, handle; void *data; int rcnt, wcnt, ecnt, eventbits; } er;
        u32 p = A(0);
        memset(&er, 0, sizeof er);
        if (p && num != 232) {
            er.type = (int)rd32(p); er.handle = (int)rd32(p + 4); er.data = (void *)(uintptr_t)rd32(p + 8);
            er.rcnt = (int)rd32(p + 12); er.wcnt = (int)rd32(p + 16); er.ecnt = (int)rd32(p + 20); er.eventbits = (int)rd32(p + 24);
        }
        if (num == 232) {
            struct timeval tv, *tp = 0;
            u32 t = A(1);
            if (t == 0xffffffffu) tp = (struct timeval *)-1L;          /* poll */
            else if (t) { tv.tv_sec = (int32_t)rd32(t); tv.tv_usec = (int32_t)rd32(t + 4); tp = &tv; }
            r = syscall(232, p ? &er : 0, tp);
            if (r == 0 && p) {
                wr32(p, (u32)er.type); wr32(p + 4, (u32)er.handle); wr32(p + 8, (u32)(uintptr_t)er.data);
                wr32(p + 12, (u32)er.rcnt); wr32(p + 16, (u32)er.wcnt); wr32(p + 20, (u32)er.ecnt); wr32(p + 24, (u32)er.eventbits);
            }
        } else r = syscall((int)num, p ? &er : 0, (int)A(1));
        ret_host(s, r);
        return 1;
    }
    case 92: {                                    /* fcntl: commands whose structures hold pointers or size_t */
        int fd = (int)A(0), cmd = (int)A(1); u32 p = A(2);
        if (cmd == 59 || cmd == 61 || cmd == 83 || cmd == 97) {   /* F_ADDSIGS, F_ADDFILESIGS(_FOR_DYLD_SIM/_RETURN) */
            struct { off_t start; void *blob; size_t size; } fs;    /* i386: off_t, ptr, size_t = 16 bytes */
            fs.start = (off_t)rd64(p); fs.blob = P32(rd32(p + 8)); fs.size = rd32(p + 12);
            r = fcntl(fd, cmd, &fs);
            if (r != -1 && cmd == 97) { wr32(p, (u32)fs.start); wr32(p + 4, (u32)((uint64_t)fs.start >> 32)); }
            ret_host(s, r);
            return 1;
        }
        if (cmd == 98) {                          /* F_CHECK_LV: off_t, size_t, char* */
            struct { off_t start; size_t msgsize; void *msg; } lv;
            lv.start = (off_t)rd64(p); lv.msgsize = rd32(p + 8); lv.msg = P32(rd32(p + 12));
            r = fcntl(fd, cmd, &lv);
            ret_host(s, r);
            return 1;
        }
        if (cmd == 49 || cmd == 65) {             /* F_LOG2PHYS(_EXT): u32 flags, off_t, off_t (packed by 4 on i386) */
            struct log2phys l; l.l2p_flags = rd32(p); l.l2p_contigbytes = (off_t)rd64(p + 4); l.l2p_devoffset = (off_t)rd64(p + 12);
            r = fcntl(fd, cmd, &l);
            if (r != -1) { wr32(p, l.l2p_flags); wr32(p + 4, (u32)l.l2p_contigbytes); wr32(p + 8, (u32)((uint64_t)l.l2p_contigbytes >> 32));
                           wr32(p + 12, (u32)l.l2p_devoffset); wr32(p + 16, (u32)((uint64_t)l.l2p_devoffset >> 32)); }
            ret_host(s, r);
            return 1;
        }
        return 0;
    }
    case 345: case 346: {                         /* statfs64 / fstatfs64: same layout */
        char pb[1100];
        r = num == 345 ? syscall(345, (long)map_path((const char *)P32(A(0)), pb, sizeof pb), (long)A(1)) : syscall(346, (long)A(0), (long)A(1));
        if (!r) { struct statfs *h = P32(A(1)); vg_fix(h, 0); data_browse64((unsigned char *)h); }
        ret_host(s, r);
        return 1;
    }
    case 347: {                                   /* getfsstat64(buf, size, flags) */
        r = syscall(347, (long)A(0), (long)(int)A(1), (long)(int)A(2));
        if (r > 0 && A(0)) {
            struct statfs *v = P32(A(0)); long k2 = 0;
            for (long k = 0; k < r; k++) {
                if (!vg_fix(&v[k], 1)) continue;
                data_browse64((unsigned char *)&v[k]);
                if (k2 != k) v[k2] = v[k];
                k2++;
            }
            r = k2;
        } else if (r > 0 && vg_on()) r--;           /* a count only: the data volume is left out */
        ret_host(s, r);
        return 1;
    }
    case 220: case 228: {                         /* getattrlist(path, alist, buf, size, opts) / fgetattrlist(fd, ...) */
        const struct attrlist32 *al = P32(A(1));
        if (g_trace > 1 && num == 220) rlog("[sys]   getattrlist %s", (const char *)P32(A(0)));
        int has_ts = (al->commonattr & (0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x10000000)) != 0;
        char pb[1100], fpath[1100]; const char *path = 0;
        if (num == 220) path = map_path((const char *)P32(A(0)), pb, sizeof pb);
        /* /.vol/<dev>/<alias> names the object itself (volfs never follows it); its stand-in path
           must not be followed either: /tmp (a symlink on the system volume) came back as the
           folder /private/tmp, so the 10.6 File Manager saw a "folder" whose FSRef was a file and
           the Open panels showed /tmp, /etc and /var greyed out ("Go to the folder" /tmp/x did
           nothing: xACT's encode folder). R2_VOL_FOLLOW=1 restores the old behaviour. */
        u32 opts = A(4);
        if (num == 220 && path == pb && vol_alias_leaf((const char *)P32(A(0))) && !getenv("R2_VOL_FOLLOW")) opts |= 1;   /* FSOPT_NOFOLLOW */
        if (!has_ts && !(al->commonattr & CMN_IDS)) {
            if (num == 220) r = syscall(220, (long)path, (long)A(1), (long)A(2), (long)A(3), (long)opts);
            else r = syscall(228, (long)A(0), (long)A(1), (long)A(2), (long)A(3), (long)opts);
            ret_host(s, r);
            return 1;
        }
        struct attrlist32 hal = attr_hostlist(al, s);
        u32 hsz = A(3) * 2 + 256;
        unsigned char *hb = calloc(1, hsz);
        if (num == 220) r = syscall(220, (long)path, (long)&hal, (long)hb, (long)hsz, (long)opts);
        else { r = syscall(228, (long)A(0), (long)&hal, (long)hb, (long)hsz, (long)opts);
               if (fcntl((int)A(0), F_GETPATH, fpath) == 0) path = fpath; }
        int e = errno;
        apath_t ap = { path, 0 };
        if (r == 0) attr_repack(al, &hal, &ap, hb, hsz, P32(A(2)), A(3));
        free(hb); errno = e;
        ret_host(s, r);
        return 1;
    }
    case 196: {
        /* getdirentries(fd, buf, nbytes, long *basep): the entries have the same layout for both
           worlds, but the kernel stores an 8-byte long at basep for a 64-bit caller. (196 was taken
           for getdirentriesattr here: Illustrator CS2's directory reads went to the wrong call and
           the out pointers it was given pointed at code - rhost died.) */
        long base = 0;
        r = syscall(196, (long)(int)A(0), (long)A(1), (long)A(2), (long)&base);
        int e = errno;
        if (r >= 0 && A(3) && mach_vm_write(mach_task_self(), A(3), (vm_offset_t)&base, 4) != KERN_SUCCESS && g_trace)
            rlog("[sys] getdirentries: basep %#x not writable", A(3));
        errno = e;
        ret_host(s, r);
        return 1;
    }
    case 222: case 461: {
        /* getdirentriesattr(fd, alist, buf, size, count *, basep *, newstate *, options) /
           getattrlistbulk(dirfd, alist, buf, size, options): a buffer of entries, each starting with
           its length; each is repacked like a getattrlist buffer. An i386 entry is never longer than
           the 64-bit one, so a host buffer of the same size gives entries that all fit. */
        const struct attrlist32 *al = P32(A(1));
        struct attrlist32 hal = attr_hostlist(al, s);
        u32 sz = A(3), hsz = sz;          /* the IDs asked for us take room: fewer entries, never more than fit */
        unsigned char *hb = calloc(1, hsz + 64);
        uint64_t cnt = 0, base = 0, nst = 0;
        char dpath[1100]; apath_t ap = { 0, 0 };
        if (fcntl((int)A(0), F_GETPATH, dpath) == 0) ap.dir = dpath;
        if (num == 222) {
            cnt = A(4) ? rd32(A(4)) : 0;
            r = syscall(222, (long)A(0), (long)&hal, (long)hb, (long)hsz, (long)&cnt, (long)&base, (long)&nst, (long)A(7));
        } else r = syscall(461, (long)A(0), (long)&hal, (long)hb, (long)hsz, (long)A(4));
        int e = errno;
        if (r >= 0) {
            u32 n = num == 222 ? (u32)cnt : (u32)r, hp = 0, op = 0;
            unsigned char *o = P32(A(2));
            for (u32 k = 0; k < n && hp + 4 <= hsz; k++) {
                u32 len; memcpy(&len, hb + hp, 4);
                if (len < 4 || hp + len > hsz) break;
                u32 ol = attr_repack(al, &hal, &ap, hb + hp, len, o + op, sz - op);
                op += ol; hp += len;
            }
            if (num == 222) {
                /* an out pointer that cannot be written is skipped, not a fault in rhost: Illustrator
                   CS2 and Adobe Help Center get a basep inside the PowerPC executable's header page
                   (0x1000, read-only here; Mojave's translator had it writable) - rhost died with
                   SIGBUS. The call still succeeds, as it did there. */
                u32 outs[3] = { A(4), A(5), A(6) }, vals[3] = { (u32)cnt, (u32)base, (u32)nst };
                for (int k = 0; k < 3; k++) {
                    if (!outs[k]) continue;
                    if (mach_vm_write(mach_task_self(), outs[k], (vm_offset_t)&vals[k], 4) != KERN_SUCCESS && g_trace)
                        rlog("[sys] getdirentriesattr: out pointer %d at %#x not writable: skipped", k, outs[k]);
                }
            }
        }
        free(hb); errno = e;
        ret_host(s, r);
        return 1;
    }
    case 476: {                                   /* getattrlistat(fd, path, alist, buf, size, opts) */
        const struct attrlist32 *al = P32(A(2));
        char pb[1100]; const char *path = (const char *)P32(A(1));
        if (g_trace > 1) rlog("[sys]   getattrlistat %d %s", (int)A(0), path);
        if (path[0] == '/') path = map_path(path, pb, sizeof pb);
        struct attrlist32 hal = attr_hostlist(al, s);
        u32 hsz = A(4) * 2 + 256;
        unsigned char *hb = calloc(1, hsz);
        r = syscall(476, (long)(int)A(0), (long)path, (long)&hal, (long)hb, (long)hsz, (long)A(5));
        int e = errno;
        apath_t ap = { path[0] == '/' ? path : 0, 0 };
        if (r == 0) attr_repack(al, &hal, &ap, hb, hsz, P32(A(3)), A(4));
        free(hb); errno = e;
        ret_host(s, r);
        return 1;
    }
    case 221: case 229: {                         /* setattrlist / fsetattrlist: times 8 -> 16 bytes */
        const struct attrlist32 *al = P32(A(1));
        u32 tsbits = al->commonattr & (0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x10000000);
        unsigned char *in = P32(A(2)), *hb = 0; u32 hlen = A(3);
        if (tsbits) {
            hb = calloc(1, hlen * 2 + 64);
            u32 ip = 0, hp = 0;
            for (const attrdef_t *d = g_cmn; d->bit && ip < A(3); d++) {
                if (!(al->commonattr & d->bit)) continue;
                if (d->kind == AK_TS) { int64_t sec = (int32_t)rd32(A(2) + ip), nsec = (int32_t)rd32(A(2) + ip + 4); memcpy(hb + hp, &sec, 8); memcpy(hb + hp + 8, &nsec, 8); ip += 8; hp += 16; }
                else { memcpy(hb + hp, in + ip, d->size); ip += d->size; hp += d->size; }
            }
            if (ip < A(3)) { memcpy(hb + hp, in + ip, A(3) - ip); hp += A(3) - ip; }
            in = hb; hlen = hp;
        }
        if (num == 221) { char pb[1100]; r = syscall(221, (long)map_path((const char *)P32(A(0)), pb, sizeof pb), (long)A(1), (long)in, (long)hlen, (long)A(4)); }
        else r = syscall(229, (long)A(0), (long)A(1), (long)in, (long)hlen, (long)A(4));
        int e = errno; free(hb); errno = e;
        ret_host(s, r);
        return 1;
    }
    case 157: case 158: {                         /* statfs(path, buf) / fstatfs(fd, buf) */
        struct statfs h; char pb[1100];
        r = num == 157 ? statfs(map_path((const char *)P32(A(0)), pb, sizeof pb), &h) : fstatfs((int)A(0), &h);
        if (!r) statfs_out(P32(A(1)), &h);
        ret_host(s, r);
        return 1;
    }
    case 18: {                                    /* getfsstat(buf, bufsize, flags) */
        int n = getfsstat(0, 0, MNT_NOWAIT);
        if (n < 0) { ret_err(s, errno); return 1; }
        if (!A(0)) { ret_ok(s, (u32)(n > 0 && vg_on() ? n - 1 : n)); return 1; }
        struct statfs *h = calloc((size_t)n + 1, sizeof *h);
        n = getfsstat(h, (int)((n + 1) * sizeof *h), (int)A(2));
        int want = (int)(A(1) / sizeof(struct statfs32)), k, j = 0;
        for (k = 0; k < n && j < want; k++) {
            struct statfs c = h[k];
            if (!vg_fix(&c, 1)) continue;             /* the data volume: part of "/" here */
            statfs_out((struct statfs32 *)P32(A(0)) + j++, &c);
        }
        k = j;
        free(h);
        if (n < 0) ret_err(s, errno); else ret_ok(s, (u32)k);
        return 1;
    }
    case 363: {                                   /* kevent(kq, changes, nchanges, events, nevents, timeout) */
        int nch = (int)A(2), nev = (int)A(4);
        if (nch < 0 || nev < 0 || nch > 65536 || nev > 65536) { ret_err(s, EINVAL); return 1; }
        struct kevent *ch = calloc((size_t)nch + 1, sizeof *ch), *ev = calloc((size_t)nev + 1, sizeof *ev);
        const struct kev32 *c32 = P32(A(1));
        for (int i = 0; i < nch; i++)
            EV_SET(&ch[i], c32[i].ident, c32[i].filter, c32[i].flags, c32[i].fflags, (intptr_t)c32[i].data, (void *)(uintptr_t)c32[i].udata);
        struct timespec ts, *tp = 0;
        if (A(5)) { ts.tv_sec = (int32_t)rd32(A(5)); ts.tv_nsec = (int32_t)rd32(A(5) + 4); tp = &ts; }
        r = kevent((int)A(0), ch, nch, ev, nev, tp);
        if (r > 0) {
            struct kev32 *e32 = P32(A(3));
            for (int i = 0; i < r; i++) {
                e32[i].ident = (u32)ev[i].ident; e32[i].filter = ev[i].filter; e32[i].flags = ev[i].flags;
                e32[i].fflags = ev[i].fflags; e32[i].data = (int32_t)ev[i].data; e32[i].udata = (u32)(uintptr_t)ev[i].udata;
            }
        }
        int e = errno; free(ch); free(ev); errno = e;
        ret_host(s, r);
        return 1;
    }
    case 374: case 375: {   /* kevent_qos(kq, ...) / kevent_id(uint64 id, ...): kevent_qos_s is fixed-size;
                               data_available is a size_t* (4 bytes here) */
        if (num == 374 && wq_kevent_qos(s, argp)) return 1;     /* the workqueue kqueue (wq.c) */
        if (num == 375 && wq_kevent_id(s, argp)) return 1;      /* workloops (wq.c) */
        int k = num == 375 ? 1 : 0;
        uint64_t first = num == 375 ? rd64(argp) : (uint64_t)A(0);
        u32 avp = A(6 + k);
        size_t avail = avp ? rd32(avp) : 0;
        r = syscall((int)num, (long)first, (long)A(1 + k), (long)(int)A(2 + k), (long)A(3 + k), (long)(int)A(4 + k),
                    (long)A(5 + k), avp ? (long)&avail : 0L, (long)A(7 + k));
        if (avp) wr32(avp, (u32)avail);
        ret_host(s, r);
        return 1;
    }
    case 515:                                     /* __ulock_wait(op, addr, uint64 value, timeout) */
        r = syscall(515, (long)A(0), (long)A(1), (long)rd64(argp + 8), (long)A(4));
        ret_host(s, r);
        return 1;
    case 516:                                     /* __ulock_wake(op, addr, uint64 wake_value) */
        r = syscall(516, (long)A(0), (long)A(1), (long)rd64(argp + 8));
        ret_host(s, r);
        return 1;
    case 93: {                                    /* select(nfds, r, w, e, timeval) */
        struct timeval tv;
        int has = tv_in(A(4), &tv);
        r = select((int)A(0), P32(A(1)), P32(A(2)), P32(A(3)), has ? &tv : 0);
        if (has) tv_out(A(4), &tv);
        ret_host(s, r);
        return 1;
    }
    case 117: {                                   /* getrusage(who, rusage) */
        struct rusage ru;
        r = getrusage((int)A(0), &ru);
        if (!r) ru_out(A(1), &ru);
        ret_host(s, r);
        return 1;
    }
    case 7: {                                     /* wait4(pid, status, options, rusage) */
        struct rusage ru; int st = 0;
        r = wait4((pid_t)A(0), &st, (int)A(2), A(3) ? &ru : 0);
        if (r > 0) { if (A(1)) wr32(A(1), (u32)st); if (A(3)) ru_out(A(3), &ru); }
        ret_host(s, r);
        return 1;
    }
    case 83: {                                    /* setitimer(which, new, old) */
        struct itimerval n, o;
        if (A(1)) { tv_in(A(1), &n.it_interval); tv_in(A(1) + 8, &n.it_value); }
        r = setitimer((int)A(0), A(1) ? &n : 0, &o);
        if (!r && A(2)) { tv_out(A(2), &o.it_interval); tv_out(A(2) + 8, &o.it_value); }
        ret_host(s, r);
        return 1;
    }
    case 86: {                                    /* getitimer(which, value) */
        struct itimerval o;
        r = getitimer((int)A(0), &o);
        if (!r && A(1)) { tv_out(A(1), &o.it_interval); tv_out(A(1) + 8, &o.it_value); }
        ret_host(s, r);
        return 1;
    }
    case 138: case 139: {                         /* utimes(path, tv[2]) / futimes(fd, tv[2]) */
        struct timeval tv[2]; char pb[1100];
        int has = A(1) != 0;
        if (has) { tv_in(A(1), &tv[0]); tv_in(A(1) + 8, &tv[1]); }
        r = num == 138 ? utimes(map_path((const char *)P32(A(0)), pb, sizeof pb), has ? tv : 0) : futimes((int)A(0), has ? tv : 0);
        ret_host(s, r);
        return 1;
    }
    case 334: {   /* __semwait_signal(cond, mutex, timeout, relative, int64 sec (two words), int32 nsec) */
        int64_t sec = (int64_t)((uint64_t)A(4) | ((uint64_t)A(5) << 32));
        r = syscall(334, (long)A(0), (long)A(1), (long)(int)A(2), (long)(int)A(3), (long)sec, (long)(int32_t)A(6));
        ret_host(s, r);
        return 1;
    }
    case 27: case 28: {                           /* recvmsg / sendmsg(fd, msghdr, flags) */
        struct msghdr32 *m32 = P32(A(1));
        struct msghdr m; memset(&m, 0, sizeof m);
        m.msg_name = m32->name ? P32(m32->name) : 0; m.msg_namelen = m32->namelen;
        m.msg_iov = iov_conv(m32->iov, m32->iovlen); m.msg_iovlen = m32->iovlen;
        m.msg_control = m32->control ? P32(m32->control) : 0; m.msg_controllen = m32->controllen;
        m.msg_flags = m32->flags;
        if (!m.msg_iov) { ret_err(s, EINVAL); return 1; }
        r = num == 27 ? recvmsg((int)A(0), &m, (int)A(2)) : sendmsg((int)A(0), &m, (int)A(2));
        int e = errno;
        if (num == 27 && r >= 0) { m32->namelen = m.msg_namelen; m32->controllen = m.msg_controllen; m32->flags = m.msg_flags; }
        free(m.msg_iov); errno = e;
        ret_host(s, r);
        return 1;
    }
    }
    return 0;
}
