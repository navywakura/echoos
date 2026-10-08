/*
 * Kernel-side echlibc. File descriptors index RXFS. mmap is a static BSS
 * window — never a dynamic allocator. spawn is a stub until user processes
 * exist (the unikernel runs one address space).
 */
#include "echlibc.h"
#include "vfs.h"

typedef struct {
    bool     used;
    char     path[RXFS_NAME];
    uint32_t off;
    int      flags;
} ech_fd_t;

static ech_fd_t g_fd[ECH_FD_MAX];
static uint8_t  g_mmap[ECH_MMAP_MAX];   /* Heap-0 BSS */
static bool     g_mmap_used;

int sys_open(const char *path, int flags) {
    int i;
    if (!path || !path[0]) return -1;
    if (flags == ECH_O_RDONLY && !vfs_path_exists(path)) return -1;
    for (i = 0; i < ECH_FD_MAX; i++) {
        if (g_fd[i].used) continue;
        g_fd[i].used = true;
        g_fd[i].off = 0;
        g_fd[i].flags = flags;
        { uint32_t n = 0; while (path[n] && n + 1 < RXFS_NAME) {
              g_fd[i].path[n] = path[n]; n++; }
          g_fd[i].path[n] = '\0'; }
        return i;
    }
    return -1;
}

int sys_read(int fd, void *buf, unsigned n) {
    uint32_t len = 0;
    const char *body;
    unsigned i;
    if (fd < 0 || fd >= ECH_FD_MAX || !g_fd[fd].used || !buf) return -1;
    body = vfs_read(g_fd[fd].path, &len);
    if (!body) return -1;
    if (g_fd[fd].off >= len) return 0;
    if (g_fd[fd].off + n > len) n = len - g_fd[fd].off;
    for (i = 0; i < n; i++) ((char *)buf)[i] = body[g_fd[fd].off + i];
    g_fd[fd].off += n;
    return (int)n;
}

int sys_write(int fd, const void *buf, unsigned n) {
    if (fd < 0 || fd >= ECH_FD_MAX || !g_fd[fd].used || !buf) return -1;
    if (fd == 1 || fd == 2) {
        unsigned i;
        for (i = 0; i < n; i++) console_putc(((const char *)buf)[i]);
        return (int)n;
    }
    if (!vfs_write(g_fd[fd].path, (const char *)buf, n)) return -1;
    return (int)n;
}

int sys_close(int fd) {
    if (fd < 0 || fd >= ECH_FD_MAX || !g_fd[fd].used) return -1;
    g_fd[fd].used = false;
    return 0;
}

void *sys_mmap(void *addr, unsigned n, int prot) {
    (void)addr; (void)prot;
    if (g_mmap_used || n > ECH_MMAP_MAX || n == 0) return NULL;
    g_mmap_used = true;
    return g_mmap;
}

int sys_spawn(const char *path) {
    (void)path;
    /* Unikernel: no second address space yet. */
    return -1;
}

void echlibc_status(char *out, size_t outmax) {
    const char *s =
        "echlibc: POSIX subset  open/read/write/close/mmap  (no malloc)\n"
        "  fds: 8 static  mmap: 64 KiB BSS  spawn: not in unikernel\n"
        "  host triple: x86_64-unknown-echos  aarch64-unknown-echos\n";
    size_t n = 0;
    if (!out || !outmax) return;
    while (s[n] && n + 1 < outmax) { out[n] = s[n]; n++; }
    out[n] = '\0';
}
