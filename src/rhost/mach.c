#include <sys/time.h>
/* mach.c - Mach traps and messages of the 32-bit world.
   Port names are shared: both worlds live in one task. Messages differ between 32-bit and 64-bit
   senders only in their out-of-line descriptors (12 vs 16 bytes) and in kernel routines whose
   arguments have the natural size (vm_allocate & co); those routines are carried out here, keeping
   the 32-bit world's memory below 4 GB. */
#include "rhost.h"
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <pthread.h>

#define A(i) rd32(argp + 4 * (i))
extern mach_port_name_t mach_reply_port(void);

/* a Mach trap with up to 6 arguments, straight to the kernel */
static long trap6(int num, long a, long b, long c, long d, long e, long f) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(ret) : "a"((long)(0x1000000 | num)), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* ---- the kernel's virtual memory routines (vm_map / mach_vm subsystems) ---- */
typedef struct { mach_msg_header_t h; NDR_record_t ndr; } req_hdr_t;
#pragma pack(push, 4)
typedef struct { mach_msg_header_t h; NDR_record_t ndr; kern_return_t ret; } reply_rc_t;
typedef struct { mach_msg_header_t h; NDR_record_t ndr; kern_return_t ret; u32 addr; } reply_addr32_t;
typedef struct { mach_msg_header_t h; NDR_record_t ndr; kern_return_t ret; uint64_t addr; } reply_addr64_t;
#pragma pack(pop)

/* R2_VMLOG=KB: log every vm_allocate of at least KB kilobytes with the 32-bit caller chain
   (eip and three frame return addresses) and the space left - who fills the 32-bit space. */
static __thread ss64_t *t_vm_s;
static void vm_log(u32 len, u32 at, int kr) {
    static long min = -2;
    if (min == -2) { const char *e = getenv("R2_VMLOG"); min = e ? atol(e) * 1024 : -1; }
    if (min < 0 || len < (u32)min) return;
    u32 ra[3] = { 0, 0, 0 }, eip = 0;
    if (t_vm_s) {
        u32 ebp = (u32)t_vm_s->__rbp; eip = (u32)t_vm_s->__rip;
        for (int d = 0; d < 3 && ebp > 0x1000 && ebp < 0xfffff000 && !(ebp & 3); d++) { ra[d] = rd32(ebp + 4); ebp = rd32(ebp); }
    }
    extern void low_stats(u32 *, u32 *);
    u32 umb, fmb; low_stats(&umb, &fmb);
    rlog("[vmlog] pid %d: %u KB -> %#x (%d) from %#x <- %#x <- %#x <- %#x; %u MB in use, largest free %u MB", (int)getpid(), len >> 10, at, kr, eip, ra[0], ra[1], ra[2], umb, fmb);
}
void r2_vm_log(ss64_t *s, u32 len, u32 at, int kr) { t_vm_s = s; vm_log(len, at, kr); }
static kern_return_t vm_alloc32_(u32 *addr, uint64_t size, int flags);
static kern_return_t vm_alloc32(u32 *addr, uint64_t size, int flags) {
    kern_return_t kr = vm_alloc32_(addr, size, flags);
    vm_log((u32)((size + R32_PAGE - 1) & ~(uint64_t)(R32_PAGE - 1)), kr ? 0 : *addr, kr);
    if (!kr && mmaplog_hit(*addr, (uint64_t)*addr + size)) {
        mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
        rlog("[mmaplog] vm_allocate %#x-%#llx thread %#x", *addr, (unsigned long long)(*addr + size), me);
    }
    return kr;
}
static kern_return_t vm_alloc32_(u32 *addr, uint64_t size, int flags) {
    u32 len = (u32)((size + R32_PAGE - 1) & ~(uint64_t)(R32_PAGE - 1));
    if (!len || size > R32_TOP) return KERN_INVALID_ARGUMENT;
    u32 at;
    if (flags & VM_FLAGS_ANYWHERE) {
        at = low_alloc(len, 0, 0);
        if (!at) {
            /* out of 32-bit address space: say how full it is (translate's own allocator then exits
               with "translate fatal": Illustrator / GoLive CS2 after a copy) */
            extern void low_stats(u32 *, u32 *);
            u32 umb, fmb; low_stats(&umb, &fmb);
            rlog("[vm] vm_allocate of %u KB: no 32-bit space (%u MB in use, largest free run %u MB)", len >> 10, umb, fmb);
            { extern void low_dump_usage(void), low_dump_holes(void); low_dump_holes(); low_dump_usage(); }
            return KERN_NO_SPACE;
        }
    }
    else {
        at = *addr & ~(R32_PAGE - 1);
        if ((uint64_t)at + len > R32_TOP || !low_is_free(at, len)) {
            if (g_trace) rlog("[vm] vm_allocate of %u KB fixed at %#x: taken", len >> 10, at);
            return KERN_NO_SPACE;
        }
        low_mark(at, len, 1);
    }
    if (mmap(P32(at), len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) {
        low_mark(at, len, 0); return KERN_NO_SPACE;
    }
    *addr = at;
    return KERN_SUCCESS;
}

/* vm_region & co for the 32-bit world: only its pages (those handed out below 4 GB, and the
   commpage area) are regions; the 64-bit side's memory and the free reservation are not. */
static kern_return_t region32(u32 *addr, u32 *size, int flavor, int *info, u32 *cnt) {
    u32 s, e;
    if (!low_next_used(*addr, &s, &e)) {
        if (*addr >= 0xfffff000u) return KERN_INVALID_ADDRESS;
        s = *addr < R32_TOP ? R32_TOP : (*addr & ~(R32_PAGE - 1)); e = 0xfffff000u;
    }
    mach_vm_address_t r = s; mach_vm_size_t hs = 0; mach_port_t on = MACH_PORT_NULL;
    int hinfo[16]; memset(hinfo, 0, sizeof hinfo);
    int hflavor = flavor == VM_REGION_BASIC_INFO ? VM_REGION_BASIC_INFO_64 : flavor;
    mach_msg_type_number_t hc = flavor == VM_REGION_BASIC_INFO ? VM_REGION_BASIC_INFO_COUNT_64 : (*cnt > 16 ? 16 : *cnt);
    kern_return_t kr = mach_vm_region(mach_task_self(), &r, &hs, hflavor, (vm_region_info_t)hinfo, &hc, &on);
    if (kr) return kr;
    if (on) mach_port_deallocate(mach_task_self(), on);
    u32 start = r > s ? (u32)r : s;
    uint64_t hend = r + hs;
    u32 end = hend < e ? (u32)hend : e;
    if (end <= start) end = start + R32_PAGE;
    *addr = start; *size = end - start;
    if (flavor == VM_REGION_BASIC_INFO) {            /* 32-bit layout: offset is 4 bytes */
        if (*cnt < 8) return KERN_INVALID_ARGUMENT;
        vm_region_basic_info_data_64_t *b = (void *)hinfo;
        info[0] = b->protection; info[1] = b->max_protection; info[2] = b->inheritance; info[3] = b->shared;
        info[4] = b->reserved; info[5] = (int)(u32)b->offset; info[6] = b->behavior; info[7] = b->user_wired_count;
        *cnt = 8;
    } else {
        memcpy(info, hinfo, hc * 4); *cnt = hc;
    }
    return KERN_SUCCESS;
}

static void reply_init(mach_msg_header_t *r, const mach_msg_header_t *q, mach_msg_size_t size) {
    r->msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(q->msgh_bits) == MACH_MSG_TYPE_MAKE_SEND_ONCE ? MACH_MSG_TYPE_MOVE_SEND_ONCE : 0, 0);
    r->msgh_size = size; r->msgh_remote_port = MACH_PORT_NULL; r->msgh_local_port = q->msgh_local_port;
    r->msgh_voucher_port = 0; r->msgh_id = q->msgh_id + 100;
}

/* Request fields in the sender's byte order: a PowerPC guest's messages (passed on by the translator)
   are big-endian, as their NDR record says. The reply is in host order with the host's NDR record; the
   guest's MIG stub swaps it, as with the Snow Leopard kernel. */
static __thread int g_be;            /* per thread: one thread's big-endian (PowerPC) request must not change how another reads its own (the size 0x81000 of a stack, read as 0x100800, freed a malloc region: cfmpstress) */
static u32 in32(const unsigned char *p) { u32 v; memcpy(&v, p, 4); return g_be ? __builtin_bswap32(v) : v; }
static uint64_t in64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return g_be ? __builtin_bswap64(v) : v; }

/* returns 1 if the request was carried out (reply written to rcv), 0 to send it to the kernel */
static int vm_routine(mach_msg_header_t *q, u32 rcv, u32 rcv_size) {
    unsigned char *b = (unsigned char *)q;
    {   /* NDR int_rep (0 = big-endian): the NDR record follows the descriptors (12 bytes each from a 32-bit sender) */
        u32 ndr = 24;
        if (q->msgh_bits & MACH_MSGH_BITS_COMPLEX) ndr = 28 + 12 * *(u32 *)(b + 24);
        g_be = b[ndr + 4] == 0;
    }
    unsigned char out[1024]; memset(out, 0, sizeof out);
    mach_msg_header_t *r = (mach_msg_header_t *)out;
    kern_return_t kr;
    mach_msg_size_t rsz;
    if (g_trace > 1) rlog("[vm] routine %d (port %#x)", q->msgh_id, q->msgh_remote_port);
    switch (q->msgh_id) {
    case 3603: {                                     /* thread_get_state(thread, flavor, count) */
        /* The threads are 64-bit to the kernel, which has no i386 state for them: a thread of the
           32-bit world is either in a trap (its 32-bit registers are in the trap's context) or
           running 32-bit code (the 64-bit state holds them, with the 32-bit code segment).
           Objective-C garbage collection (iWork '09) and liboah's guest-state lookup asked for
           x86_THREAD_STATE32 and got nothing: roots on those threads were missed. */
        int flavor = (int)in32(b + 32); u32 cnt = in32(b + 36);
        if (flavor != 1 /* x86_THREAD_STATE32 */ || cnt < 16) return 0;
        mach_port_t th = q->msgh_remote_port;
        thread_identifier_info_data_t ii; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
        u32 st[16]; memset(st, 0, sizeof st);
        kr = KERN_FAILURE;
        if (thread_info(th, THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &ic) == KERN_SUCCESS) {
            extern ucontext_t *thread_trap_uc(uint64_t tid);
            uint64_t me = 0; pthread_threadid_np(0, &me);
            ucontext_t *uc = ii.thread_id == me ? cur_uc() : thread_trap_uc(ii.thread_id);
            x86_thread_state64_t t64; mach_msg_type_number_t c64 = x86_THREAD_STATE64_COUNT;
            const x86_thread_state64_t *s = 0;
            if (uc) {   /* copied with a read that cannot fault: the other thread's trap frame lies on its 32-bit stack */
                mach_vm_size_t got = 0; _STRUCT_MCONTEXT64 *mc = 0;
                if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)&uc->uc_mcontext, sizeof mc, (mach_vm_address_t)(uintptr_t)&mc, &got) == KERN_SUCCESS && mc &&
                    mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)&mc->__ss, sizeof t64, (mach_vm_address_t)(uintptr_t)&t64, &got) == KERN_SUCCESS)
                    s = &t64;
            }
            else if (thread_get_state(th, x86_THREAD_STATE64, (thread_state_t)&t64, &c64) == KERN_SUCCESS && (unsigned short)t64.__cs == g_cs32) s = &t64;
            if (s) {
                st[0] = (u32)s->__rax; st[1] = (u32)s->__rbx; st[2] = (u32)s->__rcx; st[3] = (u32)s->__rdx;
                st[4] = (u32)s->__rdi; st[5] = (u32)s->__rsi; st[6] = (u32)s->__rbp; st[7] = (u32)s->__rsp;
                st[8] = g_ds32; st[9] = (u32)s->__rflags; st[10] = (u32)s->__rip; st[11] = g_cs32;
                st[12] = g_ds32; st[13] = g_ds32; st[14] = (u32)s->__fs; st[15] = (u32)s->__gs;
                kr = KERN_SUCCESS;
            } else {
                extern int thread_pending_state(uint64_t tid, u32 *eip, u32 *esp, u32 *regs6);
                u32 e, sp, r6[6];
                if (thread_pending_state(ii.thread_id, &e, &sp, r6)) {   /* not in 32-bit code yet: its initial registers */
                    st[0] = r6[0]; st[1] = r6[1]; st[2] = r6[2]; st[3] = r6[3]; st[4] = r6[4]; st[5] = r6[5];
                    st[6] = 0; st[7] = sp; st[8] = g_ds32; st[9] = 0x202; st[10] = e; st[11] = g_cs32;
                    st[12] = g_ds32; st[13] = g_ds32; st[14] = 0; st[15] = 0;
                    kr = KERN_SUCCESS;
                }
            }
        }
        if (g_trace || getenv("R2_TSLOG")) rlog("[thread] thread_get_state(%#x, STATE32) -> %d eip %#x esp %#x", th, kr, st[10], st[7]);
        struct { mach_msg_header_t h; NDR_record_t ndr; kern_return_t ret; u32 cnt; u32 st[16]; } *p = (void *)out;
        p->ndr = NDR_record; p->ret = kr; p->cnt = kr ? 0 : 16;
        if (!kr) memcpy(p->st, st, sizeof st);
        rsz = kr ? (mach_msg_size_t)sizeof(reply_rc_t) : (mach_msg_size_t)sizeof *p;
        break;
    }
    case 3801: {                                     /* vm_allocate(addr, size, flags) */
        u32 a = in32(b + 32), sz = in32(b + 36); int fl = (int)in32(b + 40);
        kr = vm_alloc32(&a, sz, fl);
        reply_addr32_t *p = (void *)out; p->ret = kr; p->addr = a; p->ndr = NDR_record; rsz = sizeof *p;
        if (g_trace) rlog("[vm] vm_allocate %#x flags %#x -> %#x (%d) [be %d words %08x %08x %08x %08x %08x %08x %08x]", sz, fl, a, kr, g_be,
                          *(u32 *)(b + 20), *(u32 *)(b + 24), *(u32 *)(b + 28), *(u32 *)(b + 32), *(u32 *)(b + 36), *(u32 *)(b + 40), *(u32 *)(b + 44));
        break;
    }
    case 4800: {                                     /* mach_vm_allocate(addr64, size64, flags) */
        uint64_t a; uint64_t sz; int fl;
        a = in64(b + 32); sz = in64(b + 40); fl = (int)in32(b + 48);
        u32 a32 = (u32)a;
        kr = vm_alloc32(&a32, sz, fl);
        reply_addr64_t *p = (void *)out; p->ret = kr; p->addr = a32; p->ndr = NDR_record; rsz = sizeof *p;
        if (g_trace) rlog("[vm] mach_vm_allocate %#llx flags %#x -> %#x (%d)", sz, fl, a32, kr);
        break;
    }
    case 3812: case 3826: case 4811: {               /* vm_map / vm_map_64 / mach_vm_map(addr, size, mask, flags, object, offset, copy, cur, max, inh) */
        if (!(q->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return 0;
        mach_port_t obj = *(u32 *)(b + 28);
        u32 a, sz, mask; uint64_t off; int fl, copy, cur, max, inh;
        if (q->msgh_id == 3812) {
            a = in32(b + 48); sz = in32(b + 52); mask = in32(b + 56); fl = (int)in32(b + 60);
            off = in32(b + 64); copy = (int)in32(b + 68); cur = (int)in32(b + 72); max = (int)in32(b + 76); inh = (int)in32(b + 80);
        } else if (q->msgh_id == 3826) {             /* the offset is 64-bit */
            a = in32(b + 48); sz = in32(b + 52); mask = in32(b + 56); fl = (int)in32(b + 60);
            off = in64(b + 64); copy = (int)in32(b + 72); cur = (int)in32(b + 76); max = (int)in32(b + 80); inh = (int)in32(b + 84);
        } else {
            a = (u32)in64(b + 48); sz = (u32)in64(b + 56); mask = (u32)in64(b + 64); fl = (int)in32(b + 72);
            off = in64(b + 76); copy = (int)in32(b + 84); cur = (int)in32(b + 88); max = (int)in32(b + 92); inh = (int)in32(b + 96);
        }
        u32 len = (sz + R32_PAGE - 1) & ~(R32_PAGE - 1);
        if (!len) { kr = KERN_INVALID_ARGUMENT; }
        else if (fl & VM_FLAGS_ANYWHERE) { a = low_alloc(len, mask | (R32_PAGE - 1), 0); kr = a ? KERN_SUCCESS : KERN_NO_SPACE; vm_log(len, a, kr);
            if (!a) { static int nl; if (nl++ < 8) rlog("[vm] vm_map(%d) of %u KB anywhere failed: mask %#x flags %#x obj %#x off %#llx", q->msgh_id, len >> 10, mask, fl, obj, (unsigned long long)off); } }
        else { a &= ~(R32_PAGE - 1); kr = ((uint64_t)a + len <= R32_TOP && low_is_free(a, len)) ? KERN_SUCCESS : KERN_NO_SPACE; if (!kr) low_mark(a, len, 1); }
        if (!kr) {
            mach_vm_address_t ha = a;
            if (obj == MACH_PORT_NULL) {
                kr = mmap(P32(a), len, PROT32(cur) ? PROT32(cur) : PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED ? KERN_NO_SPACE : KERN_SUCCESS;
            } else {
                kr = mach_vm_map(mach_task_self(), &ha, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | (fl & 0xff000000), obj, off, copy, cur, max, inh);
                /* shared memory objects keep their protections (a read-only entry refuses execute) */
                if (kr) {
                    mach_port_type_t pt = 0; kern_return_t tk = mach_port_type(mach_task_self(), obj, &pt);
                    rlog("[vm] vm_map of object %#x at %#x len %#x off %#llx copy %d prot %d/%d inh %d failed: %d (port type %#x, %d)",
                         obj, a, len, off, copy, cur, max, inh, kr, pt, tk);
                }
            }
            if (kr) low_free(a, len);
            /* the object's send right was carried in the message: release it as the kernel would */
            if (obj != MACH_PORT_NULL && (*(u32 *)(b + 36) >> 16 & 0xff) == MACH_MSG_TYPE_MOVE_SEND) mach_port_deallocate(mach_task_self(), obj);
        }
        if (g_trace) rlog("[vm] vm_map(%d) %#x obj %#x flags %#x mask %#x off %#llx copy %d cur %d max %d inh %d -> %#x (%d) [%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x]",
                          q->msgh_id, sz, obj, fl, mask, off, copy, cur, max, inh, a, kr,
                          *(u32 *)(b + 40), *(u32 *)(b + 44), *(u32 *)(b + 48), *(u32 *)(b + 52), *(u32 *)(b + 56), *(u32 *)(b + 60), *(u32 *)(b + 64),
                          *(u32 *)(b + 68), *(u32 *)(b + 72), *(u32 *)(b + 76), *(u32 *)(b + 80), *(u32 *)(b + 84), *(u32 *)(b + 88));
        if (q->msgh_id != 4811) { reply_addr32_t *p = (void *)out; p->ret = kr; p->addr = kr ? 0 : a; p->ndr = NDR_record; rsz = sizeof *p; }
        else { reply_addr64_t *p = (void *)out; p->ret = kr; p->addr = kr ? 0 : a; p->ndr = NDR_record; rsz = sizeof *p; }
        break;
    }
    case 3814: case 4813: {                          /* vm_remap / mach_vm_remap: into the 32-bit space */
        if (!(q->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return 0;
        mach_port_t src = *(u32 *)(b + 28);
        u32 a, sz, mask; int fl, copy, inh; uint64_t sa;
        if (q->msgh_id == 3814) {
            a = in32(b + 48); sz = in32(b + 52); mask = in32(b + 56); fl = (int)in32(b + 60);
            sa = in32(b + 64); copy = (int)in32(b + 68); inh = (int)in32(b + 72);
        } else {
            a = (u32)in64(b + 48); sz = (u32)in64(b + 56); mask = (u32)in64(b + 64); fl = (int)in32(b + 72);
            sa = in64(b + 76); copy = (int)in32(b + 84); inh = (int)in32(b + 88);
        }
        u32 len = (u32)(((uint64_t)sz + (sa & (R32_PAGE - 1)) + R32_PAGE - 1) & ~(uint64_t)(R32_PAGE - 1));
        vm_prot_t cur = 0, max = 0;
        if (fl & VM_FLAGS_ANYWHERE) { a = low_alloc(len, mask | (R32_PAGE - 1), 0); kr = a ? KERN_SUCCESS : KERN_NO_SPACE; vm_log(len, a, kr); }
        else { a &= ~(R32_PAGE - 1); kr = ((uint64_t)a + len <= R32_TOP && low_is_free(a, len)) ? KERN_SUCCESS : KERN_NO_SPACE; if (!kr) low_mark(a, len, 1); }
        if (!kr) {
            mach_vm_address_t ha = a;
            kr = mach_vm_remap(mach_task_self(), &ha, sz, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, src, sa, copy, &cur, &max, inh);
            if (kr) low_free(a, len); else a = (u32)ha;
            if (src != MACH_PORT_NULL && (*(u32 *)(b + 36) >> 16 & 0xff) == MACH_MSG_TYPE_MOVE_SEND) mach_port_deallocate(mach_task_self(), src);
        }
        if (g_trace) rlog("[vm] vm_remap %#x from %#llx -> %#x (%d)", sz, (unsigned long long)sa, a, kr);
        if (q->msgh_id == 3814) {
            *(u32 *)(out + 32) = (u32)kr; *(u32 *)(out + 36) = kr ? 0 : a; *(int *)(out + 40) = cur; *(int *)(out + 44) = max; rsz = 48;
        } else {
            uint64_t a64 = kr ? 0 : a;
            *(u32 *)(out + 32) = (u32)kr; memcpy(out + 36, &a64, 8); *(int *)(out + 44) = cur; *(int *)(out + 48) = max; rsz = 52;
        }
        memcpy(out + 24, &NDR_record, 8);
        break;
    }
    case 3809: {                                     /* vm_read_overwrite(addr, size, data) -> outsize */
        u32 a = in32(b + 32), sz = in32(b + 36), d = in32(b + 40);
        mach_vm_size_t outsz = 0;
        kr = mach_vm_read_overwrite(mach_task_self(), a, sz, d, &outsz);
        reply_addr32_t *p = (void *)out; p->ret = kr; p->addr = (u32)outsz; p->ndr = NDR_record; rsz = sizeof *p;
        break;
    }
    case 3805: {                                     /* vm_read(addr, size) -> data (OOL), dataCnt */
        /* the kernel would answer a 64-bit task with a 64-bit OOL descriptor (and an address above
           4 GB): copy into new 32-bit memory and describe it the 32-bit way {address, size, flags} */
        u32 a = in32(b + 32), sz = in32(b + 36), d = 0;
        mach_vm_size_t outsz = 0;
        kr = sz ? vm_alloc32(&d, sz, VM_FLAGS_ANYWHERE) : KERN_INVALID_ARGUMENT;
        if (!kr) {
            kr = mach_vm_read_overwrite(mach_task_self(), a, sz, d, &outsz);
            if (kr) free32_user(d & ~(R32_PAGE - 1), (sz + R32_PAGE - 1) & ~(R32_PAGE - 1));
        }
        if (g_trace) rlog("[vm] vm_read %#x size %#x -> %#x (%d)", a, sz, d, kr);
        if (kr) { reply_rc_t *p = (void *)out; p->ret = kr; p->ndr = NDR_record; rsz = sizeof *p; break; }
        r->msgh_bits = MACH_MSGH_BITS_COMPLEX;
        *(u32 *)(out + 24) = 1;                                     /* one descriptor */
        *(u32 *)(out + 28) = d; *(u32 *)(out + 32) = sz;
        *(u32 *)(out + 36) = (1u << 8) | ((u32)MACH_MSG_OOL_DESCRIPTOR << 24);   /* virtual copy, OOL */
        memcpy(out + 40, &NDR_record, 8);
        *(u32 *)(out + 48) = sz;
        rsz = 52;
        break;
    }
    case 3808: {                                     /* vm_copy(src, size, dst) */
        u32 sa = in32(b + 32), sz = in32(b + 36), da = in32(b + 40);
        kr = mach_vm_copy(mach_task_self(), sa, sz, da);
        reply_rc_t *p = (void *)out; p->ret = kr; p->ndr = NDR_record; rsz = sizeof *p;
        break;
    }
    case 3807: {                                     /* vm_write(addr, data OOL, cnt) */
        if (!(q->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return 0;
        u32 data = *(u32 *)(b + 28), a = in32(b + 48), cnt = in32(b + 52);
        kr = mach_vm_write(mach_task_self(), a, (vm_offset_t)data, cnt);
        reply_rc_t *p = (void *)out; p->ret = kr; p->ndr = NDR_record; rsz = sizeof *p;
        break;
    }
    case 3802: case 4801: {                          /* vm_deallocate / mach_vm_deallocate */
        uint64_t a, sz;
        if (q->msgh_id == 3802) { a = in32(b + 32); sz = in32(b + 36); } else { a = in64(b + 32); sz = in64(b + 40); }
        /* only the 32-bit world's part, and the range stays reserved (the translator frees its whole
           2 GB __MYZEROPAGE reservation at 0 before mapping the PowerPC program into it) */
        uint64_t e = a + sz, a0 = a;
        if (a < R32_LOW) a = R32_LOW;
        if (e > R32_TOP) e = R32_TOP;
        if (mmaplog_hit(a, e)) {
            extern u32 r2_srd32(u32); ucontext_t *uc_ = cur_uc();
            if (!uc_) rlog("[mmaplog] vm_deallocate %#llx-%#llx (msg %d) outside a trap", a, e, q->msgh_id); else {
            char fr[300] = ""; int fn = 0; u32 bp = (u32)uc_->uc_mcontext->__ss.__rbp;
            for (int k = 0; k < 12 && bp > R32_LOW && bp < R32_TOP && !(bp & 3) && !low_is_free(bp & ~0xfffu, 8); k++, bp = r2_srd32(bp))
                fn += snprintf(fr + fn, sizeof fr - fn, " %#x", r2_srd32(bp + 4));
            char gb[500]; extern int r2_guest_bt(ss64_t *, char *, int); r2_guest_bt(&uc_->uc_mcontext->__ss, gb, sizeof gb);
            rlog("[mmaplog] vm_deallocate %#llx-%#llx (asked %#llx size %#llx, msg %d) eip %#x frames%s | guest%s", a, e, a0, sz, q->msgh_id, (u32)uc_->uc_mcontext->__ss.__rip, fr, gb);
        }}
        if (e > a) free32_user((u32)a & ~(R32_PAGE - 1), (u32)(e - (a & ~(uint64_t)(R32_PAGE - 1))));
        kr = KERN_SUCCESS;
        if (g_trace) rlog("[vm] deallocate %#llx-%#llx", a, e);
        reply_rc_t *p = (void *)out; p->ret = kr; p->ndr = NDR_record; rsz = sizeof *p;
        break;
    }
    case 3810: {                                     /* vm_msync(address, size, sync_flags) (PowerPC Java) */
        u32 a = in32(b + 32), sz = in32(b + 36); int fl = (int)in32(b + 40);
        {   static int off = -1; if (off < 0) off = getenv("R2_NO_MSYNC") != 0; if (off) return 0; }
        kr = mach_vm_msync(mach_task_self(), a, sz, fl);
        if (g_trace) rlog("[vm] vm_msync(%#x, %#x, %#x) -> %d", a, sz, fl, kr);
        reply_rc_t *p = (void *)out; p->ret = kr; p->ndr = NDR_record; rsz = sizeof *p;
        break;
    }
    case 3803: case 4802: {                          /* vm_protect / mach_vm_protect */
        uint64_t a, sz; int setmax, prot;
        if (q->msgh_id == 3803) { a = in32(b + 32); sz = in32(b + 36); setmax = (int)in32(b + 40); prot = (int)in32(b + 44); }
        else { a = in64(b + 32); sz = in64(b + 40); setmax = (int)in32(b + 48); prot = (int)in32(b + 52); }
        kr = mach_vm_protect(mach_task_self(), a, sz, setmax, PROT32(prot));
        reply_rc_t *p = (void *)out; p->ret = kr; p->ndr = NDR_record; rsz = sizeof *p;
        break;
    }
    case 3800: case 3824: case 4816: {               /* vm_region / vm_region_64 / mach_vm_region */
        int wide = q->msgh_id == 4816;
        u32 a; int flavor; u32 cnt;
        if (wide) { uint64_t a64 = in64(b + 32); a = a64 > 0xffffffffull ? 0xfffff000u : (u32)a64; flavor = (int)in32(b + 40); cnt = in32(b + 44); }
        else { a = in32(b + 32); flavor = (int)in32(b + 36); cnt = in32(b + 40); }
        if (cnt > 16) cnt = 16;
        u32 sz = 0; int info[16];
        kr = region32(&a, &sz, flavor, info, &cnt);
        if (g_trace > 1) rlog("[vm] region %#x flavor %d -> %#x size %#x (%d)", in32(b + 32), flavor, a, sz, kr);
        if (kr) {
            reply_rc_t *p = (void *)out; p->ret = kr; p->ndr = NDR_record; rsz = sizeof *p;
            break;
        }
        r->msgh_bits = MACH_MSGH_BITS_COMPLEX;
        *(u32 *)(out + 24) = 1;                                     /* body: one descriptor */
        mach_msg_port_descriptor_t *d = (void *)(out + 28);
        d->name = MACH_PORT_NULL; d->disposition = MACH_MSG_TYPE_MOVE_SEND; d->type = MACH_MSG_PORT_DESCRIPTOR;
        memcpy(out + 40, &NDR_record, 8);            /* a complex reply carries no RetCode */
        if (wide) {
            uint64_t a64 = a, s64 = sz;
            memcpy(out + 48, &a64, 8); memcpy(out + 56, &s64, 8); *(u32 *)(out + 64) = cnt; memcpy(out + 68, info, cnt * 4);
            rsz = 68 + cnt * 4;
        } else {
            *(u32 *)(out + 48) = a; *(u32 *)(out + 52) = sz; *(u32 *)(out + 56) = cnt; memcpy(out + 60, info, cnt * 4);
            rsz = 60 + cnt * 4;
        }
        break;
    }
    default:
        {   /* R2_TSLOG=1: the thread routines (suspend, resume, abort, get/set state ...) with the caller's thread */
            static int tsl = -1;
            if (tsl < 0) tsl = getenv("R2_TSLOG") ? 1 : 0;
            if (tsl && q->msgh_id >= 3600 && q->msgh_id < 3700) {
                mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
                struct timeval tv; gettimeofday(&tv, 0);
                rlog("[tslog] %ld.%06d thread %#x: routine %d to %#x", (long)tv.tv_sec, (int)tv.tv_usec, me, q->msgh_id, q->msgh_remote_port);
            }
        }
        if (exc_routine(q, out, &rsz)) break;
        if (thread_state_routine(q, out, &rsz, sizeof out)) break;
        /* passed to the kernel as they are (ports and ints, no addresses or OOL data that differ in size):
           vm_inherit, mach_make_memory_entry(_64), vm_behavior_set, vm_machine_attribute, vm_purgable_control */
        if (q->msgh_id == 3804 || q->msgh_id == 3825 || q->msgh_id == 3822 || q->msgh_id == 3815 || q->msgh_id == 3813 || q->msgh_id == 3830 || q->msgh_id == 4818) {   /* 4818: mach_vm_purgable_control */
            if (g_trace) rlog("[vm] kernel routine %d to the kernel", q->msgh_id);
            return 0;
        }
        if (q->msgh_id >= 3800 && q->msgh_id < 3900) rlog("[vm] kernel routine %d not handled yet", q->msgh_id);
        if (q->msgh_id >= 4800 && q->msgh_id < 4900) rlog("[vm] kernel routine %d not handled yet", q->msgh_id);
        return 0;
    }
    {   /* keep the complex bit set above */
        mach_msg_bits_t cx = r->msgh_bits & MACH_MSGH_BITS_COMPLEX;
        reply_init(r, q, rsz);
        r->msgh_bits |= cx;
    }
    if (rsz > rcv_size) return 0;
    memcpy(P32(rcv), out, rsz);
    return 1;
}

/* The translator's code-cache flush. When guest code is modified (PowerPC Java patches its compiled
   code), the translator throws away its whole 26 MB code chunk with mach_vm_deallocate and starts a
   new one, while other threads may still be running translated code in the old chunk: PowerPC Java
   1.3 crashed every run with an x86 eip in the freed chunk (NOTES-r2.md). Large frees made by the
   translator itself (its return address in the caller chain) are kept mapped for a few seconds, so
   such a thread finishes the stale block and goes back to the dispatcher, which translates afresh.
   R2_NO_CODEQUAR=1 frees at once; R2_CODEQUAR_SECS=n sets the delay (default 3). */
#define CQ_N 16
static struct { u32 a, len; uint64_t until; } g_cq[CQ_N];
static pthread_mutex_t g_cq_mu = PTHREAD_MUTEX_INITIALIZER;
static void *cq_reaper(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        uint64_t now = (uint64_t)time(0);
        pthread_mutex_lock(&g_cq_mu);
        for (int i = 0; i < CQ_N; i++) if (g_cq[i].len && now >= g_cq[i].until) {
            u32 a = g_cq[i].a, len = g_cq[i].len; g_cq[i].len = 0;
            pthread_mutex_unlock(&g_cq_mu);
            if (g_trace || mmaplog_hit(a, (uint64_t)a + len)) rlog("[codequar] freed %#x-%#x", a, a + len);
            low_free(a, len);
            pthread_mutex_lock(&g_cq_mu);
        }
        pthread_mutex_unlock(&g_cq_mu);
    }
    return 0;
}
static void cq_start(void) { pthread_t t; pthread_create(&t, 0, cq_reaper, 0); pthread_detach(t); }
static int code_quarantine(u32 a, u32 len) {
    static int off = -1, secs = 3; static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (off < 0) { off = getenv("R2_NO_CODEQUAR") != 0; const char *v = getenv("R2_CODEQUAR_SECS"); if (v && atoi(v) > 0) secs = atoi(v); }
    if (off || len < 0x1000000u || len > 0x4000000u || !cur_uc()) return 0;   /* its code chunks (26 MB), not the start-up release of the low 2 GB */
    {   /* the translator's own free: a return address in translate's text (0xb8000000-0xb8200000) */
        extern u32 r2_srd32(u32);
        ss64_t *ss = (ss64_t *)&cur_uc()->uc_mcontext->__ss; u32 ebp = (u32)ss->__rbp; int mine = 0;
        for (int d = 0; d < 6 && ebp > 0x1000 && ebp < 0xfffff000 && !(ebp & 3); d++) {
            u32 ra = r2_srd32(ebp + 4);
            if (ra >= 0xb8000000u && ra < 0xb8200000u) { mine = 1; break; }
            ebp = r2_srd32(ebp);
        }
        if (!mine) return 0;
    }
    pthread_mutex_lock(&g_cq_mu);
    int slot = -1;
    for (int i = 0; i < CQ_N; i++) if (!g_cq[i].len) { slot = i; break; }
    if (slot >= 0) { g_cq[slot].a = a; g_cq[slot].len = len; g_cq[slot].until = (uint64_t)time(0) + (uint64_t)secs; }
    pthread_mutex_unlock(&g_cq_mu);
    if (slot < 0) return 0;                      /* full: free at once */
    pthread_once(&once, cq_start);
    {   if (g_trace || mmaplog_hit(a, (uint64_t)a + len)) rlog("[codequar] translator freed %#x-%#x: kept mapped %d s", a, a + len, secs); }
    return 1;
}

/* ---- mach_msg ---- */
#define MSG_BUF (256 * 1024)
static __thread unsigned char *t_buf;

/* the pages of [a, e) the kernel took away (sent out-of-line with deallocate): reserve them again
   (PROT_NONE, as free pages of the 32-bit range are kept) and mark them free */
static void ool_gone_fixup(u32 a, u32 e) {
    while (a < e) {
        mach_vm_address_t r = a; mach_vm_size_t sz = 0; vm_region_basic_info_data_64_t bi;
        mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &c, &on);
        if (on) mach_port_deallocate(mach_task_self(), on);
        u32 he = (kr || r >= e) ? e : (u32)r;          /* [a, he) is a hole */
        if (he > a) {
            mmap(P32(a), he - a, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            low_mark(a, he - a, 0);
            if (g_trace || mmaplog_hit(a, he)) rlog("[ool] %#x-%#x sent with deallocate: free again", a, he);
        }
        if (kr || r >= e) break;
        a = (u32)(r + sz);
    }
}

/* 32-bit message at m32 -> 64-bit message in out; returns its size */
static mach_msg_size_t msg_to64(const unsigned char *m32, unsigned char *out, mach_msg_size_t sz32) {
    const mach_msg_header_t *h = (const void *)m32;
    if (!(h->msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
        /* A CFMessagePort message of the guest's (10.4/10.5 PowerPC) CF: header with msgh_id = the
           conversation id, body, then {magic 0xF1F2F3F4, msgid, byteslen, bytes}. Today's CF has a
           room for an OOL descriptor first and {magic 0xF0F2F4F8, msgid, convid, byteslen}, and
           Catalina's drops the old layout ("dropping corrupt request Mach message (0b100010)" in
           TextInputMenuAgent at every activation: TSM's input-source notes, an XML plist). Sent in
           today's layout; msg_to32 turns today's layout back into the old one (replies, and
           messages between two translated processes). */
        {
            u32 mg = sz32 >= 40 ? *(const u32 *)(m32 + 28) : 0;
            u32 blen = sz32 >= 40 ? *(const u32 *)(m32 + 36) : 0;
            if ((mg == 0xF4F3F2F1u || mg == 0xF1F2F3F4u) && *(const u32 *)(m32 + 24) == 0 && blen <= sz32 - 40 &&
                sz32 - 40 - blen < 8 && 60 + blen + 8 <= MSG_BUF) {
                u32 r = (blen + 7) & ~7u, convid = (u32)h->msgh_id, msgid = *(const u32 *)(m32 + 32);
                memcpy(out, m32, 24);
                memset(out + 24, 0, 20);                              /* body (0 descriptors) + unused descriptor */
                *(u32 *)(out + 44) = 0xF0F2F4F8u; *(u32 *)(out + 48) = msgid;
                *(u32 *)(out + 52) = convid ? convid : 1; *(u32 *)(out + 56) = blen;
                memcpy(out + 60, m32 + 40, blen); memset(out + 60 + blen, 0, r - blen);
                ((mach_msg_header_t *)out)->msgh_size = 60 + r; ((mach_msg_header_t *)out)->msgh_id = 1;
                static int once; if (!once++ || g_trace) rlog("[cfmp] old-format CFMessagePort message (msgid %u, %u bytes) sent in today's layout", msgid, blen);
                return 60 + r;
            }
        }
        memcpy(out, m32, sz32); return sz32;
    }
    memcpy(out, m32, sizeof(mach_msg_header_t) + 4);
    u32 n = *(const u32 *)(m32 + sizeof(mach_msg_header_t));
    const unsigned char *s = m32 + sizeof(mach_msg_header_t) + 4;
    unsigned char *d = out + sizeof(mach_msg_header_t) + 4;
    for (u32 i = 0; i < n; i++) {
        unsigned type = s[11];
        if (type == MACH_MSG_PORT_DESCRIPTOR) { memcpy(d, s, 12); d += 12; s += 12; }
        else {   /* OOL memory / OOL ports / volatile: 32-bit {address, size, flags} -> 64-bit {address(8), flags, size} */
            uint64_t addr = *(const u32 *)s;
            memcpy(d, &addr, 8); memcpy(d + 8, s + 8, 4); memcpy(d + 12, s + 4, 4);
            d += 16; s += 12;
        }
    }
    mach_msg_size_t rest = sz32 - (mach_msg_size_t)(s - m32);
    memcpy(d, s, rest);
    mach_msg_size_t sz64 = (mach_msg_size_t)(d - out) + rest;
    ((mach_msg_header_t *)out)->msgh_size = sz64;
    return sz64;
}

static __thread mach_msg_size_t t_need32;      /* 32-bit size of the last message msg_to32 saw */
/* a message received for a 32-bit MACH_RCV_LARGE receive that did not fit: kept for the retry */
static __thread unsigned char *t_stash; static __thread mach_port_t t_stash_port;
/* 64-bit message (received) -> 32-bit buffer; out-of-line memory is moved below 4 GB */
mach_msg_return_t msg_to32(const unsigned char *m64, u32 dst, u32 dst_size) {
    const mach_msg_header_t *h = (const void *)m64;
    mach_msg_size_t sz64 = h->msgh_size;
    const mach_msg_trailer_t *tr = (const void *)(m64 + ((sz64 + 3) & ~3u));
    mach_msg_size_t trs = tr->msgh_trailer_size;
    unsigned char *out = P32(dst);
    {   /* the size in the 32-bit layout, checked before anything (out-of-line memory) is moved */
        mach_msg_size_t sz32 = sz64;
        if (h->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
            u32 n = *(const u32 *)(m64 + sizeof(mach_msg_header_t));
            const unsigned char *s = m64 + sizeof(mach_msg_header_t) + 4;
            for (u32 i = 0; i < n; i++) { if (s[11] == MACH_MSG_PORT_DESCRIPTOR) s += 12; else { s += 16; sz32 -= 4; } }
        }
        t_need32 = sz32;
        if (sz32 + trs > dst_size) return MACH_RCV_TOO_LARGE;
    }
    if (!(h->msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
        if (sz64 >= 60 && *(const u32 *)(m64 + 44) == 0xF0F2F4F8u && *(const u32 *)(m64 + 24) == 0) {
            u32 blen = *(const u32 *)(m64 + 56);
            if (blen <= sz64 - 60) {          /* today's CFMessagePort layout -> the guest CF's (see msg_to64) */
                u32 convid = *(const u32 *)(m64 + 52), msgid = *(const u32 *)(m64 + 48);
                memcpy(out, m64, 24);
                *(u32 *)(out + 24) = 0; *(u32 *)(out + 28) = 0xF4F3F2F1u; *(u32 *)(out + 32) = msgid; *(u32 *)(out + 36) = blen;
                memmove(out + 40, m64 + 60, blen);
                ((mach_msg_header_t *)out)->msgh_size = 40 + blen; ((mach_msg_header_t *)out)->msgh_id = (int)convid;
                memcpy(out + ((40 + blen + 3) & ~3u), tr, trs);
                return MACH_MSG_SUCCESS;
            }
        }
        memcpy(out, m64, sz64 + trs);
        return MACH_MSG_SUCCESS;
    }
    unsigned char tmp[MSG_BUF / 2];
    memcpy(tmp, m64, sizeof(mach_msg_header_t) + 4);
    u32 n = *(const u32 *)(m64 + sizeof(mach_msg_header_t));
    const unsigned char *s = m64 + sizeof(mach_msg_header_t) + 4;
    unsigned char *d = tmp + sizeof(mach_msg_header_t) + 4;
    for (u32 i = 0; i < n; i++) {
        unsigned type = s[11];
        if (type == MACH_MSG_PORT_DESCRIPTOR) { memcpy(d, s, 12); d += 12; s += 12; continue; }
        if (type == 4 /* MACH_MSG_GUARDED_PORT_DESCRIPTOR */) {
            /* 64-bit {context(8), flags/disposition/type, name} -> 32-bit {context, name, flags/...}.
               It was taken for out-of-line memory: the context (a pointer in the sender, e.g. libxpc's
               guard) was copied from and vm_deallocate'd, which unmapped pages of the host malloc
               heap (Word 2004 crashed in malloc a while later). */
            memcpy(d, s, 4); memcpy(d + 4, s + 12, 4); memcpy(d + 8, s + 8, 4);
            d += 12; s += 16;
            continue;
        }
        uint64_t addr; memcpy(&addr, s, 8);
        u32 size; memcpy(&size, s + 12, 4);
        if (type == MACH_MSG_OOL_PORTS_DESCRIPTOR) size *= 4;
        u32 a32 = 0;
        if (addr && size) {
            if (addr + size <= R32_TOP) { a32 = (u32)addr;
                if (mmaplog_hit(addr, addr + size)) rlog("[mmaplog] OOL received in the 32-bit range %#llx size %#x (id %d)", (unsigned long long)addr, size, h->msgh_id); }
            else {
                u32 len = (size + R32_PAGE - 1) & ~(R32_PAGE - 1);
                a32 = low_alloc(len, 0, 0);
                if (!a32 || mmap(P32(a32), len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED)
                    return MACH_RCV_TOO_LARGE;
                memcpy(P32(a32), (void *)addr, size);
                {   static int tr = -1; if (tr < 0) tr = getenv("R2_OOLTRACE") != 0;
                    if (tr) rlog("[ool] msg id %d descriptor %u type %u: %#llx size %#x -> %#x, deallocated", h->msgh_id, i, type, (unsigned long long)addr, size, a32); }
                /* the data need not start on a page: free the pages it touches, no more (rounding the
                   size up and adding it to an unaligned address took the next page too: the first page
                   of the host's small-malloc region, Word 2004 crashed in malloc a while later) */
                {   uint64_t pa = addr & ~4095ull, pe = (addr + size + 4095) & ~4095ull;
                    if (pa < R32_TOP) rlog("[ool] received memory %#llx-%#llx straddles 4 GB: only the part above is freed", (unsigned long long)pa, (unsigned long long)pe);
                    if (pa < R32_TOP) pa = R32_TOP;
                    if (pe > pa) mach_vm_deallocate(mach_task_self(), pa, pe - pa); }
            }
        }
        memcpy(d, &a32, 4); memcpy(d + 4, s + 12, 4); memcpy(d + 8, s + 8, 4);   /* {address, size, flags} */
        d += 12; s += 16;
    }
    mach_msg_size_t body = sz64 - (mach_msg_size_t)(s - m64);
    memcpy(d, s, body); d += body;
    mach_msg_size_t sz32 = (mach_msg_size_t)(d - tmp);
    ((mach_msg_header_t *)tmp)->msgh_size = sz32;
    memcpy(d, tr, trs);
    if (sz32 + trs > dst_size) return MACH_RCV_TOO_LARGE;
    memcpy(out, tmp, sz32 + trs);
    return MACH_MSG_SUCCESS;
}

/* XPC messages to launchd that look up a Mach service name. Services whose protocol changed in
   10.15 in a way the 32-bit world's (Mojave) clients cannot speak are made not found, so the client
   fails at once instead of waiting forever for a reply (LaunchServices' lsd). R2_XPCTRACE=1 logs the names. */
static const char *g_xpc_deny[] = { "com.apple.lsd.", "com.apple.coreservices.launchservicesd", 0 };
static void xpc_lookup_filter(unsigned char *m, mach_msg_size_t sz) {
    static int tr = -1; if (tr < 0) tr = getenv("R2_XPCTRACE") != 0;
    for (mach_msg_size_t i = 32; i + 8 < sz; i++) {
        if (m[i] != 'c' || memcmp(m + i, "com.apple.", 10)) continue;
        size_t l = strnlen((char *)m + i, sz - i);
        if (tr) rlog("[xpc] lookup? %.*s", (int)l, (char *)m + i);
        int deny = 0;
        for (int k = 0; g_xpc_deny[k]; k++) if (!strncmp((char *)m + i, g_xpc_deny[k], strlen(g_xpc_deny[k]))) deny = 1;
        const char *extra = getenv("R2_XPC_DENY");            /* comma-separated name prefixes */
        while (extra && *extra && !deny) {
            const char *e = strchr(extra, ','); size_t el = e ? (size_t)(e - extra) : strlen(extra);
            if (el && !strncmp((char *)m + i, extra, el)) deny = 1;
            extra = e ? e + 1 : 0;
        }
        if (deny) {
            m[i + 4] = 'X';                     /* "com.Xpple...": no such service */
            if (tr) rlog("[xpc]   denied for the 32-bit world");
        }
        i += l;
    }
}


/* IOConnectMapMemory for the 32-bit world (io_connect_map_memory_into_task, MIG 2863): the kernel maps
   the memory anywhere in this 64-bit task, above 4 GB, and the 32-bit IOKit kept the low 32 bits of
   the address: IOHIDLib's element values (IOHIDDeviceGetValue) read an unmapped 0x10195000 (periph
   hid). The mapping is now also made below 4 GB (mach_vm_remap, shared) and the reply carries that
   address; the unmap (2864) is given the original address and frees the low one too. */
static struct { u32 lo, len; uint64_t hi; } g_iomap[64];
static pthread_mutex_t g_iomap_mu = PTHREAD_MUTEX_INITIALIZER;
static void iomap_reply(unsigned char *r32) {
    int ret; uint64_t addr, size;
    memcpy(&ret, r32 + 32, 4); memcpy(&addr, r32 + 36, 8); memcpy(&size, r32 + 44, 8);
    if (ret || !addr || !size || addr + size <= R32_TOP) return;
    u32 off = (u32)(addr & 0xfff), len = (u32)((size + off + 0xfff) & ~0xfffull);
    u32 lo = low_alloc(len, 0, 0);
    if (!lo) { rlog("[iokit] map memory %#llx size %#llx: no 32-bit space", (unsigned long long)addr, (unsigned long long)size); return; }
    mach_vm_address_t t = lo; vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &t, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                     addr & ~0xfffull, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr) { low_free(lo, len); rlog("[iokit] map memory %#llx: remap below 4 GB failed (%d)", (unsigned long long)addr, kr); return; }
    pthread_mutex_lock(&g_iomap_mu);
    for (int i = 0; i < 64; i++) if (!g_iomap[i].lo) { g_iomap[i].lo = lo + off; g_iomap[i].len = len; g_iomap[i].hi = addr; break; }
    pthread_mutex_unlock(&g_iomap_mu);
    uint64_t a32 = lo + off; memcpy(r32 + 36, &a32, 8);
    if (g_trace) rlog("[iokit] map memory %#llx size %#llx -> %#x", (unsigned long long)addr, (unsigned long long)size, lo + off);
}
static u32 iomap_unmap_req(unsigned char *q32, u32 *lenp) {     /* 2864: low address -> the kernel's */
    uint64_t a; memcpy(&a, q32 + 52, 8);
    u32 lo = 0;
    pthread_mutex_lock(&g_iomap_mu);
    for (int i = 0; i < 64; i++) if (g_iomap[i].lo && g_iomap[i].lo == a) {
        memcpy(q32 + 52, &g_iomap[i].hi, 8); lo = g_iomap[i].lo; *lenp = g_iomap[i].len; g_iomap[i].lo = 0; break; }
    pthread_mutex_unlock(&g_iomap_mu);
    return lo;
}

/* The 32-bit IOConnectMapMemory / IOConnectUnmapMemory routines (io_connect_map_memory 2819,
   io_connect_unmap_memory 2820) carry 32-bit addresses; for this 64-bit task the kernel mapped above
   4 GB and handed back the low 32 bits (IOHIDLib's element values: IOHIDDeviceGetValue read an
   unmapped 0x10195000, periph hid / hidold). They are sent as the 64-bit routines (2863 / 2864), the
   mapping is made again below 4 GB (iomap_reply) and the 32-bit reply is written for the caller. */
static int iokit_map32(mach_msg_header_t *q, u32 msg, u32 send_size, u32 rcv_size, mach_msg_return_t *mrp) {
    union { mach_msg_header_t h; unsigned char b[256]; } m;
    int map = q->msgh_id == 2819;
    if (send_size < (map ? 0x40u : 0x38u) || rcv_size < (map ? 0x2cu : 0x24u) || !(q->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return 0;
    if (((unsigned char *)q)[0x28 + 4] == 0) return 0;     /* a big-endian (PowerPC) sender: not this way */
    memset(&m, 0, sizeof m);
    memcpy(m.b, q, 0x34);                                   /* header, body, into_task port, NDR, memory_type */
    u32 a32, s32 = 0, fl = 0; memcpy(&a32, (unsigned char *)q + 0x34, 4);
    if (map) { memcpy(&s32, (unsigned char *)q + 0x38, 4); memcpy(&fl, (unsigned char *)q + 0x3c, 4); }
    u32 lo = 0, lolen = 0;
    if (map) {
        uint64_t a = a32, sz = s32;
        memcpy(m.b + 0x34, &a, 8); memcpy(m.b + 0x3c, &sz, 8); memcpy(m.b + 0x44, &fl, 4);
        m.h.msgh_id = 2863; m.h.msgh_size = 0x48;
    } else {
        uint64_t a = a32;
        unsigned char tmp[64]; memset(tmp, 0, sizeof tmp); memcpy(tmp + 52, &a, 8);
        lo = iomap_unmap_req(tmp, &lolen);                  /* the kernel's address of a low mapping */
        memcpy(&a, tmp + 52, 8);
        memcpy(m.b + 0x34, &a, 8);
        m.h.msgh_id = 2864; m.h.msgh_size = 0x3c;
    }
    mach_port_t reply = m.h.msgh_local_port;
    mach_msg_return_t mr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG, m.h.msgh_size, sizeof m, reply, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (mr != MACH_MSG_SUCCESS) { *mrp = mr; return 1; }
    unsigned char *out = P32(msg);
    int ret; memcpy(&ret, m.b + 0x20, 4);
    if (map && m.h.msgh_id == 2963 && m.h.msgh_size >= 0x34) {
        iomap_reply(m.b);                                   /* address at 0x24 now below 4 GB, if it was above */
        uint64_t a, sz; memcpy(&a, m.b + 0x24, 8); memcpy(&sz, m.b + 0x2c, 8);
        u32 a2 = (u32)a, s2 = (u32)sz;
        memcpy(out, m.b, 0x24); ((mach_msg_header_t *)out)->msgh_id = 2919; ((mach_msg_header_t *)out)->msgh_size = 0x2c;
        memcpy(out + 0x24, &a2, 4); memcpy(out + 0x28, &s2, 4);
        if (g_trace) rlog("[iokit] IOConnectMapMemory (32-bit) -> %d, %#x size %#x", ret, a2, s2);
    } else if (!map && m.h.msgh_id == 2964) {
        memcpy(out, m.b, 0x24); ((mach_msg_header_t *)out)->msgh_id = 2920; ((mach_msg_header_t *)out)->msgh_size = 0x24;
        if (!ret && lo) low_free(lo & ~0xfffu, lolen);
    } else {                                                /* an error reply (mig_reply_error_t): as it came */
        u32 n = m.h.msgh_size < rcv_size ? m.h.msgh_size : rcv_size;
        memcpy(out, m.b, n);
    }
    *mrp = MACH_MSG_SUCCESS;
    return 1;
}
static mach_msg_return_t emu_mach_msg_(u32 msg, mach_msg_option_t opt, u32 send_size, u32 rcv_size, mach_port_t rcv_name,
                                      mach_msg_timeout_t timeout, mach_port_t notify) {
    if (!t_buf) t_buf = malloc(MSG_BUF);
    static int special = -1;
    if (special < 0) special = getenv("R2_SPECIAL_REPLY") != 0;
    if (!special) opt &= ~(0x4000 /* MACH_RCV_SYNC_WAIT */ | 0x8000 /* MACH_RCV_SYNC_PEEK */ | 0x100000 /* MACH_SEND_SYNC_OVERRIDE */);
    mach_msg_header_t *q = P32(msg);
    if ((opt & MACH_SEND_MSG) && q->msgh_remote_port != mach_task_self() &&
        ((q->msgh_id >= 3800 && q->msgh_id < 3900) || (q->msgh_id >= 4800 && q->msgh_id < 4900))) {
        /* a vm_map routine sent to another name for this task (a task port the guest got another way,
           e.g. task_for_pid(getpid())): handled as for mach_task_self. The kernel would read the
           32-bit request with 64-bit fields: vm_deallocate(0x10800000, 1 page) of a guest unmapped
           0x110800000, the first page of the host's small-malloc region (Word 2004). */
        int pid = 0;
        if (pid_for_task(q->msgh_remote_port, &pid) == KERN_SUCCESS && pid == getpid()) {
            if (g_trace) rlog("[vm] routine %d to task port %#x (this task)", q->msgh_id, q->msgh_remote_port);
            q->msgh_remote_port = mach_task_self();
        } else if (q->msgh_id != 3825 && q->msgh_id != 3804)
            rlog("[vm] routine %d to task port %#x of pid %d: passed on as is", q->msgh_id, q->msgh_remote_port, pid);
    }
    if ((opt & MACH_SEND_MSG) && q->msgh_id >= 3600 && q->msgh_id < 3700) {   /* R2_TSLOG=1: every thread routine */
        static int tsl = -1;
        if (tsl < 0) tsl = getenv("R2_TSLOG") ? 1 : 0;
        if (tsl) {
            mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
            struct timeval tv; gettimeofday(&tv, 0);
            rlog("[tslog] %ld.%06d thread %#x: msg %d to %#x", (long)tv.tv_sec, (int)tv.tv_usec, me, q->msgh_id, q->msgh_remote_port);
        }
    }
    if ((opt & MACH_SEND_MSG) && (opt & MACH_RCV_MSG) &&
        (q->msgh_remote_port == mach_task_self() || (q->msgh_id >= 3613 && q->msgh_id <= 3615) || q->msgh_id == 3603 || q->msgh_id == 3604) &&
        vm_routine(q, msg, rcv_size)) {
        return MACH_MSG_SUCCESS;
    }
    mach_msg_size_t ssz = 0;
    int sent_id = (opt & MACH_SEND_MSG) ? q->msgh_id : 0;
    if ((sent_id == 2819 || sent_id == 2820) && (opt & MACH_RCV_MSG)) {
        mach_msg_return_t imr;
        if (iokit_map32(q, msg, send_size, rcv_size, &imr)) return imr;
    }
    u32 iom_lo = 0, iom_len = 0;
    if (sent_id == 2864 && send_size >= 60 && (q->msgh_bits & MACH_MSGH_BITS_COMPLEX)) iom_lo = iomap_unmap_req((unsigned char *)q, &iom_len);
    /* out-of-line memory sent with deallocate set: the kernel takes those pages out of this task, so
       they left the 32-bit reservation and stayed "in use" in its bitmap. The guest's translator
       sends a page that way a few hundred times a second (vm_allocate, then OOL with deallocate):
       the 32-bit space filled up at ~0.7 MB/s and the translator's own allocator failed
       ("translate fatal": GoLive, Illustrator), and the holes let the host's mallocs in below 4 GB.
       After the send, a range that is gone is reserved again and marked free. */
    u32 dl_a[8], dl_e[8]; unsigned char *dl_s[8]; int ndl = 0;
    if ((opt & MACH_SEND_MSG) && (q->msgh_bits & MACH_MSGH_BITS_COMPLEX) && send_size >= 28) {
        u32 n = *(u32 *)((unsigned char *)q + 24);
        const unsigned char *s = (const unsigned char *)q + 28, *end = (const unsigned char *)q + send_size;
        for (u32 i = 0; i < n && s + 12 <= end; i++, s += 12) {
            unsigned type = s[11];
            if ((type == MACH_MSG_OOL_DESCRIPTOR || type == MACH_MSG_OOL_VOLATILE_DESCRIPTOR || type == MACH_MSG_OOL_PORTS_DESCRIPTOR) && s[8] && ndl < 8) {
                u32 a = *(const u32 *)s, sz = *(const u32 *)(s + 4);
                if (type == MACH_MSG_OOL_PORTS_DESCRIPTOR) sz *= 4;
                if (a && sz && a >= R32_LOW && (uint64_t)a + sz <= R32_TOP) {
                    dl_a[ndl] = a & ~(R32_PAGE - 1); dl_e[ndl] = (u32)(((uint64_t)a + sz + R32_PAGE - 1) & ~(uint64_t)(R32_PAGE - 1));
                    dl_s[ndl] = (unsigned char *)s + 8;
                    if (mmaplog_hit(dl_a[ndl], dl_e[ndl])) rlog("[mmaplog] OOL sent with deallocate %#x-%#x (id %d, remote %#x)", dl_a[ndl], dl_e[ndl], q->msgh_id, q->msgh_remote_port);
                    ndl++;
                }
            }
        }
    }
    /* ... and since the kernel's removal left a hole in the 32-bit reservation until the fix-up
       after the send (a blocking send-receive: seconds), host allocations landed in it: SkyLight's
       region bytes sat in a guest page that went back to the guest, and a window flush read it
       after the guest's next use of the page had unmapped it (Illustrator / Photoshop / GoLive /
       InDesign died in CGContextFlush on Save As). So the memory is sent as a copy (deallocate
       off) and freed here after the send went out, as the kernel would: no hole at any time.
       R2_OOL_KERNELDEALLOC=1: the old way. */
    static int kdl = -1;
    if (kdl < 0) kdl = getenv("R2_OOL_KERNELDEALLOC") != 0;
    if (!kdl) for (int k = 0; k < ndl; k++) *dl_s[k] = 0;
    if (opt & MACH_SEND_MSG) {
        if (send_size > MSG_BUF / 2) { if (!kdl) for (int k = 0; k < ndl; k++) *dl_s[k] = 1; return MACH_SEND_TOO_LARGE; }
        ssz = msg_to64(P32(msg), t_buf, send_size);
    }
    if (!kdl) for (int k = 0; k < ndl; k++) *dl_s[k] = 1;        /* the guest's message as it was */
#define OOL_DEALLOC_FIXUP() do { for (int k_ = 0; k_ < ndl; k_++) ool_gone_fixup(dl_a[k_], dl_e[k_]); } while (0)
    mach_msg_size_t rsz = (opt & MACH_RCV_MSG) ? (MSG_BUF - 64) : 0;
    if ((opt & MACH_SEND_MSG) && q->msgh_id == 0x10000000 && ssz < 4096) xpc_lookup_filter(t_buf, ssz);
    if (g_trace > 1) rlog("[mach_msg] opt %#x id %d remote %#x local %#x size %u", opt, (opt & MACH_SEND_MSG) ? q->msgh_id : -1,
                          (opt & MACH_SEND_MSG) ? q->msgh_remote_port : 0, (opt & MACH_SEND_MSG) ? q->msgh_local_port : 0, send_size);
    mach_msg_option_t o = opt & ~MACH_RCV_LARGE;
    mach_msg_return_t mr;
    if ((opt & MACH_RCV_MSG) && !(opt & MACH_SEND_MSG) && t_stash && t_stash_port == rcv_name) {
        /* the retry of a receive that was too large: the message is already here */
        mach_msg_size_t sz = ((mach_msg_header_t *)t_stash)->msgh_size;
        const mach_msg_trailer_t *tr = (const void *)(t_stash + ((sz + 3) & ~3u));
        memcpy(t_buf, t_stash, ((sz + 3) & ~3u) + tr->msgh_trailer_size);
        free(t_stash); t_stash = 0;
        mr = MACH_MSG_SUCCESS;
    } else {
        /* A signal for this thread while it waits here: the kernel returns *_INTERRUPTED and runs the
           handler on the way out (the caller's mach_msg wrapper then retries). The host wrapper retried
           by itself, so the handler waited for a message to arrive: the translator's code-cache flush
           signals the other threads and expects their handlers to run (PowerPC Java 1.3 returned into
           a freed cache chunk). Now the interruption goes back to the 32-bit world when one of its
           signals can be delivered; otherwise it is retried here as before. R2_NO_MSGINTR=1: old way. */
        static int nointr = -1;
        if (nointr < 0) nointr = getenv("R2_NO_MSGINTR") != 0;
        mach_msg_option_t oi = nointr ? o : (o | MACH_SEND_INTERRUPT | MACH_RCV_INTERRUPT);
        mr = mach_msg((mach_msg_header_t *)t_buf, oi, ssz, rsz, rcv_name, timeout, notify);
        while (!nointr && (mr == MACH_SEND_INTERRUPTED || mr == MACH_RCV_INTERRUPTED)) {
            extern int sig32_deliverable(void);
            if (sig32_deliverable()) {
                static int nlog;
                if (g_trace || nlog++ < 3) rlog("[mach_msg] interrupted by a signal (%#x): back to the 32-bit world", mr);
                break;
            }
            if ((mr == MACH_SEND_INTERRUPTED && (opt & MACH_SEND_INTERRUPT)) || (mr == MACH_RCV_INTERRUPTED && (opt & MACH_RCV_INTERRUPT))) break;
            if (mr == MACH_RCV_INTERRUPTED) { oi &= ~MACH_SEND_MSG; ssz = 0; }   /* the send went out: receive again */
            mr = mach_msg((mach_msg_header_t *)t_buf, oi, ssz, rsz, rcv_name, timeout, notify);
        }
    }
    if (ndl && kdl) OOL_DEALLOC_FIXUP();
    else if (ndl) {
        /* sent (a send error leaves the memory with the sender, as the kernel does: MACH_SEND_*
           codes are 0x1000000x; a receive error after a send that went out still frees) */
        int sent = (mr & ~0xffu) != 0x10000000u;
        if (sent) for (int k = 0; k < ndl; k++) {
            if (mmaplog_hit(dl_a[k], dl_e[k])) rlog("[ool] %#x-%#x sent (copied): freed after the send", dl_a[k], dl_e[k]);
            free32_user(dl_a[k], dl_e[k] - dl_a[k]);
        }
    }
    {   /* R2_PTHLOG=1: old-style libpthread's 'DEAD' messages (a dead thread to reap: pthread_t at +24) */
        static int pl = -1; if (pl < 0) pl = getenv("R2_PTHLOG") != 0;
        if (pl) {
            mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
            if ((opt & MACH_SEND_MSG) && (q->msgh_id == 0x44454144 || q->msgh_id == 0x44414544))
                rlog("[pth] DEAD sent: th %#x kthread %#x (mr %#x) by %#x", *(u32 *)((char *)q + 24), q->msgh_local_port, mr, me);
            if ((opt & MACH_RCV_MSG) && mr == MACH_MSG_SUCCESS && (((mach_msg_header_t *)t_buf)->msgh_id == 0x44454144 || ((mach_msg_header_t *)t_buf)->msgh_id == 0x44414544))
                rlog("[pth] DEAD received: th %#x kthread %#x by %#x", *(u32 *)(t_buf + 24), ((mach_msg_header_t *)t_buf)->msgh_remote_port, me);
        }
    }
    {   /* R2_MSGLOG=1: every message sent and received, with wall time and thread (who talks to this
           app at a moment of interest: the 20 s app switch) */
        static int ml = -1; if (ml < 0) ml = getenv("R2_MSGLOG") != 0;
        if (ml) {
            struct timeval tv; gettimeofday(&tv, 0);
            mach_port_t me = mach_thread_self(); mach_port_deallocate(mach_task_self(), me);
            if (opt & MACH_SEND_MSG)
                rlog("[msg] %ld.%03d th %#x send id %d remote %#x local %#x size %u opt %#x tmo %u -> %#x", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), me,
                     q->msgh_id, q->msgh_remote_port, q->msgh_local_port, send_size, opt, timeout, mr);
            if ((opt & MACH_SEND_MSG) && (q->msgh_id == 0 || q->msgh_id == 1) && send_size >= 56) {
                const u32 *w = (const u32 *)q;
                rlog("[msg]   bits %#x words %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x %08x", q->msgh_bits,
                     w[6], w[7], w[8], w[9], w[10], w[10], w[11], w[12], w[13], w[14], w[15], w[16], w[17]);
            }
            if ((opt & MACH_RCV_MSG) && mr == MACH_MSG_SUCCESS) {
                mach_msg_header_t *h = (mach_msg_header_t *)t_buf;
                rlog("[msg] %ld.%03d th %#x recv id %d remote %#x local %#x size %u on %#x", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), me,
                     h->msgh_id, h->msgh_remote_port, h->msgh_local_port, h->msgh_size, rcv_name);
            } else if (opt & MACH_RCV_MSG) rlog("[msg] %ld.%03d th %#x recv on %#x -> %#x (tmo %u)", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), me, rcv_name, mr, timeout);
        }
    }
    if (mr != MACH_MSG_SUCCESS) return mr;
    if (opt & MACH_RCV_MSG) {
        mach_msg_header_t *rh = (mach_msg_header_t *)t_buf;
        if (g_trace > 1) rlog("[mach_msg]   reply id %d size %u bits %#x word32 %#x", rh->msgh_id, rh->msgh_size, rh->msgh_bits,
                              rh->msgh_size >= 36 ? *(u32 *)(t_buf + 32) : 0);
        mr = msg_to32(t_buf, msg, rcv_size);
        if (mr == MACH_MSG_SUCCESS && sent_id == 2863 && ((mach_msg_header_t *)P32(msg))->msgh_id == 2963 &&
            ((mach_msg_header_t *)P32(msg))->msgh_size >= 52) iomap_reply(P32(msg));
        if (mr == MACH_MSG_SUCCESS && iom_lo) low_free(iom_lo & ~0xfffu, iom_len);
        if (g_trace > 1 && mr == MACH_MSG_SUCCESS && (rh->msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
            u32 *w = P32(msg);
            rlog("[mach_msg]   as 32-bit: %08x %08x %08x %08x %08x %08x | %08x | %08x %08x %08x | %08x %08x %08x %08x",
                 w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13]);
        }
        if (mr == MACH_RCV_TOO_LARGE && (opt & MACH_RCV_LARGE)) {
            /* as the kernel does with MACH_RCV_LARGE: the message stays (here), the caller learns its size */
            mach_msg_size_t sz = rh->msgh_size;
            const mach_msg_trailer_t *tr = (const void *)(t_buf + ((sz + 3) & ~3u));
            size_t tot = ((sz + 3) & ~3u) + tr->msgh_trailer_size;
            free(t_stash); t_stash = malloc(tot); memcpy(t_stash, t_buf, tot);
            t_stash_port = rcv_name;
            mach_msg_header_t *uh = P32(msg);
            uh->msgh_size = t_need32 + 64;          /* room for the trailer as well */
            if (opt & 0x8 /* MACH_RCV_LARGE_IDENTITY */) uh->msgh_local_port = rh->msgh_local_port;
            if (g_trace) rlog("[mach_msg]   too large for %u bytes: kept (%u needed)", rcv_size, t_need32);
        }
        return mr;
    }
    return mr;
}

/* R2_MSGSTAT=N: every N messages of a thread, log the (send id, receive port, result) counts of that window */
static __thread struct { int id; u32 port; int mr; unsigned n; } t_ms[16];
static __thread unsigned t_msn;
static mach_msg_return_t emu_mach_msg2(u32 msg, mach_msg_option_t opt, u32 send_size, u32 rcv_size, mach_port_t rcv_name,
                                       mach_msg_timeout_t timeout, mach_port_t notify);
static mach_msg_return_t emu_mach_msg(u32 msg, mach_msg_option_t opt, u32 send_size, u32 rcv_size, mach_port_t rcv_name,
                                      mach_msg_timeout_t timeout, mach_port_t notify) {
    extern void trap_note_msg(u32 id, u32 port, u32 opt, u32 rcv);
    mach_msg_header_t *qh = P32(msg);
    trap_note_msg((opt & MACH_SEND_MSG) ? (u32)qh->msgh_id : 0, (opt & MACH_SEND_MSG) ? qh->msgh_remote_port : 0, (u32)opt, rcv_name);
    mach_msg_return_t r = emu_mach_msg2(msg, opt, send_size, rcv_size, rcv_name, timeout, notify);
    trap_note_msg(0, 0, 0, 0);
    return r;
}
static mach_msg_return_t emu_mach_msg2(u32 msg, mach_msg_option_t opt, u32 send_size, u32 rcv_size, mach_port_t rcv_name,
                                       mach_msg_timeout_t timeout, mach_port_t notify) {
    static long every = -1;
    if (every < 0) { const char *e = getenv("R2_MSGSTAT"); every = e ? atol(e) : 0; }
    if ((opt & MACH_SEND_MSG) && (opt & MACH_RCV_MSG) && (opt & MACH_RCV_TIMEOUT) && timeout >= 500) {
        /* an RPC with a long timeout that runs out: the caller sat there (Word's launch) */
        mach_msg_header_t *qh = P32(msg);
        int id = qh->msgh_id; u32 rp = qh->msgh_remote_port;
        static long slow = -1;   /* R2_SLOWRPC=ms: log RPCs slower than that, with the 32-bit caller chain */
        if (slow < 0) { const char *e = getenv("R2_SLOWRPC"); slow = e ? atol(e) : 0; }
        uint64_t t0 = slow ? mach_absolute_time() : 0;
        mach_msg_return_t mr = emu_mach_msg_(msg, opt, send_size, rcv_size, rcv_name, timeout, notify);
        if (mr == MACH_RCV_TIMED_OUT) rlog("[mach_msg] request id %d to port %#x (size %u) got no reply in %u ms", id, rp, send_size, timeout);
        if (slow) {
            mach_timebase_info_data_t tb; mach_timebase_info(&tb);
            uint64_t ms = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1000000;
            if ((long)ms >= slow && t_vm_s) {
                u32 ra[6] = { 0 }, ebp = (u32)t_vm_s->__rbp;
                for (int d = 0; d < 6 && ebp > 0x1000 && ebp < 0xfffff000 && !(ebp & 3); d++) { ra[d] = rd32(ebp + 4); ebp = rd32(ebp); }
                rlog("[mach_msg] slow RPC id %d to port %#x: %llu ms (-> %#x) eip %#x <- %#x <- %#x <- %#x <- %#x <- %#x <- %#x",
                     id, rp, (unsigned long long)ms, mr, (u32)t_vm_s->__rip, ra[0], ra[1], ra[2], ra[3], ra[4], ra[5]);
            }
        }
        return mr;
    }
    if (!every) return emu_mach_msg_(msg, opt, send_size, rcv_size, rcv_name, timeout, notify);
    int id = (opt & MACH_SEND_MSG) ? ((mach_msg_header_t *)P32(msg))->msgh_id : -1;
    u32 rp = (opt & MACH_SEND_MSG) ? ((mach_msg_header_t *)P32(msg))->msgh_remote_port : rcv_name;
    mach_msg_return_t mr = emu_mach_msg_(msg, opt, send_size, rcv_size, rcv_name, timeout, notify);
    int i;
    for (i = 0; i < 16; i++) if (t_ms[i].n && t_ms[i].id == id && t_ms[i].port == rp && t_ms[i].mr == mr) break;
    if (i == 16) for (i = 0; i < 15 && t_ms[i].n; i++) ;
    if (!t_ms[i].n) { t_ms[i].id = id; t_ms[i].port = rp; t_ms[i].mr = mr; }
    t_ms[i].n++;
    if (++t_msn >= every) {
        for (i = 0; i < 16; i++) if (t_ms[i].n)
            rlog("[msgstat] id %d port %#x opt %#x -> %#x: %u", t_ms[i].id, t_ms[i].port, opt, t_ms[i].mr, t_ms[i].n);
        memset(t_ms, 0, sizeof t_ms); t_msn = 0;
    }
    return mr;
}

/* another name for this task's port (task_for_pid(getpid()) and the like): the vm traps were refused
   with KERN_INVALID_ARGUMENT for it, and a deallocate the guest believed done left its pages taken */
static int is_self_task(u32 name) {
    static __thread u32 last_yes, last_no;
    if (!name) return 0;
    if (name == last_yes) return 1;
    if (name == last_no) return 0;
    int pid = 0;
    int self = pid_for_task((mach_port_t)name, &pid) == KERN_SUCCESS && pid == getpid();
    if (self) { last_yes = name; rlog("[vm] task port %#x is this task (vm traps on it handled here)", name); } else last_no = name;
    return self;
}
void mach_trap(ss64_t *s, int num, u32 argp) {
    long r;
    t_vm_s = s;
    switch (-num) {
    case 26: r = mach_reply_port(); break;
    case 50:                                         /* thread_get_special_reply_port: the kernel keeps one per thread, and */
        /* the host libsystem (SkyLight's TCC check, xpc) uses the same thread's one; a second request from the */
        /* guest replaces it and the host's next sync reply fails with MACH_RCV_INVALID_NOTIFY. The guest gets an */
        /* ordinary reply port instead (its SYNC_WAIT/SYNC_OVERRIDE flags are dropped in emu_mach_msg). */
        r = getenv("R2_SPECIAL_REPLY") ? (long)trap6(50, 0, 0, 0, 0, 0, 0) : mach_reply_port();
        break;
    case 27: r = mach_thread_self(); break;
    case 28: r = mach_task_self(); break;
    case 29: r = mach_host_self(); break;
    case 31:
        {   extern int g_prof; extern void prof_caller(u32 id, u32 ra);
            if (g_prof && (A(1) & MACH_SEND_MSG)) {
                u32 id = rd32(A(0) + 20);
                static int all = -1;                            /* R2_PROF_MSG=1: every message id sent, by caller */
                if (all < 0) all = getenv("R2_PROF_MSG") != 0;
                if (all || id == 3809 || id == 3800 || id == 3805) {   /* who reads memory through the kernel: the caller's caller */
                    u32 ebp = (u32)s->__rbp, ra = 0;
                    for (int d = 0; d < 3 && ebp > 0x1000 && ebp < 0xfffff000; d++) { ra = rd32(ebp + 4); ebp = rd32(ebp); }
                    prof_caller(id, ra);
                }
            }
        }
        r = emu_mach_msg(A(0), (mach_msg_option_t)A(1), A(2), A(3), A(4), A(5), A(6)); break;
    case 32:                                         /* mach_msg_overwrite_trap: the translator's path for guest messages */
        if (A(7) && A(7) != A(0)) rlog("[mach] mach_msg_overwrite with a separate receive buffer %#x (not handled)", A(7));
        r = emu_mach_msg(A(0), (mach_msg_option_t)A(1), A(2), A(3), A(4), A(5), A(6));
        break;
    case 43: {                                       /* map_fd(fd, offset, &va, findspace, size): gone on Catalina */
        extern int g_cp_modern;
        if (g_cp_modern) {                           /* 10.7+: trap 43 is mach_generate_activity_id(task, count, u64*) */
            r = trap6(43, (long)A(0), (long)A(1), (long)A(2), 0, 0, 0);
            break;
        }
        int fd = (int)A(0); u32 off = A(1), vap = A(2), find = A(3), size = A(4);
        u32 len = (size + R32_PAGE - 1) & ~(R32_PAGE - 1), at;
        if (!len || !vap) { r = KERN_INVALID_ARGUMENT; break; }
        if (find) { at = low_alloc(len, 0, 0); if (!at) { r = KERN_NO_SPACE; break; } }
        else { at = rd32(vap) & ~(R32_PAGE - 1); if ((uint64_t)at + len > R32_TOP || at < R32_LOW) { r = KERN_INVALID_ADDRESS; break; } low_mark(at, len, 1); }
        if (mmap(P32(at), len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE, fd, off) == MAP_FAILED) {
            if (find) low_free(at, len);
            r = KERN_INVALID_ARGUMENT; break;
        }
        if (len > size) memset(P32(at + size), 0, len - size);
        wr32(vap, at);
        r = KERN_SUCCESS;
        if (g_trace) rlog("[mach] map_fd fd %d off %#x size %#x -> %#x", fd, off, size, at);
        break;
    }
    case 89: {                                       /* mach_timebase_info */
        mach_timebase_info_data_t tb; r = mach_timebase_info(&tb);
        if (A(0)) { wr32(A(0), tb.numer); wr32(A(0) + 4, tb.denom); }
        break;
    }
    /* traps with 64-bit times: a 32-bit caller passes them in two words. Passed through as is, the
       kernel saw only the low word as the deadline: a time long past, so every timer fired at once
       (the PowerPC fontd's run loop span at 200% CPU) and mach_wait_until never waited. */
    case 90:                                         /* mach_wait_until(deadline64) */
        r = trap6(90, (long)((uint64_t)A(0) | (uint64_t)A(1) << 32), 0, 0, 0, 0, 0);
        break;
    case 93:                                         /* mk_timer_arm(port, expire64) */
        r = trap6(93, (long)A(0), (long)((uint64_t)A(1) | (uint64_t)A(2) << 32), 0, 0, 0, 0);
        break;
    case 95:                                         /* mk_timer_arm_leeway(port, flags, expire64, leeway64) */
        r = trap6(95, (long)A(0), (long)A(1), (long)((uint64_t)A(2) | (uint64_t)A(3) << 32),
                  (long)((uint64_t)A(4) | (uint64_t)A(5) << 32), 0, 0);
        break;
    default:
        /* semaphores, swtch, thread_switch, timers ...: ports and ints, same layout */
        /* zero-extended: pointers above 2 GB stay valid, the kernel truncates names and ints */
        r = trap6(-num, (long)A(0), (long)A(1), (long)A(2), (long)A(3), (long)A(4), (long)A(5));
        break;
    /* the _kernelrpc_ fast paths of 10.7+ libSystem: 64-bit sizes and addresses take two words */
#define W64(i) ((uint64_t)A(i) | ((uint64_t)A((i) + 1) << 32))
    case 10: {                                       /* mach_vm_allocate(target, addr*, size64, flags) */
        if (A(0) != mach_task_self() && !is_self_task(A(0))) { r = KERN_INVALID_ARGUMENT; break; }
        u32 a = (u32)rd32(A(1));
        r = vm_alloc32(&a, W64(2), (int)A(4));
        if (!r) { wr32(A(1), a); wr32(A(1) + 4, 0); }
        break;
    }
    case 12: {                                       /* mach_vm_deallocate(target, addr64, size64) */
        if (A(0) != mach_task_self() && !is_self_task(A(0))) { r = KERN_INVALID_ARGUMENT; break; }
        uint64_t a = W64(1), e = a + W64(3);
        if (a < R32_LOW) a = R32_LOW;
        if (e > R32_TOP) e = R32_TOP;
        if (g_trace) rlog("[vm] deallocate trap %#llx-%#llx (esp %#x)", (unsigned long long)W64(1), (unsigned long long)(W64(1) + W64(3)), cur_uc() ? (u32)cur_uc()->uc_mcontext->__ss.__rsp : 0);
        if (e > a && mmaplog_hit(a, e) && cur_uc()) {   /* who frees it: the 32-bit caller chain */
            extern int r2_guest_bt(ss64_t *, char *, int); extern u32 r2_srd32(u32);
            char gb[512]; ss64_t *ss = (ss64_t *)&cur_uc()->uc_mcontext->__ss; u32 ebp = (u32)ss->__rbp, ra[6] = { 0 };
            for (int d = 0; d < 6 && ebp > 0x1000 && ebp < 0xfffff000 && !(ebp & 3); d++) { ra[d] = r2_srd32(ebp + 4); ebp = r2_srd32(ebp); }
            r2_guest_bt(ss, gb, sizeof gb);
            rlog("[mmaplog] deallocate trap %#llx-%#llx: eip %#x <- %#x <- %#x <- %#x <- %#x | %s", (unsigned long long)a, (unsigned long long)e,
                 (u32)ss->__rip, ra[0], ra[1], ra[2], ra[3], gb);
        }
        if (e > a && code_quarantine((u32)a & ~(R32_PAGE - 1), (u32)(e - (a & ~(uint64_t)(R32_PAGE - 1))))) { r = KERN_SUCCESS; break; }
        if (e > a) free32_user((u32)a & ~(R32_PAGE - 1), (u32)(e - (a & ~(uint64_t)(R32_PAGE - 1))));
        r = KERN_SUCCESS;
        break;
    }
    case 14:                                         /* mach_vm_protect(target, addr64, size64, setmax, prot) */
        r = mach_vm_protect(mach_task_self(), W64(1), W64(3), (boolean_t)A(5), PROT32((int)A(6)));
        break;
    case 15: {                                       /* mach_vm_map(target, addr*, size64, mask64, flags, cur) */
        if (A(0) != mach_task_self() && !is_self_task(A(0))) { r = KERN_INVALID_ARGUMENT; break; }
        u32 a = (u32)rd32(A(1)), len = (u32)((W64(2) + R32_PAGE - 1) & ~(uint64_t)(R32_PAGE - 1));
        u32 mask = (u32)W64(4); int fl = (int)A(6), cur = (int)A(7);
        if (fl & VM_FLAGS_ANYWHERE) a = low_alloc(len, mask | (R32_PAGE - 1), 0);
        else if (!low_is_free(a, len)) a = 0; else low_mark(a, len, 1);
        if (!a) { r = KERN_NO_SPACE; break; }
        if (mmap(P32(a), len, PROT32(cur), MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) { low_free(a, len); r = KERN_NO_SPACE; break; }
        wr32(A(1), a); wr32(A(1) + 4, 0);
        r = KERN_SUCCESS;
        break;
    }
    case 24:                                         /* mach_port_construct(target, options*, context64, name*) */
        r = trap6(24, (long)A(0), (long)A(1), (long)W64(2), (long)A(4), 0, 0);
        if (r) rlog("[mach] mach_port_construct(%#x, opts %#x {%#x %#x}, ctx %#llx, name* %#x) failed: %#lx",
                    A(0), A(1), A(1) ? rd32(A(1)) : 0, A(1) ? rd32(A(1) + 4) : 0, (unsigned long long)W64(2), A(4), r);
        break;
    case 25:                                         /* mach_port_destruct(target, name, srdelta, guard64) */
        r = trap6(25, (long)A(0), (long)A(1), (long)(int)A(2), (long)W64(3), 0, 0);
        break;
    case 41:                                         /* mach_port_guard(target, name, guard64, strict) */
        r = trap6(41, (long)A(0), (long)A(1), (long)W64(2), (long)A(4), 0, 0);
        break;
    case 42:                                         /* mach_port_unguard(target, name, guard64) */
        r = trap6(42, (long)A(0), (long)A(1), (long)W64(2), 0, 0, 0);
        break;
    }
    if (g_trace) rlog("[mach] trap %d -> %#lx", num, r);
    s->__rax = (u32)r;
}
