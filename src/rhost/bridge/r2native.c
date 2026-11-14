/* r2native.c - i386 side of the hostcall bridge: the trap and the CF property list copies */
#include "r2native.h"
#include <stdlib.h>
#include <dlfcn.h>

unsigned int r2_hostcall(unsigned int id, unsigned int *blk) {
    unsigned int r;
    /* int $0x82 with eax = 0x5232; the host reads (id, blk) above a return-address slot */
    __asm__ volatile("pushl %2\n\tpushl %1\n\tpushl $0\n\tmovl $0x5232, %%eax\n\tint $0x82\n\taddl $12, %%esp"
                     : "=a"(r) : "r"(id), "r"(blk) : "memory", "ecx", "edx");
    return r;
}

int r2_available(void) {
    static int a = -1;
    if (a < 0) { const char *e = getenv("R2_HOST"); a = e && *e == '1'; }
    return a;
}

CFDataRef r2_cf_out(CFTypeRef obj) {
    if (!obj) return 0;
    return CFPropertyListCreateData(0, obj, kCFPropertyListBinaryFormat_v1_0, 0, 0);
}

CFTypeRef r2_cf_in(unsigned int addr, unsigned int len) {
    if (!addr || !len) return 0;
    CFDataRef d = CFDataCreate(0, (const UInt8 *)addr, (CFIndex)len);
    CFTypeRef o = CFPropertyListCreateWithData(0, d, kCFPropertyListImmutable, 0, 0);
    CFRelease(d);
    return o;
}

/* an array of plain values (window IDs) from the host: it comes as an array of numbers */
CFArrayRef r2_ids_in(unsigned int addr, unsigned int len) {
    CFArrayRef a = (CFArrayRef)r2_cf_in(addr, len), r;
    CFIndex i, n;
    const void **v;
    if (!a) return 0;
    n = CFGetTypeID(a) == CFArrayGetTypeID() ? CFArrayGetCount(a) : 0;
    v = (const void **)calloc(n ? n : 1, sizeof *v);
    for (i = 0; i < n; i++) {
        unsigned int x = 0;
        CFNumberRef num = (CFNumberRef)CFArrayGetValueAtIndex(a, i);
        if (num && CFGetTypeID(num) == CFNumberGetTypeID()) CFNumberGetValue(num, kCFNumberSInt32Type, &x);
        v[i] = (const void *)(uintptr_t)x;
    }
    r = CFArrayCreate(0, v, n, 0);
    free(v); CFRelease(a);
    return r;
}

/* CFRetain / CFRelease that also take native objects (handles) */
void r2_CFRelease(CFTypeRef o) {
    if (R2_IS_HANDLE(o)) r2_release((void *)o);
    else if (o) CFRelease(o);
}
CFTypeRef r2_CFRetain(CFTypeRef o) {
    if (R2_IS_HANDLE(o)) r2_retain((void *)o);
    else if (o) CFRetain(o);
    return o;
}

/* Functions of the i386 frameworks that talk to this Mac's window server, Dock, Launch Services
   or pasteboard server: their protocols changed after Mojave, so the i386 copies must not be
   called; a function of these families that is not bridged is reported missing instead. */
static const char *const g_deny[] = {
    "CGS", "SLS", "SLPS", "_SLPS", "CGWindow", "CGDisplay", "CGEvent", "CGGetDisplays", "CGGetActiveDisplay",
    "CGMainDisplay", "CGSession", "CGShielding", "CGAcquire", "CGRelease", "CGCapture", "CGWarp", "CGAssociate",
    "_LS", "LSApplication", "CoreDock", "CoreDrag", "CoreCursor", "Pasteboard", "_Pasteboard",
    "SetFrontProcess", "ShowHideProcess", "TransformProcessType", "SetSystemUIMode", "SetApplicationDockTile",
    "GetNextProcess", "GetFrontProcess", "GetProcessForPID", "GetProcessPID",
    "_HideOtherApplications", "_ShowAllApplications", "_SignalApplicationReady",
    "GetProcessBundleLocation", "IsProcessVisible", "SameProcess", "KillProcess", "WakeUpProcess", 0 };

void *r2_lookup(const char *name, int *deny) {
    static void *self;
    Dl_info di;
    void *f;
    if (deny) *deny = 0;
    if (!r2_available()) return 0;
    if (!strcmp(name, "CFRelease")) return (void *)r2_CFRelease;
    if (!strcmp(name, "CFRetain")) return (void *)r2_CFRetain;
    if (!self) { dladdr((void *)r2_lookup, &di); self = dlopen(di.dli_fname, RTLD_LAZY | RTLD_NOLOAD); }
    f = self ? dlsym(self, name) : 0;
    if (f) {
        Dl_info a, b;
        if (!dladdr(f, &a) || !dladdr((void *)r2_lookup, &b) || a.dli_fbase != b.dli_fbase) f = 0;
    }
    if (f) return f;
    if (deny) for (int i = 0; g_deny[i]; i++)
        if (!strncmp(name, g_deny[i], strlen(g_deny[i]))) { *deny = 1; break; }
    /* OpenGL: the root's i386 OpenGL loads Mojave's GPU drivers (they crash against this Mac's
       kernel), so a GL or CGL function that is not bridged is missing too */
    if (deny && (!strncmp(name, "CGL", 3) || (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') ||
                 (!strncmp(name, "glu", 3) && name[3] >= 'A' && name[3] <= 'Z'))) *deny = 1;
    return 0;
}
