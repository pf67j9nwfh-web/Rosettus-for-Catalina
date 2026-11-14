/* sig.c - signals of the 32-bit world.
   The host keeps its own handlers (r2_sigentry) for every signal the 32-bit world catches. A signal
   that interrupts 32-bit code is delivered as Snow Leopard's kernel did for an i386 process
   (bsd/dev/i386/unix_signal.c, sendsig): a frame with the i386 machine context, ucontext and siginfo
   is built on the 32-bit stack (or its alternate stack), and the thread continues at the trampoline
   libSystem registered (_sigtramp), which calls the handler and then sigreturn (syscall 184).
   A signal that arrives while the thread runs host code (a system call being carried out) is kept
   pending and delivered on the way back to 32-bit code. The signal mask of a thread in 32-bit code is
   the mask of the host signal frame it returns through (sigprocmask changes that frame). */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <pthread.h>
#include <unistd.h>

#pragma pack(push, 4)
struct mctx32 {                        /* _STRUCT_MCONTEXT32 (10.6) */
    u32 trapno, err, faultvaddr;
    u32 eax, ebx, ecx, edx, edi, esi, ebp, esp, ss, eflags, eip, cs, ds, es, fs, gs;
    unsigned char fpu[524];
};
struct uctx32 { int onstack; u32 sigmask, ss_sp, ss_size; int ss_flags; u32 link, mcsize, mcontext; };
struct sinfo32 { int signo, err, code, pid, uid, status; u32 addr, value; int band; u32 pad[7]; };
struct frame32 { u32 retaddr, catcher; int sigstyle, sig; u32 sinfo, uctx; };
#pragma pack(pop)
#define UC_TRAD 1
#define UC_FLAVOR 30
#define UC_SET_ALT_STACK 0x40000000
#define UC_RESET_ALT_STACK 0x80000000
#define FPU_COPY 296                    /* reserved words + fxsave image up to xmm7: the same in both layouts */

static __thread u32 t_pending;          /* signals that arrived in host code */
static __thread siginfo_t t_pinfo[32];
static __thread int t_onstack;          /* on the 32-bit alternate stack */

static unsigned char *host_fpu(ucontext_t *uc) {
    if (uc->uc_mcsize >= sizeof(struct __darwin_mcontext64_full))
        return (unsigned char *)&((struct __darwin_mcontext64_full *)uc->uc_mcontext)->__fs;
    return (unsigned char *)&uc->uc_mcontext->__fs;
}

static int host_owns(int sig) { return sig == SIGILL || sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGTRAP; }
static void mask_fix(sigset_t *m) { sigdelset(m, SIGILL); sigdelset(m, SIGSEGV); sigdelset(m, SIGBUS); sigdelset(m, SIGFPE); sigdelset(m, SIGTRAP); }

/* deliver sig to the 32-bit handler; 0 if it has none */
int sig32_deliver(int sig, const siginfo_t *si, ss64_t *s, ucontext_t *uc) {
    sigact32_t *a = &g_sig32[sig];
    if (a->handler <= 1 || !a->tramp) return 0;
    /* the host's own signal frame for this trap lies just below the 32-bit stack pointer: build
       the 32-bit frame below it (and below the few words r2_sigentry pushed there) */
    u32 sp = ESP(s);
    const void *hp[3] = { uc, uc->uc_mcontext, si };
    for (int k = 0; k < 3; k++) {
        uintptr_t p = (uintptr_t)hp[k];
        if (p && p < sp && sp - p < 0x10000) sp = (u32)p;
    }
    if (sp != ESP(s)) sp -= 512;
    int was_onstack = t_onstack;
    if ((a->flags & SA_ONSTACK) && !t_onstack && g_sigaltstack32[1] && !(g_sigaltstack32[2] & SS_DISABLE)) {
        sp = g_sigaltstack32[0] + g_sigaltstack32[1];
        t_onstack = 1;
    }
    sp -= sizeof(struct mctx32); sp &= ~15u; u32 mc = sp;
    sp -= sizeof(struct uctx32); u32 ucx = sp;
    sp -= sizeof(struct sinfo32); u32 sip = sp;
    u32 fp = ((sp - sizeof(struct frame32)) & ~15u) - 4;   /* as after a call: fp + 4 is 16-byte aligned */

    struct mctx32 *m = P32(mc);
    memset(m, 0, sizeof *m);
    m->trapno = uc->uc_mcontext->__es.__trapno; m->err = uc->uc_mcontext->__es.__err;
    m->faultvaddr = (u32)uc->uc_mcontext->__es.__faultvaddr;
    m->eax = EAX(s); m->ebx = EBX(s); m->ecx = ECX(s); m->edx = EDX(s); m->edi = EDI(s); m->esi = ESI(s);
    m->ebp = EBP(s); m->esp = ESP(s); m->eip = EIP(s); m->eflags = (u32)s->__rflags;
    m->cs = g_cs32; m->ss = m->ds = m->es = g_ds32; m->fs = (u32)s->__fs; m->gs = (u32)s->__gs;
    memcpy(m->fpu, host_fpu(uc), FPU_COPY);

    struct uctx32 *u = P32(ucx);
    memset(u, 0, sizeof *u);
    u->onstack = was_onstack; u->sigmask = (u32)uc->uc_sigmask;
    u->ss_sp = g_sigaltstack32[0]; u->ss_size = g_sigaltstack32[1]; u->ss_flags = (int)g_sigaltstack32[2] | (was_onstack ? SS_ONSTACK : 0);
    u->mcsize = sizeof *m; u->mcontext = mc;

    struct sinfo32 *i = P32(sip);
    memset(i, 0, sizeof *i);
    i->signo = sig;
    if (si) {
        i->err = si->si_errno; i->code = si->si_code; i->pid = si->si_pid; i->uid = (int)si->si_uid; i->status = si->si_status;
        i->addr = (u32)(uintptr_t)si->si_addr; i->value = (u32)si->si_value.sival_int; i->band = (int)si->si_band;
    }

    struct frame32 *f = P32(fp);
    f->retaddr = 0xffffffffu; f->catcher = a->handler; f->sigstyle = (a->flags & SA_SIGINFO) ? UC_FLAVOR : UC_TRAD;
    f->sig = sig; f->sinfo = sip; f->uctx = ucx;

    s->__rip = a->tramp; s->__rsp = fp;
    s->__rflags &= ~0x500ull;                         /* TF, DF */
    /* the handler runs with its mask added (and the signal itself unless SA_NODEFER) */
    sigset_t nm = uc->uc_sigmask | (sigset_t)a->mask;
    if (!(a->flags & SA_NODEFER)) sigaddset(&nm, sig);
    mask_fix(&nm);
    uc->uc_sigmask = nm;
    if (a->flags & SA_RESETHAND) { a->handler = 0; if (!host_owns(sig)) signal(sig, SIG_DFL); }
    if (g_trace || r2_siglog()) rlog("[sig] signal %d -> handler %#x via %#x, frame %#x (eip was %#x) thread %#x", sig, a->handler, a->tramp, fp, m->eip, pthread_mach_thread_np(pthread_self()));
    return 1;
}

/* sigreturn(uctx, infostyle) */
void sig32_return(ss64_t *s, ucontext_t *uc, u32 ucx, u32 style) {
    if (style == UC_SET_ALT_STACK) { t_onstack = 1; return; }
    if (style == UC_RESET_ALT_STACK) { t_onstack = 0; return; }
    struct uctx32 *u = P32(ucx);
    struct mctx32 *m = P32(u->mcontext);
    t_onstack = u->onstack & 1;
    s->__rax = m->eax; s->__rbx = m->ebx; s->__rcx = m->ecx; s->__rdx = m->edx; s->__rdi = m->edi; s->__rsi = m->esi;
    s->__rbp = m->ebp; s->__rsp = m->esp; s->__rip = m->eip;
    s->__rflags = (s->__rflags & ~0xdd5ull) | (m->eflags & 0xdd5);     /* CF PF AF ZF SF TF DF OF */
    s->__rflags &= ~0x100ull;
    memcpy(host_fpu(uc), m->fpu, FPU_COPY);
    sigset_t nm = (sigset_t)u->sigmask; mask_fix(&nm);
    uc->uc_sigmask = nm;
    if (g_trace || r2_siglog()) rlog("[sig] sigreturn to %#x thread %#x", m->eip, pthread_mach_thread_np(pthread_self()));
}

/* a signal for the 32-bit world while the thread is in host code */
void sig32_pend(int sig, const siginfo_t *si) {
    if (sig <= 0 || sig >= 32) return;
    t_pending |= 1u << sig;
    if (si) t_pinfo[sig] = *si;
}

/* R2_SIGLOG=1: signal sends, arrivals, deliveries and returns, with thread and time (no other tracing) */
int r2_siglog(void) { static int on = -1; if (on < 0) on = getenv("R2_SIGLOG") != 0; return on; }

/* is a signal pending for the 32-bit world that the current trap's return would deliver? */
int sig32_deliverable(void) {
    ucontext_t *uc = cur_uc();
    if (!t_pending || !uc) return 0;
    for (int k = 1; k < 32; k++) if ((t_pending & (1u << k)) && !sigismember(&uc->uc_sigmask, k)) return 1;
    return 0;
}

/* on the way back into 32-bit code: deliver what is pending and not blocked */
void sig32_deliver_pending(ss64_t *s, ucontext_t *uc) {
    while (t_pending) {
        int sig = 0;
        for (int k = 1; k < 32; k++) if ((t_pending & (1u << k)) && !sigismember(&uc->uc_sigmask, k)) { sig = k; break; }
        if (!sig) return;
        t_pending &= ~(1u << sig);
        if (sig32_deliver(sig, &t_pinfo[sig], s, uc)) continue;
        if (g_sig32[sig].handler == 0 && sig != SIGCHLD && sig != SIGURG && sig != SIGWINCH && sig != SIGCONT && sig != SIGINFO) {
            if (g_trace) rlog("[sig] signal %d: default action", sig);    /* as the kernel would: terminate */
            signal(sig, SIG_DFL);
            sigset_t one; sigemptyset(&one); sigaddset(&one, sig); pthread_sigmask(SIG_UNBLOCK, &one, 0);
            kill(getpid(), sig);
        }
    }
}
