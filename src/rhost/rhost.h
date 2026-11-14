/* rhost - Rosettus 2 host: runs 32-bit Intel (i386) Mac code inside a 64-bit process.
   macOS 10.15 runs no 32-bit processes, but a 64-bit process may still execute 32-bit code through
   a code segment in its local descriptor table (i386_set_ldt). The 32-bit world (Snow Leopard's own
   i386 dyld, libSystem, ... and Rosetta's translator) lives below 4 GB in this process. Its system
   calls (int 0x80 / 0x81 / 0x82) trap with SIGILL; the 64-bit host emulates them and resumes it. */
#ifndef RHOST_H
#define RHOST_H
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <sys/ucontext.h>

#define R32_LOW   0x00001000u        /* first address the 32-bit world may use */
#define R32_TOP   0xfffec000u        /* start of the 32-bit commpage area */
/* i386 Mac OS X ran 32-bit code with readable data executable (no NX on heap/data);
   Rosetta's translation cache depends on it */
#define PROT32(p) (((p) & PROT_READ) ? ((p) | PROT_EXEC) : (p))
#define R32_PAGE  4096u

typedef uint32_t u32;
typedef struct __darwin_x86_thread_state64 ss64_t;

/* 32-bit register view of a trapped thread */
#define EAX(s) ((u32)(s)->__rax)
#define EBX(s) ((u32)(s)->__rbx)
#define ECX(s) ((u32)(s)->__rcx)
#define EDX(s) ((u32)(s)->__rdx)
#define ESI(s) ((u32)(s)->__rsi)
#define EDI(s) ((u32)(s)->__rdi)
#define EBP(s) ((u32)(s)->__rbp)
#define ESP(s) ((u32)(s)->__rsp)
#define EIP(s) ((u32)(s)->__rip)
#define P32(a) ((void *)(uintptr_t)(u32)(a))           /* 32-bit address -> host pointer */
#define A32(p) ((u32)(uintptr_t)(p))                    /* host pointer (below 4 GB) -> 32-bit address */
static inline u32 rd32(u32 a) { return *(volatile u32 *)P32(a); }
static inline void wr32(u32 a, u32 v) { *(volatile u32 *)P32(a) = v; }

/* globals (rhost.c) */
extern int g_trace;
extern unsigned short g_cs32, g_cs64, g_ds32;
extern char g_root32[1024];
void rlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rfatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* lowmem.c: the address space below 4 GB */
void low_init(void);
u32  low_alloc(u32 size, u32 align_mask, int top_down);   /* reserve pages; 0 on failure */
int  low_mark(u32 addr, u32 size, int used);
void low_free(u32 addr, u32 size);
int  mmaplog_hit(uint64_t a, uint64_t e);
void mapwatch_start(void);
int  low_is_free(u32 addr, u32 size);
int  low_next_used(u32 addr, u32 *start, u32 *end);

/* loader.c */
typedef struct { u32 entry; u32 mh; u32 slide; } image32_t;
int  load_macho32(const char *path, image32_t *out, int is_dyld);
void patch_sysenter(u32 addr, u32 size);

/* commpage.c */
void commpage_init(const char *dumpfile);
void rosetta_commpage_init(void);

/* trap.c */
void trap_install(void);
void thread_setup_altstack(void);
u32  ldt_data_segment(u32 base);                 /* new LDT data segment, returns selector */
void set_gs_via_stub(ss64_t *s, unsigned short sel);
extern u32 g_stub_popgs;                          /* 32-bit stub: pop gs; ret */
unsigned short tsd32_set(u32 base);               /* this thread's %gs base in 32-bit code; returns its selector */

/* sys.c: BSD syscalls; mach.c: Mach traps and messages */
void bsd_syscall(ss64_t *s, u32 num, u32 argp, int sysenter_style);
void mach_trap(ss64_t *s, int num, u32 argp);
void machdep_trap(ss64_t *s, u32 num, u32 argp);
const char *map_path(const char *p, char *buf, size_t n);

/* entering the 32-bit world on this thread (never returns) */
void enter32(u32 eip, u32 esp, u32 eax, u32 ebx, u32 ecx, u32 edx, u32 esi, u32 edi) __attribute__((noreturn));
void thread_unregister(void);
void wq_kernreturn(ss64_t *s, u32 argp);
void spawn_init(int nopts, char **opts);        /* spawn.c */
void sys_fork(ss64_t *s);
void sys_execve(ss64_t *s, u32 argp);
void sys_posix_spawn(ss64_t *s, u32 argp);
extern __thread int t_sys_newctx;
int wq_kevent_qos(ss64_t *s, u32 argp);         /* thread.c: the 32-bit world's workqueue */
int wq_kevent_id(ss64_t *s, u32 argp);
ucontext_t *thread_trap_uc(uint64_t tid);     /* trap.c */
int thread_state_routine(void *msg, unsigned char *out, unsigned int *rsz, unsigned int room);   /* exc.c */

/* system call results: value in eax, error with the carry flag set */
#include <errno.h>
#define CF 1ull
static inline void ret_ok(ss64_t *s, u32 v) { s->__rax = v; s->__rflags &= ~CF; }
static inline void ret_ok2(ss64_t *s, u32 lo, u32 hi) { s->__rax = lo; s->__rdx = hi; s->__rflags &= ~CF; }
static inline void ret_err(ss64_t *s, int e) { s->__rax = (u32)e; s->__rflags |= CF; }
static inline void ret_host(ss64_t *s, long r) { if (r == -1) ret_err(s, errno); else ret_ok(s, (u32)r); }
int sys_conv(ss64_t *s, u32 num, u32 argp);        /* sysconv.c */

/* sig.c: signals of the 32-bit world; sys.c keeps the handlers */
#include <signal.h>
#include <sys/ucontext.h>
typedef struct { u32 handler, tramp, mask, flags; } sigact32_t;
extern sigact32_t g_sig32[32];
extern __thread u32 g_sigaltstack32[3];                   /* ss_sp, ss_size, ss_flags */
int  sig32_deliver(int sig, const siginfo_t *si, ss64_t *s, ucontext_t *uc);
void sig32_return(ss64_t *s, ucontext_t *uc, u32 uctx, u32 style);
void sig32_pend(int sig, const siginfo_t *si);
void sig32_deliver_pending(ss64_t *s, ucontext_t *uc);
ucontext_t *cur_uc(void);
int r2_siglog(void);                                     /* R2_SIGLOG=1 (sig.c) */
void free32_user(u32 a, u32 len);                 /* munmap/vm_deallocate for the 32-bit world (trap.c) */
/* exc.c: Mach exception ports of the 32-bit world */
#include <mach/message.h>
int exc_routine(mach_msg_header_t *q, unsigned char *out, mach_msg_size_t *rsz);
int exc32_raise(int sig, ss64_t *s, ucontext_t *uc);
ucontext_t *cur_uc_dummy(void);                         /* host frame of the trap being handled (trap.c) */
#endif
