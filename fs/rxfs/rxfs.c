/* RXFS core: fixed-slot in-RAM files with directory support. Simple on purpose. */
#include "rxfs.h"
#include "rogex_core.h"

typedef struct {
    char     name[RXFS_NAME];
    char     data[RXFS_SIZE];
    uint32_t len;
    uint32_t dir_index;              /* parent directory index */
    uint32_t next_sibling;           /* next sibling in same parent */
    rxfs_file_type_t type;
    bool     used;
} rxfs_file;

static rxfs_file g_files[RXFS_MAX_FILES];
static rxfs_dir_entry g_dirs[RXFS_MAX_DIRS];
static char g_cwd[RXFS_NAME] = "/";   /* current working directory */

static bool eqs(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void cpn(char *d, const char *s, uint32_t max) {
    uint32_t i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = '\0';
}

void rxfs_init(void) {
    for (int i = 0; i < RXFS_MAX_FILES; i++) {
        g_files[i].used = false;
        g_files[i].dir_index = 0;      /* root by default */
        g_files[i].type = RXFS_FILE_REGULAR;
    }
    /* Initialize root directory */
    for (int i = 0; i < RXFS_MAX_DIRS; i++) g_dirs[i].used = false;
    g_dirs[0].used = true;
    cpn(g_dirs[0].name, "/", RXFS_DIR_NAME_LEN);
    g_dirs[0].parent_dir = 0;
    g_dirs[0].child_file = 0xFFFFFFFF;
    g_dirs[0].next_sibling = 0xFFFFFFFF;
    g_dirs[0].type = RXFS_FILE_DIR;
    cpn(g_cwd, "/", sizeof(g_cwd));
}

/* Parse path into components, handling .. and . */
static int parse_path(const char *path, char components[][RXFS_DIR_NAME_LEN], int max_depth) {
    if (!path || !path[0]) return 0;
    int count = 0;
    const char *p = path;
    
    /* Handle absolute vs relative paths */
    if (p[0] == '/') {
        p++;
        if (!p[0]) return 0;  /* just "/" */
    }
    
    while (*p && count < max_depth) {
        /* Skip leading slashes */
        while (*p == '/') p++;
        if (!*p) break;
        
        /* Find end of component */
        const char *start = p;
        while (*p && *p != '/') p++;
        
        /* Handle . and .. */
        if (p == start + 1 && start[0] == '.') {
            continue;  /* skip . */
        } else if (p == start + 2 && start[0] == '.' && start[1] == '.') {
            if (count > 0) count--;  /* go up one level */
        } else {
            /* Copy component */
            uint32_t len = (uint32_t)(p - start);
            if (len >= RXFS_DIR_NAME_LEN) len = RXFS_DIR_NAME_LEN - 1;
            for (uint32_t i = 0; i < len; i++) components[count][i] = start[i];
            components[count][len] = '\0';
            count++;
        }
    }
    return count;
}

/* Build full path from components */
static void build_path(char *out, size_t max, char components[][RXFS_DIR_NAME_LEN], int count) {
    out[0] = '/';
    size_t pos = 1;
    for (int i = 0; i < count && pos < max - 1; i++) {
        size_t len = 0;
        while (components[i][len] && pos < max - 1) {
            out[pos++] = components[i][len++];
        }
        if (pos < max - 1 && i < count - 1) out[pos++] = '/';
    }
    out[pos] = '\0';
}

/* Find directory by path, returns index or -1. Walks g_dirs by parent_dir. */
static int find_dir_by_path(const char *path) {
    if (!path || !path[0]) return -1;
    if (eqs(path, "/")) return 0;

    char components[RXFS_MAX_DEPTH][RXFS_DIR_NAME_LEN];
    int count = parse_path(path, components, RXFS_MAX_DEPTH);
    if (count == 0) return 0;  /* root */

    int current = 0;  /* start at root */
    for (int c = 0; c < count; c++) {
        int found = -1;
        for (int i = 1; i < RXFS_MAX_DIRS; i++) {
            if (!g_dirs[i].used) continue;
            if ((int)g_dirs[i].parent_dir != current) continue;
            if (eqs(g_dirs[i].name, components[c])) { found = i; break; }
        }
        if (found < 0) return -1;
        current = found;
    }
    return current;
}

/* Get parent directory index from a path */
static int get_parent_dir(const char *path) {
    if (!path || !path[0]) return 0;
    
    /* Find last slash */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    
    if (!last_slash || last_slash == path) return 0;  /* root or no parent */
    
    /* Extract parent path */
    char parent_path[RXFS_NAME];
    size_t len = (size_t)(last_slash - path);
    if (len >= sizeof(parent_path)) len = sizeof(parent_path) - 1;
    for (size_t i = 0; i < len; i++) parent_path[i] = path[i];
    parent_path[len] = '\0';
    
    return find_dir_by_path(parent_path);
}

/* Get the filename component from a path */
static void get_basename(const char *path, char *out, size_t max) {
    if (!path || !path[0]) { out[0] = '\0'; return; }
    
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    
    const char *start = last_slash ? last_slash + 1 : path;
    cpn(out, start, (uint32_t)max);
}

/* Resolve a path to a file index. */
int rxfs_find(const char *path) {
    if (!path || !path[0]) return -1;

    char basename[RXFS_DIR_NAME_LEN];
    get_basename(path, basename, sizeof(basename));
    if (!basename[0]) return -1;  /* directory path, not a file */

    int parent = get_parent_dir(path);
    if (parent < 0) return -1;

    for (int i = 0; i < RXFS_MAX_FILES; i++) {
        if (!g_files[i].used) continue;
        if ((int)g_files[i].dir_index != parent) continue;
        if (eqs(g_files[i].name, basename)) return i;
    }
    return -1;
}

bool rxfs_write(const char *path, const char *data, uint32_t len) {
    if (len > RXFS_SIZE) len = RXFS_SIZE;
    int idx = rxfs_find(path);
    if (idx < 0) {
        /* Find free slot */
        for (int i = 0; i < RXFS_MAX_FILES; i++) {
            if (!g_files[i].used) { idx = i; break; }
        }
        if (idx < 0) return false;  /* filesystem full */
        
        /* Get parent directory */
        int parent = get_parent_dir(path);
        if (parent < 0) {
            /* Try to create intermediate directories */
            char components[RXFS_MAX_DEPTH][RXFS_DIR_NAME_LEN];
            int count = parse_path(path, components, RXFS_MAX_DEPTH);
            if (count <= 0) return false;
            
            /* Create all but the last component as directories */
            char tmp_path[RXFS_NAME] = "/";
            for (int i = 0; i < count - 1; i++) {
                if (i > 0) {
                    size_t len = k_strlen(tmp_path);
                    if (len < sizeof(tmp_path) - 1) {
                        tmp_path[len] = '/';
                        tmp_path[len + 1] = '\0';
                    }
                }
                k_strncat(tmp_path, components[i], sizeof(tmp_path) - k_strlen(tmp_path) - 1);
                /* rxfs_mkdir reports "already there" as failure, which is the
                   right answer for the `mkdir` command and the wrong one here:
                   two files in the same directory would make the second write
                   fail. Restoring a saved tree hit exactly that — every file
                   after the first in /sys/modules was lost on reboot. */
                if (find_dir_by_path(tmp_path) < 0 && !rxfs_mkdir(tmp_path))
                    return false;
            }
            parent = get_parent_dir(path);
            if (parent < 0) return false;
        }
        
        g_files[idx].used = true;
        char basename[RXFS_DIR_NAME_LEN];
        get_basename(path, basename, sizeof(basename));
        cpn(g_files[idx].name, basename, RXFS_NAME);
        g_files[idx].dir_index = (uint32_t)parent;
        g_files[idx].type = RXFS_FILE_REGULAR;
        g_files[idx].next_sibling = 0xFFFFFFFF;
    }
    for (uint32_t i = 0; i < len; i++) g_files[idx].data[i] = data[i];
    g_files[idx].len = len;
    return true;
}

const char *rxfs_read(const char *path, uint32_t *len) {
    int idx = rxfs_find(path);
    if (idx < 0) return NULL;
    if (len) *len = g_files[idx].len;
    return g_files[idx].data;
}

bool rxfs_delete(const char *path) {
    int idx = rxfs_find(path);
    if (idx < 0) return false;
    g_files[idx].used = false;
    g_files[idx].len = 0;
    return true;
}

int rxfs_count(void) {
    int n = 0;
    for (int i = 0; i < RXFS_MAX_FILES; i++) if (g_files[i].used) n++;
    return n;
}

int rxfs_list(char names[][RXFS_NAME], int max) {
    int n = 0;
    for (int i = 0; i < RXFS_MAX_FILES && n < max; i++)
        if (g_files[i].used) { cpn(names[n], g_files[i].name, RXFS_NAME); n++; }
    return n;
}

static void file_full_path(int idx, char *out, uint32_t max) {
    char parts[RXFS_MAX_DEPTH][RXFS_DIR_NAME_LEN];
    int n = 0;
    uint32_t d = g_files[idx].dir_index;
    while (d != 0 && n < RXFS_MAX_DEPTH - 1 && g_dirs[d].used) {
        cpn(parts[n++], g_dirs[d].name, RXFS_DIR_NAME_LEN);
        uint32_t p = g_dirs[d].parent_dir;
        if (p == d) break;
        d = p;
    }
    char *w = out, *e = out + max;
    if (w < e - 1) *w++ = '/';
    for (int i = n - 1; i >= 0 && w < e - 1; i--) {
        const char *s = parts[i];
        while (*s && w < e - 1) *w++ = *s++;
        if (w < e - 1) *w++ = '/';
    }
    const char *s = g_files[idx].name;
    while (*s && w < e - 1) *w++ = *s++;
    *w = '\0';
}

int rxfs_list_paths(char names[][RXFS_NAME], int max) {
    int n = 0;
    for (int i = 0; i < RXFS_MAX_FILES && n < max; i++)
        if (g_files[i].used) { file_full_path(i, names[n], RXFS_NAME); n++; }
    return n;
}

bool rxfs_rename(const char *oldpath, const char *newpath) {
    if (!newpath[0]) return false;
    int idx = rxfs_find(oldpath);
    if (idx < 0) return false;                   /* source missing        */
    if (rxfs_find(newpath) >= 0) return false;   /* destination exists    */
    
    char basename[RXFS_DIR_NAME_LEN];
    get_basename(newpath, basename, sizeof(basename));
    cpn(g_files[idx].name, basename, RXFS_NAME);
    return true;
}

bool rxfs_copy(const char *src, const char *dst) {
    if (!dst[0]) return false;
    uint32_t len = 0;
    const char *data = rxfs_read(src, &len);
    if (!data) return false;
    if (rxfs_find(src) == rxfs_find(dst)) return false;  /* same file */
    return rxfs_write(dst, data, len);
}

int rxfs_size(const char *path) {
    int idx = rxfs_find(path);
    return idx < 0 ? -1 : (int)g_files[idx].len;
}

uint32_t rxfs_used_bytes(void) {
    uint32_t n = 0;
    for (int i = 0; i < RXFS_MAX_FILES; i++)
        if (g_files[i].used) n += g_files[i].len;
    return n;
}

/* --- Directory operations (NEW v4.11) ------------------------------------ */

bool rxfs_mkdir(const char *path) {
    if (!path || !path[0]) return false;
    if (eqs(path, "/")) return true;  /* root already exists */
    
    /* Check if already exists */
    if (find_dir_by_path(path) >= 0) return false;
    
    /* Find free directory slot */
    int idx = -1;
    for (int i = 1; i < RXFS_MAX_DIRS; i++) {  /* skip root (index 0) */
        if (!g_dirs[i].used) { idx = i; break; }
    }
    if (idx < 0) return false;  /* no free slots */
    
    /* Get parent directory */
    int parent = get_parent_dir(path);
    if (parent < 0) return false;
    
    /* Initialize directory entry */
    g_dirs[idx].used = true;
    get_basename(path, g_dirs[idx].name, RXFS_DIR_NAME_LEN);
    g_dirs[idx].parent_dir = (uint32_t)parent;
    g_dirs[idx].child_file = 0xFFFFFFFF;
    g_dirs[idx].next_sibling = 0xFFFFFFFF;
    g_dirs[idx].type = RXFS_FILE_DIR;
    return true;
}

bool rxfs_rmdir(const char *path) {
    if (!path || !path[0]) return false;
    if (eqs(path, "/")) return false;  /* cannot remove root */

    int idx = find_dir_by_path(path);
    if (idx < 0 || idx == 0) return false;

    /* Empty = no child dirs and no files with this parent. */
    for (int i = 1; i < RXFS_MAX_DIRS; i++)
        if (g_dirs[i].used && (int)g_dirs[i].parent_dir == idx) return false;
    for (int i = 0; i < RXFS_MAX_FILES; i++)
        if (g_files[i].used && (int)g_files[i].dir_index == idx) return false;

    g_dirs[idx].used = false;
    return true;
}

bool rxfs_chdir(const char *path) {
    if (!path) return false;
    if (eqs(path, "/") || !path[0]) {
        cpn(g_cwd, "/", sizeof(g_cwd));
        return true;
    }
    
    int dir_idx;
    if (path[0] == '/') {
        dir_idx = find_dir_by_path(path);
    } else {
        /* Relative path - resolve from cwd */
        char full_path[RXFS_NAME];
        if (eqs(g_cwd, "/")) {
            k_snprintf(full_path, sizeof(full_path), "/%s", path);
        } else {
            k_snprintf(full_path, sizeof(full_path), "%s/%s", g_cwd, path);
        }
        dir_idx = find_dir_by_path(full_path);
    }
    
    if (dir_idx < 0) return false;
    if (g_dirs[dir_idx].type != RXFS_FILE_DIR) return false;
    
    /* Update cwd */
    if (dir_idx == 0) {
        cpn(g_cwd, "/", sizeof(g_cwd));
    } else {
        /* Reconstruct path */
        char components[RXFS_MAX_DEPTH][RXFS_DIR_NAME_LEN];
        int count = 0;
        int curr = dir_idx;
        while (curr != 0 && count < RXFS_MAX_DEPTH) {
            cpn(components[count], g_dirs[curr].name, RXFS_DIR_NAME_LEN);
            count++;
            curr = (int)g_dirs[curr].parent_dir;
        }
        
        /* Build path in reverse */
        g_cwd[0] = '/';
        size_t pos = 1;
        for (int i = count - 1; i >= 0 && pos < sizeof(g_cwd) - 1; i--) {
            size_t len = k_strlen(components[i]);
            for (size_t j = 0; j < len && pos < sizeof(g_cwd) - 1; j++) {
                g_cwd[pos++] = components[i][j];
            }
            if (pos < sizeof(g_cwd) - 1 && i > 0) g_cwd[pos++] = '/';
        }
        g_cwd[pos] = '\0';
    }
    return true;
}

const char *rxfs_getcwd(void) {
    return g_cwd;
}

/* List directory entries as basenames. Directories first (with trailing '/'),
   then regular files. Scans tables by parent rather than the fragile mixed
   sibling list (dirs live in g_dirs, files in g_files). */
int rxfs_list_dir(const char *path, char names[][RXFS_NAME], int max) {
    int dir_idx = find_dir_by_path(path ? path : "/");
    if (dir_idx < 0) return -1;

    int n = 0;
    /* Subdirectories of this parent. */
    for (int i = 1; i < RXFS_MAX_DIRS && n < max; i++) {
        if (!g_dirs[i].used) continue;
        if ((int)g_dirs[i].parent_dir != dir_idx) continue;
        cpn(names[n], g_dirs[i].name, RXFS_NAME);
        /* Mark dirs with a trailing slash for the explorer / catalog. */
        { size_t L = 0; while (names[n][L] && L + 1 < RXFS_NAME) L++;
          if (L + 1 < RXFS_NAME) { names[n][L] = '/'; names[n][L + 1] = '\0'; } }
        n++;
    }
    /* Regular files in this directory. */
    for (int i = 0; i < RXFS_MAX_FILES && n < max; i++) {
        if (!g_files[i].used) continue;
        if ((int)g_files[i].dir_index != dir_idx) continue;
        if (g_files[i].type != RXFS_FILE_REGULAR) continue;
        cpn(names[n], g_files[i].name, RXFS_NAME);
        n++;
    }
    return n;
}

bool rxfs_path_exists(const char *path) {
    if (!path || !path[0]) return false;
    return rxfs_find(path) >= 0 || find_dir_by_path(path) >= 0;
}

bool rxfs_is_directory(const char *path) {
    if (!path || !path[0]) return false;
    int dir_idx = find_dir_by_path(path);
    if (dir_idx >= 0) return true;
    int file_idx = rxfs_find(path);
    if (file_idx >= 0 && g_files[file_idx].type == RXFS_FILE_DIR) return true;
    return false;
}
