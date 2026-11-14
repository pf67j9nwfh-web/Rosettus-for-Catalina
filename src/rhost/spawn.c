/* spawn.c - new processes from the 32-bit world: fork / vfork, execve and posix_spawn.
   This macOS cannot run i386 code itself, so an i386 program (the translator, an i386 tool) is
   started as rhost with the same options (root, flags) and the program after them. Host programs
   (x86_64, scripts) start directly. The 32-bit world's DYLD_* variables were meant for its own dyld:
   they travel as R2_DYLD_* (rhost hands them back to the next 32-bit world), and rhost's own R2_*
   settings, which the 32-bit world does not see, are added again. */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/dyld.h>
#include <libkern/OSByteOrder.h>
#include <dlfcn.h>

extern char **environ;
static char g_self[1024];
static char **g_opts; static int g_nopts;           /* rhost's options (without argv[0]) */

void spawn_init(int nopts, char **opts) {
    uint32_t n = sizeof g_self;
    char tmp[1024];
    if (_NSGetExecutablePath(tmp, &n) || !realpath(tmp, g_self)) snprintf(g_self, sizeof g_self, "%s", tmp);
    g_nopts = nopts;
    g_opts = calloc((size_t)nopts + 1, sizeof *g_opts);
    for (int i = 0; i < nopts; i++) g_opts[i] = strdup(opts[i]);
}

/* 1: an i386 program (run it through rhost), 0: anything else (the kernel runs it, or refuses) */
static int is_i386_only(const char *path) {
    unsigned char h[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, h, sizeof h);
    close(fd);
    if (n < 8) return 0;
    uint32_t m; memcpy(&m, h, 4);
    if (m == MH_MAGIC) return ((struct mach_header *)h)->cputype == CPU_TYPE_I386;
    if (m == FAT_CIGAM) {                             /* fat (big-endian header) */
        uint32_t na = OSSwapBigToHostInt32(((struct fat_header *)h)->nfat_arch);
        int i386 = 0, x64 = 0;
        for (uint32_t i = 0; i < na && 8 + (i + 1) * sizeof(struct fat_arch) <= (size_t)n; i++) {
            int ct = (int)OSSwapBigToHostInt32(((struct fat_arch *)(h + 8))[i].cputype);
            if (ct == CPU_TYPE_I386) i386 = 1;
            if (ct == CPU_TYPE_X86_64) x64 = 1;
        }
        return i386 && !x64;
    }
    return 0;
}

/* the environment for the new process, from the 32-bit one. DYLD_* variables: for an i386 program
   they travel as R2_DYLD_* (its rhost gives them to its dyld); for a host program, those rhost set
   for its own 32-bit world (the inserted liboah, the private shared region) are dropped and the
   program's own pass as they are (the Rosettus launcher hands them to the PowerPC program). */
static char **conv_env(u32 ev32, int i386) {
    int n = 0, nh = 0;
    if (ev32) while (rd32(ev32 + 4 * n)) n++;
    for (char **e = environ; *e; e++) nh++;
    char **ev = calloc((size_t)(n + nh + 1), sizeof *ev);
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *v = P32(rd32(ev32 + 4 * i));
        if (!strncmp(v, "DYLD_", 5)) {
            char nm[128]; const char *eq = strchr(v, '=');
            size_t nl = eq ? (size_t)(eq - v) : strlen(v);
            snprintf(nm, sizeof nm, "R2_%.*s", (int)(nl < 100 ? nl : 100), v);
            const char *mine = getenv(nm);
            int ours = mine && eq && !strcmp(mine, eq + 1);
            /* the translator's own settings (the launcher's XYLD_* become the guest's DYLD_*, and the
               translator passes them on at every exec): not the program's. A native program got the
               PowerPC root's library paths: ROOT/native/bash (scripts of PowerPC programs) died in
               dyld at launch, "Symbol not found: _environ" (Version Cue's MySQL never started). */
            if (!ours && eq) {
                char xn[128]; snprintf(xn, sizeof xn, "XYLD_%.*s", (int)(nl - 5 < 100 ? nl - 5 : 100), v + 5);
                const char *x = getenv(xn);
                ours = x && !strcmp(x, eq + 1);
            }
            if (i386) { size_t L = strlen(v) + 4; char *t = malloc(L); snprintf(t, L, "R2_%s", v); ev[k++] = t; }
            else if (!ours) ev[k++] = (char *)v;
        } else ev[k++] = (char *)v;
    }
    for (char **e = environ; *e; e++) {               /* rhost's own settings */
        if (strncmp(*e, "R2_", 3) || !strncmp(*e, "R2_DYLD_", 8)) continue;
        const char *eq = strchr(*e, '='); size_t L = eq ? (size_t)(eq - *e) + 1 : strlen(*e);
        int have = 0;
        for (int i = 0; i < k && !have; i++) have = !strncmp(ev[i], *e, L);
        if (!have) ev[k++] = *e;
    }
    ev[k] = 0;
    return ev;
}

/* argv for the new process; *path becomes rhost for an i386 program */
static char **conv_argv(u32 av32, const char **path, char *mapped, int i386) {
    int n = 0;
    if (av32) while (rd32(av32 + 4 * n)) n++;
    char **av = calloc((size_t)(n + g_nopts + 3), sizeof *av);
    int k = 0;
    if (i386) {
        av[k++] = g_self;
        for (int i = 0; i < g_nopts; i++) av[k++] = g_opts[i];
        av[k++] = mapped;                             /* the program (its argv[0] is lost) */
        for (int i = 1; i < n; i++) av[k++] = P32(rd32(av32 + 4 * i));
        *path = g_self;
    } else {
        for (int i = 0; i < n; i++) av[k++] = P32(rd32(av32 + 4 * i));
        *path = mapped;
    }
    av[k] = 0;
    if (g_trace || getenv("R2_SPAWNTRACE")) rlog("[spawn] %s%s", mapped, i386 ? " (i386: through rhost)" : "");
    return av;
}

static const char *exec_path(u32 p32, char *buf, size_t n) {
    const char *p = P32(p32);
    const char *m = map_path(p, buf, n);
    if (m != p && access(m, F_OK) == 0) return m;
    return p;
}

/* execve(path, argv, envp) */
void sys_execve(ss64_t *s, u32 argp) {
    char mb[1100], mp[1100];
    const char *path;
    snprintf(mp, sizeof mp, "%s", exec_path(rd32(argp), mb, sizeof mb));
    int i386 = is_i386_only(mp);
    char **av = conv_argv(rd32(argp + 4), &path, mp, i386);
    char **ev = conv_env(rd32(argp + 8), i386);
    {   const char *st = getenv("R2_SPAWNTRACE");
        if (st && atoi(st) >= 2) { for (char **e = ev; *e; e++) rlog("[spawn]   env %.300s", *e); for (char **a = av; *a; a++) rlog("[spawn]   arg %.300s", *a); } }
    execve(path, av, ev);
    int e = errno;
    if (g_trace || getenv("R2_SPAWNTRACE")) rlog("[spawn] execve %s failed: %s", path, strerror(e));
    free(av); free(ev);
    ret_err(s, e);
}

/* fork / vfork (as fork): child edx = 1 */
void wq_atfork_child(void);
void trap_atfork_child(void);
void sys_fork(ss64_t *s) {
    pid_t p = fork();
    if (p < 0) { ret_err(s, errno); return; }
    if (p > 0 && (g_trace || getenv("R2_SPAWNTRACE"))) rlog("[spawn] fork -> child %d", (int)p);
    if (p == 0) {
        trap_atfork_child();
        wq_atfork_child();
        if (getenv("R2_TRACE_CHILD")) g_trace = atoi(getenv("R2_TRACE_CHILD"));
        ret_ok2(s, (u32)getppid(), 1);
        return;
    }
    ret_ok2(s, (u32)p, 0);
}

/* posix_spawn(pid_t *pid, path, struct _posix_spawn_args_desc *ad, argv, envp). The attribute and
   file-action blocks are Mojave's i386 layouts (xnu-4903 spawn_internal.h); they are rebuilt with this
   Mac's own posix_spawn API, so its kernel's layouts never matter. */
struct ad32 { u32 attr_size, attrp, fa_size, fa, pa_size, pa, mac_size, mac, coal_size, coal, persona_size, persona; };
#define FA32_SIZE 1040                                 /* _psfa_action_t: type, fd, oflag, mode, path[1024] */
void sys_posix_spawn(ss64_t *s, u32 argp) {
    char mb[1100], mp[1100];
    const char *path;
    u32 pidp = rd32(argp), adp = rd32(argp + 8);
    snprintf(mp, sizeof mp, "%s", exec_path(rd32(argp + 4), mb, sizeof mb));
    int i386 = is_i386_only(mp);
    char **av = conv_argv(rd32(argp + 12), &path, mp, i386);
    char **ev = conv_env(rd32(argp + 16), i386);
    posix_spawnattr_t at; posix_spawn_file_actions_t fa;
    int have_at = 0, have_fa = 0;
    if (adp) {
        struct ad32 a; memcpy(&a, P32(adp), sizeof a);
        if (a.attrp && a.attr_size >= 40) {
            const unsigned char *b = P32(a.attrp);
            short fl; sigset_t sd, sm; pid_t pg; int pc, apptype;
            memcpy(&fl, b, 2); memcpy(&sd, b + 4, 4); memcpy(&sm, b + 8, 4); memcpy(&pg, b + 12, 4);
            memcpy(&pc, b + 32, 4); memcpy(&apptype, b + 36, 4);
            posix_spawnattr_init(&at); have_at = 1;
            posix_spawnattr_setflags(&at, fl);
            posix_spawnattr_setsigdefault(&at, &sd);
            posix_spawnattr_setsigmask(&at, &sm);
            posix_spawnattr_setpgroup(&at, pg);
            static int (*setpc)(posix_spawnattr_t *, int), (*setpt)(posix_spawnattr_t *, int);
            if (!setpc) *(void **)&setpc = dlsym(RTLD_DEFAULT, "posix_spawnattr_setpcontrol_np");
            if (!setpt) *(void **)&setpt = dlsym(RTLD_DEFAULT, "posix_spawnattr_setprocesstype_np");
            if (setpc && pc) setpc(&at, pc);
            if (setpt && apptype) setpt(&at, apptype);
            if (g_trace) rlog("[spawn] attributes: flags %#x pgroup %d", fl, pg);
        }
        if (a.fa && a.fa_size >= 8) {
            const unsigned char *b = P32(a.fa);
            int cnt; memcpy(&cnt, b + 4, 4);
            posix_spawn_file_actions_init(&fa); have_fa = 1;
            for (int i = 0; i < cnt && 8 + (u32)(i + 1) * FA32_SIZE <= a.fa_size; i++) {
                const unsigned char *x = b + 8 + i * FA32_SIZE;
                int type, fd, ofl; unsigned short mode;
                memcpy(&type, x, 4); memcpy(&fd, x + 4, 4); memcpy(&ofl, x + 8, 4); memcpy(&mode, x + 12, 2);
                const char *p = (const char *)(x + 14);
                switch (type) {
                case 0: { char pb[1100]; posix_spawn_file_actions_addopen(&fa, fd, map_path(p, pb, sizeof pb), ofl, mode); break; }
                case 1: posix_spawn_file_actions_addclose(&fa, fd); break;
                case 2: posix_spawn_file_actions_adddup2(&fa, fd, ofl); break;   /* newfiledes in psfao_oflag */
                case 3: posix_spawn_file_actions_addinherit_np(&fa, fd); break;
                default: rlog("[spawn] file action %d not supported", type);
                }
            }
        }
    }
    pid_t pid = 0;
    int r = posix_spawn(&pid, path, have_fa ? &fa : 0, have_at ? &at : 0, av, ev);
    if (r == 0 && pidp) wr32(pidp, (u32)pid);
    if (have_at) posix_spawnattr_destroy(&at);
    if (have_fa) posix_spawn_file_actions_destroy(&fa);
    free(av); free(ev);
    if (g_trace || getenv("R2_SPAWNTRACE")) rlog("[spawn] posix_spawn -> %d pid %d", r, pid);
    if (r) ret_err(s, r); else ret_ok(s, 0);
}
