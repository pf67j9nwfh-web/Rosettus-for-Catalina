/* r2test: i386 test of the hostcall bridge (window server through the host) */
#include <stdio.h>
#include <unistd.h>
#include "r2native.h"
unsigned int CGMainDisplayID(void); r2_rect32 CGDisplayBounds(unsigned int);
int CGSMainConnectionID(void); int CGSNewRegionWithRect(r2_rect32 *, void **);
int CGSNewWindow(unsigned int, int, float, float, void *, unsigned int *);
int CGSOrderWindow(unsigned int, unsigned int, int, unsigned int);
int CGSSetWindowLevel(unsigned int, unsigned int, int);
void *CGWindowContextCreate(unsigned int, unsigned int, CFTypeRef);
void CGContextSetRGBFillColor(void *, float, float, float, float);
void CGContextFillRect(void *, r2_rect32); void CGContextFlush(void *);
int CGSSetWindowTitle(unsigned int, unsigned int, CFTypeRef);
CFTypeRef CGSessionCopyCurrentDictionary(void);
CFTypeRef CGWindowListCopyWindowInfo(unsigned int, unsigned int);
int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("R2_HOST %d\n", r2_available());
    unsigned d = CGMainDisplayID(); r2_rect32 b = CGDisplayBounds(d);
    printf("display %u %.0fx%.0f\n", d, b.w, b.h);
    CFTypeRef sd = CGSessionCopyCurrentDictionary();
    printf("session dictionary %p count %ld\n", sd, sd ? (long)CFDictionaryGetCount(sd) : -1L);
    int cid = CGSMainConnectionID(); printf("connection %d\n", cid);
    r2_rect32 r = { 0, 0, 300, 200 }; void *rgn = 0;
    int e = CGSNewRegionWithRect(&r, &rgn); unsigned wid = 0;
    int e2 = CGSNewWindow(cid, 2, 200, 200, rgn, &wid);
    printf("region %d %p window %d %u\n", e, rgn, e2, wid);
    CGSSetWindowLevel(cid, wid, 3);
    CGSSetWindowTitle(cid, wid, CFSTR("Rosettus 2 bridge test"));
    printf("order %d\n", CGSOrderWindow(cid, wid, 1, 0));
    void *ctx = CGWindowContextCreate(cid, wid, 0);
    printf("context %p\n", ctx);
    for (int i = 0; i < 30; i++) {
        CGContextSetRGBFillColor(ctx, (i % 3) == 0, (i % 3) == 1, (i % 3) == 2, 1);
        CGContextFillRect(ctx, r); CGContextFlush(ctx);
        usleep(100000);
        if (i == 10) {
            CFTypeRef a = CGWindowListCopyWindowInfo(8, wid);
            if (a) { CFShow(a); CFRelease(a); } else printf("no window info\n");
            CFTypeRef o = CGWindowListCopyWindowInfo(1 | 16, 0);
            printf("onscreen windows %ld\n", o ? (long)CFArrayGetCount(o) : -1L);
            if (o) CFRelease(o);
        }
    }
    printf("done\n");
    return 0;
}
