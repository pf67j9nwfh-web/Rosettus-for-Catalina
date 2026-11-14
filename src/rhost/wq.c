/* wq.c - the workqueue of the 32-bit world, as 10.9+ libpthread and libdispatch expect it from the
   kernel (Mojave's i386 userland needs it; Snow Leopard's does not use it).

   Threads: libdispatch asks for worker threads (WQOPS_QUEUE_REQTHREADS with a pthread priority); a
   thread enters the registered wqthread entry, runs, and hands itself back (WQOPS_THREAD_RETURN, or
   WQOPS_THREAD_KEVENT_RETURN after kevents). It then parks until there is new work and re-enters
   wqthread with the REUSE flag. Here every workqueue thread is a host thread parked inside the trap of
   its return call.

   Kevents: libdispatch registers its sources on the "workqueue kqueue" (kevent_qos with
   KEVENT_FLAG_WORKQ); the kernel then delivers fired events by starting a workqueue thread with the
   events on it. Here the registrations go to a host kqueue, and a pump thread waits on it and gives
   each batch of events to a workqueue thread, as an event-manager thread (libdispatch then also
   runs its manager queue: timers and source registrations). Mach port events that ask for the
   message (MACH_RCV_MSG) get it received here and converted for the 32-bit world.
   Batches are delivered one at a time: the pump waits until the thread has returned the batch.

   Workloops: libdispatch also keeps serial queues as kernel "workloops" (kevent_id with
   KEVENT_FLAG_WORKLOOP, id = the queue). Passed to the kernel they would be serviced by the host's
   own libdispatch, so they are emulated here: each workloop has a host kqueue for its ordinary
   knotes (watched by the pump through the workqueue kqueue), a thread request (EVFILT_WORKLOOP,
   NOTE_WL_THREAD_REQUEST), sync waiters (NOTE_WL_SYNC_WAIT / WAKE) and an owner (DISCOVER_OWNER /
   END_OWNERSHIP). While it has an active thread request or fired events, no servicer and no owner,
   a workqueue thread is started for it as a workloop thread (WQ_FLAG_THREAD_WORKLOOP, the id
   just before the events); it hands its changes back with WQOPS_THREAD_WORKLOOP_RETURN. QoS
   overrides and turnstiles of the kernel are left out. */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/event.h>
#include <mach/mach.h>

extern u32 g_wqthread32, g_pthsize32, g_tsd_offset32;
void store_kport(u32 self, mach_port_t kport);
mach_msg_return_t msg_to32(const unsigned char *m64, u32 dst, u32 dst_size);

#define WQOPS_THREAD_RETURN          0x004
#define WQOPS_QUEUE_NEWSPISUPP       0x010
#define WQOPS_QUEUE_REQTHREADS       0x020
#define WQOPS_QUEUE_REQTHREADS2      0x030
#define WQOPS_THREAD_KEVENT_RETURN   0x040
#define WQOPS_SET_EVENT_MANAGER_PRIORITY 0x080
#define WQOPS_THREAD_WORKLOOP_RETURN 0x100
#define WQOPS_SHOULD_NARROW          0x200
#define WQ_FLAG_THREAD_PRIO_QOS      0x00004000u
#define WQ_FLAG_THREAD_OVERCOMMIT    0x00010000u
#define WQ_FLAG_THREAD_REUSE         0x00020000u
#define WQ_FLAG_THREAD_KEVENT        0x00080000u
#define WQ_FLAG_THREAD_EVENT_MANAGER 0x00100000u
#define WQ_FLAG_THREAD_TSD_BASE_SET  0x00200000u
#define WQ_FLAG_THREAD_WORKLOOP      0x00400000u
#define PRIO_EVENT_MANAGER           0x02000000u
#define WQ_STACK (512u * 1024)
#define WQ_MAX_THREADS 64

#define KEVENT_FLAG_WORKQ        0x020

#pragma pack(push, 4)
typedef struct { uint64_t ident; int16_t filter; uint16_t flags; int32_t qos; uint64_t udata;
                 uint32_t fflags, xflags; int64_t data; uint64_t ext[4]; } kevq_t;      /* kevent_qos_s: 72 bytes */
#pragma pack(pop)

#define NKEV 16
#define KEVMSG (128u * 1024)             /* per thread: received Mach messages of one batch */

struct wl;
typedef struct job {
    struct job *next;
    int kevent;                           /* a batch of events (else a plain thread request) */
    struct wl *wl;                        /* a workloop's servicer: events taken when the thread starts */
    u32 prio;
    int n;
    kevq_t ev[NKEV];
    unsigned char *msg[NKEV];             /* host copies of received messages */
} job_t;

typedef struct { u32 self, stacklow, kevlist, msgarea; job_t *job; struct wl *wl; } wqthr_t;
static __thread wqthr_t *t_wq;

static pthread_mutex_t wq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wq_cv = PTHREAD_COND_INITIALIZER;      /* idle threads wait here */
static pthread_cond_t wq_done = PTHREAD_COND_INITIALIZER;    /* the pump waits for its batch */
static job_t *wq_head, *wq_tail;
static int wq_idle, wq_threads, wq_batch_out;

/* ---------------------------------------------------------------- jobs and threads */
static void job_push(job_t *j) {             /* wq_mu held */
    j->next = 0;
    if (wq_tail) wq_tail->next = j; else wq_head = j;
    wq_tail = j;
}
static job_t *job_pop(void) {                /* wq_mu held */
    job_t *j = wq_head;
    if (j) { wq_head = j->next; if (!wq_head) wq_tail = 0; }
    return j;
}

/* pthread priority -> wqthread flags */
static u32 prio_flags(u32 prio) {
    if (prio & PRIO_EVENT_MANAGER) return WQ_FLAG_THREAD_EVENT_MANAGER | WQ_FLAG_THREAD_KEVENT;
    u32 qos = 4;
    for (u32 q = 1; q <= 6; q++) if (prio & (0x80u << q)) { qos = q; break; }
    return WQ_FLAG_THREAD_PRIO_QOS | qos | ((prio & 0x80000000u) ? WQ_FLAG_THREAD_OVERCOMMIT : 0);
}

/* the registers a thread enters wqthread with, for job j; the events go into the thread's own area */
typedef struct { u32 eip, esp, eax, ebx, ecx, edx, esi, edi; } entry_t;
static void wl_take(wqthr_t *w, job_t *j);
static entry_t job_entry(wqthr_t *w, job_t *j, int reuse) {
    entry_t e;
    mach_port_t kport = mach_thread_self(); mach_port_deallocate(mach_task_self(), kport);
    u32 kevlist = w->kevlist;
    if (j->wl) { wl_take(w, j); kevlist = w->kevlist + 8; }   /* the workloop's id goes just before the events */
    u32 fl = j->wl ? (WQ_FLAG_THREAD_WORKLOOP | WQ_FLAG_THREAD_KEVENT | prio_flags(j->prio))
           : j->kevent ? (WQ_FLAG_THREAD_EVENT_MANAGER | WQ_FLAG_THREAD_KEVENT) : prio_flags(j->prio);
    if (reuse) fl |= WQ_FLAG_THREAD_REUSE;
    if (g_tsd_offset32) fl |= WQ_FLAG_THREAD_TSD_BASE_SET;
    int n = 0;
    if (j->kevent) {
        kevq_t *dst = P32(kevlist);
        u32 moff = 0;
        for (int k = 0; k < j->n; k++) {
            dst[n] = j->ev[k];
            if (j->msg[k]) {
                u32 room = KEVMSG - moff;
                mach_msg_return_t kr = msg_to32(j->msg[k], w->msgarea + moff, room);
                if (kr == MACH_MSG_SUCCESS) {
                    u32 sz = ((mach_msg_header_t *)P32(w->msgarea + moff))->msgh_size;
                    dst[n].ext[0] = w->msgarea + moff; dst[n].ext[1] = sz;
                    moff += (sz + 64 + 15) & ~15u;
                } else { dst[n].fflags = (uint32_t)kr; dst[n].ext[0] = 0; }
                free(j->msg[k]); j->msg[k] = 0;
            }
            n++;
        }
    }
    e.eip = g_wqthread32; e.esp = (w->self - 16) & ~15u;
    e.eax = w->self; e.ebx = kport; e.ecx = w->stacklow;
    e.edx = n ? kevlist : 0; e.edi = fl; e.esi = (u32)n;
    if (g_trace > 1) rlog("[wq] thread %#x: %s, flags %#x, %d event(s)", w->self, j->wl ? "workloop" : j->kevent ? "events" : "request", fl, n);
    return e;
}

static void *wq_host_thread(void *p) {
    wqthr_t *w = p;
    /* a new workqueue thread starts with no signal blocked, as the kernel's do. Started from the
       event pump (a workloop's events) it inherited the pump's mask, everything blocked: a ud2 or
       any other fault of the 32-bit code there stayed pending, and the thread ran into it again
       and again at 100% CPU. */
    { sigset_t none; sigemptyset(&none); pthread_sigmask(SIG_SETMASK, &none, 0); }
    thread_setup_altstack();
    t_wq = w;
    mach_port_t kport = mach_thread_self();
    if (g_tsd_offset32) tsd32_set(w->self + g_tsd_offset32);
    store_kport(w->self, kport);
    entry_t e = job_entry(w, w->job, 0);
    if (!w->job->kevent || w->job->wl) free(w->job), w->job = 0;
    enter32(e.eip, e.esp, e.eax, e.ebx, e.ecx, e.edx, e.esi, e.edi);
}

static int wq_spawn(job_t *j) {
    u32 pth = (g_pthsize32 + R32_PAGE - 1) & ~(R32_PAGE - 1);
    u32 kev = (NKEV * sizeof(kevq_t) + R32_PAGE - 1) & ~(R32_PAGE - 1);
    u32 total = R32_PAGE + WQ_STACK + pth + kev + KEVMSG;
    u32 base = low_alloc(total, 0xffff, 1);
    if (!base || mmap(P32(base + R32_PAGE), total - R32_PAGE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
        rlog("[wq] no memory for a workqueue thread"); return -1;
    }
    wqthr_t *w = calloc(1, sizeof *w);
    w->stacklow = base + R32_PAGE; w->self = base + R32_PAGE + WQ_STACK;
    w->kevlist = w->self + pth; w->msgarea = w->kevlist + kev;
    w->job = j;
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 512 * 1024);
    pthread_t h;
    int e = pthread_create(&h, &a, wq_host_thread, w);
    pthread_attr_destroy(&a);
    if (e) { rlog("[wq] pthread_create failed"); return -1; }
    if (g_trace) rlog("[wq] new thread self %#x", w->self);
    return 0;
}

/* hand a job to an idle thread, or start one */
static void wq_submit(job_t *j) {
    pthread_mutex_lock(&wq_mu);
    if (wq_idle > 0 || wq_threads >= WQ_MAX_THREADS) {
        job_push(j);
        pthread_cond_signal(&wq_cv);
        pthread_mutex_unlock(&wq_mu);
        return;
    }
    wq_threads++;
    pthread_mutex_unlock(&wq_mu);
    if (wq_spawn(j)) { pthread_mutex_lock(&wq_mu); wq_threads--; job_push(j); pthread_mutex_unlock(&wq_mu); }
}

/* the current thread is done with its job: park until there is another, and re-enter with it */
static void wq_park_and_reenter(ss64_t *s) {
    pthread_mutex_lock(&wq_mu);
    if (t_wq->job) {                                   /* a batch of events: the pump may go on */
        free(t_wq->job); t_wq->job = 0;
        wq_batch_out = 0;
        pthread_cond_broadcast(&wq_done);
    }
    job_t *j;
    wq_idle++;
    while (!(j = job_pop())) pthread_cond_wait(&wq_cv, &wq_mu);
    wq_idle--;
    pthread_mutex_unlock(&wq_mu);
    entry_t e = job_entry(t_wq, j, 1);
    if (j->kevent && !j->wl) t_wq->job = j; else free(j);
    s->__rip = e.eip; s->__rsp = e.esp; s->__rax = e.eax; s->__rbx = e.ebx; s->__rcx = e.ecx;
    s->__rdx = e.edx; s->__rsi = e.esi; s->__rdi = e.edi;
    t_sys_newctx = 1;
}

/* ---------------------------------------------------------------- the workqueue kqueue */
static int g_wqkq = -1;
static pthread_mutex_t kq_mu = PTHREAD_MUTEX_INITIALIZER;
/* Mach port registrations that ask for the message */
typedef struct { uint64_t ident, udata; uint32_t fflags; uint64_t size; } machreg_t;
static machreg_t g_mreg[1024]; static int g_nmreg;

static void *kev_pump(void *a);
static void kq_init(void) {
    pthread_mutex_lock(&kq_mu);
    if (g_wqkq < 0) {
        g_wqkq = kqueue();
        pthread_t t; pthread_create(&t, 0, kev_pump, 0);
        if (g_trace) rlog("[wq] workqueue kqueue %d", g_wqkq);
    }
    pthread_mutex_unlock(&kq_mu);
}

static void q_to64(const kevq_t *q, struct kevent64_s *k) {
    memset(k, 0, sizeof *k);
    k->ident = q->ident; k->filter = q->filter; k->flags = q->flags; k->fflags = q->fflags;
    k->data = q->data; k->udata = q->udata; k->ext[0] = q->ext[0]; k->ext[1] = q->ext[1];
    if (q->filter == EVFILT_MACHPORT && (q->fflags & MACH_RCV_MSG)) {
        pthread_mutex_lock(&kq_mu);
        int i;
        for (i = 0; i < g_nmreg && !(g_mreg[i].ident == q->ident && g_mreg[i].udata == q->udata); i++) ;
        if (q->flags & EV_DELETE) { if (i < g_nmreg) g_mreg[i] = g_mreg[--g_nmreg]; }
        else if (i < 1024) {
            if (i == g_nmreg) g_nmreg++;
            g_mreg[i].ident = q->ident; g_mreg[i].udata = q->udata; g_mreg[i].fflags = q->fflags;
            g_mreg[i].size = q->ext[1] ? q->ext[1] : 16384;
        }
        pthread_mutex_unlock(&kq_mu);
        k->fflags = 0; k->ext[0] = k->ext[1] = 0;         /* notify only; the message is received below */
    }
}
static void q_from64(const struct kevent64_s *k, kevq_t *q) {
    memset(q, 0, sizeof *q);
    q->ident = k->ident; q->filter = k->filter; q->flags = k->flags; q->fflags = k->fflags;
    q->data = k->data; q->udata = k->udata; q->ext[0] = k->ext[0]; q->ext[1] = k->ext[1];
}

/* A deleted knote's events that fired but were not handed out yet go too, as in the kernel: a
   batch still queued here kept an event for a source libdispatch had since unregistered and freed,
   and _dispatch_kevent_merge crashed on its reused memory (ud2 on a bad filter action: GoLive CS2,
   Blender). Returns how many were dropped. */
static int purge_ev(kevq_t *ev, unsigned char **msg, int *n, const kevq_t *del) {
    int i, m = 0, dropped = 0;
    for (i = 0; i < *n; i++) {
        if (ev[i].ident == del->ident && ev[i].filter == del->filter && ev[i].udata == del->udata) {
            if (msg[i]) free(msg[i]);
            dropped++;
            continue;
        }
        ev[m] = ev[i]; msg[m] = msg[i]; m++;
    }
    *n = m;
    return dropped;
}
static void purge_queued(const kevq_t *cq, int n) {      /* wq_mu held: batches not yet taken by a thread */
    for (int i = 0; i < n; i++) {
        if (!(cq[i].flags & EV_DELETE)) continue;
        for (job_t *j = wq_head; j; j = j->next)
            if (j->kevent && !j->wl && j->n) {
                int d = purge_ev(j->ev, j->msg, &j->n, &cq[i]);
                if (d && g_trace) rlog("[wq] dropped %d queued event(s) of deleted ident %#llx filter %d", d, (unsigned long long)cq[i].ident, cq[i].filter);
            }
    }
}

/* apply kevent_qos changes to the workqueue kqueue; errors come back as events (if room) */
static int kq_apply(u32 changes, int n, u32 events, int nev, int flags) {
    kq_init();
    if (n < 0 || n > 4096) { errno = EINVAL; return -1; }
    struct kevent64_s *ch = calloc((size_t)n + 1, sizeof *ch), out[64];
    const kevq_t *cq = P32(changes);
    for (int i = 0; i < n; i++) q_to64(&cq[i], &ch[i]);
    if (nev > 64) nev = 64;
    struct timespec z = { 0, 0 };
    int r = kevent64(g_wqkq, ch, n, events ? out : 0, events ? nev : 0, KEVENT_FLAG_IMMEDIATE | (flags & KEVENT_FLAG_ERROR_EVENTS), &z);
    int e = errno;
    if (r > 0 && events) { kevq_t *oq = P32(events); for (int i = 0; i < r; i++) q_from64(&out[i], &oq[i]); }
    free(ch);
    pthread_mutex_lock(&wq_mu); purge_queued(cq, n); pthread_mutex_unlock(&wq_mu);
    if (g_trace > 1) rlog("[wq] kqueue: %d change(s) -> %d", n, r);
    errno = e;
    return r;
}

/* a fired event from a host kqueue -> the 32-bit event q (and the received message, if the
   registration asked for it); 0: nothing to deliver after all */
static int ev_take(const struct kevent64_s *ev, kevq_t *q, unsigned char **msg) {
    q_from64(ev, q);
    *msg = 0;
    if (ev->filter == EVFILT_MACHPORT && !(ev->flags & EV_ERROR)) {
        machreg_t m = { 0 }; int found = 0;
        pthread_mutex_lock(&kq_mu);
        for (int k = 0; k < g_nmreg; k++) if (g_mreg[k].ident == ev->ident && g_mreg[k].udata == ev->udata) { m = g_mreg[k]; found = 1; break; }
        pthread_mutex_unlock(&kq_mu);
        if (found) {
            /* receive it now, as the kernel would have into the event's buffer */
            u32 size = (u32)m.size * 2 + 1024;
            unsigned char *buf = calloc(1, size);
            mach_msg_option_t opt = MACH_RCV_MSG | MACH_RCV_TIMEOUT | (m.fflags & ~(MACH_RCV_MSG | MACH_RCV_VOUCHER));
            mach_msg_return_t kr = mach_msg((mach_msg_header_t *)buf, opt, 0, size, (mach_port_t)ev->ident, 0, 0);
            if (kr == MACH_RCV_TIMED_OUT) { free(buf); return 0; }          /* nothing there after all */
            q->fflags = m.fflags;
            if (kr == MACH_MSG_SUCCESS) { *msg = buf; q->fflags = MACH_MSG_SUCCESS; }
            else { q->fflags = (uint32_t)kr; q->ext[1] = ((mach_msg_header_t *)buf)->msgh_size; free(buf); }
        }
    }
    return 1;
}

#define WL_TAG 0x574c000000000000ull           /* udata of a workloop kqueue's watch: tag | wl pointer */
static void wl_fired(struct wl *wl);

static void *kev_pump(void *a) {
    (void)a;
    sigset_t all; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, 0);
    struct kevent64_s ev[NKEV];
    for (;;) {
        int n = kevent64(g_wqkq, 0, 0, ev, NKEV, 0, 0);
        if (n <= 0) continue;
        job_t *j = calloc(1, sizeof *j);
        j->kevent = 1; j->prio = PRIO_EVENT_MANAGER;
        for (int i = 0; i < n; i++) {
            if (ev[i].filter == EVFILT_READ && (ev[i].udata & 0xffff000000000000ull) == WL_TAG) {
                wl_fired((struct wl *)(uintptr_t)(ev[i].udata & 0x0000ffffffffffffull));
                continue;
            }
            if (ev_take(&ev[i], &j->ev[j->n], &j->msg[j->n])) j->n++;
        }
        if (!j->n) { free(j); continue; }
        pthread_mutex_lock(&wq_mu);
        wq_batch_out = 1;
        pthread_mutex_unlock(&wq_mu);
        wq_submit(j);
        pthread_mutex_lock(&wq_mu);
        while (wq_batch_out) pthread_cond_wait(&wq_done, &wq_mu);
        pthread_mutex_unlock(&wq_mu);
    }
    return 0;
}

/* kevent_qos(kq, changes, nchanges, events, nevents, data_out, data_available, flags) with
   KEVENT_FLAG_WORKQ: returns 1 if handled here */
int wq_kevent_qos(ss64_t *s, u32 argp) {
    u32 flags = rd32(argp + 28);
    if (!(flags & KEVENT_FLAG_WORKQ)) return 0;
    int r = kq_apply(rd32(argp + 4), (int)rd32(argp + 8), rd32(argp + 12), (int)rd32(argp + 16), (int)flags);
    ret_host(s, r);
    return 1;
}

/* ---------------------------------------------------------------- workloops */
#define EVFILT_WORKLOOP_ (-17)
#define NOTE_WL_THREAD_REQUEST 0x001
#define NOTE_WL_SYNC_WAIT      0x004
#define NOTE_WL_SYNC_WAKE      0x008
#define NOTE_WL_COMMANDS_MASK  0x00f
#define NOTE_WL_UPDATE_QOS     0x010
#define NOTE_WL_END_OWNERSHIP  0x020
#define NOTE_WL_DISCOVER_OWNER 0x080
#define NOTE_WL_IGNORE_ESTALE  0x100
#define KEVENT_FLAG_ERROR_EVENTS_ 0x002
#define KEVENT_FLAG_WORKLOOP_  0x400

typedef struct wlsync { struct wlsync *next; uint64_t ident; int woken; } wlsync_t;
typedef struct wl {
    struct wl *next;
    uint64_t id;
    int kq;                               /* host kqueue of its ordinary knotes */
    kevq_t tr; int has_tr, tr_active;     /* the thread request */
    u32 owner;                            /* owner thread's port name (0: none) */
    int serviced, queued, watch_off;
    kevq_t pend[NKEV]; unsigned char *pmsg[NKEV]; int npend;     /* fired ordinary events */
    wlsync_t *syncs;
} wl_t;
static wl_t *g_wls;
static pthread_cond_t wl_cv = PTHREAD_COND_INITIALIZER;   /* sync waiters (with wq_mu) */

static u32 cur_port_name(void) {
    mach_port_t p = mach_thread_self(); mach_port_deallocate(mach_task_self(), p); return p;
}
static void wl_watch(wl_t *wl, int on) {       /* the pump's watch of the workloop's kqueue (EV_DISPATCH) */
    struct kevent64_s k;
    EV_SET64(&k, wl->kq, EVFILT_READ, on ? (EV_ADD | EV_ENABLE | EV_DISPATCH) : EV_DISABLE, 0, 0, WL_TAG | (uintptr_t)wl, 0, 0);
    struct timespec z = { 0, 0 };
    kevent64(g_wqkq, &k, 1, 0, 0, 0, &z);
}
static wl_t *wl_get(uint64_t id) {             /* wq_mu held */
    wl_t *wl;
    for (wl = g_wls; wl && wl->id != id; wl = wl->next) ;
    if (wl) return wl;
    kq_init();
    wl = calloc(1, sizeof *wl);
    wl->id = id; wl->kq = kqueue();
    wl->next = g_wls; g_wls = wl;
    wl_watch(wl, 1);
    if (g_trace > 1) rlog("[wq] workloop %#llx: kqueue %d", (unsigned long long)id, wl->kq);
    return wl;
}
/* start a servicer if it has work and nobody else is on it (wq_mu held) */
static void wl_kick(wl_t *wl) {
    if (wl->serviced || wl->queued || wl->owner) return;
    if (!((wl->has_tr && wl->tr_active) || wl->npend)) return;
    job_t *j = calloc(1, sizeof *j);
    j->kevent = 1; j->wl = wl;
    j->prio = wl->has_tr ? (u32)wl->tr.qos : 0x000008ffu;
    if (!(j->prio & 0x3f00u)) j->prio |= 0x800u;            /* a QoS class (default) */
    wl->queued = 1;
    job_push(j);
    if (wq_idle > 0 || wq_threads >= WQ_MAX_THREADS) { pthread_cond_signal(&wq_cv); return; }
    wq_threads++;
    job_t *jj = job_pop();                     /* the one just pushed (the queue had no taker) */
    pthread_mutex_unlock(&wq_mu);
    if (wq_spawn(jj)) { pthread_mutex_lock(&wq_mu); wq_threads--; job_push(jj); pthread_mutex_unlock(&wq_mu); }
    pthread_mutex_lock(&wq_mu);
}
/* the pump saw the workloop's kqueue fire: take its events (the watch stays off until then) */
static void wl_fired(wl_t *wl) {
    struct kevent64_s ev[NKEV];
    struct timespec z = { 0, 0 };
    pthread_mutex_lock(&wq_mu);
    int room = NKEV - wl->npend;
    int n = room > 0 ? kevent64(wl->kq, 0, 0, ev, room, 0, &z) : 0;
    for (int i = 0; i < n; i++)
        if (ev_take(&ev[i], &wl->pend[wl->npend], &wl->pmsg[wl->npend])) wl->npend++;
    if (wl->serviced || wl->npend >= NKEV) wl->watch_off = 1;       /* again when the servicer returns */
    else wl_watch(wl, 1);
    wl_kick(wl);
    pthread_mutex_unlock(&wq_mu);
}
/* a servicer starts: bind it, give it the thread request (consumed: EV_CLEAR) and the fired events */
static void wl_take(wqthr_t *w, job_t *j) {
    wl_t *wl = j->wl;
    pthread_mutex_lock(&wq_mu);
    wl->queued = 0; wl->serviced = 1; w->wl = wl;
    j->n = 0;
    if (wl->has_tr && wl->tr_active && !wl->owner) { j->ev[j->n] = wl->tr; j->msg[j->n] = 0; j->n++; wl->tr_active = 0; }
    for (int i = 0; i < wl->npend && j->n < NKEV; i++) { j->ev[j->n] = wl->pend[i]; j->msg[j->n] = wl->pmsg[i]; j->n++; }
    wl->npend = 0;
    pthread_mutex_unlock(&wq_mu);
    memcpy(P32(w->kevlist), &wl->id, 8);
}
/* one EVFILT_WORKLOOP change; returns an errno (wq_mu held; may wait for a sync wake) */
static int wl_change(wl_t *wl, kevq_t *k) {
    int err = 0;
    u32 cmd = k->fflags & NOTE_WL_COMMANDS_MASK;
    uint64_t val = 0;
    if (k->ext[1]) {                          /* debounce: the queue's state must still be what the caller saw */
        memcpy(&val, P32((u32)k->ext[1]), 8);
        uint64_t want = k->ext[3];
        k->ext[3] = val;                      /* the caller learns the state seen (as the kernel reports it) */
        if ((val & k->ext[2]) != (want & k->ext[2])) err = ESTALE;
        else if ((k->fflags & NOTE_WL_DISCOVER_OWNER) && ((u32)val & ~3u)) {
            /* the drain lock holds the owner's port name with its low 2 bits borrowed for flags; the
               kernel puts them back (ipc_entry_name_mask: name | 3) before looking the thread up.
               Kept as name & ~3, the owner never matched the thread ending its ownership, the
               workloop kept an owner for good and no servicer ever ran (dispatch_sync hung). An
               empty name leaves the owner as it is, as in filt_wlupdate. */
            wl->owner = ((u32)val & ~3u) | 3u;
        }
    }
    if ((k->fflags & NOTE_WL_END_OWNERSHIP) && wl->owner && wl->owner == cur_port_name()) wl->owner = 0;
    if (err == ESTALE && (k->fflags & NOTE_WL_IGNORE_ESTALE)) {
        /* R2_WL_STALEKEEP=1 (diagnosis): a thread request re-armed with a stale state stays armed
           (the kernel ignores it, like the default here). */
        static int keep = -1;
        if (keep < 0) keep = getenv("R2_WL_STALEKEEP") != 0;
        if (keep && cmd == NOTE_WL_THREAD_REQUEST && !(k->flags & EV_DELETE) && wl->has_tr) wl->tr_active = 1;
        wl_kick(wl); return 0;
    }
    if (err) { wl_kick(wl); return err; }
    if (cmd == NOTE_WL_THREAD_REQUEST) {
        if (k->flags & EV_DELETE) { wl->has_tr = 0; wl->tr_active = 0; }
        else {
            if (!wl->has_tr) { wl->tr = *k; wl->has_tr = 1; }
            else { if (k->fflags & NOTE_WL_UPDATE_QOS) wl->tr.qos = k->qos; wl->tr.udata = k->udata;
                   memcpy(wl->tr.ext, k->ext, sizeof k->ext); }
            wl->tr.fflags = k->fflags; wl->tr.flags = EV_ADD | EV_ENABLE | EV_CLEAR;
            wl->tr_active = 1;
        }
    } else if (cmd == NOTE_WL_SYNC_WAIT || cmd == NOTE_WL_SYNC_WAKE) {
        wlsync_t **pp, *y;
        for (pp = &wl->syncs; (y = *pp) && y->ident != k->ident; pp = &y->next) ;
        if (!y && !(k->flags & EV_DELETE)) { y = calloc(1, sizeof *y); y->ident = k->ident; y->next = wl->syncs; wl->syncs = y; }
        if (y && cmd == NOTE_WL_SYNC_WAKE) { y->woken = 1; pthread_cond_broadcast(&wl_cv); }
        if (y && (k->flags & EV_DELETE)) {
            for (pp = &wl->syncs; *pp && *pp != y; pp = &(*pp)->next) ;
            if (*pp) *pp = y->next;
            y->woken = -1; pthread_cond_broadcast(&wl_cv);   /* a waiter still in there goes; it frees */
            wl_kick(wl);
            return 0;
        }
        if (y && cmd == NOTE_WL_SYNC_WAIT) {
            wl_kick(wl);
            while (!y->woken) pthread_cond_wait(&wl_cv, &wq_mu);
            if (y->woken < 0) free(y);
            return 0;
        }
    } else return EINVAL;
    wl_kick(wl);
    return 0;
}
/* apply changes to a workloop: EVFILT_WORKLOOP here, others on its host kqueue; error events into
   out (up to nout); returns the number of errors */
static int wl_apply(wl_t *wl, u32 changes, int n, kevq_t *out, int nout) {
    int nerr = 0;
    kevq_t *cq = P32(changes);
    for (int i = 0; i < n; i++) {
        kevq_t k = cq[i];
        int err = 0;
        if (g_trace > 1) rlog("[wq] workloop %#llx change: ident %#llx filter %d flags %#x fflags %#x qos %#x udata %#llx ext %#llx %#llx %#llx %#llx",
                              (unsigned long long)wl->id, (unsigned long long)k.ident, k.filter, k.flags, k.fflags, k.qos,
                              (unsigned long long)k.udata, (unsigned long long)k.ext[0], (unsigned long long)k.ext[1],
                              (unsigned long long)k.ext[2], (unsigned long long)k.ext[3]);
        if (k.filter == EVFILT_WORKLOOP_) err = wl_change(wl, &k);
        else {
            struct kevent64_s c, o;
            q_to64(&k, &c);
            c.flags |= EV_RECEIPT;
            struct timespec z = { 0, 0 };
            int r = kevent64(wl->kq, &c, 1, &o, 1, 0, &z);
            if (r < 0) err = errno;
            else if (r == 1 && (o.flags & EV_ERROR) && o.data) err = (int)o.data;
            if (k.flags & EV_DELETE) {          /* its fired events not yet handed to a servicer go too */
                int d = purge_ev(wl->pend, wl->pmsg, &wl->npend, &k);
                if (d && g_trace) rlog("[wq] workloop %#llx: dropped %d pending event(s) of deleted ident %#llx",
                                       (unsigned long long)wl->id, d, (unsigned long long)k.ident);
            }
        }
        if (err && g_trace) rlog("[wq] workloop %#llx: change filter %d fflags %#x flags %#x -> error %d",
                                 (unsigned long long)wl->id, k.filter, k.fflags, k.flags, err);
        if (err && out && nerr < nout) { out[nerr] = k; out[nerr].flags |= EV_ERROR; out[nerr].data = err; nerr++; }
        else if (err && !out) nerr++;
    }
    return nerr;
}
/* kevent_id(id, changes, n, events, nevents, data_out, data_available, flags) with
   KEVENT_FLAG_WORKLOOP: returns 1 if handled here */
int wq_kevent_id(ss64_t *s, u32 argp) {
    u32 flags = rd32(argp + 32);
    if (!(flags & KEVENT_FLAG_WORKLOOP_)) return 0;
    uint64_t id = (uint64_t)rd32(argp) | ((uint64_t)rd32(argp + 4) << 32);
    u32 changes = rd32(argp + 8), events = rd32(argp + 16);
    int n = (int)rd32(argp + 12), nev = (int)rd32(argp + 20);
    if (n < 0 || n > 1024) { ret_err(s, EINVAL); return 1; }
    kevq_t errs[16];
    pthread_mutex_lock(&wq_mu);
    wl_t *wl = wl_get(id);
    int ne = wl_apply(wl, changes, n, errs, 16);
    pthread_mutex_unlock(&wq_mu);
    if (g_trace > 1) rlog("[wq] kevent_id %#llx: %d change(s), %d error(s)", (unsigned long long)id, n, ne);
    if ((flags & KEVENT_FLAG_ERROR_EVENTS_) && events && nev > 0) {
        if (ne > nev) ne = nev;
        memcpy(P32(events), errs, (size_t)ne * sizeof(kevq_t));
        ret_ok(s, (u32)ne);
    } else ret_ok(s, 0);
    return 1;
}
/* the servicer is done: its changes, then unbind; service again if there is more. A change that
   fails comes back to the thread as an error event (returns their number; the thread stays bound
   and hands them back again), as the kernel does: libdispatch redoes a failed "leave" that way. */
static int wl_return(u32 changes, int n) {
    wl_t *wl = t_wq->wl;
    kevq_t errs[NKEV];
    pthread_mutex_lock(&wq_mu);
    int ne = (changes && n > 0) ? wl_apply(wl, changes, n, errs, NKEV) : 0;
    if (ne > 0) {
        memcpy(P32(changes), errs, (size_t)ne * sizeof(kevq_t));
        pthread_mutex_unlock(&wq_mu);
        if (g_trace > 1) rlog("[wq] workloop %#llx: %d change(s) failed, back to the servicer", (unsigned long long)wl->id, ne);
        return ne;
    }
    wl->serviced = 0; t_wq->wl = 0;
    if (wl->watch_off) { wl->watch_off = 0; wl_watch(wl, 1); }
    wl_kick(wl);
    pthread_mutex_unlock(&wq_mu);
    return 0;
}

/* in a forked child: only the forking thread is left, and kqueues are not inherited */
void wq_atfork_child(void) {
    pthread_mutex_t m0 = PTHREAD_MUTEX_INITIALIZER; pthread_cond_t c0 = PTHREAD_COND_INITIALIZER;
    wq_mu = m0; kq_mu = m0; wq_cv = c0; wq_done = c0; wl_cv = c0;
    wq_head = wq_tail = 0; wq_idle = wq_threads = wq_batch_out = 0;
    g_wqkq = -1; g_nmreg = 0; g_wls = 0;
    if (t_wq) { t_wq->wl = 0; t_wq->job = 0; }
}

/* ---------------------------------------------------------------- workq_kernreturn */
void wq_kernreturn(ss64_t *s, u32 argp) {
    u32 op = rd32(argp), item = rd32(argp + 4), arg2 = rd32(argp + 8), arg3 = rd32(argp + 12);
    switch (op) {
    case WQOPS_QUEUE_REQTHREADS: case WQOPS_QUEUE_REQTHREADS2: {
        int n = (int)arg2;
        if (g_trace) rlog("[wq] request %d thread(s) prio %#x", n, arg3);
        for (int k = 0; k < n && k < 256; k++) {
            job_t *j = calloc(1, sizeof *j);
            j->prio = arg3;
            wq_submit(j);
        }
        ret_ok(s, 0);
        return;
    }
    case WQOPS_THREAD_WORKLOOP_RETURN:
        if (!t_wq || !t_wq->wl) {
            rlog("[wq] workloop return from a thread %s", t_wq ? "bound to no workloop" : "not of the workqueue");
            ret_err(s, EINVAL); return;
        }
        {   int ne = wl_return(item, (int)arg2);                    /* changes, unbind, maybe service again */
            if (ne > 0) { ret_ok(s, (u32)ne); return; }
        }
        wq_park_and_reenter(s);
        return;
    case WQOPS_THREAD_KEVENT_RETURN:
        if (item && arg2) kq_apply(item, (int)arg2, 0, 0, 0);    /* the changes the thread hands back */
        /* fall through */
    case WQOPS_THREAD_RETURN:
        if (!t_wq) { rlog("[wq] thread return (op %#x) from a thread not of the workqueue", op); ret_err(s, EINVAL); return; }
        wq_park_and_reenter(s);
        return;
    case WQOPS_QUEUE_NEWSPISUPP: case WQOPS_SET_EVENT_MANAGER_PRIORITY: case WQOPS_SHOULD_NARROW:
        ret_ok(s, 0);
        return;
    default:
        rlog("[wq] workq_kernreturn op %#x not supported", op);
        ret_err(s, ENOTSUP);
        return;
    }
}
