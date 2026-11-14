/* redzone: words just below the stack pointer survive a system call (as on a real kernel, and as
   when a signal frame is built 128 bytes below) 
   Build (Snow Leopard): gcc -std=gnu99 -arch i386 -O1 -mdynamic-no-pic -o redzone redzone.c */
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int bad = 0; getppid();
    for (int off = 4; off <= 124; off += 4) {
        unsigned got;
        __asm__ volatile(
            "movl $0x5eed1234, %%eax\n\t"
            "negl %1\n\t"
            "movl %%eax, (%%esp,%1)\n\t"
            "call _getppid\n\t"
            "movl (%%esp,%1), %0\n\t"
            "negl %1\n\t"
            : "=&r"(got), "+r"(off) : : "eax", "ecx", "edx", "memory", "cc");
        if (got != 0x5eed1234 && off > 8) { if (bad < 8) printf("esp-%d: %08x\n", off, got); bad++; }
    }
    printf("red zone: %d words changed: %s\n", bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
