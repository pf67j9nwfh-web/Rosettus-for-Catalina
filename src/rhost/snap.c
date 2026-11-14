/* snap.c - R2_SNAP_DIR=dir: every R2_SNAP_SECS (10) seconds, write a PNG of each on-screen window this
   process owns (dir/win-<id>.png). A process may always capture its own windows, so this works without
   the Screen Recording permission that screencapture needs over ssh. Test aid only. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
static void snap_once(const char *dir) {
    /* R2_SNAP_ALL=1: off-screen windows too (e.g. the menu bar images published with SLSSetMenuBars) */
    CFArrayRef l = CGWindowListCopyWindowInfo(getenv("R2_SNAP_ALL") ? kCGWindowListOptionAll : kCGWindowListOptionOnScreenOnly,
                                              kCGNullWindowID);
    if (!l) return;
    int me = getpid();
    for (CFIndex i = 0; i < CFArrayGetCount(l); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(l, i);
        int pid = 0, wid = 0;
        CFNumberRef n = CFDictionaryGetValue(d, kCGWindowOwnerPID); if (n) CFNumberGetValue(n, kCFNumberIntType, &pid);
        if (pid != me) continue;
        n = CFDictionaryGetValue(d, kCGWindowNumber); if (n) CFNumberGetValue(n, kCFNumberIntType, &wid);
        CGImageRef im = CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow, (CGWindowID)wid, kCGWindowImageBoundsIgnoreFraming);
        if (!im) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/win-%d.png", dir, wid);
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)p, (CFIndex)strlen(p), false);
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL(u, CFSTR("public.png"), 1, 0);
        if (dst) { CGImageDestinationAddImage(dst, im, 0); CGImageDestinationFinalize(dst); CFRelease(dst); }
        CFRelease(u); CGImageRelease(im);
    }
    CFRelease(l);
    if (getenv("R2_SNAP_TOP")) {   /* the top of the screen as composited (menu bar); without Screen
                                      Recording only this process's windows and the system's show */
        CGImageRef im = CGWindowListCreateImage(CGRectMake(0, 0, 900, 44), kCGWindowListOptionOnScreenOnly, kCGNullWindowID, 0);
        if (im) {
            char p[1024]; snprintf(p, sizeof p, "%s/screen-top.png", dir);
            CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)p, (CFIndex)strlen(p), false);
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL(u, CFSTR("public.png"), 1, 0);
            if (dst) { CGImageDestinationAddImage(dst, im, 0); CGImageDestinationFinalize(dst); CFRelease(dst); }
            CFRelease(u); CGImageRelease(im);
        }
    }
}
static void *snap_thread(void *a) {
    const char *dir = a; const char *s = getenv("R2_SNAP_SECS"); int secs = s ? atoi(s) : 10;
    if (secs < 1) secs = 10;
    for (;;) { sleep((unsigned)secs); snap_once(dir); }
    return 0;
}
#include <stdint.h>
typedef uint32_t u32;
void rlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* R2_LOWDUMP=secs: every secs seconds, how full the 32-bit space is and what fills it (lowmem.c) */
static void *lowdump_thread(void *a) {
    int secs = (int)(intptr_t)a;
    extern void low_dump_usage_force(void); extern void low_stats(u32 *, u32 *);
    for (;;) {
        sleep((unsigned)secs);
        u32 umb, fmb; low_stats(&umb, &fmb);
        rlog("[vm] 32-bit space: %u MB in use, largest free run %u MB", umb, fmb);
        low_dump_usage_force();
        { extern void low_dump_owners(void); low_dump_owners(); }
    }
    return 0;
}
void snap_init(void) {
    const char *d = getenv("R2_SNAP_DIR"), *ld = getenv("R2_LOWDUMP");
    pthread_t t;
    if (d && *d) pthread_create(&t, 0, snap_thread, (void *)d);
    if (ld && atoi(ld) > 0) pthread_create(&t, 0, lowdump_thread, (void *)(intptr_t)atoi(ld));
}
