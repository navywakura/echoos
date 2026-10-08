/* The VFS operation vector for RXFS.
 *
 * It used to live in the PC userland's init, which meant a second board
 * could not mount the filesystem without pulling a desktop in behind it.
 * The table is glue between two generic modules and belongs with them.
 */
#include "rxfs.h"
#include "vfs.h"

/* RXFS operation vector for the VFS mount. */
const vfs_ops_t RXFS_OPS = {
    .fsname     = "rxfs",
    .count      = rxfs_count,
    .list       = rxfs_list,
    .read       = rxfs_read,
    .write      = rxfs_write,
    .del        = rxfs_delete,
    .rename     = rxfs_rename,
    .copy       = rxfs_copy,
    .size       = rxfs_size,
    .used_bytes = rxfs_used_bytes,
    .mkdir      = rxfs_mkdir,
    .rmdir      = rxfs_rmdir,
    .chdir      = rxfs_chdir,
    .getcwd     = rxfs_getcwd,
    .list_dir   = rxfs_list_dir,
    .path_exists= rxfs_path_exists,
    .is_directory = rxfs_is_directory,
};
