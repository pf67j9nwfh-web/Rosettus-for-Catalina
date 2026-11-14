/* kinfo32.h : the 32-bit (i386 / ppc user32) struct kinfo_proc, which a 64-bit host must hand to a
   32-bit guest for sysctl CTL_KERN.KERN_PROC. The kernel answers a 64-bit caller (rhost) with its
   64-bit layout (648 bytes per process); i386 code expects 492, so without conversion a guest
   walking the list read garbage pids and arguments (liboah's "is another Office 2004 app running"
   check saw none and removed Project Gallery's live Office shared memory: the Word it started
   crashed). Layout as xnu's user32_kinfo_proc, 4-byte packing. */
#ifndef KINFO32_H
#define KINFO32_H
#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>

#pragma pack(push, 4)
struct k32_timeval { int32_t tv_sec; int32_t tv_usec; };
struct k32_extern_proc {
    union { struct { uint32_t f, b; } st1; struct k32_timeval starttime; } p_un;
    uint32_t p_vmspace, p_sigacts;
    int32_t p_flag; char p_stat; int32_t p_pid, p_oppid, p_dupfd;
    uint32_t user_stack, exit_thread;
    int32_t p_debugger, sigwait;
    uint32_t p_estcpu; int32_t p_cpticks; uint32_t p_pctcpu;
    uint32_t p_wchan, p_wmesg;
    uint32_t p_swtime, p_slptime;
    struct k32_timeval it_interval, it_value;
    struct k32_timeval p_rtime;
    uint64_t p_uticks, p_sticks, p_iticks;
    int32_t p_traceflag; uint32_t p_tracep; int32_t p_siglist; uint32_t p_textvp; int32_t p_holdcnt;
    uint32_t p_sigmask, p_sigignore, p_sigcatch;
    unsigned char p_priority, p_usrpri; char p_nice; char p_comm[MAXCOMLEN + 1];
    uint32_t p_pgrp, p_addr; uint16_t p_xstat, p_acflag; uint32_t p_ru;
};
struct k32_eproc {
    uint32_t e_paddr, e_sess;
    struct { char pc_lock[72]; uint32_t pc_ucred; uint32_t p_ruid, p_svuid, p_rgid, p_svgid; int32_t p_refcnt; } e_pcred;
    struct { int32_t cr_ref; uint32_t cr_uid; int16_t cr_ngroups; uint32_t cr_groups[NGROUPS]; } e_ucred;
    struct { int32_t vm_refcnt; uint32_t vm_shm; int32_t vm_rssize, vm_swrss, vm_tsize, vm_dsize, vm_ssize;
             uint32_t vm_taddr, vm_daddr, vm_maxsaddr; } e_vm;
    int32_t e_ppid, e_pgid; int16_t e_jobc; int32_t e_tdev; int32_t e_tpgid; uint32_t e_tsess;
    char e_wmesg[WMESGLEN + 1];
    int32_t e_xsize; int16_t e_xrssize, e_xccount, e_xswrss; int32_t e_flag;
    char e_login[COMAPT_MAXLOGNAME]; int32_t e_spare[4];
};
struct k32_kinfo_proc { struct k32_extern_proc kp_proc; struct k32_eproc kp_eproc; };
#pragma pack(pop)

#define P2U(x) ((uint32_t)(uintptr_t)(x))
static inline void kinfo_to32(struct k32_kinfo_proc *d, const struct kinfo_proc *k) {
    const struct extern_proc *p = &k->kp_proc; const struct eproc *e = &k->kp_eproc;
    struct k32_extern_proc *q = &d->kp_proc; struct k32_eproc *f = &d->kp_eproc;
    memset(d, 0, sizeof *d);
    q->p_un.starttime.tv_sec = (int32_t)p->p_starttime.tv_sec; q->p_un.starttime.tv_usec = p->p_starttime.tv_usec;
    q->p_vmspace = P2U(p->p_vmspace); q->p_sigacts = P2U(p->p_sigacts);
    q->p_flag = p->p_flag; q->p_stat = p->p_stat; q->p_pid = p->p_pid; q->p_oppid = p->p_oppid; q->p_dupfd = p->p_dupfd;
    q->user_stack = P2U(p->user_stack); q->exit_thread = P2U(p->exit_thread);
    q->p_debugger = p->p_debugger; q->sigwait = p->sigwait;
    q->p_estcpu = p->p_estcpu; q->p_cpticks = p->p_cpticks; q->p_pctcpu = p->p_pctcpu;
    q->p_wchan = P2U(p->p_wchan); q->p_wmesg = P2U(p->p_wmesg);
    q->p_swtime = p->p_swtime; q->p_slptime = p->p_slptime;
    q->it_interval.tv_sec = (int32_t)p->p_realtimer.it_interval.tv_sec; q->it_interval.tv_usec = p->p_realtimer.it_interval.tv_usec;
    q->it_value.tv_sec = (int32_t)p->p_realtimer.it_value.tv_sec; q->it_value.tv_usec = p->p_realtimer.it_value.tv_usec;
    q->p_rtime.tv_sec = (int32_t)p->p_rtime.tv_sec; q->p_rtime.tv_usec = p->p_rtime.tv_usec;
    q->p_uticks = p->p_uticks; q->p_sticks = p->p_sticks; q->p_iticks = p->p_iticks;
    q->p_traceflag = p->p_traceflag; q->p_tracep = P2U(p->p_tracep); q->p_siglist = p->p_siglist;
    q->p_textvp = P2U(p->p_textvp); q->p_holdcnt = p->p_holdcnt;
    q->p_sigmask = p->p_sigmask; q->p_sigignore = p->p_sigignore; q->p_sigcatch = p->p_sigcatch;
    q->p_priority = p->p_priority; q->p_usrpri = p->p_usrpri; q->p_nice = p->p_nice;
    memcpy(q->p_comm, p->p_comm, sizeof q->p_comm);
    q->p_pgrp = P2U(p->p_pgrp); q->p_addr = P2U(p->p_addr); q->p_xstat = p->p_xstat; q->p_acflag = p->p_acflag; q->p_ru = P2U(p->p_ru);
    f->e_paddr = P2U(e->e_paddr); f->e_sess = P2U(e->e_sess);
    memcpy(f->e_pcred.pc_lock, e->e_pcred.pc_lock, sizeof f->e_pcred.pc_lock);
    f->e_pcred.pc_ucred = P2U(e->e_pcred.pc_ucred);
    f->e_pcred.p_ruid = e->e_pcred.p_ruid; f->e_pcred.p_svuid = e->e_pcred.p_svuid;
    f->e_pcred.p_rgid = e->e_pcred.p_rgid; f->e_pcred.p_svgid = e->e_pcred.p_svgid; f->e_pcred.p_refcnt = e->e_pcred.p_refcnt;
    f->e_ucred.cr_ref = e->e_ucred.cr_ref; f->e_ucred.cr_uid = e->e_ucred.cr_uid; f->e_ucred.cr_ngroups = e->e_ucred.cr_ngroups;
    { int i; for (i = 0; i < NGROUPS; i++) f->e_ucred.cr_groups[i] = e->e_ucred.cr_groups[i];
      /* user space sees struct vmspace as dummy fields */
      f->e_vm.vm_refcnt = e->e_vm.dummy; f->e_vm.vm_shm = P2U(e->e_vm.dummy2);
      f->e_vm.vm_rssize = e->e_vm.dummy3[0]; f->e_vm.vm_swrss = e->e_vm.dummy3[1]; f->e_vm.vm_tsize = e->e_vm.dummy3[2];
      f->e_vm.vm_dsize = e->e_vm.dummy3[3]; f->e_vm.vm_ssize = e->e_vm.dummy3[4];
      f->e_vm.vm_taddr = P2U(e->e_vm.dummy4[0]); f->e_vm.vm_daddr = P2U(e->e_vm.dummy4[1]); f->e_vm.vm_maxsaddr = P2U(e->e_vm.dummy4[2]); }
    f->e_ppid = e->e_ppid; f->e_pgid = e->e_pgid; f->e_jobc = e->e_jobc; f->e_tdev = e->e_tdev; f->e_tpgid = e->e_tpgid;
    f->e_tsess = P2U(e->e_tsess);
    memcpy(f->e_wmesg, e->e_wmesg, sizeof f->e_wmesg);
    f->e_xsize = e->e_xsize; f->e_xrssize = e->e_xrssize; f->e_xccount = e->e_xccount; f->e_xswrss = e->e_xswrss;
    f->e_flag = e->e_flag;
    memcpy(f->e_login, e->e_login, sizeof f->e_login);
    memcpy(f->e_spare, e->e_spare, sizeof f->e_spare);
}
#endif
