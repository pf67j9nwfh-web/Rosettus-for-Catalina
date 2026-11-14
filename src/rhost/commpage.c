/* commpage.c - the 32-bit commpage (0xfffec000-0xfffff000).
   A 64-bit process gets no 32-bit commpage, but Snow Leopard's i386 libSystem calls code there
   (atomics, spinlocks, bcopy, nanotime, gettimeofday) and reads data from it. The pages are a dump
   taken from a Snow Leopard Mac (commpage32.bin); the time data is kept current from this Mac's
   64-bit commpage, whose time fields sit at the same offsets. */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>

#define CP32 0xffff0000u
#define CP64 ((volatile unsigned char *)0x00007fffffe00000ull)
/* offsets in both commpages */
#define O_CAPS      0x020   /* u32 cpu capabilities (number of CPUs in bits 16-23) */
#define O_NT_BASE   0x050   /* nanotime: tsc_base u64, scale u32, shift u32, ns_base u64, generation u32 */
#define O_NT_GEN    0x068
#define O_GTOD_GEN  0x06c   /* gettimeofday: generation u32, ns_base u64, sec_base u64 */
#define O_GTOD_END  0x080

static void copy_time(void) {
    volatile unsigned char *d = (volatile unsigned char *)P32(CP32);
    unsigned char tmp[0x18];
    for (int tries = 0; tries < 100; tries++) {                 /* nanotime block */
        uint32_t g = *(volatile uint32_t *)(CP64 + O_NT_GEN);
        if (!g) { usleep(10); continue; }
        memcpy(tmp, (const void *)(CP64 + O_NT_BASE), 0x18);
        if (*(volatile uint32_t *)(CP64 + O_NT_GEN) != g) continue;
        if (*(volatile uint32_t *)(d + O_NT_GEN) == g && !memcmp(tmp, (const void *)(d + O_NT_BASE), 0x18)) break;
        *(volatile uint32_t *)(d + O_NT_GEN) = 0;
        __sync_synchronize();
        memcpy((void *)(d + O_NT_BASE), tmp, 0x18);
        __sync_synchronize();
        *(volatile uint32_t *)(d + O_NT_GEN) = g;
        break;
    }
    unsigned char gt[0x10];
    for (int tries = 0; tries < 100; tries++) {                 /* gettimeofday block */
        uint32_t g = *(volatile uint32_t *)(CP64 + O_GTOD_GEN);
        /* 0: no valid data. Catalina's kernel leaves this block at 0 for good (it keeps its
           gettimeofday data elsewhere), and waiting for it here was 100 sleeps every 20 ms in
           every translated process (7% of this thread's time in samples). The guest's block
           stays at generation 0 too: its gettimeofday() makes the system call. */
        if (!g) break;
        memcpy(gt, (const void *)(CP64 + O_GTOD_GEN + 4), 0x10);
        if (*(volatile uint32_t *)(CP64 + O_GTOD_GEN) != g) continue;
        if (*(volatile uint32_t *)(d + O_GTOD_GEN) == g && !memcmp(gt, (const void *)(d + O_GTOD_GEN + 4), 0x10)) break;
        *(volatile uint32_t *)(d + O_GTOD_GEN) = 0;
        __sync_synchronize();
        memcpy((void *)(d + O_GTOD_GEN + 4), gt, 0x10);
        __sync_synchronize();
        *(volatile uint32_t *)(d + O_GTOD_GEN) = g;
        break;
    }
}

/* A 10.7+ i386 libSystem (the Mojave one) reads more of the commpage than Snow Leopard's: CPU
   counts, page shifts, memory size, timebase, approximate time, the newer gettimeofday data. Those
   offsets are the same in the 64-bit commpage: copy everything but the two time blocks above
   (and the code pages, which only Snow Leopard's libSystem calls). */
int g_cp_modern;
static void copy_rest(void) {
    unsigned char *d = (unsigned char *)P32(CP32);
    memcpy(d + 0x10, (const void *)(CP64 + 0x10), 0x44 - 0x10);                  /* (not 0x44: below) */
    memcpy(d + 0x48, (const void *)(CP64 + 0x48), O_NT_BASE - 0x48);
    memcpy(d + O_GTOD_END, (const void *)(CP64 + O_GTOD_END), 0x1000 - O_GTOD_END);
    /* kdebug tracing is off for the 32-bit world: with tracing on (fs_usage, Instruments, spindump)
       the i386 kdebug_is_enabled() reads the kernel's type filter, a 64-bit mapping it got as a
       truncated 32-bit address: SIGBUS in libsystem_kernel (Photoshop at quit, its settings file
       written as 0 bytes; TextWrangler). */
    *(volatile uint32_t *)(d + 0x44) = 0;
}

void commpage_modern(void) { copy_rest(); }

static void *time_thread(void *a) {
    (void)a;
    sigset_t all; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, 0);   /* signals are for the 32-bit threads */
    for (;;) { copy_time(); if (g_cp_modern) copy_rest(); usleep(20000); }
    return 0;
}

/* The PowerPC commpage, as Snow Leopard's kernel provided it to the translator (which moves the
   guest's 0xffff8000.. 0x200000 down): the same pages Rosettus 1's shim maps on Mojave (cp_data.h). */
#include "cp_data.h"
void rosetta_commpage_init(void) {
    static const struct { u32 addr; const unsigned char *data; } pg[] = {
        { 0xfffef000u, cp_fef }, { 0xffdef000u, cp_fef }, { 0xffdf3000u, cp_3000 },
        { 0xffdf8000u, cp_8000 }, { 0xffdf9000u, cp_9000 },
    };
    for (unsigned i = 0; i < sizeof pg / sizeof *pg; i++) {
        u32 a = pg[i].addr;
        if (a < R32_TOP) {
            if (!low_is_free(a, R32_PAGE)) { rlog("[commpage] %#x is taken", a); continue; }
            low_mark(a, R32_PAGE, 1);
        }
        if (mmap(P32(a), R32_PAGE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED)
            rfatal("cannot map the PowerPC commpage page %#x", a);
        memcpy(P32(a), pg[i].data, R32_PAGE);
    }
    if (g_trace) rlog("[commpage] PowerPC commpage pages mapped");
}

void commpage_init(const char *dumpfile) {
    FILE *f = fopen(dumpfile, "rb");
    if (!f) rfatal("cannot open the commpage dump %s", dumpfile);
    if (mmap(P32(R32_TOP), 0xfffff000u - R32_TOP, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED)
        rfatal("cannot map the 32-bit commpage area");
    u32 hdr[2]; int pages = 0;
    static unsigned char buf[4096];
    while (fread(hdr, 4, 2, f) == 2 && hdr[1] == 4096 && fread(buf, 1, 4096, f) == 4096) {
        if (hdr[0] >= R32_TOP && hdr[0] < 0xfffff000u) { memcpy(P32(hdr[0]), buf, 4096); pages++; }
    }
    fclose(f);
    if (!pages) rfatal("commpage dump %s is empty", dumpfile);
    *(volatile uint32_t *)P32(CP32 + O_CAPS) = *(volatile uint32_t *)(CP64 + O_CAPS);
    *(volatile uint32_t *)P32(CP32 + O_NT_GEN) = 0;
    *(volatile uint32_t *)P32(CP32 + O_GTOD_GEN) = 0;
    copy_time();
    pthread_t t; pthread_create(&t, 0, time_thread, 0);
    if (g_trace) rlog("[commpage] %d pages, caps %#x, nt gen %u", pages, *(uint32_t *)P32(CP32 + O_CAPS), *(uint32_t *)P32(CP32 + O_NT_GEN));
}
