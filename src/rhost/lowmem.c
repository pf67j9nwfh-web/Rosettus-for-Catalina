/* lowmem.c - the 32-bit world's address space (below 4 GB).
   The whole range is reserved at exec by a zero-fill segment (reserve.s, linked at 0x1000), so the
   64-bit side (dyld, malloc, frameworks) never puts anything there. This bitmap says which pages
   are handed out to the 32-bit world; free pages are kept mapped PROT_NONE so the range stays ours. */
#include "rhost.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#define NPAGES (1u << 20)
static uint8_t g_map[NPAGES / 8];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_hint = 0x00100000u >> 12;

static inline int used(u32 pg) { return g_map[pg >> 3] & (1 << (pg & 7)); }
static u32 g_first_free = 0x00100000u >> 12;    /* no free page below this (above 1 MB) */
static inline int full64(u32 pg) {             /* pages pg..pg+63 all used (pg a multiple of 64) */
    uint64_t w; memcpy(&w, &g_map[pg >> 3], 8); return w == ~0ull;
}
static u32 next_free(u32 f) {                   /* the first free page at or above f (g_lock held) */
    while (f < (R32_TOP >> 12)) {
        if (!(f & 63) && full64(f)) { f += 64; continue; }
        if (!(f & 7) && g_map[f >> 3] == 0xff) { f += 8; continue; }
        if (!used(f)) break;
        f++;
    }
    return f;
}
static u32 g_nused;                             /* pages handed out (low_init's fixed ones included) */
static inline void setu(u32 pg, int u) { uint8_t *b = &g_map[pg >> 3], m = (uint8_t)(1 << (pg & 7));
    if (u) { if (!(*b & m)) { *b |= m; g_nused++; } } else if (*b & m) { *b &= (uint8_t)~m; g_nused--; } }

/* The reservation segment comes in read/write and zero: every page is free for the 32-bit world;
   free pages are made inaccessible. */
void low_init(void) {
    memset(g_map, 0, sizeof g_map);
    for (u32 pg = 0; pg < (R32_LOW >> 12); pg++) setu(pg, 1);
    for (u32 pg = R32_TOP >> 12; pg < NPAGES; pg++) setu(pg, 1);
    mach_vm_address_t a = R32_LOW;          /* the kernel reports the segment in pieces: they must cover it all */
    while (a < R32_TOP) {
        mach_vm_address_t r = a; mach_vm_size_t sz = 0; vm_region_basic_info_data_64_t bi;
        mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on;
        if (mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &c, &on) || r != a)
            rfatal("the 32-bit reservation is missing at %#llx: rhost must be built with build.sh", a);
        a = r + sz;
    }
    if (mprotect(P32(R32_LOW), R32_TOP - R32_LOW, PROT_NONE)) rfatal("cannot protect the 32-bit reservation");
}

/* R2_LOWDUMP: who holds the pages - each page remembers the code that took it (two return
   addresses), and the dump lists the callers holding the most (rhost addresses: atos -o rhost) */
#define OWN_N 1024
static uint16_t *g_own;                       /* page -> owner slot + 1 (0: none / not tracked) */
static struct { uintptr_t ra0, ra1; int64_t pages; } g_owners[OWN_N];
static int g_own_on = -1;
static void own_set(u32 a, u32 e, uintptr_t ra0, uintptr_t ra1) {      /* g_lock held */
    if (g_own_on < 0) { g_own_on = getenv("R2_LOWDUMP") ? 1 : 0; if (g_own_on) g_own = calloc(NPAGES, sizeof *g_own); if (!g_own) g_own_on = 0; }
    if (!g_own_on) return;
    unsigned h = (unsigned)((ra0 ^ (ra1 << 7)) * 2654435761u) % OWN_N, i;
    for (i = 0; i < OWN_N && g_owners[h].ra0 && !(g_owners[h].ra0 == ra0 && g_owners[h].ra1 == ra1); i++) h = (h + 1) % OWN_N;
    if (i == OWN_N) return;
    g_owners[h].ra0 = ra0; g_owners[h].ra1 = ra1;
    for (u32 pg = a; pg < e && pg < NPAGES; pg++) {
        if (g_own[pg]) g_owners[g_own[pg] - 1].pages--;
        g_own[pg] = (uint16_t)(h + 1); g_owners[h].pages++;
    }
}
static void own_clear(u32 a, u32 e) {         /* g_lock held */
    if (g_own_on <= 0) return;
    for (u32 pg = a; pg < e && pg < NPAGES; pg++) if (g_own[pg]) { g_owners[g_own[pg] - 1].pages--; g_own[pg] = 0; }
}
void low_dump_owners(void) {
    if (g_own_on <= 0) return;
    pthread_mutex_lock(&g_lock);
    int done[OWN_N] = { 0 };
    for (int k = 0; k < 12; k++) {
        int b = -1;
        for (int i = 0; i < OWN_N; i++) if (!done[i] && g_owners[i].ra0 && (b < 0 || g_owners[i].pages > g_owners[b].pages)) b = i;
        if (b < 0 || g_owners[b].pages <= 0) break;
        done[b] = 1;
        rlog("[vm]   held %7.1f MB by %#lx <- %#lx", g_owners[b].pages / 256.0, (unsigned long)g_owners[b].ra0, (unsigned long)g_owners[b].ra1);
    }
    pthread_mutex_unlock(&g_lock);
}

static int low_mark_at(u32 addr, u32 size, int u, uintptr_t ra0, uintptr_t ra1) {
    u32 a = addr >> 12, e = (u32)(((uint64_t)addr + size + R32_PAGE - 1) >> 12);
    pthread_mutex_lock(&g_lock);
    for (u32 pg = a; pg < e && pg < NPAGES; pg++) setu(pg, u);
    if (u) own_set(a, e, ra0, ra1); else { own_clear(a, e); if (a < g_first_free && a >= (0x00100000u >> 12)) g_first_free = a; }
    pthread_mutex_unlock(&g_lock);
    return 0;
}
int __attribute__((noinline)) low_mark(u32 addr, u32 size, int u) {
    return low_mark_at(addr, size, u, (uintptr_t)__builtin_return_address(0), (uintptr_t)__builtin_return_address(1));
}

int low_is_free(u32 addr, u32 size) {
    u32 a = addr >> 12, e = (u32)(((uint64_t)addr + size + R32_PAGE - 1) >> 12);
    int ok = 1;
    pthread_mutex_lock(&g_lock);
    for (u32 pg = a; pg < e; pg++) if (pg >= NPAGES || used(pg)) { ok = 0; break; }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* Once per process, when the 32-bit space runs out: what fills it, by the kernel's view of the
   regions below 4 GB - mapped files (by name) and anonymous memory (by VM tag), largest first. */
#include <libproc.h>
#include <stdio.h>
#include <unistd.h>
static void low_dump(void);
void low_dump_usage(void) { static int done; if (!done++) low_dump(); }
void low_dump_usage_force(void) { low_dump(); }
static void low_dump(void) {
    enum { N = 400 };
    static struct { char name[200]; uint64_t sz; int n; } e[N];
    int ne = 0;
    mach_vm_address_t a = R32_LOW;
    while (a < R32_TOP) {
        mach_vm_address_t r = a; mach_vm_size_t sz = 0; vm_region_extended_info_data_t xi;
        mach_msg_type_number_t c = VM_REGION_EXTENDED_INFO_COUNT; mach_port_t on = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&xi, &c, &on)) break;
        if (on) mach_port_deallocate(mach_task_self(), on);
        if (r >= R32_TOP) break;
        uint64_t end = r + sz > R32_TOP ? R32_TOP : r + sz;
        char nm[200] = "";
        if (xi.protection == 0 && xi.pages_resident == 0 && xi.share_mode == SM_EMPTY) nm[0] = 0;   /* free reservation */
        if (proc_regionfilename(getpid(), r, nm, sizeof nm) <= 0) {
            if (xi.protection == 0) snprintf(nm, sizeof nm, "(no access: reserved / free)");
            else snprintf(nm, sizeof nm, "(anonymous, tag %u)", xi.user_tag);
        }
        int i;
        for (i = 0; i < ne && strcmp(e[i].name, nm); i++) ;
        if (i == ne && ne < N) { strlcpy(e[ne].name, nm, sizeof e[ne].name); e[ne].sz = 0; e[ne].n = 0; ne++; }
        if (i < ne) { e[i].sz += end - r; e[i].n++; }
        a = end;
    }
    {   /* and the largest stretches handed out to the 32-bit world (the bitmap), with their addresses */
        u32 rs[12], rl[12]; int nr = 0;
        memset(rl, 0, sizeof rl);
        pthread_mutex_lock(&g_lock);
        for (u32 pg = R32_LOW >> 12; pg < (R32_TOP >> 12); ) {
            if (!used(pg)) { pg++; continue; }
            u32 s = pg; while (pg < (R32_TOP >> 12) && used(pg)) pg++;
            u32 l = pg - s; int k = 0;
            for (k = 0; k < 12 && rl[k] >= l; k++) ;
            if (k < 12) { for (int j = 11; j > k; j--) { rs[j] = rs[j - 1]; rl[j] = rl[j - 1]; } rs[k] = s; rl[k] = l; if (nr < 12) nr++; }
        }
        pthread_mutex_unlock(&g_lock);
        for (int k = 0; k < nr; k++) rlog("[vm]   used %08x-%08x %7.1f MB", rs[k] << 12, (rs[k] + rl[k]) << 12, rl[k] / 256.0);
    }
    for (int k = 0; k < 25; k++) {          /* the largest 25 */
        int best = -1;
        for (int i = 0; i < ne; i++) if (e[i].n && (best < 0 || e[i].sz > e[best].sz)) best = i;
        if (best < 0) break;
        rlog("[vm]   %7.1f MB in %4d region(s): %s", e[best].sz / 1048576.0, e[best].n, e[best].name);
        e[best].n = 0;
    }
}

/* for a failed allocation's log line: pages in use and the largest free run (MB) */
void low_stats(u32 *used_mb, u32 *largest_free_mb) {
    u32 u = 0, run = 0, best = 0;
    pthread_mutex_lock(&g_lock);
    for (u32 pg = R32_LOW >> 12; pg < (R32_TOP >> 12); pg++) {
        if (used(pg)) { u++; run = 0; } else if (++run > best) best = run;
    }
    pthread_mutex_unlock(&g_lock);
    *used_mb = u >> 8; *largest_free_mb = best >> 8;
}

/* the free runs of the 32-bit space by size (once, when it runs out): fragmentation or full? */
void low_dump_holes(void) {
    static int done; if (done++) return;
    u32 cnt[8] = { 0 }, pages[8] = { 0 }, run = 0, start = 0;
    static const char *nm[8] = { "<64K", "<256K", "<1M", "<4M", "<16M", "<64M", "<256M", ">=256M" };
    char big[600]; int bo = 0; big[0] = 0;
    pthread_mutex_lock(&g_lock);
    for (u32 pg = R32_LOW >> 12; pg <= (R32_TOP >> 12); pg++) {
        if (pg < (R32_TOP >> 12) && !used(pg)) { if (!run) start = pg; run++; continue; }
        if (run) {
            u32 kb = run * 4, b = kb < 64 ? 0 : kb < 256 ? 1 : kb < 1024 ? 2 : kb < 4096 ? 3 : kb < 16384 ? 4 : kb < 65536 ? 5 : kb < 262144 ? 6 : 7;
            cnt[b]++; pages[b] += run;
            if (kb >= 4096 && bo < (int)sizeof big - 40) bo += snprintf(big + bo, sizeof big - bo, " %x+%uM", start << 12, kb >> 10);
            run = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
    for (int b = 0; b < 8; b++) if (cnt[b]) rlog("[vm]   free runs %-6s %5u, %7.1f MB", nm[b], cnt[b], pages[b] / 256.0);
    if (big[0]) rlog("[vm]   free runs >= 4 MB:%s", big);
}

/* size in bytes; align_mask e.g. 0xfff; returns the address, pages marked used (memory still PROT_NONE) */
static int nextfit = -1, g_bignext = -1;
static u32 g_ff_pages = 0xffffffffu;           /* large requests first fit once this many pages are used */
/* Default 0: first fit for every request from the start, as the kernel's vm_map does on Mojave and
   on real Macs. Next fit for large requests walked its hint upward, so after the launch the guest
   heap (malloc's large / small regions) sat above 2 GB, where PowerPC code that does pointer
   arithmetic as signed numbers goes wrong: Kid Pix Deluxe 4's Flash (sticker) renderer drew nothing
   into its GWorlds at 0xb1xxxxxx (fine at 0x0axxxxxx on Mojave). R2_LOW_FFMB=MB: next fit for large
   requests until that much is in use (the old default was 1536). */
u32 __attribute__((noinline)) low_alloc(u32 size, u32 align_mask, int top_down) {
    if (nextfit < 0) nextfit = getenv("R2_LOW_NEXTFIT") ? 1 : 0;
    if (g_bignext < 0) { const char *e = getenv("R2_LOW_FFMB"); g_bignext = getenv("R2_LOW_BIGNEXT") ? 1 : 0;
        g_ff_pages = (u32)(e ? atoi(e) : 0) << 8; }
    u32 n = (size + R32_PAGE - 1) >> 12, step = ((align_mask | 0xfff) + 1) >> 12;
    if (!n) n = 1;
    if (!step) step = 1;
    pthread_mutex_lock(&g_lock);
    u32 found = 0;
    if (top_down) {
        /* a used page at pg+k rules out every start above pg+k-n: jump there (stepping one page
           at a time with a fresh check each time made an mmap of a crowded top ~1 ms) */
        int64_t pg = (int64_t)(R32_TOP >> 12) - n;
        pg -= pg % step;
        while (pg >= (R32_LOW >> 12)) {
            int64_t k = (int64_t)n - 1;
            while (k >= 0 && !used((u32)(pg + k))) k--;     /* the highest used page in the window */
            if (k < 0) { found = (u32)pg; break; }
            pg = pg + k - n;                                 /* the window just below that page */
            if (pg < 0) break;
            pg -= pg % step;
        }
    } else if (step == 1 && !nextfit && !g_bignext && (n <= 16 || g_nused >= g_ff_pages)) {
        /* first fit for every size, a word (64 pages) at a time. Next fit for large requests (below)
           left the space in pieces: InDesign CS2 had 525 MB free, none of it 26 MB in one run, when
           its Save needed that ("translate fatal"); Mojave's kernel (first fit) ran the same session
           at 3.6 GB in use. R2_LOW_BIGNEXT=1: the old next fit for large requests. */
        u32 run = 0, start = 0, top = R32_TOP >> 12;
        for (u32 pg = g_first_free; pg < top; ) {
            if (!(pg & 63) && pg + 64 <= top) {
                uint64_t w; memcpy(&w, &g_map[pg >> 3], 8);    /* bit i: page pg+i used */
                if (w == ~0ull) { run = 0; pg += 64; continue; }
                if (w == 0) { if (!run) start = pg; run += 64; if (run >= n) { found = start; break; } pg += 64; continue; }
                if (n > 64) {   /* only the word's free head (joins the run) and free tail (starts one) can count */
                    u32 head = (u32)__builtin_ctzll(w), tail = (u32)__builtin_clzll(w);
                    if (run && run + head >= n) { found = start; break; }
                    run = tail; start = pg + 64 - tail; pg += 64; continue;
                }
            }
            if (used(pg)) run = 0; else { if (!run) start = pg; if (++run >= n) { found = start; break; } }
            pg++;
        }
    } else {
        /* first fit from the lowest free page (as the kernel's vm_map does), not next fit from the
           last allocation: next fit spread short-lived pages all over the free space, and after a
           while 850 MB were free in 5000 pieces with none of the 26 MB the translator's allocator
           wanted (Illustrator: "translate fatal"). R2_LOW_NEXTFIT=1: next fit as before. */
        for (int pass = 0; pass < 2 && !found; pass++) {
            /* small requests (up to 64 KB: the bulk, and the short-lived ones) first fit; larger ones
               next fit from their own hint, which kept the launch as fast as before (a first-fit scan
               for a large request walked past every small hole: TextWrangler 7 s -> 11 s) */
            u32 start = pass ? (R32_LOW >> 12) : (nextfit || (n > 16 && g_nused < g_ff_pages)) ? g_hint : g_first_free;
            for (u32 pg = (start + step - 1) / step * step; pg + n <= (R32_TOP >> 12); pg += step) {
                if (!(pg & 63) && step <= 64 && full64(pg)) { pg += 64 - step; continue; }          /* 64 used pages */
                if (!(pg & 7) && step <= 8 && g_map[pg >> 3] == 0xff) { pg += 8 - step; continue; }   /* a full byte */
                u32 k = 0;
                while (k < n && !used(pg + k)) k++;
                if (k == n) { found = pg; break; }
                pg += (k / step) * step;
            }
        }
    }
    if (found) { for (u32 k = 0; k < n; k++) setu(found + k, 1); if (!top_down && (nextfit || n > 16)) g_hint = found + n;
                 if (!top_down && found <= g_first_free) g_first_free = next_free(found + n);
                 own_set(found, found + n, (uintptr_t)__builtin_return_address(0), (uintptr_t)__builtin_return_address(1)); }
    pthread_mutex_unlock(&g_lock);
    return found << 12;
}

/* the first run of used pages at or above addr: [*start, *end); 0 if none below R32_TOP */
int low_next_used(u32 addr, u32 *start, u32 *end) {
    u32 pg = addr >> 12;
    pthread_mutex_lock(&g_lock);
    const u32 top = R32_TOP >> 12;
    /* a byte (8 pages) or a word (64 pages) at a time where the map allows: vm_region from the
       translator came here some 10000 times a second during an iWork launch, and a page at a
       time over hundreds of MB of used or free pages took seconds of it */
    while (pg < top) {
        if (!(pg & 63) && pg + 64 <= top) { uint64_t w; memcpy(&w, &g_map[pg >> 3], 8); if (!w) { pg += 64; continue; } }
        if (!(pg & 7) && pg + 8 <= top && !g_map[pg >> 3]) { pg += 8; continue; }
        if (used(pg)) break;
        pg++;
    }
    if (pg >= top) { pthread_mutex_unlock(&g_lock); return 0; }
    u32 e = pg;
    while (e < top) {
        if (!(e & 63) && e + 64 <= top && full64(e)) { e += 64; continue; }
        if (!(e & 7) && e + 8 <= top && g_map[e >> 3] == 0xff) { e += 8; continue; }
        if (!used(e)) break;
        e++;
    }
    pthread_mutex_unlock(&g_lock);
    *start = pg << 12; *end = e << 12;
    return 1;
}

/* R2_MMAPLOG=LO-HI (hex): does [a, e) touch the range being watched? */
int mmaplog_hit(uint64_t a, uint64_t e) {
    static int ml = -1; static u32 mlo, mhi;
    if (ml < 0) { const char *v = getenv("R2_MMAPLOG"); if (v) { mlo = (u32)strtoul(v, 0, 16); const char *d = strchr(v, '-'); mhi = d ? (u32)strtoul(d + 1, 0, 16) : mlo + 1; } ml = v != 0; }
    return ml && a < mhi && e > mlo;
}

/* R2_MAPWATCH=ADDR (hex): a thread checks every 100 us that the page at ADDR is still mapped, and
   logs (once per change) when it goes away or comes back, with the time */
static void *mapwatch_thread(void *arg) {
    mach_vm_address_t w = (mach_vm_address_t)(uintptr_t)arg; int was = -1;
    for (;;) {
        mach_vm_address_t r = w; mach_vm_size_t sz = 0; vm_region_basic_info_data_64_t bi;
        mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &c, &on);
        if (on) mach_port_deallocate(mach_task_self(), on);
        int now = kr == KERN_SUCCESS && r <= w;
        if (now != was) {
            if (now) rlog("[mapwatch] %#llx mapped: %#llx-%#llx prot %d", (unsigned long long)w, (unsigned long long)r, (unsigned long long)(r + sz), bi.protection);
            else rlog("[mapwatch] %#llx NOT MAPPED (next region %#llx)", (unsigned long long)w, kr ? 0ull : (unsigned long long)r);
            was = now;
        }
        usleep(100);
    }
    return 0;
}
void mapwatch_start(void) {
    const char *v = getenv("R2_MAPWATCH");
    if (!v) return;
    pthread_t t; pthread_create(&t, 0, mapwatch_thread, (void *)(uintptr_t)(strtoull(v, 0, 16) & ~4095ull));
}

void low_free(u32 addr, u32 size) {
    u32 len = (size + R32_PAGE - 1) & ~(R32_PAGE - 1);
    if (mmaplog_hit(addr, (uint64_t)addr + len))     /* R2_MMAPLOG: also the frees in that range, with the caller */
        rlog("[mmaplog] free %#x-%#x <- %p <- %p", addr, addr + len, __builtin_return_address(0), __builtin_return_address(1));
    mmap(P32(addr), len, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    low_mark(addr, len, 0);
}
