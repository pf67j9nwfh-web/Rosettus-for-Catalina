/* sigsys2: a slow system call (read 64 KB of /dev/zero) under a fast timer signal whose handler uses
   the vector registers; xmm7 (set before the call) must be the same after it. A signal delivered on
   the way back from the call runs the handler first; its sigreturn must restore xmm7. 
   Build (Snow Leopard): gcc -std=gnu99 -arch i386 -O1 -msse2 -o sigsys2 sigsys2.c */
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
static volatile int nsig;
static volatile double sink;
static void h(int s) {
    volatile double a = 1.5, b = 2.25; (void)s;
    __asm__ volatile("pcmpeqd %%xmm7, %%xmm7" ::: "xmm7");   /* clobber xmm7 in the handler */
    sink = a * b; nsig++;
}
static char buf[65536];
int main(void) {
    struct sigaction sa; struct itimerval it;
    int fd = open("/dev/zero", O_RDONLY);
    sa.sa_handler = h; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, 0);
    it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 100;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, 0);
    long bad = 0, n;
    unsigned in[4] __attribute__((aligned(16))), out[4] __attribute__((aligned(16)));
    for (n = 0; n < 400000 && nsig < 30000; n++) {
        in[0] = 0x1234u + (unsigned)n; in[1] = 0xabcdu; in[2] = (unsigned)n * 7u; in[3] = 0x5a5a5a5au;
        __asm__ volatile("movdqa (%0), %%xmm7" :: "r"(in) : "xmm7");
        ssize_t r = read(fd, buf, sizeof buf);
        __asm__ volatile("movdqa %%xmm7, (%0)" :: "r"(out) : "memory");
        if (r != (ssize_t)sizeof buf && r >= 0) { }
        if (memcmp(in, out, 16)) {
            if (bad < 5) printf("xmm7 changed at %ld: %08x %08x %08x %08x -> %08x %08x %08x %08x\n", n, in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
            bad++;
        }
    }
    it.it_value.tv_usec = 0; it.it_interval.tv_usec = 0; setitimer(ITIMER_REAL, &it, 0);
    printf("%ld calls, %d signals, %ld bad: %s\n", n, nsig, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
