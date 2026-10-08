/* VFS mount table + dispatch. RXFS is the only mount today. */
#include "vfs.h"

static const vfs_ops_t *g_fs;

bool vfs_mount(const vfs_ops_t *ops) {
    if (!ops || !ops->read || !ops->write) return false;
    g_fs = ops;
    return true;
}

bool vfs_mounted(void)        { return g_fs != NULL; }
const char *vfs_fsname(void)  { return g_fs ? g_fs->fsname : "(none)"; }

int vfs_count(void) { return g_fs ? g_fs->count() : 0; }
int vfs_list(char names[][RXFS_NAME], int max) { return g_fs ? g_fs->list(names, max) : 0; }
uint32_t vfs_used_bytes(void) { return g_fs ? g_fs->used_bytes() : 0; }

/* Resolve every user path against cwd so relative names work from any
   working directory (the bug that made `cd /home/guest` then `cat README`
   fail even though the file was right there). */
const char *vfs_read(const char *path, uint32_t *len) {
    char p[RXFS_NAME];
    if (!g_fs || !path_resolve(path, p, sizeof(p))) return NULL;
    return g_fs->read(p, len);
}

bool vfs_write(const char *path, const char *data, uint32_t len) {
    char p[RXFS_NAME];
    if (!g_fs || !path_resolve(path, p, sizeof(p))) return false;
    return g_fs->write(p, data, len);
}

bool vfs_delete(const char *path) {
    char p[RXFS_NAME];
    if (!g_fs || !path_resolve(path, p, sizeof(p))) return false;
    return g_fs->del(p);
}

bool vfs_rename(const char *oldpath, const char *newpath) {
    char a[RXFS_NAME], b[RXFS_NAME];
    if (!g_fs || !path_resolve(oldpath, a, sizeof(a)) ||
        !path_resolve(newpath, b, sizeof(b))) return false;
    return g_fs->rename(a, b);
}

bool vfs_copy(const char *src, const char *dst) {
    char a[RXFS_NAME], b[RXFS_NAME];
    if (!g_fs || !path_resolve(src, a, sizeof(a)) ||
        !path_resolve(dst, b, sizeof(b))) return false;
    return g_fs->copy(a, b);
}

int vfs_size(const char *path) {
    char p[RXFS_NAME];
    if (!g_fs || !path_resolve(path, p, sizeof(p))) return -1;
    return g_fs->size(p);
}

bool vfs_mkdir(const char *path) {
    char p[RXFS_NAME];
    if (!g_fs || !g_fs->mkdir || !path_resolve(path, p, sizeof(p))) return false;
    return g_fs->mkdir(p);
}
bool vfs_rmdir(const char *path) {
    char p[RXFS_NAME];
    if (!g_fs || !g_fs->rmdir || !path_resolve(path, p, sizeof(p))) return false;
    return g_fs->rmdir(p);
}
bool vfs_chdir(const char *path) {
    char p[RXFS_NAME];
    if (!g_fs || !g_fs->chdir || !path) return false;
    /* Empty cd -> stay; otherwise resolve relative against cwd. */
    if (!path[0]) return true;
    if (!path_resolve(path, p, sizeof(p))) return false;
    return g_fs->chdir(p);
}
const char *vfs_getcwd(void) {
    return (g_fs && g_fs->getcwd) ? g_fs->getcwd() : "/";
}
int vfs_list_dir(const char *path, char names[][RXFS_NAME], int max) {
    char p[RXFS_NAME];
    if (!g_fs || !g_fs->list_dir) return -1;
    if (!path || !path[0]) {
        const char *cwd = vfs_getcwd();
        return g_fs->list_dir(cwd && cwd[0] ? cwd : "/", names, max);
    }
    if (!path_resolve(path, p, sizeof(p))) return -1;
    return g_fs->list_dir(p, names, max);
}
bool vfs_path_exists(const char *path) {
    char p[RXFS_NAME];
    if (!g_fs || !g_fs->path_exists || !path_resolve(path, p, sizeof(p))) return false;
    return g_fs->path_exists(p);
}
bool vfs_is_directory(const char *path) {
    char p[RXFS_NAME];
    if (!g_fs || !g_fs->is_directory) return false;
    if (!path || !path[0] || (path[0] == '/' && !path[1])) return true; /* root */
    if (!path_resolve(path, p, sizeof(p))) return false;
    return g_fs->is_directory(p);
}
