/* cfnet.c - CFNetwork stress for the PowerPC world: N asynchronous HTTP GETs (CFReadStream scheduled
   on the main run loop, as Sparkle / WebKit / NSURLConnection do), one after another, each with a
   time limit. Prints one line per request and a summary. A lost stream callback shows as a
   request that times out with bytes still owed.
   build (Snow Leopard, PowerPC compiler): gcc -arch ppc -O1 -mmacosx-version-min=10.4 -o cfnet cfnet.c \
       -framework CoreServices -framework CoreFoundation
   run: rosettus ./cfnet [count] [url ...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

static long g_bytes; static int g_done, g_status, g_err, g_events;
static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }

static void cb(CFReadStreamRef rs, CFStreamEventType ev, void *info) {
    UInt8 buf[8192]; CFIndex n;
    (void)info; g_events++;
    switch (ev) {
    case kCFStreamEventHasBytesAvailable:
        while (CFReadStreamHasBytesAvailable(rs) && (n = CFReadStreamRead(rs, buf, sizeof buf)) > 0) g_bytes += n;
        break;
    case kCFStreamEventEndEncountered: {
        CFHTTPMessageRef resp = (CFHTTPMessageRef)CFReadStreamCopyProperty(rs, kCFStreamPropertyHTTPResponseHeader);
        if (resp) { g_status = (int)CFHTTPMessageGetResponseStatusCode(resp); CFRelease(resp); }
        g_done = 1; CFRunLoopStop(CFRunLoopGetCurrent()); break; }
    case kCFStreamEventErrorOccurred: g_err = 1; g_done = 1; CFRunLoopStop(CFRunLoopGetCurrent()); break;
    default: break;
    }
}

static int fetch(const char *url, double limit) {
    if (getenv("CFNET_DEBUG")) { double a = now(), b = now(), c = now(); printf("debug start %.3f %.3f %.3f\n", a, b, c); }
    CFStringRef s = CFStringCreateWithCString(0, url, kCFStringEncodingUTF8);
    CFURLRef u = CFURLCreateWithString(0, s, 0);
    CFHTTPMessageRef rq = CFHTTPMessageCreateRequest(0, CFSTR("GET"), u, kCFHTTPVersion1_1);
    CFReadStreamRef rs = CFReadStreamCreateForHTTPRequest(0, rq);
    CFStreamClientContext ctx = { 0, 0, 0, 0, 0 };
    static volatile double t0; double t; t0 = now();   /* in memory: stock Rosetta loses f30/f31 across CFNetwork host lookups (fphost.c) */
    int dbg = getenv("CFNET_DEBUG") != 0;
#define DBG(tag) if (dbg) printf("debug %-8s t0 %.3f\n", tag, t0)
    DBG("init");
    g_bytes = 0; g_done = g_status = g_err = g_events = 0;
    CFReadStreamSetProperty(rs, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue);
    DBG("setprop");
    CFReadStreamSetClient(rs, kCFStreamEventHasBytesAvailable | kCFStreamEventEndEncountered | kCFStreamEventErrorOccurred, cb, &ctx);
    DBG("client");
    CFReadStreamScheduleWithRunLoop(rs, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
    DBG("sched");
    if (!CFReadStreamOpen(rs)) g_err = 2;
    DBG("open");
    if (getenv("CFNET_DEBUG")) { struct timeval a, b; int ra = gettimeofday(&a, 0), rb = gettimeofday(&b, 0);
        printf("debug t0 %.3f now %.3f gtod %d %ld.%06d, %d %ld.%06d\n", t0, now(), ra, (long)a.tv_sec, (int)a.tv_usec, rb, (long)b.tv_sec, (int)b.tv_usec); }
    while (!g_done && !g_err && now() - t0 < limit) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
    t = now() - t0;
    printf("%-55s %s status %d, %ld bytes, %d callbacks, %.2f s\n", url,
           g_done && !g_err ? "ok     " : g_err ? "ERROR  " : "TIMEOUT", g_status, g_bytes, g_events, t);
    fflush(stdout);
    CFReadStreamSetClient(rs, 0, 0, 0);
    CFReadStreamUnscheduleFromRunLoop(rs, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
    CFReadStreamClose(rs);
    CFRelease(rs); CFRelease(rq); CFRelease(u); CFRelease(s);
    return g_done && !g_err && g_status >= 200 && g_status < 400 ? 0 : 1;
}

int main(int argc, char **argv) {
    static const char *def[] = { "http://update.mactracker.ca/appcast.xml", "http://update.mactracker.ca/changelogs/mactracker651.html",
                                 "http://example.com/", 0 };
    int n = argc > 1 ? atoi(argv[1]) : 10, i, k, bad = 0, tot = 0;
    const char **urls = argc > 2 ? (const char **)argv + 2 : def;
    for (i = 0; i < n; i++)
        for (k = 0; urls[k]; k++) { bad += fetch(urls[k], 20.0); tot++; }
    printf("RESULT cfnet %s %d of %d requests failed\n", bad ? "FAIL" : "PASS", bad, tot);
    return bad != 0;
}
