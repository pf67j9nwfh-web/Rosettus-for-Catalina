/* heapchk.c - R2_HEAPCHECK=N: after every Nth syscall, Mach trap or hostcall, check the host malloc
   heap (malloc_zone_check); on the first damage, print the last operations of each kind and abort.
   Test aid for heap corruption. */
#include "rhost.h"
#include <malloc/malloc.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
static int g_hc_n = -1;
void heapchk_dump(void);
static unsigned long g_hc_cnt;
static struct { const char *what; long num; unsigned long seq; uint64_t tid; } g_ring[64];
static unsigned g_ri;
void heapchk(const char *what, long num) {
    if (g_hc_n < 0) { const char *e = getenv("R2_HEAPCHECK"); g_hc_n = e ? atoi(e) : 0; }
    if (!g_hc_n) return;
    uint64_t tid = 0; pthread_threadid_np(0, &tid);
    unsigned long seq = __sync_add_and_fetch(&g_hc_cnt, 1);
    unsigned i = __sync_fetch_and_add(&g_ri, 1) & 63;
    g_ring[i].what = what; g_ring[i].num = num; g_ring[i].seq = seq; g_ring[i].tid = tid;
    if (seq % (unsigned long)g_hc_n) return;
    {   /* cheap check first: are the first pages of the tiny and small malloc regions still mapped? */
        static uint64_t pg[2];
        if (!pg[0]) { void *a = malloc(16), *b = malloc(3000); pg[0] = (uint64_t)(uintptr_t)a & ~0xfffffull; pg[1] = (uint64_t)(uintptr_t)b & ~0x7fffffull; }
        for (int k = 0; k < 2; k++) {
            mach_vm_address_t ra = pg[k]; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi;
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on = 0;
            if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &on) || ra > pg[k]) {
                rlog("[heapchk] %s region page %#llx unmapped after %s %ld (op %lu, thread %llu)", k ? "small" : "tiny",
                     (unsigned long long)pg[k], what, num, seq, (unsigned long long)tid);
                heapchk_dump();
                abort();
            }
        }
        if (getenv("R2_HEAPCHECK_PAGES")) return;
    }
    if (malloc_zone_check(NULL)) return;
    rlog("[heapchk] heap damaged after %s %ld (op %lu, thread %llu)", what, num, seq, (unsigned long long)tid);
    heapchk_dump();
    abort();
}
/* also from the host fault handler (the check itself can fault on a damaged heap) */
void heapchk_dump(void) {
    if (g_hc_n <= 0) return;
    rlog("[heapchk] the last operations:");
    for (unsigned k = 0; k < 64; k++) {
        unsigned j = (g_ri + k) & 63;
        if (g_ring[j].what) rlog("[heapchk]   #%lu %s %ld (thread %llu)", g_ring[j].seq, g_ring[j].what, g_ring[j].num, (unsigned long long)g_ring[j].tid);
    }
}
