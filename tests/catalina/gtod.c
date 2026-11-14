/* gtod.c - the first gettimeofday() / time() / CFAbsoluteTimeGetCurrent() values of a PowerPC
   process (a zero first gettimeofday made cfnet's first request "time out" at once).
   gcc -arch ppc -O1 -mmacosx-version-min=10.4 -o gtod gtod.c -framework CoreFoundation */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
int main(void) {
    struct timeval tv[5]; int i;
    for (i = 0; i < 5; i++) gettimeofday(&tv[i], 0);
    for (i = 0; i < 5; i++) printf("gettimeofday %d: %ld.%06d\n", i, (long)tv[i].tv_sec, (int)tv[i].tv_usec);
    printf("time: %ld\n", (long)time(0));
    printf("CFAbsoluteTimeGetCurrent: %.3f\n", CFAbsoluteTimeGetCurrent());
    printf("mach_absolute_time: %llu\n", (unsigned long long)mach_absolute_time());
    return 0;
}
