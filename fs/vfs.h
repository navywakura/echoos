/* Virtual filesystem API with directory support. */
#ifndef RXOS_VFS_H
#define RXOS_VFS_H

#include "rogex.h"
#include "rxfs.h"          /* for RXFS_NAME as the name-length contract */

typedef struct {
    const char *fsname;                                        /* "rxfs"   */
    int         (*count)(void);
    int         (*list)(char names[][RXFS_NAME], int max);
    const char *(*read)(const char *name, uint32_t *len);
    bool        (*write)(const char *name, const char *data, uint32_t len);
    bool        (*del)(const char *name);
    bool        (*rename)(const char *oldname, const char *newname);
    bool        (*copy)(const char *src, const char *dst);
    int         (*size)(const char *name);
    uint32_t    (*used_bytes)(void);
    /* Directory operations (NEW v4.11) */
    bool        (*mkdir)(const char *path);
    bool        (*rmdir)(const char *path);
    bool        (*chdir)(const char *path);
    const char *(*getcwd)(void);
    int         (*list_dir)(const char *path, char names[][RXFS_NAME], int max);
    bool        (*path_exists)(const char *path);
    bool        (*is_directory)(const char *path);
} vfs_ops_t;

bool        vfs_mount(const vfs_ops_t *ops);   /* single mount for now      */
bool        vfs_mounted(void);
const char *vfs_fsname(void);

/* Path-based API. Relative paths resolve against cwd via path_resolve(). */
int         vfs_count(void);
int         vfs_list(char names[][RXFS_NAME], int max);
const char *vfs_read(const char *path, uint32_t *len);
bool        vfs_write(const char *path, const char *data, uint32_t len);
bool        vfs_delete(const char *path);
bool        vfs_rename(const char *oldpath, const char *newpath);
bool        vfs_copy(const char *src, const char *dst);
int         vfs_size(const char *path);
uint32_t    vfs_used_bytes(void);

/* Directory operations (NEW v4.11) */
bool        vfs_mkdir(const char *path);
bool        vfs_rmdir(const char *path);
bool        vfs_chdir(const char *path);
const char *vfs_getcwd(void);
int         vfs_list_dir(const char *path, char names[][RXFS_NAME], int max);
bool        vfs_path_exists(const char *path);
bool        vfs_is_directory(const char *path);

/* fs/path.c: absolute normalize (keep '/', collapse //, resolve . and ..). */
bool path_normalize(const char *in, char *out, size_t max);
/* Resolve relative against cwd, then normalize to an absolute path. */
bool path_resolve(const char *in, char *out, size_t max);

#endif /* RXOS_VFS_H */
