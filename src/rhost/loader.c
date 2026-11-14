/* loader.c - map an i386 Mach-O (main executable or dyld) into the 32-bit world, as the kernel's
   exec would. Everything else (libSystem, frameworks) is loaded by Snow Leopard's own i386 dyld. */
#include "rhost.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>

/* Snow Leopard's syscall stub "_sysenter_trap: popl %edx; movl %esp,%ecx; sysenter". In a 64-bit
   process sysenter enters the kernel as a 64-bit system call, so it becomes "int $0x80"; the trap
   handler sees "89 e1 cd 80" before eip and returns to %edx, as sysexit would. */
u32 gate_tramp32(void);                         /* trap.c: 0 when the gate is off */
void patch_sysenter(u32 addr, u32 size) {
    unsigned char *p = P32(addr);
    int n = 0;
    u32 tr = gate_tramp32();
    for (u32 i = 0; i + 5 <= size; i++) {
        if (p[i] == 0x5a && p[i + 1] == 0x89 && p[i + 2] == 0xe1 && p[i + 3] == 0x0f && p[i + 4] == 0x34) {
            if (tr) {                               /* jmp rel32 to the trampoline (it does the two moves) */
                int32_t rel = (int32_t)(tr - (addr + i + 5));
                p[i] = 0xe9; memcpy(p + i + 1, &rel, 4);
            } else { p[i + 3] = 0xcd; p[i + 4] = 0x80; }
            n++;
        }
    }
    if (n && g_trace) rlog("[load] %d sysenter stub(s) patched in %#x-%#x", n, addr, addr + size);
}

int load_macho32(const char *path, image32_t *out, int is_dyld) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { rlog("cannot open %s", path); return -1; }
    unsigned char hdr[4096];
    if (pread(fd, hdr, sizeof hdr, 0) < (ssize_t)sizeof(struct mach_header)) { close(fd); return -1; }
    off_t base = 0;
    if (OSSwapBigToHostInt32(*(uint32_t *)hdr) == FAT_MAGIC) {
        struct fat_header *fh = (struct fat_header *)hdr;
        uint32_t n = OSSwapBigToHostInt32(fh->nfat_arch);
        struct fat_arch *fa = (struct fat_arch *)(fh + 1);
        for (uint32_t i = 0; i < n; i++)
            if (OSSwapBigToHostInt32(fa[i].cputype) == CPU_TYPE_I386) base = OSSwapBigToHostInt32(fa[i].offset);
        if (!base) { rlog("%s has no i386 code", path); close(fd); return -1; }
        if (pread(fd, hdr, sizeof hdr, base) < (ssize_t)sizeof(struct mach_header)) { close(fd); return -1; }
    }
    struct mach_header *mh = (struct mach_header *)hdr;
    if (mh->magic != MH_MAGIC || mh->cputype != CPU_TYPE_I386) { rlog("%s: not i386 Mach-O", path); close(fd); return -1; }
    unsigned char *lc = hdr + sizeof *mh;
    memset(out, 0, sizeof *out);
    /* a slidable dyld (10.7 and later: linked at 0) goes where Snow Leopard's was linked */
    u32 slide = 0;
    if (is_dyld) {
        unsigned char *c = lc;
        for (uint32_t i = 0; i < mh->ncmds; i++, c += ((struct load_command *)c)->cmdsize) {
            struct segment_command *sg = (struct segment_command *)c;
            if (sg->cmd == LC_SEGMENT && sg->vmsize && !strcmp(sg->segname, SEG_TEXT)) {
                if (sg->vmaddr < R32_LOW) slide = 0x8fe00000u - sg->vmaddr;
                break;
            }
        }
        if (slide && g_trace) rlog("[load] dyld slid by %#x", slide);
    }
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        struct load_command *l = (struct load_command *)lc;
        if (l->cmd == LC_SEGMENT) {
            struct segment_command *sg = (struct segment_command *)l;
            if (sg->vmsize && sg->maxprot == 0 && strcmp(sg->segname, SEG_PAGEZERO)) {
                /* an address-space reservation (the translator's __MYZEROPAGE: the low 2 GB, kept for
                   the PowerPC program): no allocation may land there; the program maps into it itself */
                u32 va = sg->vmaddr < R32_LOW ? R32_LOW : sg->vmaddr;
                u32 end = (u32)((uint64_t)sg->vmaddr + sg->vmsize > R32_TOP ? R32_TOP : sg->vmaddr + sg->vmsize);
                if (end > va) low_mark(va, end - va, 1);
                if (g_trace) rlog("[load] %s reserves %#x-%#x", sg->segname, va, end);
            } else if (sg->vmsize && strcmp(sg->segname, SEG_PAGEZERO)) {
                u32 va = sg->vmaddr + slide, vs = (sg->vmsize + R32_PAGE - 1) & ~(R32_PAGE - 1);
                if (!low_is_free(va, vs)) rfatal("%s: segment %s at %#x is not free", path, sg->segname, va);
                low_mark(va, vs, 1);
                int prot = (sg->initprot & VM_PROT_READ ? PROT_READ : 0) | (sg->initprot & VM_PROT_WRITE ? PROT_WRITE : 0) |
                           (sg->initprot & VM_PROT_EXECUTE ? PROT_EXEC : 0);
                if (sg->filesize) {
                    u32 fs = (sg->filesize + R32_PAGE - 1) & ~(R32_PAGE - 1);
                    if (mmap(P32(va), fs, prot | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fd, base + sg->fileoff) == MAP_FAILED)
                        rfatal("%s: cannot map %s", path, sg->segname);
                    if (sg->filesize < fs) memset((char *)P32(va) + sg->filesize, 0, fs - sg->filesize);
                    if (fs < vs) mmap(P32(va + fs), vs - fs, prot | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
                    if (sg->fileoff == 0) out->mh = va;
                } else {
                    mmap(P32(va), vs, prot | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
                }
                if (prot & PROT_EXEC) patch_sysenter(va, sg->filesize);
                mprotect(P32(va), vs, prot);
                if (g_trace) rlog("[load] %s %s %#x-%#x prot %d", is_dyld ? "dyld" : "exe", sg->segname, va, va + vs, prot);
            }
        } else if (l->cmd == LC_UNIXTHREAD) {
            uint32_t *st = (uint32_t *)(lc + sizeof(struct thread_command));
            /* flavor, count, then i386_thread_state: eax ebx ecx edx edi esi ebp esp ss eflags eip ... */
            if (st[0] == 1 /* i386_THREAD_STATE */) out->entry = st[2 + 10] + slide;
        }
        lc += l->cmdsize;
    }
    close(fd);
    out->slide = slide;
    return out->mh ? 0 : -1;
}
