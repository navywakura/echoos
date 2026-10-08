/*
 * echlibc — POSIX subset for static ELFs on EchOS (no malloc in kernel).
 * Host packages link against these names via packages/echlibc.
 */
#ifndef ECHOS_ECHLIBC_H
#define ECHOS_ECHLIBC_H

#include "rogex.h"

#define ECH_O_RDONLY  0
#define ECH_O_WRONLY  1
#define ECH_O_RDWR    2
#define ECH_FD_MAX    8
#define ECH_MMAP_MAX  (64 * 1024)   /* static window, not anonymous heap */

int   sys_open(const char *path, int flags);
int   sys_read(int fd, void *buf, unsigned n);
int   sys_write(int fd, const void *buf, unsigned n);
int   sys_close(int fd);
void *sys_mmap(void *addr, unsigned n, int prot);
int   sys_spawn(const char *path);
void  echlibc_status(char *out, size_t outmax);

#endif
