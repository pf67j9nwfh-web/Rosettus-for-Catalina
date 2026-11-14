/* vmbench: cost of the translator's hot Mach calls on its own task under rhost:
   vm_read_overwrite (3809), vm_region (3800), and a null system call for comparison.
   Build (Snow Leopard): gcc -std=gnu99 -arch i386 -O1 -o vmbench vmbench.c */
#include <stdio.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
static double us(uint64_t t) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (double)t * tb.numer / tb.denom / 1e3; }
int main(void) {
    static char src[4096], dst[4096];
    const int N = 100000;
    uint64_t t0 = mach_absolute_time();
    for (int i = 0; i < N; i++) getppid();
    uint64_t t1 = mach_absolute_time();
    for (int i = 0; i < N; i++) { vm_size_t out = 0; vm_read_overwrite(mach_task_self(), (vm_address_t)src, 64, (vm_address_t)dst, &out); }
    uint64_t t2 = mach_absolute_time();
    for (int i = 0; i < N; i++) {
        vm_address_t a = (vm_address_t)src; vm_size_t sz = 0; vm_region_basic_info_data_t bi;
        mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT; mach_port_t on = 0;
        vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO, (vm_region_info_t)&bi, &c, &on);
    }
    uint64_t t3 = mach_absolute_time();
    printf("getppid %.2f us, vm_read_overwrite %.2f us, vm_region %.2f us\n", us(t1 - t0) / N, us(t2 - t1) / N, us(t3 - t2) / N);
    return 0;
}
