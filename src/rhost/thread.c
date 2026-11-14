/* thread.c - threads of the 32-bit world (bsdthread_create / bsdthread_terminate).
   Each is a host pthread that registers itself (trap.c) and enters 32-bit code at the thread_start
   routine libSystem registered, with the registers Snow Leopard's kernel would set:
   eax pthread, ebx kernel thread port, ecx function, edx argument, edi stack size, esi flags. */
#include "rhost.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <mach/mach.h>

extern u32 g_thread_start32, g_wqthread32, g_pthsize32;
u32 g_tsd_offset32;                         /* 10.9+ libpthread: TSD offset in the pthread (0: Snow Leopard) */
u32 g_mts_offset32;                         /* ... and the offset of the mach_thread_self slot in the TSD */
/* the kernel stores a new thread's port in its TSD before the thread runs (pthread_join waits on it) */
void store_kport(u32 self, mach_port_t kport) {
    if (g_tsd_offset32 && g_mts_offset32) wr32(self + g_tsd_offset32 + g_mts_offset32, kport);
}
#define PTHREAD_START_TSD_BASE_SET 0x10000000u

#define PTHREAD_START_CUSTOM  0x01000000u
#define PTHREAD_START_SETSCHED 0x02000000u

typedef struct { u32 func, arg, stack_top, self, stacksize, flags; } newthr_t;

static pthread_mutex_t g_create_mu = PTHREAD_MUTEX_INITIALIZER;
/* Threads created but not yet in 32-bit code (still waiting here, or created suspended): the kernel
   would show their initial registers to thread_get_state. PowerPC Java 1.5 creates its threads
   suspended and reads their state at once (the translator maps it to the PowerPC context); with
   KERN_FAILURE the translator went on with a null context and crashed (b80c5d10). */
static struct { uint64_t tid; u32 r[8]; } g_pend[64];
static pthread_mutex_t g_pend_mu = PTHREAD_MUTEX_INITIALIZER;
int thread_pending_state(uint64_t tid, u32 *eip, u32 *esp, u32 *regs6) {   /* regs6: eax ebx ecx edx edi esi */
    int ok = 0;
    pthread_mutex_lock(&g_pend_mu);
    for (int i = 0; i < 64; i++) if (g_pend[i].tid == tid && tid) {
        *eip = g_pend[i].r[0]; *esp = g_pend[i].r[1]; memcpy(regs6, &g_pend[i].r[2], 24); ok = 1; break;
    }
    pthread_mutex_unlock(&g_pend_mu);
    return ok;
}
/* thread_set_state on a thread that has not started: it starts with these registers */
int thread_pending_update(uint64_t tid, u32 eip, u32 esp, const u32 *regs6) {
    int ok = 0;
    pthread_mutex_lock(&g_pend_mu);
    for (int i = 0; i < 64; i++) if (g_pend[i].tid == tid && tid) {
        g_pend[i].r[0] = eip; g_pend[i].r[1] = esp; memcpy(&g_pend[i].r[2], regs6, 24); ok = 1; break;
    }
    pthread_mutex_unlock(&g_pend_mu);
    return ok;
}
static void pend_set(uint64_t tid, const u32 *r) {
    pthread_mutex_lock(&g_pend_mu);
    for (int i = 0; i < 64; i++) if (!g_pend[i].tid) { g_pend[i].tid = tid; memcpy(g_pend[i].r, r, sizeof g_pend[i].r); break; }
    pthread_mutex_unlock(&g_pend_mu);
}
static void pend_clear(uint64_t tid) {
    pthread_mutex_lock(&g_pend_mu);
    for (int i = 0; i < 64; i++) if (g_pend[i].tid == tid) g_pend[i].tid = 0;
    pthread_mutex_unlock(&g_pend_mu);
}
static void *host_thread(void *p) {
    pthread_mutex_lock(&g_create_mu); pthread_mutex_unlock(&g_create_mu);
    newthr_t t = *(newthr_t *)p;
    free(p);
    thread_setup_altstack();
    mach_port_t kport = mach_thread_self();
    if (g_trace) rlog("[thread] start %#x(%#x) self %#x stack %#x port %#x", t.func, t.arg, t.self, t.stack_top, kport);
    if (g_tsd_offset32) tsd32_set(t.self + g_tsd_offset32);   /* as the kernel does */
    {   /* its registers as recorded at creation, or as a thread_set_state made them since */
        uint64_t me = 0; u32 e = 0, sp = 0, r6[6];
        pthread_threadid_np(0, &me);
        if (thread_pending_state(me, &e, &sp, r6)) {
            pend_clear(me);
            enter32(e, sp, r6[0], r6[1], r6[2], r6[3], r6[5], r6[4]);   /* r6: eax ebx ecx edx edi esi */
        }
        pend_clear(me);
    }
    enter32(g_thread_start32, t.stack_top, t.self, kport, t.func, t.arg, t.flags, t.stacksize);
}

/* returns the new thread's pthread address, or 0 with *err */
u32 thread_create32(u32 func, u32 arg, u32 stack, u32 pthread, u32 flags, int *err) {
    newthr_t *t = calloc(1, sizeof *t);
    if (mmaplog_hit(stack - 0x100000, (uint64_t)stack + 0x1000) || mmaplog_hit(pthread, (uint64_t)pthread + 1)) {
        mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
        rlog("[mmaplog] bsdthread_create stack %#x pthread %#x flags %#x by %#x", stack, pthread, flags, me);
    }
    t->func = func; t->arg = arg;
    t->flags = flags & ~0x20000000u;   /* the kernel handles PTHREAD_START_SUSPENDED and does not pass it on:
                                          libpthread's _pthread_start aborts when it sees it (ud2) */
    if (flags & PTHREAD_START_CUSTOM) {
        t->stack_top = stack; t->self = pthread; t->stacksize = 0;
    } else {
        /* the kernel's layout: guard page, stack, then the pthread structure above the stack */
        u32 size = (stack + R32_PAGE - 1) & ~(R32_PAGE - 1);
        u32 pth = (g_pthsize32 + R32_PAGE - 1) & ~(R32_PAGE - 1);
        u32 total = R32_PAGE + size + pth;
        u32 base = low_alloc(total, 0xffff, 1);
        if (!base || mmap(P32(base + R32_PAGE), size + pth, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
            free(t); *err = ENOMEM; return 0;
        }
        if (mmaplog_hit(base, (uint64_t)base + total)) rlog("[mmaplog] thread stack %#x-%#x (stack %#x, pthread %#x)", base, base + total, size, pth);
        t->stack_top = base + R32_PAGE + size;
        t->self = t->stack_top;
        t->stacksize = size;
    }
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 512 * 1024);
    pthread_t h;
    u32 self = t->self;
    u32 stack_top0 = t->stack_top, flags0 = t->flags, stacksize0;
    if (g_tsd_offset32) t->stacksize = stack;        /* 10.9+: edi carries the stack argument */
    stacksize0 = t->stacksize;
    pthread_mutex_lock(&g_create_mu);                /* the new thread waits until its port is stored */
    int e = pthread_create(&h, &a, host_thread, t);
    pthread_attr_destroy(&a);
    if (e) { pthread_mutex_unlock(&g_create_mu); free(t); *err = e; return 0; }
    store_kport(self, pthread_mach_thread_np(h));
    {   /* its initial registers, as host_thread will enter32 with them: eip esp | eax ebx ecx edx edi esi */
        uint64_t tid = 0;
        if (!pthread_threadid_np(h, &tid)) {
            u32 r[8] = { g_thread_start32, stack_top0, self, pthread_mach_thread_np(h), func, arg, stacksize0, flags0 };
            pend_set(tid, r);
        }
    }
    /* PTHREAD_START_SUSPENDED: the kernel makes the thread suspended and the caller resumes it
       (thread_resume on its port) once the pthread is set up (pthread_create_suspended_np: Bejeweled
       2's sound thread). Started at once, it ran into libpthread's checks (ud2 in _pthread_start). */
    if (flags & 0x20000000u) {
        kern_return_t kr = thread_suspend(pthread_mach_thread_np(h));
        if (g_trace) rlog("[thread] created suspended (%d)", kr);
    }
    pthread_mutex_unlock(&g_create_mu);
    return self;
}

/* bsdthread_terminate(stackaddr, freesize, port, sem): free the stack, signal, and end this thread */
void thread_terminate32(u32 stackaddr, u32 freesize, u32 port, u32 sem) {
    if (g_trace || (freesize && mmaplog_hit(stackaddr, (uint64_t)stackaddr + freesize))) {
        mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
        rlog("[thread] terminate stack %#x size %#x port %#x sem %#x (thread %#x)", stackaddr, freesize, port, sem, me);
    }
    { extern void thread_trap_gone(void); thread_trap_gone(); }
    if (freesize) low_free(stackaddr & ~(R32_PAGE - 1), freesize);
    if (sem) semaphore_signal(sem);
    if (port) mach_port_deallocate(mach_task_self(), port);
    thread_unregister();
    pthread_exit(0);
}

