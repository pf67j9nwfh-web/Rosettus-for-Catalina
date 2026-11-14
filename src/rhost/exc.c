/* exc.c - Mach exceptions of the 32-bit world.
   The translator catches the faults of translated code (and of its own) with a Mach exception port,
   behaviour EXCEPTION_STATE_IDENTITY with the i386 thread state. The kernel cannot report an i386
   state for a 64-bit thread, so the exception ports the 32-bit world sets are kept here, and a fault
   in 32-bit code (seen by the host's signal handler) is sent to that port as Snow Leopard's kernel
   would have sent it; the handler's reply gives the state to continue with. */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>

#define NEXC 14
typedef struct { mach_port_t port; int behavior, flavor; } exc_handler_t;
static exc_handler_t g_exc[NEXC];          /* by exception type; task and thread ports alike */

extern mach_port_name_t mach_reply_port(void);
static u32 rdu(const unsigned char *p) { u32 v; memcpy(&v, p, 4); return v; }

/* task/thread_set/swap/get_exception_ports from the 32-bit world: returns 1 with a reply in out */
int exc_routine(mach_msg_header_t *q, unsigned char *out, mach_msg_size_t *rsz) {
    int id = q->msgh_id;
    unsigned char *b = (unsigned char *)q;
    if (id != 3413 && id != 3415 && id != 3613 && id != 3615 && id != 3414 && id != 3614) return 0;
    mach_msg_header_t *r = (mach_msg_header_t *)out;
    if (id == 3413 || id == 3415 || id == 3613 || id == 3615) {
        if (!(q->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return 0;
        mach_port_t port = rdu(b + 28);
        unsigned disp = b[38];
        u32 mask = rdu(b + 48); int beh = (int)rdu(b + 52), fl = (int)rdu(b + 56);
        for (int t = 1; t < NEXC; t++) if (mask & (1u << t)) {
            if (g_exc[t].port) mach_port_deallocate(mach_task_self(), g_exc[t].port);
            g_exc[t].port = port; g_exc[t].behavior = beh; g_exc[t].flavor = fl;
            /* keep a send right of our own for each type the port serves */
            if (port) {
                if (disp == MACH_MSG_TYPE_MAKE_SEND) mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
                else mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
            }
        }
        if (g_trace || getenv("R2_EXCLOG")) rlog("[exc] %s_exception_ports mask %#x port %#x behavior %#x flavor %d", id < 3600 ? "task" : "thread", mask, port, beh, fl);
        if (id == 3413 || id == 3613) {                 /* set: RetCode only */
            memcpy(out + 24, &NDR_record, 8); *(int *)(out + 32) = KERN_SUCCESS; *rsz = 36;
            return 1;
        }
    }
    /* swap / get: no earlier handlers are reported. 32 port descriptors, NDR, masksCnt 0. */
    r->msgh_bits = MACH_MSGH_BITS_COMPLEX;
    *(u32 *)(out + 24) = 32;
    for (int k = 0; k < 32; k++) {
        mach_msg_port_descriptor_t *d = (void *)(out + 28 + 12 * k);
        d->name = MACH_PORT_NULL; d->disposition = MACH_MSG_TYPE_MOVE_SEND; d->type = MACH_MSG_PORT_DESCRIPTOR;
    }
    memcpy(out + 412, &NDR_record, 8);
    *(u32 *)(out + 420) = 0;
    *rsz = 424;
    return 1;
}

/* the i386 thread state (flavor 1) of a trapped thread */
static void state_get(const ss64_t *s, u32 *st) {
    st[0] = EAX(s); st[1] = EBX(s); st[2] = ECX(s); st[3] = EDX(s); st[4] = EDI(s); st[5] = ESI(s); st[6] = EBP(s); st[7] = ESP(s);
    st[8] = g_ds32; st[9] = (u32)s->__rflags; st[10] = EIP(s); st[11] = g_cs32; st[12] = g_ds32; st[13] = g_ds32;
    st[14] = (u32)s->__fs; st[15] = (u32)s->__gs;
}
static void state_set(ss64_t *s, const u32 *st) {
    s->__rax = st[0]; s->__rbx = st[1]; s->__rcx = st[2]; s->__rdx = st[3]; s->__rdi = st[4]; s->__rsi = st[5]; s->__rbp = st[6];
    s->__rsp = st[7]; s->__rflags = (s->__rflags & ~0xdd5ull) | (st[9] & 0xdd5); s->__rip = st[10];
}

/* ---- thread_get_state / thread_set_state (MIG 3603 / 3604) with the i386 flavors, which the kernel
   refuses for a 64-bit thread. A thread running 32-bit code (suspended by the caller) has its state
   in the kernel; a thread inside a trap has it in the signal frame of the trap. */
#define F32_THREAD 1
#define F32_FLOAT 2
#define F32_EXC 3
#define F_THREAD 7
#define F_FLOAT 8
#define F_EXC 9
#define F32_DEBUG 10
#define F_DEBUG 12
typedef struct { ss64_t *ss; struct __darwin_x86_float_state64 *fs; struct __darwin_x86_exception_state64 *es;
                 x86_thread_state64_t kss; x86_float_state64_t kfs; int live, intrap, pending; mach_port_t th; uint64_t tid; } tstate_t;
static int ts_load(mach_port_t th, tstate_t *t) {
    memset(t, 0, sizeof *t);
    t->th = th;
    mach_port_t self = mach_thread_self(); mach_port_deallocate(mach_task_self(), self);
    thread_identifier_info_data_t ii; mach_msg_type_number_t n = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(th, THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &n) != KERN_SUCCESS) return KERN_INVALID_ARGUMENT;
    ucontext_t *uc = th == self ? cur_uc() : 0;
    if (th != self) {
        n = x86_THREAD_STATE64_COUNT;
        if (thread_get_state(th, x86_THREAD_STATE64, (thread_state_t)&t->kss, &n) == KERN_SUCCESS && t->kss.__cs == g_cs32) {
            n = x86_FLOAT_STATE64_COUNT;
            thread_get_state(th, x86_FLOAT_STATE64, (thread_state_t)&t->kfs, &n);
            t->ss = (ss64_t *)&t->kss; t->fs = (void *)&t->kfs; t->live = 1;
            return KERN_SUCCESS;
        }
        uc = thread_trap_uc(ii.thread_id);
        t->intrap = uc != 0; t->tid = ii.thread_id;
        if (!uc) {   /* created and not started yet (or created suspended): the registers it will start with */
            extern int thread_pending_state(uint64_t tid, u32 *eip, u32 *esp, u32 *regs6);
            u32 e, sp, r6[6];
            if (thread_pending_state(ii.thread_id, &e, &sp, r6)) {
                t->kss.__rip = e; t->kss.__rsp = sp; t->kss.__rbp = 0;
                t->kss.__rax = r6[0]; t->kss.__rbx = r6[1]; t->kss.__rcx = r6[2]; t->kss.__rdx = r6[3]; t->kss.__rdi = r6[4]; t->kss.__rsi = r6[5];
                t->kss.__rflags = 0x202; t->kss.__cs = g_cs32;
                memset(&t->kfs, 0, sizeof t->kfs);
                *(unsigned short *)&t->kfs.__fpu_fcw = 0x37f; t->kfs.__fpu_mxcsr = 0x1f80;
                t->ss = (ss64_t *)&t->kss; t->fs = (void *)&t->kfs; t->pending = 1;
                return KERN_SUCCESS;
            }
        }
    }
    if (!uc) return KERN_FAILURE;                  /* in host code outside a trap: no 32-bit state now */
    t->ss = &uc->uc_mcontext->__ss; t->fs = (void *)&uc->uc_mcontext->__fs; t->es = (void *)&uc->uc_mcontext->__es;
    if (uc->uc_mcsize >= sizeof(struct __darwin_mcontext64_full))   /* full layout: longer thread state (as sig.c host_fpu) */
        t->fs = (void *)&((struct __darwin_mcontext64_full *)uc->uc_mcontext)->__fs;
    return KERN_SUCCESS;
}
/* the float state: the i386 layout is the 64-bit one up to xmm7 (296 bytes); 524 bytes in all */
static void fs_get(const tstate_t *t, unsigned char *o) {
    memset(o, 0, 524);
    if (t->fs) memcpy(o, t->fs, 296);
}
static void fs_set(tstate_t *t, const unsigned char *in) {
    if (t->fs) memcpy(t->fs, in, 296);
}
static void ex_get(const tstate_t *t, u32 *o) {
    o[0] = o[1] = o[2] = 0;
    if (t->es) { o[0] = t->es->__trapno | ((u32)t->es->__cpu << 16); o[1] = t->es->__err; o[2] = (u32)t->es->__faultvaddr; }
}
static void state_get(const ss64_t *s, u32 *st);
static void state_set(ss64_t *s, const u32 *st);
int thread_state_routine(void *msg, unsigned char *out, unsigned int *rsz, unsigned int room) {
    mach_msg_header_t *q = msg;
    unsigned char *b = (unsigned char *)q;
    int id = q->msgh_id;
    if (id != 3603 && id != 3604) return 0;
    int flavor = (int)rdu(b + 32);
    u32 cnt = rdu(b + 36);
    tstate_t t;
    {   static int nlog;
        if (g_trace && nlog < 40) { nlog++; rlog("[ts] thread_%s_state(%#x, flavor %d, count %u)", id == 3603 ? "get" : "set", q->msgh_remote_port, flavor, cnt); }
    }
    kern_return_t kr = ts_load(q->msgh_remote_port, &t);
    memcpy(out + 24, &NDR_record, 8);
    *rsz = 36;
    if (kr && getenv("R2_TSLOG")) rlog("[tslog]   %s %#x flavor %d -> %#x (no 32-bit state)", id == 3603 ? "get" : "set", q->msgh_remote_port, flavor, kr);
    if (kr) { *(int *)(out + 32) = kr; return 1; }
    u32 st[256]; u32 n = 0;
    memset(st, 0, sizeof st);
    if (id == 3603) {                               /* get: RetCode, old_stateCnt, old_state */
        switch (flavor) {
        case F32_THREAD: state_get(t.ss, st); n = 16; break;
        case F_THREAD: st[0] = F32_THREAD; st[1] = 16; state_get(t.ss, st + 2); n = 44; break;
        case F32_FLOAT: fs_get(&t, (unsigned char *)st); n = 131; break;
        case F_FLOAT: st[0] = F32_FLOAT; st[1] = 131; fs_get(&t, (unsigned char *)(st + 2)); n = 133; break;
        case F32_EXC: ex_get(&t, st); n = 3; break;
        case F_EXC: st[0] = F32_EXC; st[1] = 3; ex_get(&t, st + 2); n = 6; break;
        case F32_DEBUG: n = 8; break;
        case F_DEBUG: st[0] = F32_DEBUG; st[1] = 8; n = 10; break;
        default: *(int *)(out + 32) = KERN_INVALID_ARGUMENT; return 1;
        }
        if (n > cnt || 40 + n * 4 > room) { *(int *)(out + 32) = KERN_INVALID_ARGUMENT; return 1; }
        if (getenv("R2_TSLOG") && (flavor == F32_THREAD || flavor == F_THREAD))
            rlog("[tslog]   get %#x flavor %d: eip %#x esp %#x (%s)", q->msgh_remote_port, flavor, st[flavor == F_THREAD ? 2 + 10 : 10], st[flavor == F_THREAD ? 2 + 7 : 7],
                 t.live ? "running" : t.intrap ? "in a trap" : "self");
        *(int *)(out + 32) = KERN_SUCCESS; *(u32 *)(out + 36) = n;
        memcpy(out + 40, st, n * 4);
        *rsz = 40 + n * 4;
        return 1;
    }
    /* set: RetCode */
    const u32 *in = (const u32 *)(b + 40);
    int ok = 1;
    if (flavor == F32_THREAD && cnt >= 16) state_set(t.ss, in);
    else if (flavor == F_THREAD && cnt >= 18 && in[0] == F32_THREAD) state_set(t.ss, in + 2);
    else if (flavor == F32_FLOAT && cnt >= 74) fs_set(&t, (const unsigned char *)in);
    else if (flavor == F_FLOAT && cnt >= 76 && in[0] == F32_FLOAT) fs_set(&t, (const unsigned char *)(in + 2));
    else if (flavor == F32_DEBUG || flavor == F_DEBUG || flavor == F32_EXC || flavor == F_EXC) ;   /* accepted, ignored */
    else ok = 0;
    if (getenv("R2_TSLOG")) rlog("[tslog]   set %#x flavor %d (%s)%s", q->msgh_remote_port, flavor, t.live ? "running" : t.intrap ? "in a trap" : "self", ok ? "" : " REFUSED");
    if (ok && t.intrap && (flavor == F32_THREAD || flavor == F_THREAD)) {
        extern void thread_trap_state_set(uint64_t tid, const ss64_t *s);
        thread_trap_state_set(t.tid, t.ss);
    }
    if (ok && t.pending && (flavor == F32_THREAD || flavor == F_THREAD)) {
        extern int thread_pending_update(uint64_t tid, u32 eip, u32 esp, const u32 *regs6);
        u32 r6[6] = { (u32)t.kss.__rax, (u32)t.kss.__rbx, (u32)t.kss.__rcx, (u32)t.kss.__rdx, (u32)t.kss.__rdi, (u32)t.kss.__rsi };
        thread_pending_update(t.tid, (u32)t.kss.__rip, (u32)t.kss.__rsp, r6);
    }
    if (ok && t.live) {
        thread_set_state(t.th, x86_THREAD_STATE64, (thread_state_t)&t.kss, x86_THREAD_STATE64_COUNT);
        if (flavor == F32_FLOAT || flavor == F_FLOAT) thread_set_state(t.th, x86_FLOAT_STATE64, (thread_state_t)&t.kfs, x86_FLOAT_STATE64_COUNT);
    }
    *(int *)(out + 32) = ok ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
    return 1;
}

/* the exception a fault in 32-bit code raised on Snow Leopard (osfmk/i386/trap.c, user_trap) */
static int exc_of(int sig, const ucontext_t *uc, int64_t *code, int64_t *sub) {
    unsigned trapno = uc->uc_mcontext->__es.__trapno, err = uc->uc_mcontext->__es.__err;
    *sub = 0;
    switch (trapno) {
    case 0:  *code = EXC_I386_DIV; return EXC_ARITHMETIC;
    case 4:  *code = EXC_I386_INTO; return EXC_ARITHMETIC;
    case 16: *code = EXC_I386_EXTERR; return EXC_ARITHMETIC;
    case 19: *code = EXC_I386_SSEEXTERR; return EXC_ARITHMETIC;
    case 1:  *code = EXC_I386_SGL; return EXC_BREAKPOINT;
    case 3:  *code = EXC_I386_BPT; return EXC_BREAKPOINT;
    case 5:  *code = EXC_I386_BOUND; return EXC_ARITHMETIC;
    case 6:  *code = EXC_I386_INVOP; return EXC_BAD_INSTRUCTION;
    case 13: *code = EXC_I386_GPFLT; return EXC_BAD_ACCESS;
    case 12: *code = EXC_I386_STKFLT; return EXC_BAD_ACCESS;
    case 11: *code = EXC_I386_SEGNPFLT; return EXC_BAD_ACCESS;
    case 14: *code = (err & 1) ? KERN_PROTECTION_FAILURE : KERN_INVALID_ADDRESS;
             *sub = (int64_t)(u32)uc->uc_mcontext->__es.__faultvaddr; return EXC_BAD_ACCESS;
    }
    if (sig == SIGTRAP) { *code = EXC_I386_BPT; return EXC_BREAKPOINT; }
    return 0;
}

static __thread mach_port_t t_reply;

/* send a fault of 32-bit code to the 32-bit world's exception port; 1 if its handler took it */
int exc32_raise(int sig, ss64_t *s, ucontext_t *uc) {
    int64_t code, sub;
    int exc = exc_of(sig, uc, &code, &sub);
    if (exc <= 0 || exc >= NEXC || !g_exc[exc].port) { if (getenv("R2_EXCLOG")) rlog("[exc] fault (exception %d) at %#x: no port", exc, EIP(s)); return 0; }
    exc_handler_t h = g_exc[exc];
    int beh = h.behavior & ~MACH_EXCEPTION_CODES, wide = (h.behavior & MACH_EXCEPTION_CODES) != 0;
    if (!t_reply) t_reply = mach_reply_port();
    union { mach_msg_header_t h; unsigned char b[1024]; } m;
    memset(&m, 0, sizeof m);
    unsigned char *b = m.b;
    u32 off = 24;
    mach_port_t thr = mach_thread_self();
    if (beh == EXCEPTION_DEFAULT || beh == EXCEPTION_STATE_IDENTITY) {
        *(u32 *)(b + 24) = 2;
        mach_msg_port_descriptor_t *d = (void *)(b + 28);
        d[0].name = thr; d[0].disposition = MACH_MSG_TYPE_COPY_SEND; d[0].type = MACH_MSG_PORT_DESCRIPTOR;
        d[1].name = mach_task_self(); d[1].disposition = MACH_MSG_TYPE_COPY_SEND; d[1].type = MACH_MSG_PORT_DESCRIPTOR;
        off = 52;
    }
    memcpy(b + off, &NDR_record, 8); off += 8;
    *(int *)(b + off) = exc; off += 4;
    *(u32 *)(b + off) = 2; off += 4;
    if (wide) { memcpy(b + off, &code, 8); memcpy(b + off + 8, &sub, 8); off += 16; }
    else { *(int *)(b + off) = (int)code; *(int *)(b + off + 4) = (int)sub; off += 8; }
    u32 st[18], cnt = 0;
    if (beh != EXCEPTION_DEFAULT) {
        if (h.flavor == 1) { state_get(s, st); cnt = 16; }
        else if (h.flavor == 7) { st[0] = 1; st[1] = 16; state_get(s, st + 2); cnt = 18; }
        else { rlog("[exc] state flavor %d not supported", h.flavor); mach_port_deallocate(mach_task_self(), thr); return 0; }
        *(int *)(b + off) = h.flavor; off += 4;
        *(u32 *)(b + off) = cnt; off += 4;
        memcpy(b + off, st, cnt * 4); off += cnt * 4;
    }
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) |
                    (beh == EXCEPTION_STATE ? 0 : MACH_MSGH_BITS_COMPLEX);
    m.h.msgh_size = off; m.h.msgh_remote_port = h.port; m.h.msgh_local_port = t_reply;
    m.h.msgh_id = (wide ? 2405 : 2401) + (beh - 1);
    if (g_trace || getenv("R2_EXCLOG")) rlog("[exc] exception %d code %#llx %#llx at %#x -> port %#x (msg %d)", exc, (long long)code, (long long)sub, EIP(s), h.port, m.h.msgh_id);
    kern_return_t kr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG, off, sizeof m, t_reply, 0, 0);
    mach_port_deallocate(mach_task_self(), thr);
    if (kr) { rlog("[exc] exception message failed: %#x", kr); return 0; }
    int rc = *(int *)(b + 32);
    if (rc) { if (g_trace || getenv("R2_EXCLOG")) rlog("[exc] handler declined (%d)", rc); return 0; }
    if (beh != EXCEPTION_DEFAULT) {
        int fl = *(int *)(b + 36); u32 n = *(u32 *)(b + 40);
        const u32 *ns = (const u32 *)(b + 44);
        if (fl == 7 && n >= 18) ns += 2; else if (fl != 1 || n < 16) { rlog("[exc] reply state flavor %d count %u", fl, n); return 1; }
        state_set(s, ns);
    }
    if (g_trace || getenv("R2_EXCLOG")) rlog("[exc] handled, continuing at %#x", EIP(s));
    return 1;
}
