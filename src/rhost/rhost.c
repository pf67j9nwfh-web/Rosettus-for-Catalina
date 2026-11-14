/* rhost.c - start a 32-bit Intel program inside this 64-bit process.
   usage: rhost [-t|-tt] [-r ROOT] [-c COMMPAGE] PROGRAM [ARGS...]
     ROOT      Snow Leopard tree with i386 code (default $R2_ROOT, else ~/r2/Rosettus)
     COMMPAGE  dump of a Snow Leopard 32-bit commpage (default ROOT/../commpage32.bin) */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

int g_trace;
char g_root32[1024];
extern char **environ;

int g_rlog_fd = -2;   /* the R2_LOGDIR log; the guest's close() leaves it open (sys.c) */
void rlog(const char *fmt, ...) {
    char b[2048]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    /* R2_LOGDIR: each process logs to <dir>/rhost-<pid>.log (daemons the guest starts have no stderr of use) */
    static pid_t fpid; int fd = g_rlog_fd;
    if (fd == -2 || fpid != getpid()) {
        const char *d = getenv("R2_LOGDIR"); fd = -1; fpid = getpid();
        if (d) { char p[1024]; snprintf(p, sizeof p, "%s/rhost-%d.log", d, (int)fpid);
                 fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644); }
        g_rlog_fd = fd;
    }
    write(fd >= 0 ? fd : 2, b, n);
}
void rfatal(const char *fmt, ...) {
    char b[2048]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    rlog("rhost: %s", b);
    _exit(70);
}

/* the initial 32-bit stack, as the kernel builds it for dyld:
   mh, argc, argv..., 0, envp..., 0, apple[0] (executable path), 0, then the strings */
static u32 build_stack(u32 mh, int argc, char **argv, const char *exepath) {
    u32 size = 8u << 20, top = 0xc0000000u, base = top - size;
    if (!low_is_free(base, size)) { base = low_alloc(size, 0xffff, 1); top = base + size; }
    else low_mark(base, size, 1);
    if (!base || mmap(P32(base), size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED)
        rfatal("no 32-bit stack");
    /* the host's own DYLD_* and R2_* settings stay out; R2_DYLD_X=... becomes DYLD_X=... for the 32-bit world */
#define ENV32(e) (!strncmp((e), "R2_DYLD_", 8) ? (e) + 3 : (strncmp((e), "DYLD_", 5) && strncmp((e), "R2_", 3)) ? (e) : 0)
    int envc = 0;
    for (char **e = environ; *e; e++) if (ENV32(*e)) envc++;
    envc++;                                     /* R2_HOST=1: the 32-bit world can tell it runs here */
    u32 *ptrs = calloc(argc + envc + 8, sizeof(u32));
    u32 sp = top;
#define PUTS(str) ({ size_t l_ = strlen(str) + 1; sp -= (u32)l_; memcpy(P32(sp), (str), l_); sp; })
    u32 exe_s = PUTS(exepath);
    int k = 0;
    u32 *av = calloc(argc + 1, 4), *ev = calloc(envc + 1, 4);
    for (int i = 0; i < argc; i++) av[i] = PUTS(argv[i]);
    int j = 0;
    for (char **e = environ; *e; e++) if (ENV32(*e)) { const char *v = ENV32(*e); ev[j++] = PUTS(v); }
    ev[j++] = PUTS("R2_HOST=1");
    sp &= ~15u;
    ptrs[k++] = mh; ptrs[k++] = (u32)argc;
    for (int i = 0; i < argc; i++) ptrs[k++] = av[i];
    ptrs[k++] = 0;
    for (int i = 0; i < envc; i++) ptrs[k++] = ev[i];
    ptrs[k++] = 0;
    ptrs[k++] = exe_s; ptrs[k++] = 0;
    sp -= (u32)(k * 4);
    sp &= ~15u;
    memcpy(P32(sp), ptrs, k * 4);
    free(ptrs); free(av); free(ev);
    return sp;
#undef PUTS
}

int main(int argc, char **argv) {
    {   /* R2_GMALLOC=1: run under Guard Malloc (host heap overruns stop where they happen). The
           launcher is a shell script and the system's bash drops DYLD_ variables, so set it here. */
        const char *gm = getenv("R2_GMALLOC"), *di = getenv("DYLD_INSERT_LIBRARIES");
        if (gm && *gm && !(di && strstr(di, "libgmalloc"))) {
            extern int _NSGetExecutablePath(char *, uint32_t *);
            char self[1024]; uint32_t sz = sizeof self;
            setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/libgmalloc.dylib", 1);
            if (!_NSGetExecutablePath(self, &sz)) execv(self, argv);
        }
        if (gm && *gm) unsetenv("DYLD_INSERT_LIBRARIES");     /* not for children */
    }
    const char *root = getenv("R2_ROOT"), *cp = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-t")) g_trace = 1;
        else if (!strcmp(argv[i], "-tt")) g_trace = 2;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cp = argv[++i];
        else break;
    }
    if (getenv("R2_TRACE")) g_trace = atoi(getenv("R2_TRACE"));
    if (i >= argc) { fprintf(stderr, "usage: rhost [-t] [-r ROOT] [-c COMMPAGE] PROGRAM [ARGS...]\n"); return 64; }
    if (!root) { static char d[1024]; snprintf(d, sizeof d, "%s/r2/Rosettus", getenv("HOME")); root = d; }
    snprintf(g_root32, sizeof g_root32, "%s", root);
    { extern void snap_init(void); snap_init(); } { extern void prof_init(void); prof_init(); }       /* R2_SNAP_DIR (snap.c): window snapshots for tests */
    spawn_init(i - 1, argv + 1);                     /* the options, for i386 programs started from here */
    char cpbuf[1100];
    if (!cp) { snprintf(cpbuf, sizeof cpbuf, "%s/../commpage32.bin", root); cp = cpbuf; }

    if (getenv("R2_MALLOCTEST")) { void *m = malloc(64); rlog("malloc before low_init: %p", m); }
    low_init();
    mapwatch_start();
    if (getenv("R2_MALLOCTEST")) { void *m = malloc(64); rlog("malloc after low_init: %p", m); }
    commpage_init(cp);
    trap_install();

    char dyldpath[1100]; snprintf(dyldpath, sizeof dyldpath, "%s/usr/lib/dyld", g_root32);
    char exe[1024];
    if (!realpath(argv[i], exe)) rfatal("cannot find %s", argv[i]);
    image32_t img, dy;
    if (load_macho32(exe, &img, 0)) rfatal("cannot load %s", exe);
    if (load_macho32(dyldpath, &dy, 1)) rfatal("cannot load %s", dyldpath);
    {   /* a slidable dyld: a 10.7+ i386 userland (Mojave's), which reads more of the commpage */
        extern int g_cp_modern; void commpage_modern(void);
        g_cp_modern = getenv("R2_MODERN") ? atoi(getenv("R2_MODERN")) : dy.slide != 0;
        if (g_cp_modern) commpage_modern();
        if (g_trace) rlog("[rhost] i386 userland: %s", g_cp_modern ? "10.7 or later" : "Snow Leopard");
    }
    const char *base = strrchr(exe, '/'); base = base ? base + 1 : exe;
    if (!strncmp(base, "translate", 9)) {          /* the translator's code: system calls made for the PowerPC guest */
        extern u32 g_xlat_lo, g_xlat_hi;
        u32 nc = rd32(img.mh + 16), lc = img.mh + 28;
        for (u32 k = 0; k < nc && k < 256; k++) {
            if (rd32(lc) == 1 /* LC_SEGMENT */ && !strncmp((const char *)P32(lc + 8), "__TEXT", 16)) {
                g_xlat_lo = rd32(lc + 24) + img.slide; g_xlat_hi = g_xlat_lo + rd32(lc + 28);
            }
            lc += rd32(lc + 4);
        }
        if (g_trace) rlog("[rhost] translator text %#x-%#x", g_xlat_lo, g_xlat_hi);
        {   /* The guest's vfork: the translator takes its locks, calls vfork, and only the child (which
               shares the parent's memory under a real vfork) releases them; the parent then waits on
               them. A vfork here is a fork (the 32-bit world cannot share memory with a child), so the
               parent spun for good (Word 2004 starting its font cache tool). Its vfork routine is made
               to jump to its fork routine, which releases the locks on both sides. Only for the
               translator this release was built with (the bytes are checked); R2_NO_VFORKPATCH=1: off. */
            static const unsigned char vf[] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x4c, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x5b };
            static const unsigned char fk[] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x3c, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x5b };
            u32 va = 0xb8169f2a + img.slide, fa = 0xb816773c + img.slide;
            if (!getenv("R2_NO_VFORKPATCH") && va + 16 < g_xlat_hi && fa + 16 < g_xlat_hi &&
                !memcmp(P32(va), vf, sizeof vf) && !memcmp(P32(fa), fk, sizeof fk) &&
                *(const u32 *)P32(va + 0x42 + 1) == 0xb819bf22 - (0xb8169f6c + 5)) {     /* calll _vfork at +0x42 */
                u32 pg = va & ~(R32_PAGE - 1);
                mprotect(P32(pg), 2 * R32_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC);
                unsigned char *c = P32(va);
                c[0] = 0xe9; u32 rel = fa - (va + 5); memcpy(c + 1, &rel, 4);
                mprotect(P32(pg), 2 * R32_PAGE, PROT_READ | PROT_EXEC);
                if (g_trace) rlog("[rhost] translator: vfork goes the fork way");
            } else if (g_trace) rlog("[rhost] translator: vfork routine not recognised, left alone");
        }
    }
    /* the translator without liboah needs the PowerPC commpage pages mapped; liboah maps them itself
       (and sets up its stubs only when it could) */
    if (!strncmp(base, "translate", 9) && !getenv("R2_DYLD_INSERT_LIBRARIES")) rosetta_commpage_init();
    const char *apple0 = getenv("R2_APPLE0") ? getenv("R2_APPLE0") : exe;
    u32 sp = build_stack(img.mh, argc - i, argv + i, apple0);
    if (g_trace) rlog("[rhost] exe mh %#x, dyld entry %#x, stack %#x", img.mh, dy.entry, sp);
    enter32(dy.entry, sp, 0, 0, 0, 0, 0, 0);
}
