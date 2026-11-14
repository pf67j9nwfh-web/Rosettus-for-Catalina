/* hostcall.c - the 32-bit world calls this Mac's own (64-bit) frameworks through the host.
   libr2native.dylib (i386) has a stub for each function: it packs the arguments into a block of
   32-bit words and traps (machine-dependent call 0x5232 with the call number and the block). The
   host converts the arguments (CGFloat widths, structures, native objects as small handles, CF
   property lists copied across) and calls the function; results go back into the block. */
#include "hostcall.h"
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <math.h>

void *hc_sym(const char *name) {
    static void *libs[8]; static int nlibs;
    if (!nlibs) {
        static const char *const paths[] = {
            "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
            "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
            "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices",
            "/System/Library/Frameworks/Carbon.framework/Carbon",
            "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
            "/System/Library/Frameworks/OpenGL.framework/OpenGL", 0 };
        for (int i = 0; paths[i]; i++) { void *h = dlopen(paths[i], RTLD_LAZY | RTLD_GLOBAL); if (h) libs[nlibs++] = h; }
    }
    void *f = dlsym(RTLD_DEFAULT, name);
    char b[160];
    if (!f && !strncmp(name, "CGS", 3)) { snprintf(b, sizeof b, "SLS%s", name + 3); f = dlsym(RTLD_DEFAULT, b); }
    if (!f && !strncmp(name, "CG", 2)) { snprintf(b, sizeof b, "SL%s", name + 2); f = dlsym(RTLD_DEFAULT, b); }
    return f;
}

void hc_missing(const char *name) { rlog("[hostcall] %s is not available on this Mac", name); }

/* ---- handles: native objects for the 32-bit world.
   One handle per object (the same object comes back as the same handle), reference counted:
   each time a function hands the object out counts once (Create/Copy results own +1 on the
   host; a Get result shares the host's reference), r2_retain adds one, and a releasing call
   (h! in the spec, r2_release) takes one; the host object is released with the last one. */
static pthread_mutex_t g_hmu = PTHREAD_MUTEX_INITIALIZER;
static void **g_hobj; static u32 *g_href; static u32 g_hcap, g_hfree = 0, g_hmax = 0;
#define HBASE 0x52000000u                 /* recognisable in traces; never a valid pointer there */
u32 hc_handle_put(void *obj) {
    if (!obj) return 0;
    pthread_mutex_lock(&g_hmu);
    u32 i;
    for (i = 0; i < g_hmax; i++) if (g_hobj[i] == obj) { g_href[i]++; pthread_mutex_unlock(&g_hmu); return HBASE + i + 1; }
    for (i = g_hfree; i < g_hcap && g_hobj[i]; i++) ;
    if (i == g_hcap) {
        u32 n = g_hcap ? g_hcap * 2 : 1024;
        g_hobj = realloc(g_hobj, n * sizeof *g_hobj); memset(g_hobj + g_hcap, 0, (n - g_hcap) * sizeof *g_hobj);
        g_href = realloc(g_href, n * sizeof *g_href); memset(g_href + g_hcap, 0, (n - g_hcap) * sizeof *g_href);
        g_hcap = n;
    }
    g_hobj[i] = obj; g_href[i] = 1; g_hfree = i + 1;
    if (i + 1 > g_hmax) g_hmax = i + 1;
    pthread_mutex_unlock(&g_hmu);
    return HBASE + i + 1;
}
void hc_handle_ref(u32 h) {
    if (h < HBASE + 1 || h > HBASE + g_hcap) return;
    pthread_mutex_lock(&g_hmu);
    if (g_hobj[h - HBASE - 1]) g_href[h - HBASE - 1]++;
    pthread_mutex_unlock(&g_hmu);
}
/* one reference less: the count left (0: the slot is free, the caller releases the host object) */
int hc_handle_unref(u32 h) {
    static int keep = -1;                 /* R2_NO_HOSTRELEASE=1: never release host objects (a test) */
    if (keep < 0) keep = getenv("R2_NO_HOSTRELEASE") != 0;
    if (keep) return 1;
    if (h < HBASE + 1 || h > HBASE + g_hcap) return 0;
    pthread_mutex_lock(&g_hmu);
    u32 i = h - HBASE - 1; int left = 0;
    if (g_hobj[i] && g_href[i] > 1) left = (int)--g_href[i];
    else { g_hobj[i] = 0; g_href[i] = 0; if (i < g_hfree) g_hfree = i; }
    pthread_mutex_unlock(&g_hmu);
    return left;
}
/* r2_retain (spec: r2_retain=r2h_retain v H) */
void r2h_retain(u32 h) { hc_handle_ref(h); }
/* a CF constant of this Mac's frameworks (spec @NAME): its value, +1 */
CFTypeRef r2h_cfconst(const char *name) {
    hc_sym("CGMainDisplayID");                  /* the frameworks are loaded */
    CFTypeRef *p = (CFTypeRef *)dlsym(RTLD_DEFAULT, name);
    if (!p || !*p) { hc_missing(name); return 0; }
    return CFRetain(*p);
}

/* ---- pointer-sized IDs (PasteboardItemID): small values pass as they are, others get a number */
static void **g_ids; static u32 g_nids, g_idcap;
#define IDBASE 0x53000000u
u32 hc_id_put(void *v) {
    if ((uintptr_t)v < 0x10000000u) return (u32)(uintptr_t)v;
    pthread_mutex_lock(&g_hmu);
    u32 i;
    for (i = 0; i < g_nids && g_ids[i] != v; i++) ;
    if (i == g_nids) {
        if (g_nids == g_idcap) { g_idcap = g_idcap ? g_idcap * 2 : 256; g_ids = realloc(g_ids, g_idcap * sizeof *g_ids); }
        g_ids[g_nids++] = v;
    }
    pthread_mutex_unlock(&g_hmu);
    return IDBASE + i;
}
void *hc_id_get(u32 w) {
    if (w >= IDBASE && w - IDBASE < g_nids) return g_ids[w - IDBASE];
    return (void *)(uintptr_t)w;
}
void *hc_handle_get(u32 h) {
    if (h < HBASE + 1 || h > HBASE + g_hcap) return 0;
    return g_hobj[h - HBASE - 1];
}
void hc_handle_free(u32 h) {
    if (h < HBASE + 1 || h > HBASE + g_hcap) return;
    pthread_mutex_lock(&g_hmu);
    g_hobj[h - HBASE - 1] = 0;
    if (h - HBASE - 1 < g_hfree) g_hfree = h - HBASE - 1;
    pthread_mutex_unlock(&g_hmu);
}

/* ---- CF property lists across */
CFTypeRef hc_cf_in(u32 addr, u32 len) {
    if (!addr || !len) return 0;
    CFDataRef d = CFDataCreateWithBytesNoCopy(0, P32(addr), len, kCFAllocatorNull);
    CFTypeRef o = CFPropertyListCreateWithData(0, d, kCFPropertyListImmutable, 0, 0);
    CFRelease(d);
    return o;
}
/* the result goes into this thread's 32-bit scratch buffer; the stub copies it at once */
static __thread u32 t_scr, t_scrlen;
/* a copy with only property list values: LaunchServices' information dictionaries carry LSASN
   objects (and the like), which made the whole dictionary fail to serialize (+1 or NULL) */
static CFTypeRef hc_plist_clean(CFTypeRef o) {
    CFTypeID t = CFGetTypeID(o);
    if (t == CFStringGetTypeID() || t == CFNumberGetTypeID() || t == CFBooleanGetTypeID() ||
        t == CFDataGetTypeID() || t == CFDateGetTypeID()) return CFRetain(o);
    if (t == CFArrayGetTypeID()) {
        CFIndex n = CFArrayGetCount(o);
        CFMutableArrayRef m = CFArrayCreateMutable(0, n, &kCFTypeArrayCallBacks);
        for (CFIndex i = 0; i < n; i++) { CFTypeRef v = hc_plist_clean(CFArrayGetValueAtIndex(o, i)); if (v) { CFArrayAppendValue(m, v); CFRelease(v); } }
        return m;
    }
    if (t == CFDictionaryGetTypeID()) {
        CFIndex n = CFDictionaryGetCount(o);
        const void **k = malloc(sizeof(void *) * (size_t)(n + 1)), **v = malloc(sizeof(void *) * (size_t)(n + 1));
        CFMutableDictionaryRef m = CFDictionaryCreateMutable(0, n, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionaryGetKeysAndValues(o, k, v);
        for (CFIndex i = 0; i < n; i++) {
            if (CFGetTypeID(k[i]) != CFStringGetTypeID()) continue;
            CFTypeRef c = hc_plist_clean(v[i]);
            if (c) { CFDictionarySetValue(m, k[i], c); CFRelease(c); }
        }
        free(k); free(v);
        return m;
    }
    return 0;
}
void hc_cf_out(CFTypeRef obj, u32 *b) {
    b[0] = b[1] = 0;
    if (!obj) return;
    CFDataRef d = CFPropertyListCreateData(0, obj, kCFPropertyListBinaryFormat_v1_0, 0, 0);
    if (!d) { CFTypeRef c = hc_plist_clean(obj); if (c) { d = CFPropertyListCreateData(0, c, kCFPropertyListBinaryFormat_v1_0, 0, 0); CFRelease(c); } }
    CFRelease(obj);
    if (!d) return;
    u32 len = (u32)CFDataGetLength(d);
    if (len > t_scrlen) {
        if (t_scr) low_free(t_scr, t_scrlen);
        u32 sz = (len + 0xffff) & ~0xffffu;
        t_scr = low_alloc(sz, 0, 1); t_scrlen = 0;
        if (!t_scr || mmap(P32(t_scr), sz, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) { t_scr = 0; CFRelease(d); return; }
        t_scrlen = sz;
    }
    memcpy(P32(t_scr), CFDataGetBytePtr(d), len);
    b[0] = t_scr; b[1] = len;
    CFRelease(d);
}

/* an array of plain values (window IDs, NULL callbacks) goes across as an array of numbers */
void hc_ids_out(CFArrayRef a, u32 *b) {
    b[0] = b[1] = 0;
    if (!a) return;
    CFIndex n = CFArrayGetCount(a);
    CFMutableArrayRef m = CFArrayCreateMutable(0, n, &kCFTypeArrayCallBacks);
    for (CFIndex i = 0; i < n; i++) {
        int32_t x = (int32_t)(uintptr_t)CFArrayGetValueAtIndex(a, i);
        CFNumberRef num = CFNumberCreate(0, kCFNumberSInt32Type, &x);
        CFArrayAppendValue(m, num); CFRelease(num);
    }
    CFRelease(a);
    hc_cf_out(m, b);
}

/* machine-dependent call 0x5232 (id, block) */
void hostcall(ss64_t *s, u32 argp) {
    u32 id = rd32(argp), blk = rd32(argp + 4);
    if (id >= (u32)g_hc_count || !blk) { s->__rax = (u32)-1; return; }
    if (g_trace > 1) rlog("[hostcall] %s", g_hc_table[id].name);
    extern void *objc_autoreleasePoolPush(void); extern void objc_autoreleasePoolPop(void *);
    void *pool = objc_autoreleasePoolPush();
    { extern int g_prof; extern void prof_add(u32, uint64_t);
      uint64_t pt = g_prof ? mach_absolute_time() : 0;
      g_hc_table[id].fn(P32(blk));
      if (pt) prof_add((4u << 16) | id, mach_absolute_time() - pt); }
    { extern void heapchk(const char *, long); heapchk(g_hc_table[id].name, (long)id); }
    objc_autoreleasePoolPop(pool);
    s->__rax = 0;
}

/* GetProcessInformation for the i386 world: its ProcessInfoRec (60 bytes) has 32-bit pointers
   (processName at 4, processAppSpec FSSpec* at 56); this Mac's has 64-bit ones and an FSRef* at
   the end. Name and the 32-bit fields are copied; the FSSpec is left alone (liboah rebuilds it).
   The i386 Process Manager registers the app with LaunchServices first and aborts on Catalina;
   without this Office 2004 said "There is not enough memory or disk space to run Word". */
int r2h_gpi(const void *psn, unsigned char *rec) {
    static int (*gpi)(const void *, void *);
    if (!gpi) *(void **)&gpi = hc_sym("GetProcessInformation");
    if (!gpi) { hc_missing("GetProcessInformation"); return -50; }
    struct __attribute__((packed, aligned(2))) {
        uint32_t len; unsigned char *name; uint32_t psn[2]; uint32_t type, sig, mode; void *loc;
        uint32_t size, freemem; uint32_t launcher[2]; uint32_t launchdate, activetime; void *appref;
    } h;
    unsigned char nm[256] = "";
    memset(&h, 0, sizeof h);
    h.len = sizeof h;
    uint32_t gname = *(uint32_t *)(rec + 4);
    h.name = gname ? nm : 0;
    int err = gpi(psn, &h);
    if (err) return err;
    memcpy(rec + 8, h.psn, 8);
    *(uint32_t *)(rec + 16) = h.type; *(uint32_t *)(rec + 20) = h.sig; *(uint32_t *)(rec + 24) = h.mode;
    *(uint32_t *)(rec + 28) = 0;
    *(uint32_t *)(rec + 32) = h.size; *(uint32_t *)(rec + 36) = h.freemem;
    memcpy(rec + 40, h.launcher, 8);
    *(uint32_t *)(rec + 48) = h.launchdate; *(uint32_t *)(rec + 52) = h.activetime;
    if (gname) memcpy(P32(gname), nm, nm[0] < 63 ? nm[0] + 1u : 64u);
    if (g_trace) rlog("[hostcall] GetProcessInformation -> %d size %u free %u", err, h.size, h.freemem);
    return 0;
}

/* R2DockTile(path, data, len): the running app's Dock tile from an icon file (path) or from 'icns'
   data (a resource fork icon). 64-bit HIToolbox dropped SetApplicationDockTileImage; HIServices'
   CoreDockGetProcessContext gives the tile's bitmap context, and flushing it sends the image to
   the Dock (as NSDockTile does). The largest image up to 512 pixels is drawn. */
#include <ImageIO/ImageIO.h>
int r2h_docktile(const char *path, const void *data, unsigned int len) {
    static OSStatus (*getctx)(CGContextRef *, CGRect *);
    if (!getctx) *(void **)&getctx = dlsym(RTLD_DEFAULT, "CoreDockGetProcessContext");
    if (!getctx) { hc_missing("CoreDockGetProcessContext"); return -1; }
    CGImageSourceRef src = 0;
    if (data && len) {
        CFDataRef d = CFDataCreate(NULL, data, len);
        if (d) { src = CGImageSourceCreateWithData(d, NULL); CFRelease(d); }
    } else if (path && *path) {
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
        if (u) { src = CGImageSourceCreateWithURL(u, NULL); CFRelease(u); }
    }
    if (!src) return -43;
    CGImageRef best = 0; size_t bw = 0, n = CGImageSourceGetCount(src);
    for (size_t k = 0; k < n; k++) {
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, k, NULL);
        size_t w = img ? CGImageGetWidth(img) : 0;
        if (img && w > bw && w <= 512) { if (best) CGImageRelease(best); best = img; bw = w; }
        else if (img) CGImageRelease(img);
    }
    CFRelease(src);
    if (!best) return -43;
    CGContextRef ctx = 0; CGRect r = CGRectZero;
    OSStatus e = getctx(&ctx, &r);
    if (!e && ctx) {
        if (r.size.width <= 0) r = CGRectMake(0, 0, 128, 128);
        CGContextClearRect(ctx, r);
        CGContextDrawImage(ctx, r, best);
        CGContextFlush(ctx);
    }
    CGImageRelease(best);
    return e ? (int)e : ctx ? 0 : -1;
}

/* R2SetBackgroundKind(kind): 2 background-only, 1 UI element. LaunchServices takes the application
   type from the main bundle's Info dictionary when the process checks in; rhost has no bundle, so
   a background guest (Microsoft Database Daemon) would check in as a foreground app. */
int r2h_bgkind(int kind) {
    CFBundleRef mb = CFBundleGetMainBundle();
    CFDictionaryRef info = mb ? CFBundleGetInfoDictionary(mb) : 0;
    if (!info || kind < 1 || kind > 2) return -1;
    CFDictionarySetValue((CFMutableDictionaryRef)info, kind == 2 ? CFSTR("LSBackgroundOnly") : CFSTR("LSUIElement"), kCFBooleanTrue);
    return 0;
}

/* R2MenuTitleDraw(ctx, title, bold, cx, r, g, b, a): one menu bar title in this Mac's menu-title
   font, centred at cx, into the menu bar image window's context (a host CGContext). liboah used
   the 10.6 bar's own pixels under Rosettus 2 (the i386 CoreText cannot draw into a host context):
   blurry on Retina, and with 10.6's white emboss under every title. */
#include <CoreText/CoreText.h>
int r2h_mbtitle(CGContextRef ctx, CFStringRef t, int bold, float cx, float r, float g, float b, float a) {
    static CTFontRef f[2];
    if (!ctx || !t || CFGetTypeID(t) != CFStringGetTypeID()) return 0;
    bold = bold ? 1 : 0;
    if (!f[bold]) f[bold] = CTFontCreateUIFontForLanguage(bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontMenuTitle, 14, NULL);
    if (!f[bold]) return 0;
    const void *k[2] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    const void *v[2] = { f[bold], kCFBooleanTrue };
    CFDictionaryRef at = CFDictionaryCreate(0, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(0, t, at);
    CTLineRef line = as ? CTLineCreateWithAttributedString(as) : 0;
    if (line) {
        double wd = CTLineGetTypographicBounds(line, 0, 0, 0);
        CGContextSaveGState(ctx);
        CGContextSetShadowWithColor(ctx, CGSizeZero, 0, NULL);
        CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
        CGContextSetRGBFillColor(ctx, r, g, b, a);
        CGContextSetTextPosition(ctx, cx - wd / 2, 6);
        CTLineDraw(line, ctx);
        CGContextRestoreGState(ctx);
        CFRelease(line);
    }
    if (as) CFRelease(as);
    CFRelease(at);
    return line ? 1 : 0;
}

const char *prof_hc_name(u32 id) { return id < (u32)g_hc_count ? g_hc_table[id].name : "?"; }

/* ---- OpenGL results that are pointers: this Mac's OpenGL hands out 64-bit addresses, so they
   are copied to memory below 4 GB for the 32-bit world. */
static u32 low_buf(u32 size) {          /* zeroed read/write pages below 4 GB, 0 on failure */
    u32 rl = (size + 0xfff) & ~0xfffu, a = low_alloc(rl, 0, 1);
    if (!a) return 0;
    if (mmap(P32(a), rl, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) { low_free(a, rl); return 0; }
    return a;
}
/* glGetString: one copy per (name, string); the strings of a renderer do not change */
u32 r2h_glGetString(u32 name) {
    static const unsigned char *(*f)(unsigned int);
    static struct { u32 name, addr, cap; } c[16];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (!f) *(void **)&f = hc_sym("glGetString");
    if (!f) return 0;
    const char *s = (const char *)f(name);
    if (!s) return 0;
    u32 n = (u32)strlen(s) + 1, r = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 16; i++) {
        if (c[i].addr && c[i].name == name && c[i].cap >= n) { r = c[i].addr; break; }
        if (!c[i].addr || c[i].name == name) {
            if (c[i].addr) low_free(c[i].addr, c[i].cap);
            c[i].cap = (n + 0xfff) & ~0xfffu; c[i].addr = low_buf(c[i].cap); c[i].name = name; r = c[i].addr; break;
        }
    }
    if (r && strcmp((const char *)P32(r), s)) memcpy(P32(r), s, n);
    pthread_mutex_unlock(&mu);
    return r;
}
/* glMapBuffer / glUnmapBuffer: the mapping is shadowed below 4 GB (copied in at the map, copied
   back at the unmap unless it was read-only) */
static struct { u32 target, addr, size, access; void *host; } g_map[16];
static pthread_mutex_t g_mapmu = PTHREAD_MUTEX_INITIALIZER;
u32 r2h_glMapBuffer(u32 target, u32 access) {
    static void *(*map)(unsigned int, unsigned int);
    static void (*getp)(unsigned int, unsigned int, int *);
    if (!map) { *(void **)&map = hc_sym("glMapBuffer"); *(void **)&getp = hc_sym("glGetBufferParameteriv"); }
    if (!map || !getp) return 0;
    int size = 0; getp(target, 0x8764 /* GL_BUFFER_SIZE */, &size);
    void *h = map(target, access);
    if (!h || size <= 0) return 0;
    u32 a = low_buf((u32)size);
    if (!a) return 0;
    if (access != 0x88B9 /* GL_WRITE_ONLY */) memcpy(P32(a), h, (size_t)size);
    pthread_mutex_lock(&g_mapmu);
    for (int i = 0; i < 16; i++) if (!g_map[i].addr) {
        g_map[i].target = target; g_map[i].addr = a; g_map[i].size = (u32)size; g_map[i].access = access; g_map[i].host = h; break; }
    pthread_mutex_unlock(&g_mapmu);
    return a;
}
u32 r2h_glUnmapBuffer(u32 target) {
    static unsigned char (*unmap)(unsigned int);
    if (!unmap) *(void **)&unmap = hc_sym("glUnmapBuffer");
    if (!unmap) return 0;
    pthread_mutex_lock(&g_mapmu);
    for (int i = 0; i < 16; i++) if (g_map[i].addr && g_map[i].target == target) {
        if (g_map[i].access != 0x88B8 /* GL_READ_ONLY */) memcpy(g_map[i].host, P32(g_map[i].addr), g_map[i].size);
        { extern void free32_user(u32, u32); free32_user(g_map[i].addr, (g_map[i].size + 0xfff) & ~0xfffu); }   /* as munmap(2) */
        g_map[i].addr = 0;
        break;
    }
    pthread_mutex_unlock(&g_mapmu);
    return unmap(target);
}
/* GL calls taking arrays of pointers (the 32-bit world's are 4-byte pointers; this Mac's GL reads 8):
   glShaderSource read a garbage string array and Pages / Keynote / Numbers hung in the shader
   compiler with no window; glGetPointerv & co wrote 8 bytes into a 4-byte slot. */
static const char **ptrs64(const u32 *p32, int n, const char **stack, int cap) {
    if (!p32 || n <= 0) return 0;
    const char **v = n <= cap ? stack : malloc(sizeof *v * (size_t)n);
    for (int i = 0; i < n; i++) v[i] = p32[i] ? (const char *)P32(p32[i]) : 0;
    return v;
}
#define PTRS_FREE(v, st) do { if ((v) && (v) != (st)) free((void *)(v)); } while (0)
static void shader_source(const char *name, unsigned int sh, int count, void *strings, void *lengths) {
    void (*f)(unsigned int, int, const char **, const int *) = (void (*)(unsigned int, int, const char **, const int *))hc_sym(name);
    const char *st[16], **v = ptrs64(strings, count, st, 16);
    if (f) f(sh, count, v, lengths);
    PTRS_FREE(v, st);
}
void r2h_glShaderSource(unsigned int sh, int count, void *s, void *l) { shader_source("glShaderSource", sh, count, s, l); }
void r2h_glShaderSourceARB(unsigned int sh, int count, void *s, void *l) { shader_source("glShaderSourceARB", sh, count, s, l); }
void r2h_glGetUniformIndices(unsigned int prog, int count, void *names, void *idx) {
    void (*f)(unsigned int, int, const char **, unsigned int *) = (void (*)(unsigned int, int, const char **, unsigned int *))hc_sym("glGetUniformIndices");
    const char *st[16], **v = ptrs64(names, count, st, 16);
    if (f) f(prog, count, v, idx);
    PTRS_FREE(v, st);
}
void r2h_glTransformFeedbackVaryingsEXT(unsigned int prog, int count, void *names, unsigned int mode) {
    void (*f)(unsigned int, int, const char **, unsigned int) = (void (*)(unsigned int, int, const char **, unsigned int))hc_sym("glTransformFeedbackVaryingsEXT");
    const char *st[16], **v = ptrs64(names, count, st, 16);
    if (f) f(prog, count, v, mode);
    PTRS_FREE(v, st);
}
static void multi_draw_elements(const char *name, unsigned int mode, void *count, unsigned int type, void *indices, int n) {
    void (*f)(unsigned int, const int *, unsigned int, const void *const *, int) = (void (*)(unsigned int, const int *, unsigned int, const void *const *, int))hc_sym(name);
    const char *st[64], **v = ptrs64(indices, n, st, 64);   /* offsets into a bound buffer, or pointers: zero-extended */
    if (f) f(mode, count, type, (const void *const *)v, n);
    PTRS_FREE(v, st);
}
void r2h_glMultiDrawElements(unsigned int m, void *c, unsigned int t, void *i, int n) { multi_draw_elements("glMultiDrawElements", m, c, t, i, n); }
void r2h_glMultiDrawElementsEXT(unsigned int m, void *c, unsigned int t, void *i, int n) { multi_draw_elements("glMultiDrawElementsEXT", m, c, t, i, n); }
/* pointer queries: the 32-bit caller's slot gets the low 4 bytes (its own arrays live below 4 GB) */
static void get_ptr2(const char *name, unsigned int a, unsigned int b, void *out) {
    void (*f)(unsigned int, unsigned int, void **) = (void (*)(unsigned int, unsigned int, void **))hc_sym(name);
    void *p = 0;
    if (f) f(a, b, &p);
    if (out) *(u32 *)out = (u32)(uintptr_t)p;
}
void r2h_glGetPointerv(unsigned int pname, void *out) {
    void (*f)(unsigned int, void **) = (void (*)(unsigned int, void **))hc_sym("glGetPointerv");
    void *p = 0;
    if (f) f(pname, &p);
    if (out) *(u32 *)out = (u32)(uintptr_t)p;
}
void r2h_glGetPointervEXT(unsigned int pname, void *out) { r2h_glGetPointerv(pname, out); }
void r2h_glGetVertexAttribPointerv(unsigned int i, unsigned int pn, void *out) { get_ptr2("glGetVertexAttribPointerv", i, pn, out); }
void r2h_glGetVertexAttribPointervARB(unsigned int i, unsigned int pn, void *out) { get_ptr2("glGetVertexAttribPointervARB", i, pn, out); }
void r2h_glGetVertexAttribPointervNV(unsigned int i, unsigned int pn, void *out) { get_ptr2("glGetVertexAttribPointervNV", i, pn, out); }
void r2h_glGetBufferPointerv(unsigned int t, unsigned int pn, void *out) {
    /* the mapped pointer the 32-bit world knows is our shadow (r2h_glMapBuffer) */
    u32 a = 0;
    pthread_mutex_lock(&g_mapmu);
    for (int i = 0; i < 16; i++) if (g_map[i].addr && g_map[i].target == t) { a = g_map[i].addr; break; }
    pthread_mutex_unlock(&g_mapmu);
    if (a || pn == 0x88BD /* GL_BUFFER_MAP_POINTER */) { if (out) *(u32 *)out = a; return; }
    get_ptr2("glGetBufferPointerv", t, pn, out);
}
void r2h_glGetBufferPointervARB(unsigned int t, unsigned int pn, void *out) { r2h_glGetBufferPointerv(t, pn, out); }

/* gluErrorString / gluGetString: static strings, copied below 4 GB once each */
static u32 low_strcopy(const char *s) {
    if (!s) return 0;
    u32 n = (u32)strlen(s) + 1, a = low_buf(n);
    if (a) memcpy(P32(a), s, n);
    return a;
}
u32 r2h_gluErrorString(u32 e) {
    static const char *(*f)(unsigned int);
    static struct { u32 e, a; } c[32]; static int nc;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (!f) *(void **)&f = hc_sym("gluErrorString");
    if (!f) return 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nc; i++) if (c[i].e == e) { u32 a = c[i].a; pthread_mutex_unlock(&mu); return a; }
    u32 a = low_strcopy(f(e));
    if (nc < 32) { c[nc].e = e; c[nc].a = a; nc++; }
    pthread_mutex_unlock(&mu);
    return a;
}
u32 r2h_gluGetString(u32 name) {
    static const char *(*f)(unsigned int);
    static u32 a[2];
    if (!f) *(void **)&f = hc_sym("gluGetString");
    if (!f) return 0;
    u32 k = name == 100801 /* GLU_EXTENSIONS */ ? 1 : 0;
    if (!a[k]) a[k] = low_strcopy(f(name));
    return a[k];
}

/* ---- legacy display mode dictionaries (CGDisplayCurrentMode & co. crash on this macOS): made
   from CGDisplayMode objects. Keys as in CGDirectDisplay.h of 10.6. */
static void md_num(CFMutableDictionaryRef d, const char *k, CFNumberType t, const void *v) {
    CFStringRef ks = CFStringCreateWithCString(0, k, kCFStringEncodingUTF8);
    CFNumberRef n = CFNumberCreate(0, t, v); CFDictionarySetValue(d, ks, n); CFRelease(n); CFRelease(ks);
}
static CFDictionaryRef mode_dict(CGDisplayModeRef m) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    long w = (long)CGDisplayModeGetWidth(m), h = (long)CGDisplayModeGetHeight(m), bpp = 32, bps = 8, spp = 3;
    long id = (long)CGDisplayModeGetIODisplayModeID(m), rb = w * 4; int fl = (int)CGDisplayModeGetIOFlags(m);
    double hz = CGDisplayModeGetRefreshRate(m);
    md_num(d, "Width", kCFNumberLongType, &w); md_num(d, "Height", kCFNumberLongType, &h);
    md_num(d, "BitsPerPixel", kCFNumberLongType, &bpp); md_num(d, "BitsPerSample", kCFNumberLongType, &bps);
    md_num(d, "SamplesPerPixel", kCFNumberLongType, &spp); md_num(d, "RefreshRate", kCFNumberDoubleType, &hz);
    md_num(d, "Mode", kCFNumberLongType, &id); md_num(d, "IOFlags", kCFNumberIntType, &fl);
    md_num(d, "kCGDisplayBytesPerRow", kCFNumberLongType, &rb);
    CFDictionarySetValue(d, CFSTR("UsableForDesktopGUI"), CGDisplayModeIsUsableForDesktopGUI(m) ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("kCGDisplayModeIsSafeForHardware"), kCFBooleanTrue);
    return d;
}
static CFArrayRef mode_list(u32 disp) {        /* usable desktop modes, one per size */
    return CGDisplayCopyAllDisplayModes(disp, 0);
}
CFDictionaryRef r2h_CGDisplayCurrentMode(u32 disp) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(disp);   /* results are +1: hc_cf_out releases them */
    if (!m) return 0;
    CFDictionaryRef d = mode_dict(m); CGDisplayModeRelease(m);
    return d;
}
CFArrayRef r2h_CGDisplayAvailableModes(u32 disp) {
    CFArrayRef l = mode_list(disp);
    if (!l) return 0;
    CFMutableArrayRef a = CFArrayCreateMutable(0, 0, &kCFTypeArrayCallBacks);
    for (CFIndex i = 0; i < CFArrayGetCount(l); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(l, i);
        if (!CGDisplayModeIsUsableForDesktopGUI(m)) continue;
        CFDictionaryRef d = mode_dict(m); CFArrayAppendValue(a, d); CFRelease(d);
    }
    CFRelease(l);
    return a;
}
static CGDisplayModeRef mode_best(u32 disp, size_t w, size_t h, double hz, int *exact) {
    CFArrayRef l = mode_list(disp); CGDisplayModeRef best = 0; double bs = 1e30;
    if (exact) *exact = 0;
    if (!l) return 0;
    for (CFIndex i = 0; i < CFArrayGetCount(l); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(l, i);
        double mw = (double)CGDisplayModeGetWidth(m), mh = (double)CGDisplayModeGetHeight(m), mz = CGDisplayModeGetRefreshRate(m);
        if (mw < w || mh < h) continue;                 /* at least the size asked for */
        double sc = (mw - w) * (mh - h) + (mw - w) + (mh - h) + (hz > 0 ? fabs(mz - hz) / 1000 : 0);
        if (sc < bs) { bs = sc; best = m; }
    }
    if (best) { CGDisplayModeRetain(best); if (exact) *exact = CGDisplayModeGetWidth(best) == w && CGDisplayModeGetHeight(best) == h; }
    CFRelease(l);
    return best;
}
CFDictionaryRef r2h_CGDisplayBestModeForParametersAndRefreshRate(u32 disp, unsigned long bpp, unsigned long w, unsigned long h, double hz, int *exact) {
    (void)bpp;
    CGDisplayModeRef m = mode_best(disp, w, h, hz, exact);
    if (!m) return r2h_CGDisplayCurrentMode(disp);
    CFDictionaryRef d = mode_dict(m); CGDisplayModeRelease(m);
    return d;
}
CFDictionaryRef r2h_CGDisplayBestModeForParameters(u32 disp, unsigned long bpp, unsigned long w, unsigned long h, int *exact) {
    return r2h_CGDisplayBestModeForParametersAndRefreshRate(disp, bpp, w, h, 0, exact);
}
int r2h_CGDisplaySwitchToMode(u32 disp, CFDictionaryRef d) {
    long id = -1, w = 0, h = 0; CFNumberRef n;
    if (!d) return kCGErrorIllegalArgument;
    if ((n = CFDictionaryGetValue(d, CFSTR("Mode")))) CFNumberGetValue(n, kCFNumberLongType, &id);
    if ((n = CFDictionaryGetValue(d, CFSTR("Width")))) CFNumberGetValue(n, kCFNumberLongType, &w);
    if ((n = CFDictionaryGetValue(d, CFSTR("Height")))) CFNumberGetValue(n, kCFNumberLongType, &h);
    CFArrayRef l = mode_list(disp); int r = kCGErrorIllegalArgument;
    if (!l) return r;
    for (CFIndex i = 0; i < CFArrayGetCount(l); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(l, i);
        if ((long)CGDisplayModeGetIODisplayModeID(m) == id || ((long)CGDisplayModeGetWidth(m) == w && (long)CGDisplayModeGetHeight(m) == h)) {
            r = CGDisplaySetDisplayMode(disp, m, 0);
            rlog("[display] switch to %ldx%ld (mode %ld) -> %d", w, h, id, r);
            break;
        }
    }
    CFRelease(l);
    return r;
}
unsigned long r2h_CGDisplayBitsPerPixel(u32 disp) { (void)disp; return 32; }
unsigned long r2h_CGDisplayBitsPerSample(u32 disp) { (void)disp; return 8; }
unsigned long r2h_CGDisplaySamplesPerPixel(u32 disp) { (void)disp; return 3; }
unsigned long r2h_CGDisplayBytesPerRow(u32 disp) { return CGDisplayPixelsWide(disp) * 4; }

/* ---- sound output: Mojave's i386 AudioToolbox finds no output device on this Mac, so liboah mixes
   into a ring of float stereo frames in 32-bit memory and this Mac's own output unit plays it.
   Ring: u32 w, r (frame counters, written by the 32-bit side / by us), u32 n (frames, a power of
   two), u32 underruns, then n*2 floats. r2_audio_ring(ring, n, rate) -> 0 when playing. */
#include <AudioToolbox/AudioToolbox.h>
typedef struct { volatile u32 w, r; u32 n, under; float d[]; } r2_ring;
static r2_ring *g_ring;
static OSStatus r2_audio_render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts,
                                UInt32 bus, UInt32 nf, AudioBufferList *io) {
    (void)ref; (void)fl; (void)ts; (void)bus;
    float *out = (float *)io->mBuffers[0].mData;
    r2_ring *g = g_ring;
    u32 w = g->w, r = g->r, have = w - r, m = g->n - 1, k = nf < have ? nf : have;
    __sync_synchronize();
    for (u32 i = 0; i < k; i++) { u32 j = ((r + i) & m) * 2; out[2 * i] = g->d[j]; out[2 * i + 1] = g->d[j + 1]; }
    if (k < nf) { memset(out + 2 * k, 0, (nf - k) * 8); if (have) g->under++; }
    __sync_synchronize();
    g->r = r + k;
    return noErr;
}
int r2h_audio_ring(void *ring, u32 n, u32 rate) {
    static AudioUnit au; static int state;
    if (state) return state > 0 ? 0 : -1;
    state = -1;
    if (!ring || !n || (n & (n - 1))) return -1;
    void *h = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY | RTLD_GLOBAL);
    AudioComponent (*fFind)(AudioComponent, const AudioComponentDescription *) = h ? dlsym(h, "AudioComponentFindNext") : 0;
    OSStatus (*fNew)(AudioComponent, AudioComponentInstance *) = h ? dlsym(h, "AudioComponentInstanceNew") : 0;
    OSStatus (*fSet)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32) = h ? dlsym(h, "AudioUnitSetProperty") : 0;
    OSStatus (*fInit)(AudioUnit) = h ? dlsym(h, "AudioUnitInitialize") : 0;
    OSStatus (*fStart)(AudioUnit) = h ? dlsym(h, "AudioOutputUnitStart") : 0;
    if (!fFind || !fNew || !fSet || !fInit || !fStart) { rlog("[snd] host AudioToolbox missing"); return -1; }
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = fFind(0, &d);
    OSStatus e;
    if (!c || (e = fNew(c, &au))) { rlog("[snd] host: no default output unit"); return -1; }
    AudioStreamBasicDescription f = { (Float64)rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                      8, 1, 8, 2, 32, 0 };
    AURenderCallbackStruct cb = { r2_audio_render, 0 };
    g_ring = (r2_ring *)ring;
    if ((e = fSet(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) ||
        (e = fSet(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb)) ||
        (e = fInit(au)) || (e = fStart(au))) { rlog("[snd] host output unit -> %d", (int)e); return -1; }
    state = 1;
    rlog("[snd] host output unit playing a %u-frame ring at %u Hz", n, rate);
    return 0;
}

/* GetKeys in one trap: the 128 key states as a KeyMap (16 bytes, big-endian bit order as the
   PowerPC toolbox has it: key k is bit (k & 7) of byte k >> 3). liboah's CGSGetKeys and its input
   poller asked CGEventSourceKeyState 128 times each: Illustrator calls GetKeys 60 times a second
   when idle, 7700 hostcall traps a second (a quarter of a core, never "settled" for behave). */
int r2h_getkeys(void *out, u32 state) {
    unsigned char b[16] = { 0 };
    for (int k = 0; k < 128; k++) if (CGEventSourceKeyState((CGEventSourceStateID)(int)state, (CGKeyCode)k)) b[k >> 3] |= 1 << (k & 7);
    if (out) memcpy(out, b, 16);
    return 0;
}

/* FSEvents for the 32-bit world. The i386 (Mojave) FSEvents client registers with this Mac's
   fseventsd and never gets an event message; streams run here instead, on a queue of their own,
   and liboah polls their events (records: u32 size, u32 flags, u64 id, path, NUL, padding). */
#include <dispatch/dispatch.h>
typedef struct { void *s; pthread_mutex_t mu; unsigned char *buf; size_t len, cap; int used; } r2fse_t;
#define R2FSE_N 64
static r2fse_t g_r2fse[R2FSE_N];
static pthread_mutex_t g_r2fse_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { long version; void *info; void *retain, *release, *desc; } r2fse_ctx;
static void *(*p_fseCreate)(CFAllocatorRef, void *, r2fse_ctx *, CFArrayRef, uint64_t, double, u32);
static void (*p_fseSetQ)(void *, dispatch_queue_t);
static unsigned char (*p_fseStart)(void *);
static void (*p_fseStop)(void *), (*p_fseInval)(void *), (*p_fseRel)(void *), (*p_fseFlush)(void *);
static int r2fse_syms(void) {
    if (p_fseCreate) return 1;
    void *h = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_LAZY);
    if (!h) return 0;
    *(void **)&p_fseSetQ = dlsym(h, "FSEventStreamSetDispatchQueue"); *(void **)&p_fseStart = dlsym(h, "FSEventStreamStart");
    *(void **)&p_fseStop = dlsym(h, "FSEventStreamStop"); *(void **)&p_fseInval = dlsym(h, "FSEventStreamInvalidate");
    *(void **)&p_fseRel = dlsym(h, "FSEventStreamRelease"); *(void **)&p_fseFlush = dlsym(h, "FSEventStreamFlushSync");
    *(void **)&p_fseCreate = dlsym(h, "FSEventStreamCreate");
    return p_fseCreate && p_fseSetQ && p_fseStart && p_fseStop && p_fseInval && p_fseRel;
}
static void r2fse_cb(const void *s, void *info, size_t n, void *paths, const u32 fl[], const uint64_t ids[]) {
    r2fse_t *f = info; char **p = paths; (void)s;
    pthread_mutex_lock(&f->mu);
    for (size_t i = 0; i < n; i++) {
        size_t pl = strlen(p[i] ? p[i] : ""), rec = 16 + ((pl + 1 + 3) & ~(size_t)3);
        if (f->len + rec > 4u << 20) break;                 /* nobody polls: drop beyond 4 MB */
        if (f->len + rec > f->cap) { size_t c = f->cap ? f->cap * 2 : 65536; while (c < f->len + rec) c *= 2;
                                     unsigned char *nb = realloc(f->buf, c); if (!nb) break; f->buf = nb; f->cap = c; }
        unsigned char *d = f->buf + f->len;
        u32 r = (u32)rec, fv = fl[i]; uint64_t id = ids[i];
        memcpy(d, &r, 4); memcpy(d + 4, &fv, 4); memcpy(d + 8, &id, 8);
        memset(d + 16, 0, rec - 16); memcpy(d + 16, p[i] ? p[i] : "", pl);
        f->len += rec;
    }
    pthread_mutex_unlock(&f->mu);
}
int r2h_fse_start(CFTypeRef paths, long long since, double latency, u32 flags) {
    static dispatch_queue_t q;
    if (!paths || CFGetTypeID(paths) != CFArrayGetTypeID() || !r2fse_syms()) return 0;
    if (!q) q = dispatch_queue_create("com.rosettus.r2fsevents", NULL);
    pthread_mutex_lock(&g_r2fse_mu);
    int k; for (k = 0; k < R2FSE_N && g_r2fse[k].used; k++) ;
    if (k == R2FSE_N) { pthread_mutex_unlock(&g_r2fse_mu); return 0; }
    r2fse_t *f = &g_r2fse[k]; memset(f, 0, sizeof *f); f->used = 1; pthread_mutex_init(&f->mu, 0);
    pthread_mutex_unlock(&g_r2fse_mu);
    r2fse_ctx c = { 0, f, 0, 0, 0 };
    f->s = p_fseCreate(NULL, (void *)r2fse_cb, &c, (CFArrayRef)paths, (uint64_t)since, latency, flags);
    if (f->s) { p_fseSetQ(f->s, q); if (!p_fseStart(f->s)) { p_fseInval(f->s); p_fseRel(f->s); f->s = 0; } }
    if (!f->s) { f->used = 0; return 0; }
    if (g_trace) rlog("[fsevents] host stream %d started (flags %#x, latency %.2f)", k + 1, flags, latency);
    return k + 1;
}
int r2h_fse_poll(u32 h, void *out, u32 cap) {
    if (!h || h > R2FSE_N || !g_r2fse[h - 1].used) return -1;
    r2fse_t *f = &g_r2fse[h - 1];
    if (f->s && p_fseFlush && cap == 0xffffffffu) { p_fseFlush(f->s); return 0; }   /* (flush only) */
    pthread_mutex_lock(&f->mu);
    size_t n = 0;
    while (n + 16 <= f->len) { u32 r; memcpy(&r, f->buf + n, 4); if (n + r > cap) break; n += r; }
    if (n) { memcpy(out, f->buf, n); memmove(f->buf, f->buf + n, f->len - n); f->len -= n; }
    pthread_mutex_unlock(&f->mu);
    return (int)n;
}
int r2h_fse_stop(u32 h) {
    if (!h || h > R2FSE_N || !g_r2fse[h - 1].used) return -1;
    r2fse_t *f = &g_r2fse[h - 1];
    if (f->s) { p_fseStop(f->s); p_fseInval(f->s); p_fseRel(f->s); f->s = 0; }
    pthread_mutex_lock(&f->mu); free(f->buf); f->buf = 0; f->len = f->cap = 0; pthread_mutex_unlock(&f->mu);
    f->used = 0;
    return 0;
}

/* r2_space_join (liboah, OAH_WINDOW_SPACE_LEVEL): put a window of this process into a space of its
   own at an absolute level. Catalina shows the screen saver in a space above every other, whatever
   a window's level: the PowerPC saver host's window goes into a space at level 300 to show over it. */
int r2h_space_join(u32 cid, u32 wid, int level) {
    static uint64_t sp;
    static void *h;
    static uint64_t (*s_create)(int, int, int);
    static void (*s_level)(int, uint64_t, int), (*s_show)(int, CFArrayRef), (*s_add)(int, uint64_t, CFArrayRef, int);
    if (!h) {
        h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
        if (!h) return -1;
        *(void **)&s_create = dlsym(h, "SLSSpaceCreate"); *(void **)&s_level = dlsym(h, "SLSSpaceSetAbsoluteLevel");
        *(void **)&s_show = dlsym(h, "SLSShowSpaces"); *(void **)&s_add = dlsym(h, "SLSSpaceAddWindowsAndRemoveFromSpaces");
    }
    if (!s_create || !s_level || !s_show || !s_add) return -1;
    if (!sp) {
        CFNumberRef n; CFArrayRef a;
        sp = s_create((int)cid, 1, 0);
        if (!sp) return -2;
        s_level((int)cid, sp, level);
        n = CFNumberCreate(0, kCFNumberSInt64Type, &sp);
        a = CFArrayCreate(0, (const void **)&n, 1, &kCFTypeArrayCallBacks);
        s_show((int)cid, a);
        CFRelease(a); CFRelease(n);
    }
    {   int w = (int)wid;
        CFNumberRef n = CFNumberCreate(0, kCFNumberSInt32Type, &w);
        CFArrayRef a = CFArrayCreate(0, (const void **)&n, 1, &kCFTypeArrayCallBacks);
        s_add((int)cid, sp, a, 7);
        CFRelease(a); CFRelease(n);
    }
    return 0;
}

/* ---- r2_dnd_*: files dragged onto the 32-bit world's windows (liboah oah_dnd.h) ----
   A drag finds its destination through CoreDrag: the window server names the connection that owns
   the window under the pointer, and CoreDrag on that side calls the handlers installed for the
   window. The 32-bit world's CoreDrag (Mojave's) cannot reach this Mac's drag server, so it runs
   here: a host thread registers the connection as a CoreDrag client, the handlers accept drags that
   carry files (copy) and queue each drop's paths, and liboah polls the queue and hands the files to
   the app as an 'odoc' Apple Event, as on Mojave. */
#include <limits.h>
typedef int (*hd_track_t)(int msg, u32 wid, void *refcon, void *drag);
typedef int (*hd_recv_t)(u32 wid, void *refcon, void *drag);
static struct {
    int (*reg)(u32, CFArrayRef);
    int (*track)(u32, u32, hd_track_t, void *, void **);
    int (*recv)(u32, u32, hd_recv_t, void *, void **);
    int (*pb)(void *, void **);
    int (*setact)(void *, u32);
    int (*sync)(void *);
    int (*count)(void *, unsigned long *);
    int (*ident)(void *, unsigned long, void **);
    int (*copy)(void *, void *, CFStringRef, CFDataRef *);
} g_hd;
static pthread_mutex_t g_hd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hd_cv = PTHREAD_COND_INITIALIZER;
static u32 g_hd_cid;
static int g_hd_state;                 /* 0 not started, 1 starting, 2 running, -1 failed */
static char *g_hd_q; static size_t g_hd_qlen;   /* queued drops, each "paths\0" (paths newline-separated) */
static char *hd_paths(void *drag) {
    void *pb = 0; unsigned long n = 0; char *out = 0; size_t len = 0;
    if (g_hd.pb(drag, &pb) || !pb) return 0;
    g_hd.sync(pb);
    if (g_hd.count(pb, &n)) return 0;
    for (unsigned long i = 1; i <= n; i++) {
        void *id; CFDataRef d = 0; char path[PATH_MAX];
        if (g_hd.ident(pb, i, &id)) continue;
        if (g_hd.copy(pb, id, CFSTR("public.file-url"), &d) || !d) continue;
        CFURLRef u = CFURLCreateWithBytes(0, CFDataGetBytePtr(d), CFDataGetLength(d), kCFStringEncodingUTF8, 0);
        CFRelease(d);
        if (!u) continue;
        if (CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path)) {
            size_t l = strlen(path);
            char *o = realloc(out, len + l + 2);
            if (o) { out = o; memcpy(out + len, path, l); len += l; out[len++] = '\n'; out[len] = 0; }
        }
        CFRelease(u);
    }
    if (out && len) out[len - 1] = 0;
    return out;
}
static int hd_track(int msg, u32 wid, void *refcon, void *drag) {
    static void *last; static int files;
    (void)wid; (void)refcon;
    if (msg <= 1 || drag != last) { char *p = hd_paths(drag); files = p != 0; last = drag; free(p); }
    if (files && g_hd.setact) g_hd.setact(drag, 1);    /* copy */
    return 0;
}
static int hd_recv(u32 wid, void *refcon, void *drag) {
    (void)wid; (void)refcon;
    char *p = hd_paths(drag);
    if (!p) return -1;                                  /* not ours: the drag slides back */
    size_t l = strlen(p);
    pthread_mutex_lock(&g_hd_mu);
    char *q = realloc(g_hd_q, g_hd_qlen + l + 1);
    if (q) { g_hd_q = q; memcpy(g_hd_q + g_hd_qlen, p, l + 1); g_hd_qlen += l + 1; }
    pthread_mutex_unlock(&g_hd_mu);
    free(p);
    return q ? 0 : -1;
}
static void *hd_thread(void *arg) {
    (void)arg;
    int ok = 0;
    void *h = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices", RTLD_LAZY);
    if (h) {
        *(void **)&g_hd.reg = dlsym(h, "CoreDragRegisterClientInModes");
        *(void **)&g_hd.track = dlsym(h, "CoreDragInstallTrackingHandlerOnConnection");
        *(void **)&g_hd.recv = dlsym(h, "CoreDragInstallReceiveHandlerOnConnection");
        *(void **)&g_hd.pb = dlsym(h, "CoreDragGetPasteboardRef");
        *(void **)&g_hd.setact = dlsym(h, "CoreDragSetDropActions");
        *(void **)&g_hd.sync = dlsym(h, "PasteboardSynchronize");
        *(void **)&g_hd.count = dlsym(h, "PasteboardGetItemCount");
        *(void **)&g_hd.ident = dlsym(h, "PasteboardGetItemIdentifier");
        *(void **)&g_hd.copy = dlsym(h, "PasteboardCopyItemFlavorData");
        ok = g_hd.reg && g_hd.track && g_hd.recv && g_hd.pb && g_hd.sync && g_hd.count && g_hd.ident && g_hd.copy;
    }
    if (ok) {
        CFArrayRef modes = CFArrayCreate(0, (const void **)&kCFRunLoopDefaultMode, 1, &kCFTypeArrayCallBacks);
        ok = g_hd.reg(g_hd_cid, modes) == 0;
        CFRelease(modes);
    }
    pthread_mutex_lock(&g_hd_mu); g_hd_state = ok ? 2 : -1; pthread_cond_broadcast(&g_hd_cv); pthread_mutex_unlock(&g_hd_mu);
    if (!ok) return 0;
    for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1e6, false);   /* CoreDrag's messages arrive here */
    return 0;
}
int r2h_dnd_start(u32 cid) {
    pthread_mutex_lock(&g_hd_mu);
    if (!g_hd_state) {
        pthread_t t; g_hd_cid = cid; g_hd_state = 1;
        if (pthread_create(&t, 0, hd_thread, 0)) g_hd_state = -1; else pthread_detach(t);
    }
    while (g_hd_state == 1) pthread_cond_wait(&g_hd_cv, &g_hd_mu);
    int r = g_hd_state == 2 ? 0 : -1;
    pthread_mutex_unlock(&g_hd_mu);
    return r;
}
int r2h_dnd_window(u32 wid) {
    void *r1 = 0, *r2 = 0;
    if (g_hd_state != 2) return -1;
    int e1 = g_hd.track(g_hd_cid, wid, hd_track, 0, &r1), e2 = g_hd.recv(g_hd_cid, wid, hd_recv, 0, &r2);
    return e1 ? e1 : e2;
}
/* the oldest queued drop's paths into out (size bytes): its length, 0 if none, -1 if out is too small */
int r2h_dnd_poll(void *out, u32 size) {
    int r = 0;
    pthread_mutex_lock(&g_hd_mu);
    if (g_hd_qlen) {
        size_t l = strlen(g_hd_q);
        if (l + 1 > size) r = -1;
        else { memcpy(out, g_hd_q, l + 1); memmove(g_hd_q, g_hd_q + l + 1, g_hd_qlen - l - 1); g_hd_qlen -= l + 1; r = (int)l; }
    }
    pthread_mutex_unlock(&g_hd_mu);
    return r;
}
