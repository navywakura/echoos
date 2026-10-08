/* Userland init: mount RXFS, seed built-in apps, start the shell. */
#include "init.h"
#include "vfs.h"
#include "rxfs.h"
#include "shell.h"
#include "disk.h"
#include "apps.h"
#include "pkg.h"
#include "update.h"
#include "version.h"
#include "layout.h"

/* The ops vector moved to fs/rxfs/rxfs_vfs.c: mounting the filesystem is
   not a userland decision, and another board must be able to do it. */
extern const vfs_ops_t RXFS_OPS;

int init_app_count(void) { return RXOS_BUILTIN_APP_COUNT; }
const char *init_app_name(int i) {
    return (i >= 0 && i < RXOS_BUILTIN_APP_COUNT) ? RXOS_BUILTIN_APPS[i].name : NULL;
}

bool init_userland(void) {
    if (!vfs_mount(&RXFS_OPS)) {
        klog_error("init: VFS mount failed");
        return false;
    }

    /* Persistence: if a disk (virtio-blk or ATA) with a valid RXFS image is
       attached, restore it before seeding, so the user's files (and profile)
       survive a reboot. No disk / no image -> classic amnesic boot, no error. */
    if (disk_present() && rxfs_disk_has_image((struct blockdev *)disk_dev())) {
        if (rxfs_load((struct blockdev *)disk_dev())) {
            console_write("[echOS] init: restored ");
            console_write_dec((uint64_t)rxfs_count());
            console_write(" file(s) from "); console_write(disk_name());
            console_write(" (persistent RXFS)\n");
        } else {
            klog_warn("init: disk has an RXFS image but loading it failed");
        }
    }

#if ECHOS_CLI_ONLY
    echos2_layout_seed();
#else
    /* 1.x desktop tree. 2.0 Universal uses /users not /home. */
    vfs_mkdir("/bin");
    vfs_mkdir("/etc");
    vfs_mkdir("/home");
    vfs_mkdir("/home/guest");
    vfs_mkdir("/var");
    vfs_mkdir("/var/log");
    vfs_mkdir("/tmp");
    vfs_mkdir("/trash");
    vfs_mkdir("/dev");
#endif

    /* Welcome note + sample tree leaves (idempotent upserts). */
    {
#if !ECHOS_CLI_ONLY
        static const char welcome[] =
            "Welcome to EchOS vault.\n"
            "This is RXFS — the in-memory filesystem.\n"
            "Open the File Explorer to walk /bin /etc /home /var.\n"
            "In the shell try: catalog, scribe, vanish, forge, wander.\n";
        vfs_write("/etc/motd", welcome, (uint32_t)(sizeof(welcome) - 1));
#endif
        static const char hosts[] = "# EchOS host table\nlocalhost  127.0.0.1\n";
        vfs_write("/etc/hosts", hosts, (uint32_t)(sizeof(hosts) - 1));
#if !ECHOS_CLI_ONLY
        static const char readme[] =
            "Guest home on EchOS.\nDrop notes here with `scribe` or the explorer.\n";
        vfs_write("/home/guest/README", readme, (uint32_t)(sizeof(readme) - 1));
        static const char log0[] = "boot: vault mounted\n";
        vfs_write("/var/log/boot.log", log0, (uint32_t)(sizeof(log0) - 1));
#endif
    }

    /* Seed the built-in Roxenite apps under /bin (and short names for run).
       2.0 CLI skips them: they burn RXFS slots the LIVE tree needs. */
#if !ECHOS_CLI_ONLY
    for (int i = 0; i < RXOS_BUILTIN_APP_COUNT; i++) {
        const rxos_builtin_app *a = &RXOS_BUILTIN_APPS[i];
        char path[RXFS_NAME];
        /* short name at vault root for compatibility */
        if (!vfs_write(a->name, a->source, (uint32_t)k_strlen(a->source)))
            klog_warn("init: could not seed a built-in app (fs full?)");
        /* and under /bin/ for the explorer tree */
        { char *d = path; const char *p = "/bin/";
          while (*p) *d++ = *p++;
          p = a->name; while (*p && d < path + sizeof(path) - 1) *d++ = *p++;
          *d = '\0';
          vfs_write(path, a->source, (uint32_t)k_strlen(a->source)); }
    }
    pkg_init();
#endif
    /* Update channel: seed /etc/os-release, apply a staged patch from disk. */
    update_init();
    update_boot_apply();

    klog_info("init: userland up (rxfs mounted, tree + apps seeded)");
    return true;
}

void init_start_shell(const hwid_t *hw) {
    wired_run(hw);
}
