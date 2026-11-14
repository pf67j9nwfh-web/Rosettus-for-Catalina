/* trap.c - LDT segments, entering 32-bit code, and the signal handler that turns the 32-bit world's
   traps (int 0x80 BSD, int 0x81 Mach, int 0x82 machine-dependent) into host calls. */
#include "rhost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <libkern/OSByteOrder.h>
#include <architecture/i386/table.h>
#include <i386/user_ldt.h>

unsigned short g_cs32, g_cs64, g_ds32;
u32 g_stub_popgs, g_stub_fixds;

static void make_desc(union ldt_entry *d, u32 base, int code) {
    memset(d, 0, sizeof *d);
    d->code.limit00 = 0xffff; d->code.limit16 = 0xf;
    d->code.base00 = base & 0xffff; d->code.base16 = (base >> 16) & 0xff; d->code.base24 = base >> 24;
    d->code.type = code ? DESC_CODE_READ : DESC_DATA_WRITE;
    d->code.dpl = USER_PRIV; d->code.present = 1;
    d->code.opsz = DESC_CODE_32B; d->code.granular = DESC_GRAN_PAGE;
}

/* LDT entries for the threads' %gs. Each 32-bit thread gets its own data segment; they used to leak
   at thread exit, and after ~4,000 threads (a program that starts many short threads) the table was
   full: new threads ran without a usable %gs, faulted on every access and the process span forever
   (tsdtest: 4,095 threads). The entries of ended threads are now reused (ldt_release), and an entry
   is changed in place when its thread sets a new base. A new entry is allocated with a spare one
   after it: the last entry of the table is the one whose %gs accesses fault for good (see tsd32_set). */
static pthread_mutex_t g_ldt_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ldt_free[8192], g_ldt_nfree;
static int ldt_alloc_desc(union ldt_entry *d) {
    int idx = -1;
    pthread_mutex_lock(&g_ldt_mu);
    while (g_ldt_nfree > 0 && idx < 0) {
        int c = g_ldt_free[--g_ldt_nfree];
        if (i386_set_ldt(c, d, 1) >= 0) idx = c;
    }
    pthread_mutex_unlock(&g_ldt_mu);
    if (idx >= 0) return idx;
    if (!getenv("R2_LDT_NOSPARE")) {
        union ldt_entry two[2]; two[0] = *d; two[1] = *d;          /* the spare stays allocated (and valid) */
        idx = i386_set_ldt(LDT_AUTO_ALLOC, two, 2);
        if (idx >= 0) return idx;
    }
    return i386_set_ldt(LDT_AUTO_ALLOC, d, 1);
}
void ldt_release(u32 sel) {
    int idx = (int)(sel >> 3);
    if (!sel || !(sel & 4) || getenv("R2_LDT_NOREUSE")) return;
    pthread_mutex_lock(&g_ldt_mu);
    if (g_ldt_nfree < 8192) g_ldt_free[g_ldt_nfree++] = idx;
    pthread_mutex_unlock(&g_ldt_mu);
}
u32 ldt_data_segment(u32 base) {
    union ldt_entry d; make_desc(&d, base, 0);
    int idx = ldt_alloc_desc(&d);
    if (idx < 0) rfatal("i386_set_ldt (data segment) failed");
    return (u32)((idx << 3) | 4 | 3);
}
/* the same entry with a new base (the calling thread's own selector) */
static int ldt_set_base(u32 sel, u32 base) {
    union ldt_entry d; make_desc(&d, base, 0);
    return i386_set_ldt((int)(sel >> 3), &d, 1) >= 0;
}

/* 32-bit stubs the handler can return through */
static void stubs_init(void) {
    u32 pg = low_alloc(R32_PAGE, 0, 1);
    if (!pg || mmap(P32(pg), R32_PAGE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED)
        rfatal("no page for stubs");
    unsigned char *p = P32(pg);
    static const unsigned char popgs[] = { 0x0f, 0xa9, 0xc3 };                             /* pop gs; ret */
    /* pop gs; push eax; mov ax,ss; mov ds,ax; mov es,ax; pop eax; ret */
    static const unsigned char fixds[] = { 0x0f, 0xa9, 0x50, 0x8c, 0xd0, 0x8e, 0xd8, 0x8e, 0xc0, 0x58, 0xc3 };
    memcpy(p, popgs, sizeof popgs); g_stub_popgs = pg;
    memcpy(p + 16, fixds, sizeof fixds); g_stub_fixds = pg + 16;
    mprotect(p, R32_PAGE, PROT_READ | PROT_EXEC);
}

/* push a return address (and optional word) on the 32-bit stack and continue at a stub */
void set_gs_via_stub(ss64_t *s, unsigned short sel) {
    u32 sp = ESP(s) - 8;
    wr32(sp, sel); wr32(sp + 4, EIP(s));
    s->__rsp = sp; s->__rip = g_stub_popgs;
}
/* reload gs (with sel), ds and es, then retry the instruction */
static void fix_segs_via_stub(ss64_t *s, unsigned short sel) {
    u32 sp = ESP(s) - 8;
    wr32(sp, sel); wr32(sp + 4, EIP(s));
    s->__rsp = sp; s->__rip = g_stub_fixds;
}

/* the 32-bit image holding address a: the Mach-O header at or below it, and its install name */
static u32 img_of(u32 a, const char **name) {
    *name = "?";
    for (u32 pg = a & ~0xfffu, n = 0; pg > R32_PAGE && n < 8192; pg -= 0x1000, n++) {
        if (low_is_free(pg, 0x1000)) return 0;
        if (rd32(pg) != 0xfeedface) continue;
        u32 ncmds = rd32(pg + 16), lc = pg + 28;
        for (u32 i = 0; i < ncmds && i < 256; i++) {
            u32 cmd = rd32(lc), sz = rd32(lc + 4);
            if (cmd == 0xd /* LC_ID_DYLIB */ || cmd == 0xe /* LC_LOAD_DYLINKER */) { *name = (const char *)P32(lc + rd32(lc + 8)); break; }
            if (sz < 8) break;
            lc += sz;
        }
        if (rd32(pg + 12) == 2 /* MH_EXECUTE */) *name = "(main executable)";
        return pg;
    }
    return 0;
}

static void dump_state(const char *why, int sig, siginfo_t *si, ss64_t *s) {
    char m[512];
    int n = snprintf(m, sizeof m, "[rhost] %s: signal %d code %#x addr %p cs %#llx eip %#x esp %#x eax %#x ebx %#x ecx %#x edx %#x esi %#x edi %#x ebp %#x\n",
                     why, sig, si ? si->si_code : -1, si ? si->si_addr : 0, (unsigned long long)s->__cs, EIP(s), ESP(s), EAX(s), EBX(s), ECX(s), EDX(s), ESI(s), EDI(s), EBP(s));
    write(2, m, n);
    if (s->__cs != g_cs32) {                          /* host code: rbp chain */
        uint64_t bp = s->__rbp;
        n = snprintf(m, sizeof m, "[rhost] host rip %#llx rsp %#llx, frames:", (unsigned long long)s->__rip, (unsigned long long)s->__rsp);
        for (int k = 0; k < 12 && bp > 0x100000 && n < 440; k++) {
            n += snprintf(m + n, sizeof m - n, " %#llx", (unsigned long long)((uint64_t *)bp)[1]);
            uint64_t nb = ((uint64_t *)bp)[0]; if (nb <= bp) break; bp = nb;
        }
        n += snprintf(m + n, sizeof m - n, " (rhost text at %p)\n", (void *)&dump_state);
        write(2, m, n);
        if (si && si->si_addr) {                      /* is the fault address mapped at all? */
            mach_vm_address_t ra = (mach_vm_address_t)(uintptr_t)si->si_addr; mach_vm_size_t rs = 0;
            vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on = 0;
            kern_return_t kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &on);
            n = snprintf(m, sizeof m, "[rhost] fault address %p: next region %#llx-%#llx prot %d/%d (kr %d)\n", si->si_addr,
                         (unsigned long long)ra, (unsigned long long)(ra + rs), kr ? -1 : bi.protection, kr ? -1 : bi.max_protection, kr);
            write(2, m, n);
        }
    }
    if (s->__cs == g_cs32) {
        unsigned char *ip = P32(EIP(s));
        n = snprintf(m, sizeof m, "[rhost] code at eip: %02x %02x %02x %02x | %02x %02x %02x %02x; stack: %08x %08x %08x %08x\n",
                     ip[-4], ip[-3], ip[-2], ip[-1], ip[0], ip[1], ip[2], ip[3], rd32(ESP(s)), rd32(ESP(s) + 4), rd32(ESP(s) + 8), rd32(ESP(s) + 12));
        write(2, m, n);
        {   /* the 32-bit frame chain (ebp) */
            u32 bp = EBP(s);
            n = snprintf(m, sizeof m, "[rhost] 32-bit frames:");
            for (int k = 0; k < 12 && bp > 0x1000 && bp < 0xfffff000u && n < 440; k++) {
                if (low_is_free(bp & ~0xfffu, 0x1000)) break;
                const char *nm; u32 ra = rd32(bp + 4), base = img_of(ra, &nm);
                const char *sl = strrchr(nm, '/');
                n += snprintf(m + n, sizeof m - n, " %#x(%s+%#x)", ra, sl ? sl + 1 : nm, base ? ra - base : 0);
                u32 nb = rd32(bp); if (nb <= bp) break; bp = nb;
            }
            n += snprintf(m + n, sizeof m - n, "\n"); write(2, m, n);
        }
        for (u32 a = ESP(s); a < ESP(s) + 64; a += 4) {        /* a message on the stack (abort reasons) */
            const unsigned char *t = P32(a); int k = 0;
            while (k < 200 && t[k] >= 0x20 && t[k] < 0x7f) k++;
            if (k >= 12) { n = snprintf(m, sizeof m, "[rhost] text on the stack: %.*s\n", k, (const char *)t); write(2, m, n); break; }
        }
    }
}

static __thread u32 t_last_fault_eip, t_last_fault_n;

/* %gs: 64-bit code on macOS finds its thread data through the %gs base, and so does Snow Leopard's
   i386 libSystem. In a process with 32-bit code segments the kernel saves the full thread state in
   the signal frame (x86_THREAD_FULL_STATE64: also ds, es, ss and the %gs base). The signal entry
   (sigentry.s) finds the thread's record by thread id, switches to the host stack and the host's
   %gs base before any C code runs, and sets the 32-bit world's base again on the way back; the frame
   carries the same values, with a %gs selector whose LDT base is the 32-bit thread data. */
typedef struct { ss64_t ss64; uint64_t ds, es, ss, gsbase; } full64_t;
static __thread full64_t *t_full;          /* full state of the signal being handled, or 0 */
typedef struct { uint64_t host_tsd, tsd32, stack_top, depth, tid;      /* layout used by sigentry.s */
                 ucontext_t *volatile uc; volatile u32 trap;          /* for the watchdog: the trap being handled */
                 volatile int ts_set; ss64_t ts_ss;                    /* a thread_set_state that arrived during the trap */
                 mach_port_t port;                                      /* the thread (for the watchdog) */
                 volatile u32 msg_id, msg_port, msg_opt, msg_rcv; } r2thr_t;   /* the Mach message it is in (emu_mach_msg) */
static __thread r2thr_t *t_alt;
#define HOST_STACK (8u << 20)          /* the host side of a trap may run framework code (hostcalls) */
#define THR_SLOTS 4096
static r2thr_t *volatile g_thr[THR_SLOTS];

/* called from sigentry.s before %gs is the host's: no thread-local storage, no library calls */
r2thr_t *r2_thread_lookup(uint64_t tid);
r2thr_t *r2_thread_lookup(uint64_t tid) {
    for (unsigned i = 0, h = (unsigned)(tid * 2654435761u) % THR_SLOTS; i < THR_SLOTS; i++, h = (h + 1) % THR_SLOTS) {
        r2thr_t *t = g_thr[h];
        if (!t) return 0;
        if (t->tid == tid) return t;
    }
    return 0;
}

static uint64_t find_host_tsd(void) {
    uint64_t self, g1, g2;
    __asm__ volatile("mov %%gs:0, %0; mov %%gs:8, %1; mov %%gs:16, %2" : "=r"(self), "=r"(g1), "=r"(g2));
    for (uint64_t off = 0; off < 0x2000; off += 8) {           /* tsd[] is inside the pthread structure */
        uint64_t *p = (uint64_t *)(self + off);
        if (p[0] == self && p[1] == g1 && p[2] == g2) return self + off;
    }
    rfatal("cannot find this thread's TSD base");
}

/* the thread's %gs selector in 32-bit code: an LDT data segment based at its thread data */
static __thread unsigned short t_gssel;
static __thread int t_tsd_sets;          /* diagnostics: how often this thread set its %gs base */
unsigned short tsd32_set(u32 base) {
    t_tsd_sets++;
    /* the same base again (libpthread's _pthread_set_self after the thread start already set it):
       keep the thread's selector. A fresh LDT entry each time leaked one per call, and the thread
       then ran on the newest entry of the table - the case in which Build-a-lot 2 and Illustrator
       had %gs fail for good (always the last index: 28, 29, 30). */
    static __thread u32 t_gsbase;
    if (t_gssel && t_gsbase == base && !getenv("R2_NO_GSREUSE")) {
        if (t_alt) t_alt->tsd32 = base;
        if (t_full) { t_full->gsbase = base; t_full->ss64.__gs = t_gssel; }
        return t_gssel;
    }
    t_gsbase = base;
    if (t_alt) t_alt->tsd32 = base;
    if (!(t_gssel && !getenv("R2_LDT_NOREUSE") && ldt_set_base(t_gssel, base)))
        t_gssel = (unsigned short)ldt_data_segment(base);
    if (t_full) { t_full->gsbase = base; t_full->ss64.__gs = t_gssel; }
    return t_gssel;                   /* without a frame (a new thread): enter32 loads the selector */
}
static void on_signal32(int sig, siginfo_t *si, ss64_t *s);
/* a signal sent by kill / pthread_kill rather than raised by a fault: si_code says so for kill; a
   thread signalling itself is interrupted right after its syscall instruction (0f 05) */
static int is_sent(const siginfo_t *si, const ss64_t *s) {
    if (si && (si->si_code == 0 || si->si_code >= 0x10000)) return 1;
    return s->__cs != g_cs32 && s->__rip >= 0x1002 && *(const unsigned short *)(uintptr_t)(s->__rip - 2) == 0x050f;
}
static __thread ucontext_t *t_uc;
ucontext_t *cur_uc(void) { return t_uc; }
/* the host signal mask this thread returns to 32-bit code with (the 32-bit world's mask, which
   sigprocmask changes in the frame the trap returns through): kept for the gate, which has no frame */
static __thread sigset_t t_gate_mask;
static __thread int t_gate_mask_ok;

/* Free 32-bit memory for the 32-bit world (munmap, vm_deallocate). The host's signal frame for the
   trap being handled lies on the 32-bit stack, just below the stack pointer: a thread that frees the
   unused part of its own stack (libpthread does at exit) must not take that away from under the
   handler. Those pages stay mapped (and reserved). */
void free32_user(u32 a, u32 len) {
    u32 e = a + len;
    if (t_uc) {
        u32 sp = (u32)t_uc->uc_mcontext->__ss.__rsp, lo = sp;
        uintptr_t p[2] = { (uintptr_t)t_uc, (uintptr_t)t_uc->uc_mcontext };
        for (int k = 0; k < 2; k++) if (p[k] < sp && sp - p[k] < 0x10000) lo = (u32)p[k];
        /* and the next traps' frames (about 2 KB each, below the stack pointer): keep 16 KB */
        if (lo > sp - 0x4000) lo = sp - 0x4000;
        lo &= ~(R32_PAGE - 1); u32 hi = (sp + R32_PAGE) & ~(R32_PAGE - 1);
        if (a < hi && e > lo) {
            if (a < lo) low_free(a, lo - a);
            if (e > hi) low_free(hi, e - hi);
            if (g_trace) rlog("[mem] kept %#x-%#x (the trap frame is there) of %#x-%#x", lo, hi, a, e);
            return;
        }
    }
    low_free(a, len);
}

/* ---- R2_PROF=seconds: where the 32-bit world's traps go. Every period, the keys (syscall, Mach
   trap, machine-dependent call / hostcall by name) with the most time are logged with their counts.
   Time includes blocking (a mach_msg receive waiting counts as long as it waits). */
int g_prof;
#define PROF_N 4096
static struct { u32 key; uint64_t n, t; } g_profk[PROF_N];
static pthread_mutex_t g_prof_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_prof_traps;
void prof_add(u32 key, uint64_t dt) {
    unsigned h = (key * 2654435761u) % PROF_N, i;
    pthread_mutex_lock(&g_prof_mu);
    for (i = 0; i < PROF_N && g_profk[h].n && g_profk[h].key != key; i++) h = (h + 1) % PROF_N;
    if (i < PROF_N) { g_profk[h].key = key; g_profk[h].n++; g_profk[h].t += dt; }
    g_prof_traps++;
    pthread_mutex_unlock(&g_prof_mu);
}
#define PROFC_N 512
static struct { u32 id, ra; uint64_t n; } g_profc[PROFC_N];
void prof_caller(u32 id, u32 ra) {
    unsigned h = ((ra ^ id) * 2654435761u) % PROFC_N, i;
    pthread_mutex_lock(&g_prof_mu);
    for (i = 0; i < PROFC_N && g_profc[h].n && (g_profc[h].ra != ra || g_profc[h].id != id); i++) h = (h + 1) % PROFC_N;
    if (i < PROFC_N) { g_profc[h].id = id; g_profc[h].ra = ra; g_profc[h].n++; }
    pthread_mutex_unlock(&g_prof_mu);
}
static void prof_callers_dump(void) {
    static struct { u32 id, ra; uint64_t n; } c[PROFC_N];
    pthread_mutex_lock(&g_prof_mu); memcpy(c, g_profc, sizeof c); memset(g_profc, 0, sizeof g_profc); pthread_mutex_unlock(&g_prof_mu);
    for (int k = 0; k < 16; k++) {
        int b = -1; for (int i = 0; i < PROFC_N; i++) if (c[i].n && (b < 0 || c[i].n > c[b].n)) b = i;
        if (b < 0) break;
        rlog("[prof]   message id %u from %#x: %llu", c[b].id, c[b].ra, (unsigned long long)c[b].n);
        c[b].n = 0;
    }
}
const char *prof_hc_name(u32 id);
static void *prof_thread(void *a) {
    int secs = (int)(long)a;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    for (;;) {
        sleep(secs);
        pthread_mutex_lock(&g_prof_mu);
        static struct { u32 key; uint64_t n, t; } c[PROF_N];
        memcpy(c, g_profk, sizeof c); memset(g_profk, 0, sizeof g_profk);
        uint64_t traps = g_prof_traps; g_prof_traps = 0;
        pthread_mutex_unlock(&g_prof_mu);
        uint64_t tot = 0; for (int i = 0; i < PROF_N; i++) tot += c[i].t;
        rlog("[prof] %d s: %llu traps (%.0f/s), %.1f ms in them", secs, (unsigned long long)traps, (double)traps / secs,
             (double)tot * tb.numer / tb.denom / 1e6);
        for (int k = 0; k < 20; k++) {
            int b = -1; for (int i = 0; i < PROF_N; i++) if (c[i].n && (b < 0 || c[i].t > c[b].t)) b = i;
            if (b < 0) break;
            u32 kind = c[b].key >> 16, num = c[b].key & 0xffff;
            const char *nm = kind == 3 && num == 0x5232 ? "hostcall?" : "";
            if (kind == 4) nm = prof_hc_name(num);
            if (kind == 5) nm = "";
            rlog("[prof]   %s %u %s: %llu calls, %.2f ms (%.1f us each)", kind == 1 ? "syscall" : kind == 2 ? "mach" : kind == 3 ? "machdep" : kind == 5 ? "gate" : "hostcall",
                 num, nm ? nm : "", (unsigned long long)c[b].n, (double)c[b].t * tb.numer / tb.denom / 1e6,
                 (double)c[b].t * tb.numer / tb.denom / 1e3 / c[b].n);
            c[b].n = 0;
        }
        prof_callers_dump();
    }
    return 0;
}
void prof_init(void) {
    const char *e = getenv("R2_PROF");
    if (!e || atoi(e) <= 0) return;
    g_prof = 1;
    pthread_t t; pthread_create(&t, 0, prof_thread, (void *)(long)atoi(e)); pthread_detach(t);
}
/* R2_MEMWATCH_HW=1: a hardware write watchpoint (DR0) on the watched word in every thread; each write
   is logged with the writer's registers and its guest backtrace (the translator's register file
   is found as a register that points at r0..r31, lr at +0x90), and execution goes on. */
volatile u32 g_wp_addr;
static struct { u32 port, v, eip; uint64_t t; } g_wp_ring[64];
static volatile u32 g_wp_ri;
/* a guest word read that cannot fault (guard pages are mapped but unreadable) */
static u32 srd32(u32 a) {
    u32 v = 0; mach_vm_size_t n = 0;
    if (mach_vm_read_overwrite(mach_task_self(), a, 4, (mach_vm_address_t)(uintptr_t)&v, &n) != KERN_SUCCESS || n != 4) return 0;
    return v;
}
/* the guest (PowerPC) backtrace of a thread in a trap: the translator's register file is found as a
   register or stack word that points at r0..r31 (r1 at +4, lr at +0x90) */
int r2_guest_bt(ss64_t *s, char *gb, int cap) {
    uint64_t regs[7 + 1024] = { s->__rax, s->__rbx, s->__rcx, s->__rdx, s->__rsi, s->__rdi, s->__rbp };
    int nr = 7, n = 0; gb[0] = 0;
    for (u32 a = ESP(s); nr < 7 + 1024 && a < ESP(s) + 0x1000; a += 4)
        if (!low_is_free(a & ~0xfffu, 1)) regs[nr++] = srd32(a);
    extern u32 g_xlat_lo, g_xlat_hi;
    for (int i = 0; i < nr && !n; i++) {
        u32 R = (u32)regs[i];
        if (R < 0x10000 || R > 0xfff00000u || low_is_free(R & ~0xfffu, 1) || low_is_free((R + 0x98) & ~0xfffu, 1)) continue;
        u32 r1 = srd32(R + 4), lr = srd32(R + 0x90);
        if (r1 < 0x10000 || r1 > 0xc0000000u || (r1 & 0xf) || low_is_free(r1 & ~0xfffu, 1) || lr < 0x1000 || (lr & 3)) continue;
        if ((r1 >= g_xlat_lo && r1 < g_xlat_hi) || (lr >= g_xlat_lo && lr < g_xlat_hi)) continue;
        {   u32 back = OSSwapBigToHostInt32(srd32(r1)); if (back <= r1 || back - r1 > 0x100000) continue; }
        n += snprintf(gb + n, cap - n, " R=%#x r1=%#x r3=%#x r4=%#x r28=%#x r29=%#x r30=%#x r31=%#x lr=%#x bt", R, r1, srd32(R + 12), srd32(R + 16),
                      srd32(R + 28 * 4), srd32(R + 29 * 4), srd32(R + 30 * 4), srd32(R + 31 * 4), lr);
        u32 fp = r1;
        for (int k = 0; k < 12; k++) {
            if (fp < 0x10000 || (fp & 3) || low_is_free(fp & ~0xfffu, 1)) break;
            fp = OSSwapBigToHostInt32(srd32(fp));
            if (fp < 0x10000 || (fp & 3) || low_is_free((fp + 8) & ~0xfffu, 1)) break;
            n += snprintf(gb + n, cap - n, " %#x", OSSwapBigToHostInt32(srd32(fp + 8)));
        }
    }
    return n;
}
u32 r2_srd32(u32 a) { return srd32(a); }
static void wp_log(ss64_t *s) {
    u32 v = OSSwapBigToHostInt32(*(volatile u32 *)P32(g_wp_addr));
    if (!(v & 0x80000000u) && getenv("R2_MEMWATCH_QUIET")) {     /* only a note in a ring: the timing stays closer */
        mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
        u32 k = __sync_fetch_and_add(&g_wp_ri, 1) % 64;
        g_wp_ring[k].port = me; g_wp_ring[k].v = v; g_wp_ring[k].eip = EIP(s); g_wp_ring[k].t = mach_absolute_time();
        return;
    }
    if ((v & 0x80000000u) && getenv("R2_MEMWATCH_QUIET")) {
        u32 n = g_wp_ri, from = n > 64 ? n - 64 : 0;
        for (u32 k = from; k < n; k++) rlog("[wp] ring: %#x <- %#x by thread %#x", g_wp_addr, g_wp_ring[k % 64].v, g_wp_ring[k % 64].port);
    }
    uint64_t regs[7 + 1024] = { s->__rax, s->__rbx, s->__rcx, s->__rdx, s->__rsi, s->__rdi, s->__rbp };
    int nr = 7;
    for (u32 a = ESP(s); nr < 7 + 1024 && a < ESP(s) + ((v & 0x80000000u) ? 0x1000 : 0x400); a += 4)       /* and the stack: the translator's caller frames hold it */
        if (!low_is_free(a & ~0xfffu, 1)) regs[nr++] = srd32(a);
    char gb[400] = ""; int n = 0;
    for (int i = 0; i < nr && !n; i++) {
        u32 R = (u32)regs[i];
        if (R < 0x10000 || R > 0xfff00000u || low_is_free(R & ~0xfffu, 1) || low_is_free((R + 0x98) & ~0xfffu, 1)) continue;
        u32 r1 = srd32(R + 4), lr = srd32(R + 0x90);
        extern u32 g_xlat_lo, g_xlat_hi;
        if (r1 < 0x10000 || r1 > 0xc0000000u || (r1 & 0xf) || low_is_free(r1 & ~0xfffu, 1) || lr < 0x1000 || (lr & 3)) continue;
        if ((r1 >= g_xlat_lo && r1 < g_xlat_hi) || (lr >= g_xlat_lo && lr < g_xlat_hi)) continue;   /* the translator's own */
        {   u32 back = OSSwapBigToHostInt32(srd32(r1));              /* a guest stack frame: back chain upwards */
            if (back <= r1 || back - r1 > 0x100000) continue; }
        n += snprintf(gb + n, sizeof gb - n, " R=%#x(reg %d) r1=%#x lr=%#x bt", R, i, r1, lr);
        if (v & 0x80000000u) {          /* in dispatch_barrier_sync_f_slow's frame: dbss2 at r1+0x50 {dq, func, ctxt, sema} */
            char x[400]; int xn = 0;
            for (int k = 0; k < 4; k++) xn += snprintf(x + xn, sizeof x - xn, " %08x", OSSwapBigToHostInt32(srd32(r1 + 0x50 + 4 * k)));
            u32 sem = OSSwapBigToHostInt32(srd32(r1 + 0x5c));
            if (sem > 0x10000 && !low_is_free(sem & ~0xfffu, 1) && !low_is_free((sem + 0x30) & ~0xfffu, 1)) {
                xn += snprintf(x + xn, sizeof x - xn, " | sema %#x:", sem);
                for (int k = 0; k < 12; k++) xn += snprintf(x + xn, sizeof x - xn, " %08x", OSSwapBigToHostInt32(srd32(sem + 4 * k)));
            }
            rlog("[wp]   dbss2%s", x);
        }
        u32 fp = r1;
        for (int k = 0; k < 10; k++) {
            if (fp < 0x10000 || (fp & 3) || low_is_free(fp & ~0xfffu, 1)) break;
            fp = OSSwapBigToHostInt32(srd32(fp));
            if (fp < 0x10000 || (fp & 3) || low_is_free((fp + 8) & ~0xfffu, 1)) break;
            n += snprintf(gb + n, sizeof gb - n, " %#x", OSSwapBigToHostInt32(srd32(fp + 8)));
        }
    }
    mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
    char fr[300] = ""; int fn = 0; u32 bp = EBP(s);
    for (int k = 0; k < 14 && bp > R32_LOW && bp < R32_TOP && !(bp & 3) && !low_is_free(bp & ~0xfffu, 8); k++, bp = srd32(bp))
        fn += snprintf(fr + fn, sizeof fr - fn, " %#x", srd32(bp + 4));
    rlog("[wp] %#x <- %#x by thread %#x eip %#x eax %#x ebx %#x ecx %#x edx %#x esi %#x edi %#x ebp %#x esp %#x%s | i386%s",
         g_wp_addr, v, me, EIP(s), EAX(s), (u32)s->__rbx, ECX(s), (u32)s->__rdx, (u32)s->__rsi, (u32)s->__rdi, EBP(s), ESP(s), gb, fr);
}
int on_signal_c(int sig, siginfo_t *si, void *uc_);
int on_signal_c(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    ss64_t *s = &uc->uc_mcontext->__ss;
    if (sig == SIGTRAP && g_wp_addr && uc->uc_mcontext->__es.__trapno == 1) {
        if (s->__cs == g_cs32) wp_log(s);
        else rlog("[wp] %#x written by host code at %#llx", g_wp_addr, (unsigned long long)s->__rip);
        if (s->__cs == g_cs32 && uc->uc_mcsize >= sizeof(struct __darwin_mcontext64_full) && t_alt && t_alt->tsd32) {
            ((full64_t *)s)->gsbase = t_alt->tsd32; s->__gs = t_gssel;
            return 1;
        }
        if (s->__cs != g_cs32 && uc->uc_mcsize >= sizeof(struct __darwin_mcontext64_full) && t_alt) {
            ((full64_t *)s)->gsbase = t_alt->host_tsd; s->__gs = 0;
        }
        return 0;
    }
    if (s->__cs != g_cs32 && ((sig != SIGILL && sig != SIGSEGV && sig != SIGBUS && sig != SIGFPE && sig != SIGTRAP) ||
                              is_sent(si, s))) {
        sig32_pend(sig, si);                          /* for the 32-bit world, while in host code */
        /* the frame holds the hardware %gs selector of the 32-bit code this thread came from; the
           kernel would load it on return and with it that segment's base: return with the host's.
           Not on the gate's way out (gate.s), where the 32-bit world's base is already set again. */
        extern char r2_gate_exit_lo[], r2_gate_exit_hi[];
        if (uc->uc_mcsize >= sizeof(struct __darwin_mcontext64_full) && t_alt) {
            if (s->__rip >= (uintptr_t)r2_gate_exit_lo && s->__rip < (uintptr_t)r2_gate_exit_hi && t_alt->tsd32) {
                ((full64_t *)s)->gsbase = t_alt->tsd32; s->__gs = t_gssel;
            } else { ((full64_t *)s)->gsbase = t_alt->host_tsd; s->__gs = 0; }
        }
        if (g_trace || r2_siglog()) rlog("[sig] signal %d (code %#x) in host code at %#llx: pending (thread %#x)", sig, si ? si->si_code : -1, (unsigned long long)s->__rip, pthread_mach_thread_np(pthread_self()));
        return 0;
    }
    if (s->__cs != g_cs32) {                          /* the host itself crashed */
        { extern void heapchk_dump(void); heapchk_dump(); }
        dump_state("host fault", sig, si, s);
        signal(sig, SIG_DFL);
        return 0;
    }
    full64_t *saved = t_full;
    t_full = uc->uc_mcsize >= sizeof(struct __darwin_mcontext64_full) ? (full64_t *)s : 0;
    if (t_full) {                     /* the kernel reloads the selectors from here: keep them valid */
        t_full->ds = t_full->es = g_ds32;
        if (t_alt && t_alt->tsd32) { t_full->ss64.__gs = t_gssel; t_full->gsbase = t_alt->tsd32; }
        else if (!t_full->ss64.__gs) t_full->ss64.__gs = g_ds32;
    }
    if (g_trace > 2 || (g_trace && sig != SIGILL))
        rlog("[trap] sig %d mcsize %lu full %d gs %#llx gsbase %#llx ds %#llx ss %#llx trapno %u err %#x fault %#llx", sig, uc->uc_mcsize, t_full != 0,
             (unsigned long long)s->__gs, t_full ? t_full->gsbase : 0, t_full ? t_full->ds : 0, t_full ? t_full->ss : 0,
             uc->uc_mcontext->__es.__trapno, uc->uc_mcontext->__es.__err, (unsigned long long)uc->uc_mcontext->__es.__faultvaddr);
    ucontext_t *saved_uc = t_uc;
    t_uc = uc;
    if (sig != SIGILL && sig != SIGSEGV && sig != SIGBUS && sig != SIGFPE && sig != SIGTRAP) {
        if (!sig32_deliver(sig, si, s, uc) && g_trace) rlog("[sig] signal %d: no 32-bit handler", sig);
    } else {
        int ts_was = t_alt ? t_alt->ts_set : 0;
        if (t_alt) { t_alt->ts_set = 0; t_alt->uc = uc; t_alt->trap = sig == SIGILL ? (((unsigned char *)P32(EIP(s)))[-1] << 24) | (EAX(s) & 0xffffff) : 0xff000000u | sig; }
        on_signal32(sig, si, s);
        if (t_alt) {
            t_alt->uc = 0;
            __sync_synchronize();
            if (t_alt->ts_set) {
                static int nlog;
                if (nlog++ < 10 || g_trace) rlog("[ts] state set during trap %#x: eip %#x -> %#x, eax %#x -> %#x", t_alt->trap, EIP(s), (u32)t_alt->ts_ss.__rip, EAX(s), (u32)t_alt->ts_ss.__rax);
                *s = t_alt->ts_ss;
                t_alt->ts_set = 0;
            } else t_alt->ts_set = ts_was;
        }
    }
    if (s->__cs == g_cs32) sig32_deliver_pending(s, uc);
    if (s->__cs == g_cs32) { t_gate_mask = uc->uc_sigmask; t_gate_mask_ok = 1; }
    t_uc = saved_uc;
    t_full = saved;
    return (s->__cs == g_cs32 && t_alt && t_alt->tsd32) ? 1 : 0;
}

/* ---- the system-call gate (gate.s) ----
   The sysenter stubs of the 32-bit world (all but a few of libsystem_kernel's system
   calls and Mach traps) jump to a trampoline in low memory: "popl %edx; movl %esp,%ecx" as the stub
   would, then a far jump through a 64-bit stub to r2_gate64, which calls r2_gate_c on the thread's
   host stack. A trap through a signal took the kernel's signal delivery, a sigreturn and three more
   system calls; the gate takes two (thread id is one, the %gs base twice: three). Everything else
   (int 0x80/0x81/0x82, faults, signals) still goes through r2_sigentry. R2_GATE=0: no gate. */
int g_gate_xsz = 4096;                    /* xsave area size (cpuid 0xd), used by gate.s */
static u32 g_gate_tramp, g_gate_fakeip;
static int g_gate_on = -1;
u32 gate_tramp32(void) {
    if (g_gate_on < 0) {
        const char *e = getenv("R2_GATE");
        g_gate_on = e ? atoi(e) > 0 : 1;
        if (g_gate_on) {
            unsigned a, b, c, dd;
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(dd) : "a"(0xd), "c"(0));
            if (b < 576 || b > 64 * 1024) { rlog("[gate] xsave area size %u: gate off", b); g_gate_on = 0; return 0; }
            g_gate_xsz = (int)((b + 63) & ~63u) + 64;
            u32 pg = low_alloc(R32_PAGE, 0, 1);
            if (!pg || mmap(P32(pg), R32_PAGE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
                rlog("[gate] no page for the trampoline: gate off"); g_gate_on = 0; return 0;
            }
            unsigned char *p = P32(pg);
            unsigned short cs64; __asm__ volatile("mov %%cs,%0" : "=r"(cs64));
            extern void r2_gate64(void);
            /* +0: 32-bit trampoline: popl %edx; movl %esp,%ecx; ljmp *(pg+32) */
            static const unsigned char t32[] = { 0x5a, 0x89, 0xe1, 0xff, 0x2d };
            memcpy(p, t32, sizeof t32); u32 fp = pg + 32; memcpy(p + 5, &fp, 4);
            /* +32: the far pointer: offset (the 64-bit stub), selector */
            u32 off = pg + 48; memcpy(p + 32, &off, 4); memcpy(p + 36, &cs64, 2);
            /* +48: 64-bit stub: movabs $r2_gate64, %r11; jmp *%r11 */
            uint64_t tgt = (uint64_t)(uintptr_t)r2_gate64;
            p[48] = 0x49; p[49] = 0xbb; memcpy(p + 50, &tgt, 8); p[58] = 0x41; p[59] = 0xff; p[60] = 0xe3;
            /* +64: what the 32-bit state shows as the trap's place (never run): a converted stub */
            static const unsigned char fake[] = { 0x5a, 0x89, 0xe1, 0xcd, 0x80 };
            memcpy(p + 64, fake, sizeof fake);
            mprotect(p, R32_PAGE, PROT_READ | PROT_EXEC);
            g_gate_tramp = pg; g_gate_fakeip = pg + 64 + 5;
            rlog("[gate] system-call gate at %#x (xsave %d bytes)", pg, g_gate_xsz);
        }
    }
    return g_gate_on ? g_gate_tramp : 0;
}
/* b: the registers at the call, below the 32-bit %esp; o: the registers to return with (on the host
   stack: a 32-bit signal frame built here, below the 32-bit %esp, may cover b) */
void r2_gate_c(u32 *b, unsigned char *xs, u32 *o);
void r2_gate_c(u32 *b, unsigned char *xs, u32 *o) {
    struct __darwin_mcontext64_full mc;
    ucontext_t uc;
    memset(&mc, 0, sizeof mc); memset(&uc, 0, sizeof uc);
    full64_t *f = (full64_t *)&mc.__ss;
    ss64_t *s = &f->ss64;
    s->__rax = b[0]; s->__rbx = b[1]; s->__rcx = b[2]; s->__rdx = b[3];
    s->__rsi = b[4]; s->__rdi = b[5]; s->__rbp = b[6]; s->__rflags = b[7];
    s->__rsp = b[8]; s->__rip = g_gate_fakeip; s->__cs = g_cs32;
    s->__gs = t_gssel ? t_gssel : b[10];
    f->ds = f->es = f->ss = g_ds32; f->gsbase = t_alt ? t_alt->tsd32 : 0;
    memcpy((unsigned char *)&mc.__fs + 8, xs, 512);           /* the fxsave image, after two reserved words */
    if (!t_gate_mask_ok) { pthread_sigmask(SIG_SETMASK, 0, &t_gate_mask); t_gate_mask_ok = 1; }
    uc.uc_sigmask = t_gate_mask;
    uc.uc_mcontext = (struct __darwin_mcontext64 *)&mc; uc.uc_mcsize = sizeof mc;
    full64_t *saved = t_full; t_full = f;
    ucontext_t *saved_uc = t_uc; t_uc = &uc;
    int ts_was = t_alt ? t_alt->ts_set : 0;
    if (t_alt) { t_alt->ts_set = 0; t_alt->uc = &uc; t_alt->trap = (0x80u << 24) | (EAX(s) & 0xffffff); }
    t_last_fault_eip = 0;
    uint64_t pt = g_prof ? mach_absolute_time() : 0;
    u32 key = (5u << 16) | (EAX(s) & 0xffff);         /* R2_PROF: "gate" */
    bsd_syscall(s, EAX(s), ECX(s) + 4, 1);
    if (pt) prof_add(key, mach_absolute_time() - pt);
    if (t_alt) {
        t_alt->uc = 0;
        __sync_synchronize();
        if (t_alt->ts_set) { *s = t_alt->ts_ss; t_alt->ts_set = 0; } else t_alt->ts_set = ts_was;
    }
    if (s->__cs == g_cs32) sig32_deliver_pending(s, &uc);
    t_uc = saved_uc; t_full = saved;
    if (s->__cs != g_cs32) rfatal("[gate] a system call left 32-bit code (cs %#llx rip %#llx)", (unsigned long long)s->__cs, (unsigned long long)s->__rip);
    if (memcmp(&uc.uc_sigmask, &t_gate_mask, sizeof t_gate_mask)) {   /* sigprocmask, a signal handler's mask */
        t_gate_mask = uc.uc_sigmask;
        pthread_sigmask(SIG_SETMASK, &t_gate_mask, 0);
    }
    o[0] = EAX(s); o[1] = EBX(s); o[2] = ECX(s); o[3] = EDX(s); o[4] = ESI(s); o[5] = EDI(s); o[6] = EBP(s);
    o[7] = (u32)s->__rflags; o[8] = ESP(s); o[9] = EIP(s); o[10] = t_gssel ? t_gssel : (u32)s->__gs;
    memcpy(xs, (unsigned char *)&mc.__fs + 8, 512);
    *(uint64_t *)(xs + 512) |= 3;                     /* x87 and SSE from the (maybe changed) legacy area */
}

/* %gs accesses of a thread whose %gs never works. On some threads (Build-a-lot 2, Illustrator) a
   %gs-relative access faults (#GP, error 0) however often the selector is reloaded, though the LDT
   entry is valid and points at the thread's TSD; why is not known (LDT growth, reuse of the entry and
   a pre-grown table made no difference). The thread data is plain memory at the known base, so the
   moves that reach it - "mov r32, %gs:disp32" (65 8b /r, mod 00 r/m 101), "mov %gs:disp32, r32"
   (65 89 /r), and the eax short forms (65 a1 / 65 a3) - are done here and the thread goes on. */
static __thread int t_gs_broken;
static uint64_t *gs_reg(ss64_t *s, int r) {
    switch (r & 7) {
    case 0: return &s->__rax; case 1: return &s->__rcx; case 2: return &s->__rdx; case 3: return &s->__rbx;
    case 4: return &s->__rsp; case 5: return &s->__rbp; case 6: return &s->__rsi; default: return &s->__rdi;
    }
}
static int gs_emulate(ss64_t *s, uint64_t base) {
    if (getenv("R2_NO_GSEMU")) return 0;
    const unsigned char *ip = P32(EIP(s));
    if (ip[0] != 0x65) return 0;
    u32 disp, len, val, *mem;
    if (ip[1] == 0xa1 || ip[1] == 0xa3) {
        memcpy(&disp, ip + 2, 4); len = 6;
        mem = P32((u32)base + disp);
        if (ip[1] == 0xa1) s->__rax = *mem; else *mem = (u32)s->__rax;
    } else if ((ip[1] == 0x8b || ip[1] == 0x89) && (ip[2] & 0xc7) == 0x05) {
        memcpy(&disp, ip + 3, 4); len = 7;
        mem = P32((u32)base + disp);
        uint64_t *r = gs_reg(s, (ip[2] >> 3) & 7);
        if (ip[1] == 0x8b) { val = *mem; *r = val; } else *mem = (u32)*r;
    } else return 0;
    s->__rip = EIP(s) + len;
    return 1;
}

/* Memory another process maps into this one with our task port lands where the kernel puts it,
   above 4 GB, and the mapper hands the 32-bit world the address's low 32 bits. Snow Leopard's
   coreservicesd (translated, with its own rhost) does that for a root client: it shares the root
   session's file system universe with the client through vm_map on the client's task port (the task
   broker, liboah), and every PowerPC program run as root crashed in CarbonCore at the first file
   system call (Version Cue's server, its Java and MySQL). A fault on a free page of the 32-bit space
   whose address + 4 GB lies in such a mapping is answered by mapping it there too (shared, not
   copied), and the access is retried. Host malloc regions and stacks are never aliased.
   R2_NO_ALIASFIX=1 disables. */
static int foreign_alias_fix(uint64_t fa) {
    static int off = -1;
    if (off < 0) off = getenv("R2_NO_ALIASFIX") != 0;
    if (off || fa < 0x1000 || fa >= R32_TOP) return 0;
    u32 pg = (u32)fa & ~(R32_PAGE - 1);
    if (!low_is_free(pg, R32_PAGE)) return 0;
    mach_vm_address_t ra = (mach_vm_address_t)pg + 0x100000000ull, want = ra;
    mach_vm_size_t rs = 0;
    vm_region_extended_info_data_t ei;
    mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t on = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&ei, &cnt, &on) != KERN_SUCCESS) return 0;
    if (ra > want || ra < 0x100000000ull || ra + rs - 0x100000000ull > R32_TOP) return 0;
    unsigned tag = ei.user_tag;
    if ((tag >= 1 && tag <= 11) || tag == 30 /* VM_MEMORY_STACK */ || (tag >= 60 && tag <= 66) /* dyld */) return 0;
    u32 lo = (u32)(ra - 0x100000000ull), len = (u32)rs;
    if (!low_is_free(lo, len)) return 0;
    mach_vm_address_t t = lo;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &t, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), ra, FALSE,
                                     &cur, &max, VM_INHERIT_SHARE);
    if (g_trace || getenv("R2_VMLOG")) rlog("[vm] fault at %#llx: memory mapped in from outside at %#llx (%u KB, tag %u, share mode %d) -> alias at %#x: %d",
         (unsigned long long)fa, (unsigned long long)ra, len >> 10, tag, ei.share_mode, lo, kr);
    if (kr != KERN_SUCCESS) return 0;
    low_mark(lo, len, 1);
    return 1;
}

static void on_signal32(int sig, siginfo_t *si, ss64_t *s) {
    unsigned char *ip = P32(EIP(s));
    if (sig == SIGILL && ip[-2] == 0xcd) {
        t_last_fault_eip = 0;
        uint64_t pt = g_prof ? mach_absolute_time() : 0;
        u32 key = ip[-1] == 0x80 ? (1u << 16) | (EAX(s) & 0xffff) : ip[-1] == 0x81 ? (2u << 16) | ((u32)-(int)EAX(s) & 0xffff) : (3u << 16) | (EAX(s) & 0xffff);
        switch (ip[-1]) {
        case 0x80: {
            int sysenter = (ip[-5] == 0x5a && ip[-4] == 0x89 && ip[-3] == 0xe1);
            bsd_syscall(s, EAX(s), (sysenter ? ECX(s) : ESP(s)) + 4, sysenter);
            if (pt) prof_add(key, mach_absolute_time() - pt);
            return;
        }
        case 0x81: mach_trap(s, (int)EAX(s), ESP(s) + 4); if (pt) prof_add(key, mach_absolute_time() - pt); return;
        case 0x82: machdep_trap(s, EAX(s), ESP(s) + 4); if (pt) prof_add(key, mach_absolute_time() - pt); return;
        }
    }
    /* a general protection fault with error code 0 in 32-bit code: a segment register is null. The
       kernel clears the hardware selectors now and then (it keeps none for 64-bit threads); reload
       them and retry, unless the same instruction keeps failing that way (the selectors can be
       cleared again before the retried instruction runs: a thread switch in between). */
    if ((sig == SIGSEGV || sig == SIGBUS) && t_uc->uc_mcontext->__es.__trapno == 13 && t_alt && t_alt->tsd32 &&
        (t_gs_broken || (t_last_fault_eip == EIP(s) && t_last_fault_n >= 3)) && gs_emulate(s, t_alt->tsd32)) {
        if (!t_gs_broken) {
            static int nlog;
            if (nlog++ < 10) rlog("[trap] %%gs keeps faulting on this thread (selector %#x, base %#x) at %#x: its %%gs accesses are emulated from now on",
                                  t_gssel, (u32)t_alt->tsd32, EIP(s) - 0);
            t_gs_broken = 1;
        }
        t_last_fault_eip = 0; t_last_fault_n = 0;
        return;
    }
    if ((sig == SIGSEGV || sig == SIGBUS) && t_uc->uc_mcontext->__es.__trapno == 13 &&
        (t_last_fault_eip != EIP(s) || t_last_fault_n < 16)) {
        t_last_fault_n = t_last_fault_eip == EIP(s) ? t_last_fault_n + 1 : 1;
        t_last_fault_eip = EIP(s);
        fix_segs_via_stub(s, t_gssel ? t_gssel : g_ds32);
        if (g_trace > 1) rlog("[trap] segment registers reloaded at %#x", EIP(s) + 0);
        return;
    }
    if (t_last_fault_n >= 16 && t_last_fault_eip == EIP(s)) {
        rlog("[trap] general protection fault at %#x persists after reloading the selectors (gs %#llx, thread's %#x)",
             EIP(s), (unsigned long long)s->__gs, t_gssel);
        {   /* what the LDT holds for the selector now, and the fault's error code / address */
            union ldt_entry d; memset(&d, 0, sizeof d);
            int idx = (int)(((t_gssel ? t_gssel : (u32)s->__gs) >> 3) & 0x1fff);
            int r = i386_get_ldt(idx, &d, 1);
            u32 *w = (u32 *)&d;
            rlog("[trap]   LDT[%d] -> %d: %08x %08x (err %#x, fault addr %#llx, ds %#llx es %#llx fs %#llx)", idx, r, w[0], w[1],
                 t_uc->uc_mcontext->__es.__err, (unsigned long long)t_uc->uc_mcontext->__es.__faultvaddr,
                 t_full ? (unsigned long long)t_full->ds : 0ull, t_full ? (unsigned long long)t_full->es : 0ull, (unsigned long long)s->__fs);
        }
        {   /* whose thread this is, as the signal entry saw it */
            uint64_t tid = 0; pthread_threadid_np(0, &tid);
            rlog("[trap]   thread %llu: record %p (tid %llu, tsd32 %#llx, host_tsd %#llx, depth %llu), gs base set %d times; frame %s gsbase %#llx gs %#llx ss %#llx cs %#llx mcsize %lu",
                 (unsigned long long)tid, (void *)t_alt, t_alt ? (unsigned long long)t_alt->tid : 0ull, t_alt ? (unsigned long long)t_alt->tsd32 : 0ull,
                 t_alt ? (unsigned long long)t_alt->host_tsd : 0ull, t_alt ? (unsigned long long)t_alt->depth : 0ull, t_tsd_sets,
                 t_full ? "full" : "short", t_full ? (unsigned long long)t_full->gsbase : 0ull, t_full ? (unsigned long long)t_full->ss64.__gs : 0ull,
                 t_full ? (unsigned long long)t_full->ss : 0ull, (unsigned long long)s->__cs, (unsigned long)t_uc->uc_mcsize);
            r2thr_t *lk = r2_thread_lookup(tid);
            rlog("[trap]   lookup by tid -> %p", (void *)lk);
        }
    }
    t_last_fault_eip = 0; t_last_fault_n = 0;
    /* ud2 (a library's deliberate crash: libdispatch / libpthread "BUG IN ..."): a handler that
       resumes there runs into it again. The thread span at 100% CPU forever (fontd, Word's
       workqueue). After a few rounds it is the crash it was meant to be. */
    if (sig == SIGILL && ip[0] == 0x0f && ip[1] == 0x0b) {
        static __thread u32 ud_eip; static __thread int ud_n;
        ud_n = ud_eip == EIP(s) ? ud_n + 1 : 1; ud_eip = EIP(s);
        if (ud_n > 8) { dump_state("ud2 (deliberate crash) in 32-bit code", sig, si, s); _exit(128 + sig); }
    }
    int sent = is_sent(si, s);                        /* kill / pthread_kill, not a fault */
    if (!sent && (sig == SIGBUS || sig == SIGSEGV) && foreign_alias_fix(t_uc->uc_mcontext->__es.__faultvaddr)) return;
    if (!sent && exc32_raise(sig, s, t_uc)) return;  /* the translator's exception port */
    if (sig32_deliver(sig, si, s, t_uc)) return;      /* the 32-bit world's own handler */
    dump_state("unhandled 32-bit fault", sig, si, s);
    _exit(128 + sig);
}

/* register the calling thread: its host %gs base, a host stack for the signal handler */
/* The records of ended threads (tombstones: tid ~0) used to stay in the table for good, with their
   8 MB host stacks: the 4,096th 32-bit thread of a process looped for ever looking for a free slot
   (holding the lock, so every later thread start hung too) - a program that starts many short
   threads stopped after 4,095 (tsdtest; long sessions of apps whose work threads come and go).
   A tombstone ended more than 0.1 s ago is now reused, record and host stack (the ended thread has
   left it by then: it only had pthread_exit to run). With the table full of fresh tombstones a new
   thread waits for one to age. */
static volatile uint64_t g_thr_dead[THR_SLOTS];          /* mach_absolute_time when the slot's thread ended */
void thread_setup_altstack(void) {
    uint64_t tid = 0; pthread_threadid_np(0, &tid);
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t two_s;
    if (!two_s) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); two_s = 100000000ull * tb.denom / tb.numer; }
    r2thr_t *t = 0;
    unsigned h, n;
    for (int tries = 0;; tries++) {
        pthread_mutex_lock(&lk);
        h = (unsigned)(tid * 2654435761u) % THR_SLOTS; n = 0;
        uint64_t now = mach_absolute_time();
        for (; n < THR_SLOTS; n++, h = (h + 1) % THR_SLOTS) {
            r2thr_t *o = g_thr[h];
            if (!o) break;
            if (o->tid == ~0ull && g_thr_dead[h] && now - g_thr_dead[h] > two_s && !getenv("R2_THR_NOREUSE")) { t = o; break; }
        }
        if (n < THR_SLOTS) break;
        pthread_mutex_unlock(&lk);
        if (tries == 3000) rfatal("more than %d threads at once", THR_SLOTS);
        usleep(1000);
    }
    if (t) {                                            /* a dead thread's record: keep its host stack */
        uint64_t st = t->stack_top;
        memset(t, 0, sizeof *t);
        t->stack_top = st;
    }
    g_thr_dead[h] = 0;
    pthread_mutex_unlock(&lk);
    if (!t) {
        t = calloc(1, sizeof *t);
        /* mmap'd with a no-access guard page below (not malloc'd: an overrun went into the host heap) */
        char *stk = mmap(0, HOST_STACK + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (stk == MAP_FAILED) stk = 0;
        else { mprotect(stk, 0x10000, PROT_NONE); stk += 0x10000; }
        if (!t || !stk) rfatal("no memory for a thread record");
        t->stack_top = ((uint64_t)(stk + HOST_STACK)) & ~15ull;
    }
    t->host_tsd = find_host_tsd();
    t->tid = tid;
    t->port = mach_thread_self();
    t_alt = t;
    pthread_mutex_lock(&lk);
    if (g_thr[h] != t) {                                /* a new record: into a free slot from h */
        while (g_thr[h] && g_thr[h] != t) h = (h + 1) % THR_SLOTS;
        g_thr[h] = t;
    }
    pthread_mutex_unlock(&lk);
}
static void thr_mark_dead(r2thr_t *t) {
    for (unsigned i = 0; i < THR_SLOTS; i++) if (g_thr[i] == t) { g_thr_dead[i] = mach_absolute_time(); break; }
}

/* R2_WATCHDOG=seconds: after that long, print every 32-bit thread's state (where it is waiting) and exit */
/* in a forked child: the only thread left has a new thread id (the signal entry finds its record
   by it); the other threads' records go */
void trap_atfork_child(void) {
    uint64_t tid = 0; pthread_threadid_np(0, &tid);
    for (unsigned i = 0; i < THR_SLOTS; i++) g_thr[i] = 0;
    if (!t_alt) return;
    t_alt->tid = tid;
    unsigned h = (unsigned)(tid * 2654435761u) % THR_SLOTS;
    g_thr[h] = t_alt;
}

/* thread_set_state on a thread that is inside a trap (exc.c) changes the trap's saved context, but
   the trap was still running: when it finished it wrote its result into %eax/%edx/carry over the new
   state. The translator redirects a thread blocked in a system call that way (suspend, get, set,
   resume) and expects the new registers as set - with the call's result in %eax instead, Photoshop
   CS2 crashed at quit (a store through %eax = 14). On a real kernel the call has returned before the
   state can be read or set, so the state set wins: remember it and put it back when the trap ends. */
void thread_trap_state_set(uint64_t tid, const ss64_t *s) {
    r2thr_t *t = r2_thread_lookup(tid);
    if (!t || !t->uc) return;
    t->ts_ss = *s;
    __sync_synchronize();
    t->ts_set = 1;
}
/* the 32-bit context of thread tid while it is in a trap (host code), or 0 */
ucontext_t *thread_trap_uc(uint64_t tid) {
    r2thr_t *t = r2_thread_lookup(tid);
    return t ? t->uc : 0;
}

static void *watchdog(void *a) {
    sigset_t all; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, 0);
    sleep((unsigned)(uintptr_t)a);
    rlog("[watchdog] %lu s: threads", (unsigned long)(uintptr_t)a);
    for (unsigned i = 0; i < THR_SLOTS; i++) {
        r2thr_t *t = g_thr[i];
        if (!t || t->tid == ~0ull) continue;
        {   thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
            if (t->port && !thread_info(t->port, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc))
                rlog("[watchdog]  tid %llu port %#x: run state %d suspend count %d flags %#x", (unsigned long long)t->tid, t->port, bi.run_state, bi.suspend_count, bi.flags);
        }
        ucontext_t *uc = t->uc;
        if (uc) {
            ss64_t *s = &uc->uc_mcontext->__ss;
            u32 bp = EBP(s); char m[700]; int n = 0;
            for (int k = 0; k < 24 && bp > R32_LOW && bp < R32_TOP && !(bp & 3) && !low_is_free(bp & ~0xfffu, 1); k++, bp = rd32(bp))
                n += snprintf(m + n, sizeof m - n, " %#x", rd32(bp + 4));
            rlog("[watchdog]  tid %llu in trap %#x (int %#x num %d) eip %#x esp %#x frames%s; msg id %u port %#x opt %#x rcv %#x", (unsigned long long)t->tid, t->trap,
                 t->trap >> 24, (int)(t->trap << 8) >> 8, EIP(s), ESP(s), m, t->msg_id, t->msg_port, t->msg_opt, t->msg_rcv);
        } else {
            x86_thread_state64_t ks; mach_msg_type_number_t c = x86_THREAD_STATE64_COUNT; char m[700] = ""; int n = 0;
            if (t->port && !thread_suspend(t->port)) {
                if (!thread_get_state(t->port, x86_THREAD_STATE64, (thread_state_t)&ks, &c)) {
                    ss64_t *s = (ss64_t *)&ks; u32 bp = EBP(s);
                    for (int k = 0; k < 24 && bp > R32_LOW && bp < R32_TOP && !(bp & 3) && !low_is_free(bp & ~0xfffu, 1); k++, bp = rd32(bp))
                        n += snprintf(m + n, sizeof m - n, " %#x", rd32(bp + 4));
                    rlog("[watchdog]  tid %llu running %s code: eip %#llx esp %#x eax %#x ebx %#x ecx %#x edx %#x esi %#x edi %#x frames%s; msg id %u port %#x opt %#x rcv %#x", (unsigned long long)t->tid,
                         ks.__cs == g_cs32 ? "32-bit" : "host", (unsigned long long)ks.__rip, ESP(s), EAX(s), (u32)s->__rbx, ECX(s), (u32)s->__rdx, (u32)s->__rsi, (u32)s->__rdi, m,
                         t->msg_id, t->msg_port, t->msg_opt, t->msg_rcv);
                    n = -1;
                    {   char w[400]; int wn = 0; u32 sp = ESP(s), cx = ECX(s);
                        if (sp > R32_LOW && sp < R32_TOP && !low_is_free(sp & ~0xfffu, 1))
                            for (int k = 0; k < 10; k++) wn += snprintf(w + wn, sizeof w - wn, " %08x", rd32(sp + 4 * k));
                        wn += snprintf(w + wn, sizeof w - wn, " | ecx:");
                        if (cx > R32_LOW && cx < R32_TOP && !low_is_free(cx & ~0xfffu, 1))
                            for (int k = 0; k < 8; k++) wn += snprintf(w + wn, sizeof w - wn, " %08x", rd32(cx + 4 * k));
                        rlog("[watchdog]    stack%s", w);
                    }
                }
                thread_resume(t->port);
            }
            if (n >= 0) rlog("[watchdog]  tid %llu running 32-bit code", (unsigned long long)t->tid);
        }
    }
    _exit(124);
}

/* R2_MEMWATCH=PTR+OFF (hex, guest addresses): a debugging aid. A host thread follows the guest
   pointer stored at PTR and watches the big-endian word at OFF in the object it points to (a
   libdispatch queue's suspend count, say). It logs every value it sees change (the last 64, when
   one turns negative), and when the word turns negative it stops all other threads at once and
   logs where each one is, then asks for the usual hang dump (SIGUSR2) for the guest backtraces. */
static void *memwatch(void *a) {
    sigset_t all; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, 0);
    const char *e = getenv("R2_MEMWATCH");
    u32 ptr, off = 0;
    const char *colon = strchr(e, ':');
    if (!strcmp(e, "file")) {     /* R2_MEMWATCH=file: wait for $HOME/rosettus-work/logs/r2mw.PID holding "PTR+OFF" (liboah writes it) */
        char fn[512]; snprintf(fn, sizeof fn, "%s/rosettus-work/logs/r2mw.%d", getenv("HOME") ? getenv("HOME") : "/tmp", getpid());
        static char spec[64];
        for (;;) { FILE *f = fopen(fn, "r"); if (f) { int ok = fgets(spec, sizeof spec, f) != 0; fclose(f); if (ok) break; } usleep(2000); }
        e = spec; colon = 0;
        rlog("[memwatch] spec from %s: %s", fn, spec);
    }
    if (colon) {                  /* IMAGE:OFFSET+FIELD: OFFSET from the image's first executable mapping */
        extern volatile u32 g_mw_base;
        while (!g_mw_base) usleep(200);
        ptr = g_mw_base + (u32)strtoul(colon + 1, 0, 16);
        rlog("[memwatch] %.*s at %#x", (int)(colon - e), e, g_mw_base);
    } else ptr = (u32)strtoul(e, 0, 16);
    const char *p = strchr(e, '+'); if (p) off = (u32)strtoul(p + 1, 0, 16);
    struct { uint64_t t; u32 v; } h[64]; unsigned hn = 0;
    u32 obj = 0, last = 0x12345678;
    uint64_t t0 = mach_absolute_time();
    rlog("[memwatch] watching *(%#x) + %#x", ptr, off);
    /* R2_MEMWATCH=low24=XXXXXX+OFF: the object itself sits at ??XXXXXX (the heap layout repeats from
       run to run, its base does not): try every high byte, take the one whose label matches */
    /* low12=XXX: any page, at that offset (slower: one probe per mapped page) */
    int low24 = !strncmp(e, "low24=", 6), low12 = !strncmp(e, "low12=", 6);
    if (low24 || low12) { ptr = (u32)strtoul(e + 6, 0, 16) & (low12 ? 0xfffu : 0xffffffu); rlog("[memwatch] looking for the object at offset %#x", ptr); low24 = 1; }
    for (unsigned long spin = 0;; spin++) {
        if (!obj && low24) {
            const char *lb = getenv("R2_MEMWATCH_LABEL");
            u32 lo = (u32)strtoul(getenv("R2_MEMWATCH_LABELOFF") ? getenv("R2_MEMWATCH_LABELOFF") : "3c", 0, 16);
            u32 step = low12 ? 0x1000u : 0x1000000u, n = low12 ? 0xc0000u : 0xffu;
            for (u32 k = 1; k < n && !obj; k++) {
                u32 o = k * step | ptr;
                if (o < R32_LOW || low_is_free(o & ~0xfffu, 1) || low_is_free((o + lo) & ~0xfffu, 1)) continue;
                if (lb && !memcmp(P32(o + lo), lb, strlen(lb))) { obj = o; rlog("[memwatch] object %#x", obj); }
            }
            if (!obj) { usleep(low12 ? 20000 : 500); continue; }
        }
        if (!obj) {
            if (low_is_free(ptr & ~0xfffu, 1)) { usleep(1000); continue; }
            u32 o = OSSwapBigToHostInt32(rd32(ptr));
            if (!o || o < R32_LOW || o > 0xfff00000u || low_is_free(o & ~0xfffu, 1)) { usleep(1000); continue; }
            /* R2_MEMWATCH_LABEL=text: the object must hold that text at R2_MEMWATCH_LABELOFF (a queue's label) */
            const char *lb = getenv("R2_MEMWATCH_LABEL");
            if (lb) { u32 lo = (u32)strtoul(getenv("R2_MEMWATCH_LABELOFF") ? getenv("R2_MEMWATCH_LABELOFF") : "3c", 0, 16);
                      if (low_is_free((o + lo) & ~0xfffu, 1) || memcmp(P32(o + lo), lb, strlen(lb))) { usleep(1000); continue; } }
            obj = o; rlog("[memwatch] object %#x", obj);
        }
        static int hw = -1;
        if (hw < 0) { hw = getenv("R2_MEMWATCH_HW") != 0; if (hw) { g_wp_addr = obj + off; rlog("[memwatch] hardware watchpoint on %#x", g_wp_addr); } }
        if (hw) {                       /* (re)arm DR0 in every thread, new ones included */
            thread_act_array_t th; mach_msg_type_number_t nt = 0;
            if (!task_threads(mach_task_self(), &th, &nt)) {
                x86_debug_state64_t ds; memset(&ds, 0, sizeof ds);
                ds.__dr0 = g_wp_addr; ds.__dr7 = 1u | (1u << 16) | (3u << 18);   /* L0, write, 4 bytes */
                for (unsigned i = 0; i < nt; i++) { thread_set_state(th[i], x86_DEBUG_STATE64, (thread_state_t)&ds, x86_DEBUG_STATE64_COUNT); mach_port_deallocate(mach_task_self(), th[i]); }
                vm_deallocate(mach_task_self(), (vm_address_t)th, nt * sizeof *th);
            }
            usleep(5000);
        }
        u32 v = OSSwapBigToHostInt32(*(volatile u32 *)P32(obj + off));
        if (v == last) { if (!hw && !(spin & 0xffff)) usleep(50); continue; }
        h[hn % 64].t = mach_absolute_time() - t0; h[hn % 64].v = v; hn++;
        last = v;
        if (!(v & 0x80000000u)) continue;
        if (hw) {   /* the writer's own trap logs it; stopping every thread here could catch it holding the log lock */
            usleep(300000);
            rlog("[memwatch] %#x+%#x became %#x (hardware watch)", obj, off, v);
            kill(getpid(), SIGUSR2);
            return 0;
        }
        /* negative: stop everyone now */
        thread_act_array_t th; mach_msg_type_number_t n = 0;
        mach_port_t me = mach_thread_self();
        task_threads(mach_task_self(), &th, &n);
        for (unsigned i = 0; i < n; i++) if (th[i] != me) thread_suspend(th[i]);
        rlog("[memwatch] %#x+%#x became %#x; the last %u values:", obj, off, v, hn < 64 ? hn : 64);
        for (unsigned k = hn > 64 ? hn - 64 : 0; k < hn; k++)
            rlog("[memwatch]   +%llu us: %#x", (unsigned long long)(h[k % 64].t / 1000), h[k % 64].v);
        for (unsigned i = 0; i < n; i++) {
            if (th[i] == me) continue;
            x86_thread_state64_t ks; mach_msg_type_number_t c = x86_THREAD_STATE64_COUNT;
            thread_identifier_info_data_t ii; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
            thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &ic);
            if (thread_get_state(th[i], x86_THREAD_STATE64, (thread_state_t)&ks, &c)) continue;
            ss64_t *s = (ss64_t *)&ks; const char *where = "32-bit code";
            if (ks.__cs != g_cs32) { ucontext_t *uc = thread_trap_uc(ii.thread_id); if (uc) { s = &uc->uc_mcontext->__ss; where = "trap"; } else where = "host code"; }
            u32 bp = EBP(s); char m[400]; int k2 = 0;
            for (int k = 0; k < 12 && bp > R32_LOW && bp < R32_TOP && !(bp & 3) && !low_is_free(bp & ~0xfffu, 1); k++, bp = rd32(bp))
                k2 += snprintf(m + k2, sizeof m - k2, " %#x", rd32(bp + 4));
            rlog("[memwatch]  thread %#x (%s) eip %#x eax %#x ebx %#x ecx %#x edx %#x esi %#x edi %#x esp %#x ebp %#x frames%s", th[i], where,
                 EIP(s), EAX(s), (u32)s->__rbx, ECX(s), (u32)s->__rdx, (u32)s->__rsi, (u32)s->__rdi, ESP(s), EBP(s), m);
        }
        for (unsigned i = 0; i < n; i++) { if (th[i] != me) thread_resume(th[i]); mach_port_deallocate(mach_task_self(), th[i]); }
        mach_port_deallocate(mach_task_self(), me);
        kill(getpid(), SIGUSR2);
        return 0;
    }
}

void trap_install(void) {
    if (getenv("R2_WATCHDOG")) { pthread_t w; pthread_create(&w, 0, watchdog, (void *)(uintptr_t)atoi(getenv("R2_WATCHDOG"))); }
    if (getenv("R2_MEMWATCH")) { pthread_t w; pthread_create(&w, 0, memwatch, 0); }
    union ldt_entry d; make_desc(&d, 0, 1);
    int idx = i386_set_ldt(LDT_AUTO_ALLOC, &d, 1);
    if (idx < 0) rfatal("i386_set_ldt failed: this macOS does not allow 32-bit code segments");
    g_cs32 = (unsigned short)((idx << 3) | 4 | 3);
    unsigned short cs, ss;
    __asm__ volatile("mov %%cs,%0; mov %%ss,%1" : "=r"(cs), "=r"(ss));
    g_cs64 = cs; g_ds32 = ss;
    stubs_init();
    thread_setup_altstack();
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    extern void r2_sigentry(int, siginfo_t *, void *);
    sa.sa_sigaction = r2_sigentry; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGTRAP };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, 0);
    if (g_trace) rlog("[trap] cs32 %#x cs64 %#x ds %#x stubs %#x", g_cs32, g_cs64, g_ds32, g_stub_popgs);
}

/* emu_mach_msg notes the message it is in (0 when done), for the watchdog */
void trap_note_msg(u32 id, u32 port, u32 opt, u32 rcv) {
    if (!t_alt) return;
    t_alt->msg_id = id; t_alt->msg_port = port; t_alt->msg_opt = opt; t_alt->msg_rcv = rcv;
}
/* forget the calling thread (it is about to exit) */
/* a thread that is about to free its own 32-bit stack (bsdthread_terminate): its trap frame there is
   no longer to be read by others (thread_get_state from another thread read a freed stack: rhost
   faulted in vm_routine, cfmpstress) */
void thread_trap_gone(void) { if (t_alt) { t_alt->uc = 0; __sync_synchronize(); } }
void thread_unregister(void) {
    if (t_alt) { t_alt->tid = ~0ull; thr_mark_dead(t_alt); }   /* a tombstone keeps the probe chains intact */
    if (t_gssel) { ldt_release(t_gssel); t_gssel = 0; }   /* its LDT entry for the next thread */
    if (t_alt && t_alt->port) { mach_port_deallocate(mach_task_self(), t_alt->port); t_alt->port = 0; }
}

void enter32(u32 eip, u32 esp, u32 eax, u32 ebx, u32 ecx, u32 edx, u32 esi, u32 edi) {
    uint64_t frame[5] = { eip, g_cs32, 0x202, esp, g_ds32 };    /* iretq frame: rip cs rflags rsp ss */
    u32 regs[6] = { eax, ebx, ecx, edx, esi, edi };
    extern void r2_enter32(uint64_t *frame, u32 *regs, uint64_t ds, uint64_t gs) __attribute__((noreturn));
    r2_enter32(frame, regs, g_ds32, t_alt && t_alt->tsd32 ? t_gssel : 0);
}
