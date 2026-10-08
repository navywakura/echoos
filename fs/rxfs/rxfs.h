/* RXFS — Rogex filesystem. Now with hierarchical directories. */
#ifndef ROGEX_RXFS_H
#define ROGEX_RXFS_H

#include "rogex.h"

#define RXFS_MAX_FILES    64       /* doubled for directory support */
#define RXFS_NAME         64       /* extended for full paths */
#define RXFS_SIZE         65536    /* bytes per file (wallpaper-sized assets fit) */
#define RXFS_MAX_DIRS     64       /* directory entries */
#define RXFS_DIR_NAME_LEN 32       /* directory name length */
#define RXFS_MAX_DEPTH    8        /* maximum path depth */

/* File types for directory awareness */
typedef enum {
    RXFS_FILE_REGULAR = 0,
    RXFS_FILE_DIR,
    RXFS_FILE_SYMLINK    /* stub for future */
} rxfs_file_type_t;

/* Directory entry structure */
typedef struct ROGEX_PACKED {
    char     name[RXFS_DIR_NAME_LEN];
    uint32_t parent_dir;            /* parent directory index */
    uint32_t child_file;            /* first child file/dir index */
    uint32_t next_sibling;          /* next sibling in same parent */
    rxfs_file_type_t type;
    bool     used;
} rxfs_dir_entry;

/* --- core operations (rxfs.c) — IMPLEMENTED ------------------------------ */
void        rxfs_init(void);
int         rxfs_find(const char *path);               /* index or -1        */
bool        rxfs_write(const char *path, const char *data, uint32_t len); /* upsert */
const char *rxfs_read(const char *path, uint32_t *len);/* ptr or NULL        */
bool        rxfs_delete(const char *path);
bool        rxfs_rename(const char *oldpath, const char *newpath);
bool        rxfs_copy(const char *src, const char *dst);
int         rxfs_count(void);
int         rxfs_size(const char *path);               /* bytes or -1        */
uint32_t    rxfs_used_bytes(void);
int         rxfs_list(char names[][RXFS_NAME], int max);
/* Like list(), but each name is a full path (`/etc/profile`) so save/load
   still work after chdir into the user's home. */
int         rxfs_list_paths(char names[][RXFS_NAME], int max);

/* --- directory operations (NEW v4.11) ------------------------------------ */
bool        rxfs_mkdir(const char *path);              /* create directory   */
bool        rxfs_rmdir(const char *path);              /* remove empty dir   */
bool        rxfs_chdir(const char *path);              /* change working dir */
const char *rxfs_getcwd(void);                         /* get current dir    */
int         rxfs_list_dir(const char *path, char names[][RXFS_NAME], int max);
bool        rxfs_path_exists(const char *path);
bool        rxfs_is_directory(const char *path);

/* --- image format (rxfs_format.c) ----------------------------------------- */
#define RXFS_IMG_MAGIC 0x31465852u   /* "RXF1" little-endian on disk */
typedef struct ROGEX_PACKED {
    uint32_t magic;                  /* RXFS_IMG_MAGIC                     */
    uint32_t version;                /* 2 (added directory support)        */
    uint32_t count;                  /* number of file records that follow */
    uint32_t dir_count;              /* number of directory entries        */
} rxfs_img_header;
typedef struct ROGEX_PACKED {
    char     name[RXFS_NAME];
    uint32_t len;
    uint32_t dir_index;              /* parent directory index */
    rxfs_file_type_t type;
    uint8_t  data[RXFS_SIZE];
} rxfs_img_file;

void rxfs_format(void);              /* wipe to an empty, valid filesystem */

/* --- persistence (rxfs_persist.c) ------------------------------------------
   Serialize/load the FS image to/from a block device. IMPLEMENTED against the
   ramdisk (proves the code path); real cross-reboot persistence is TODO v4.1
   because there is no physical-disk driver yet. */
struct blockdev;
bool rxfs_save(struct blockdev *dev);
bool rxfs_load(struct blockdev *dev);
bool rxfs_disk_has_image(struct blockdev *dev);  /* valid RXF1 header?     */
bool rxfs_disk_format(struct blockdev *dev);     /* write empty RXF1 header */

/* --- self-test (rxfs_tests.c) — run at boot; true = PASS ------------------ */
bool rxfs_selftest(void);

#endif /* ROGEX_RXFS_H */
