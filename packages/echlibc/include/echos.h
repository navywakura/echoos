/* Host-side echlibc stubs for static ELFs targeting EchOS. */
#ifndef ECHOS_HOST_ECHLIBC_H
#define ECHOS_HOST_ECHLIBC_H

#define ECH_O_RDONLY 0
#define ECH_O_WRONLY 1
#define ECH_O_RDWR   2

int   sys_open(const char *path, int flags);
int   sys_read(int fd, void *buf, unsigned n);
int   sys_write(int fd, const void *buf, unsigned n);
int   sys_close(int fd);
void *sys_mmap(void *addr, unsigned n, int prot);
int   sys_spawn(const char *path);

#endif
