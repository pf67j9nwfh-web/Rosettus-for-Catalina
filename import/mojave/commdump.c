/* commdump: write the i386 commpage of this Snow Leopard Mac (gcc -arch i386) to commpage32.bin:
   records of (addr u32, size u32, bytes) for every readable page from 0xfffe0000 up */
#include <stdio.h>
#include <mach/mach.h>
int main(void) {
    FILE *f = fopen("commpage32.bin", "wb");
    static unsigned char buf[4096];
    unsigned n = 0;
    for (unsigned long a = 0xfffe0000UL; a < 0xfffff000UL + 1 && a != 0; a += 4096) {
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), a, 4096, (vm_address_t)buf, &got) || got != 4096) continue;
        unsigned aa = (unsigned)a, ss = 4096;
        fwrite(&aa, 4, 1, f); fwrite(&ss, 4, 1, f); fwrite(buf, 1, 4096, f); n++;
        if (a == 0xfffff000UL) break;
    }
    fclose(f);
    printf("%u pages; cpu_capabilities %#x ncpus %d\n", n, *(unsigned *)0xffff0020, *(unsigned char *)0xffff0022);
    return 0;
}
