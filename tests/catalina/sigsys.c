/* sigsys: system calls under a fast timer signal; the registers a system call keeps (ebx esi edi ebp)
   must hold their values across a signal handler that runs on the way back from the call. 
   Build (Snow Leopard): gcc -std=gnu99 -arch i386 -O1 -mdynamic-no-pic -o sigsys sigsys.c */
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/syscall.h>
static volatile int nsig;
static void h(int s) { (void)s; nsig++; }
int main(void) {
    struct sigaction sa; struct itimerval it;
    sa.sa_handler = h; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, 0);
    it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 200;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, 0);
    long bad = 0, n;
    for (n = 0; n < 3000000 && nsig < 20000; n++) {
        unsigned b, si, di, out_b, out_si, out_di;
        b = 0x11110000u + (unsigned)n; si = 0x22220000u ^ (unsigned)n; di = 0x33330000u - (unsigned)n;
        __asm__ volatile(
            "pushl %%ebp\n\t"
            "movl $0x55aa55aa, %%ebp\n\t"
            "subl $12, %%esp\n\t"
            "call _getppid\n\t"
            "addl $12, %%esp\n\t"
            "cmpl $0x55aa55aa, %%ebp\n\t"
            "je 1f\n\t"
            "movl $0, %%ebx\n\t"
            "1: popl %%ebp\n\t"
            : "=b"(out_b), "=S"(out_si), "=D"(out_di)
            : "0"(b), "1"(si), "2"(di)
            : "eax", "ecx", "edx", "memory", "cc");
        if (out_b != b || out_si != si || out_di != di) {
            if (bad < 5) printf("mismatch at %ld: ebx %08x/%08x esi %08x/%08x edi %08x/%08x\n", n, out_b, b, out_si, si, out_di, di);
            bad++;
        }
    }
    it.it_value.tv_usec = 0; it.it_interval.tv_usec = 0; setitimer(ITIMER_REAL, &it, 0);
    printf("%ld calls, %d signals, %ld bad: %s\n", n, nsig, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
