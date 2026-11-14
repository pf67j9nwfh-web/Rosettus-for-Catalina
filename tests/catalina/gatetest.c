/* gatetest: an i386 stress test for rhost's system-call paths (the gate and the signal trap).
   Threads make system calls and Mach traps while holding values in x87 / SSE registers and checking
   them after each call; SIGALRM interrupts them all the time; sigprocmask blocks and unblocks;
   errors come back with errno; a child is forked; a signal handler's own system calls nest.
   Build (Snow Leopard): gcc -arch i386 -O1 -mmacosx-version-min=10.6 -o gatetest gatetest.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

static volatile int g_alarms, g_bad, g_stop, g_in_handler_calls;
static void on_alarm(int s) {
    (void)s; g_alarms++;
    if (getpid() <= 0) g_bad++;                    /* a system call inside the handler */
    g_in_handler_calls++;
}
static int fp_check(int iters, int id) {
    int bad = 0;
    double a = 1.25 + id, b = 3.5 * id + 0.5;
    for (int i = 0; i < iters && !g_stop; i++) {
        double x, y; float fx;
        /* values live in xmm1/xmm2 and st(0) across the calls */
        __asm__ volatile("movsd %0, %%xmm1" :: "m"(a) : "xmm1");
        __asm__ volatile("movsd %0, %%xmm2" :: "m"(b) : "xmm2");
        __asm__ volatile("flds %0" :: "m"((float){2.5f}));
        switch (i % 6) {
        case 0: getpid(); break;
        case 1: { mach_port_t p = mach_thread_self(); mach_port_deallocate(mach_task_self(), p); } break;
        case 2: { int fd = open("/nonexistent/x", O_RDONLY); if (fd >= 0 || errno != ENOENT) bad++; } break;
        case 3: { struct timeval tv; gettimeofday(&tv, 0); } break;
        case 4: { sigset_t m, o; sigemptyset(&m); sigaddset(&m, SIGUSR2); sigprocmask(SIG_BLOCK, &m, &o); sigprocmask(SIG_SETMASK, &o, 0); } break;
        case 5: { char buf[8]; int fd = open("/dev/zero", O_RDONLY); read(fd, buf, 8); close(fd); if (buf[0]) bad++; } break;
        }
        __asm__ volatile("movsd %%xmm1, %0" : "=m"(x));
        __asm__ volatile("movsd %%xmm2, %0" : "=m"(y));
        __asm__ volatile("fstps %0" : "=m"(fx));
        if (x != a || y != b || fx != 2.5f) { bad++; if (bad < 4) fprintf(stderr, "thread %d: FP changed at %d: %g %g %g\n", id, i, x, y, fx); }
    }
    return bad;
}
static void *thr(void *p) { int id = (int)(long)p; int b = fp_check(200000, id); __sync_fetch_and_add(&g_bad, b); return 0; }

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, 0);
    struct itimerval it = { { 0, 2000 }, { 0, 2000 } }; setitimer(ITIMER_REAL, &it, 0);
    uint64_t t0 = mach_absolute_time();
    pthread_t t[8];
    for (long i = 0; i < 8; i++) pthread_create(&t[i], 0, thr, (void *)(i + 1));
    int b0 = fp_check(200000, 0);
    for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    g_bad += b0;
    /* a blocked signal waits for the unblock */
    sigset_t m, o; sigemptyset(&m); sigaddset(&m, SIGALRM); sigprocmask(SIG_BLOCK, &m, &o);
    int before = g_alarms; usleep(50000); int during = g_alarms - before;
    sigprocmask(SIG_SETMASK, &o, 0); usleep(20000); int after = g_alarms - before;
    printf("blocked: %d alarms while blocked, %d after unblock\n", during, after);
    if (during != 0 || after == 0) g_bad++;
    struct itimerval z = { { 0, 0 }, { 0, 0 } }; setitimer(ITIMER_REAL, &z, 0);
    pid_t c = fork();
    if (c == 0) { if (getppid() <= 1) _exit(3); _exit(fp_check(2000, 9) ? 2 : 7); }
    int st = 0; waitpid(c, &st, 0);
    printf("fork: child exit %d\n", WEXITSTATUS(st));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 7) g_bad++;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ms = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
    printf("alarms %d (handler system calls %d), 1.8 million calls in %.0f ms, problems %d\n", g_alarms, g_in_handler_calls, ms, g_bad);
    printf("%s\n", g_bad ? "FAIL" : "PASS");
    return g_bad ? 1 : 0;
}
