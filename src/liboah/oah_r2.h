/* oah_r2.h - Rosettus 2 (Catalina and later): liboah built with -DOAH_R2.
   The translator runs as 32-bit code inside rhost, a 64-bit process. The i386 frameworks there
   are Mojave's; those that talk to the window server, the Dock, Launch Services or the pasteboard
   server do not work with a later macOS. Their functions are bridged to this Mac's own 64-bit
   frameworks by libr2native.dylib (next to liboah), and every native lookup of liboah (dlsym)
   goes there first. A function of those families that is not bridged is not used (dlsym returns
   0, logged once with OAH_R2TRACE=1); everything else still comes from the i386 frameworks.
   Native objects (CGContext, CGImage, CGEvent...) are handles in the 32-bit world: CFRelease and
   CFRetain of liboah take both. */
#ifndef OAH_R2_H
#define OAH_R2_H
static void *(*g_r2_lookup)(const char *, int *);
static int g_r2_init, g_r2_trace;
static int oah_r2(void) {
    if (!g_r2_init) {
        Dl_info di;
        g_r2_init = 1;
        g_r2_trace = getenv("OAH_R2TRACE") != 0;
        if (getenv("R2_HOST") && dladdr((void *)oah_r2, &di) && di.dli_fname) {
            char p[1024], *s;
            void *h;
            snprintf(p, sizeof p, "%s", di.dli_fname);
            s = strrchr(p, '/');
            if (s) { snprintf(s + 1, sizeof p - (s + 1 - p), "libr2native.dylib");
                     h = dlopen(p, RTLD_LAZY | RTLD_LOCAL);
                     if (h) *(void **)&g_r2_lookup = dlsym(h, "r2_lookup");
                     if (!g_r2_lookup) fprintf(stderr, "[r2] %s: %s\n", p, dlerror()); }
            if (g_r2_trace) fprintf(stderr, "[r2] liboah at %p, libr2native %s\n", di.dli_fbase, g_r2_lookup ? "loaded" : "missing");
        }
    }
    return g_r2_lookup != 0;
}
/* Lookups are cached by (handle, name pointer): the names are literals, and some bridges look a
   function up on every call. dyld's dlsym over Mojave's ~400 images took ~1 ms each, so an app
   calling such a bridge from its event loop (TextWrangler) ran at 100% CPU. */
static void *oah_r2_dlsym(void *h, const char *n) {
    static struct { void *h; char n[64]; void *f; int used; } cache[512];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    unsigned int k = (unsigned int)((uintptr_t)h >> 4), i;
    size_t len = strlen(n);
    void *f;
    for (i = 0; n[i]; i++) k = k * 31 + (unsigned char)n[i];
    k &= 511;
    if (len < 64) {
        pthread_mutex_lock(&mu);
        if (cache[k].used && cache[k].h == h && !strcmp(cache[k].n, n)) { f = cache[k].f; pthread_mutex_unlock(&mu); return f; }
        pthread_mutex_unlock(&mu);
    }
    f = 0;
    if (oah_r2()) {
        int deny = 0;
        f = g_r2_lookup(n, &deny);
        if (!f && deny) { if (g_r2_trace) fprintf(stderr, "[r2] native %s: not bridged\n", n); }
        else if (!f) f = dlsym(h, n);
    } else f = dlsym(h, n);
    if (len < 64 && (f || (h && h != RTLD_DEFAULT && h != RTLD_NEXT))) {   /* a miss on RTLD_DEFAULT may be found after a dlopen */
        pthread_mutex_lock(&mu);
        cache[k].h = h; memcpy(cache[k].n, n, len + 1); cache[k].f = f; cache[k].used = 1;
        pthread_mutex_unlock(&mu);
    }
    return f;
}
#define dlsym(h, n) oah_r2_dlsym(h, n)
static void oah_r2_CFRelease(CFTypeRef o) {
    static void (*rel)(CFTypeRef);
    if (!rel) { if (oah_r2()) *(void **)&rel = g_r2_lookup("CFRelease", 0); if (!rel) rel = CFRelease; }
    rel(o);
}
static CFTypeRef oah_r2_CFRetain(CFTypeRef o) {
    static CFTypeRef (*ret)(CFTypeRef);
    if (!ret) { if (oah_r2()) *(void **)&ret = g_r2_lookup("CFRetain", 0); if (!ret) ret = CFRetain; }
    return ret(o);
}
#define CFRelease(o) oah_r2_CFRelease(o)
#define CFRetain(o) oah_r2_CFRetain(o)
/* backtrace() finds no frames on the translator's native stack (it is not the thread's stack as
   libpthread knows it), and Mojave's CarbonCore logs a failed path lookup with
   strdup(backtrace_symbols(...)[0] + 4): a crash (the Folder Manager meets Catalina's volumes now and
   then, e.g. /private/var/vm). With no frames, walk the frame pointers. */
#include <execinfo.h>
static int oah_r2_backtrace(void **buf, int max) {
    int n = backtrace(buf, max);
    void **fp;
    if (n > 0 || max <= 0) return n;
    fp = (void **)__builtin_frame_address(0);
    while (fp && !((unsigned int)fp & 3) && n < max) {
        void **nfp;
        if (!fp[1]) break;
        buf[n++] = fp[1];
        nfp = (void **)fp[0];
        if (nfp <= fp || (char *)nfp - (char *)fp > 0x100000) break;
        fp = nfp;
    }
    if (!n) buf[n++] = (void *)oah_r2_backtrace;
    return n;
}
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *rep, *orig; }
    oah_r2_interpose_backtrace = { (const void *)oah_r2_backtrace, (const void *)backtrace };
#endif
