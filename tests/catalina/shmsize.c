/* shmsize: size of notifyd's shared memory (what the i386 libnotify maps) */
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    int fd = shm_open("apple.shm.notification_center", O_RDONLY, 0);
    struct stat st; fstat(fd, &st);
    void *p = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
    printf("fd %d size %lld mmap(4096) %p\n", fd, (long long)st.st_size, p);
    return 0;
}
