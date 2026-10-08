/*
 * echOS sovereign filesystem. Flat, deterministic, Heap-0 friendly.
 *
 *   /boot  /sys/{editions,modules,network}  /bin  /etc
 *   /epk/{db,manifests}  /var/{log,run}  /dev  /users  /tmp
 *
 * No /usr, no /usr/local, no /home (replaced by /users).
 */
#include "layout.h"
#include "vfs.h"
#include "version.h"
#include "echos_arch.h"
#include "epk.h"
#include "clock.h"

static const char TREE[] =
    "/\n"
    "+-- boot/                 kernel ELF, GRUB, LIVE ramdisk\n"
    "+-- sys/\n"
    "|   +-- editions/         installed .manifest\n"
    "|   +-- modules/          static drivers + Akida AKD1000\n"
    "|   +-- network/          bare-metal net + ROGEX-PQC\n"
    "+-- bin/                  doas rcctl epk echofetch\n"
    "+-- etc/                  doas.conf pf.conf sudoers echos-edition\n"
    "+-- epk/\n"
    "|   +-- db/               installed package records\n"
    "|   +-- manifests/        active edition manifests\n"
    "+-- var/\n"
    "|   +-- log/              dmesg + daemons\n"
    "|   +-- run/              IPC sockets + daemon PIDs\n"
    "+-- dev/                  MMIO / hardware nodes\n"
    "+-- users/                home directories (not /home)\n"
    "+-- tmp/                  Heap-0 scratch\n";

static const char SUDOERS[] =
    "# " ECHOS_VERSION_NAME " sudoers  (doas is the live binary; this is the policy list)\n"
    "root    ALL=(ALL) ALL\n"
    "%wheel  ALL=(ALL) ALL\n"
    "live    ALL=(ALL) NOPASSWD: ALL\n";

static const char PERMS[] =
    "# path     mode  owner group\n"
    "/boot      0755  root  wheel\n"
    "/sys       0755  root  wheel\n"
    "/bin       0755  root  wheel\n"
    "/etc       0755  root  wheel\n"
    "/epk       0750  root  wheel\n"
    "/var       0755  root  wheel\n"
    "/var/log   0755  root  wheel\n"
    "/var/run   0755  root  wheel\n"
    "/dev       0755  root  wheel\n"
    "/users     0755  root  wheel\n"
    "/tmp       1777  root  wheel\n";

static const char AKIDA_MAP[] =
    "# Akida AKD1000 (PCI 1e7c:bca1) — detection only\n"
    "device: AKD1000\n"
    "pci: 1e7c:bca1\n"
    "state: unsupported\n"
    "driver: none\n"
    "offload: none  # software LIF runs on the CPU with or without the card\n"
    "regs: unknown  # no verified register contract in this tree\n";

void echos2_layout_seed(void) {
    static const char *dirs[] = {
        "/boot",
        "/sys", "/sys/editions", "/sys/modules", "/sys/network",
        "/bin",
        "/etc",
        "/epk", "/epk/db", "/epk/manifests",
        "/var", "/var/log", "/var/run",
        "/dev",
        "/users",
        "/tmp",
        NULL
    };
    int i;
    for (i = 0; dirs[i]; i++) vfs_mkdir(dirs[i]);

    vfs_write("/sys/TREE", TREE, (uint32_t)(sizeof(TREE) - 1));
    vfs_write("/sys/modules/akida.mmio", AKIDA_MAP, (uint32_t)(sizeof(AKIDA_MAP) - 1));
    vfs_write("/sys/modules/arch", ECHOS_ARCH, (uint32_t)k_strlen(ECHOS_ARCH));
    vfs_write("/etc/echos-edition", ECHOS_EDITION, (uint32_t)k_strlen(ECHOS_EDITION));

    {
        static const char motd[] =
            ECHOS_VERSION_NAME "  —  CLI, JetBrains Mono, Heap-0.\n"
            "Tree: /boot /sys /bin /etc /epk /var /dev /users /tmp\n"
            "LIVE: every .epk is available.  echos-install  writes a disk.\n";
        vfs_write("/etc/motd", motd, (uint32_t)(sizeof(motd) - 1));
    }
    {
        static const char log0[] = "boot: 2.0 layout seeded\n";
        vfs_write("/var/log/boot.log", log0, (uint32_t)(sizeof(log0) - 1));
    }
    vfs_write("/etc/sudoers", SUDOERS, (uint32_t)(sizeof(SUDOERS) - 1));
    vfs_write("/etc/perms", PERMS, (uint32_t)(sizeof(PERMS) - 1));
    {
        uint32_t n = 0;
        const char *z = vfs_read("/etc/timezone", &n);
        if (z && n) {
            char id[40];
            uint32_t i = 0;
            while (i < n && i + 1 < sizeof(id) && z[i] && z[i] != '\n' && z[i] != '\r') {
                id[i] = z[i]; i++;
            }
            id[i] = '\0';
            if (id[0]) clock_tz_apply(id);
        } else {
            z = clock_tz_name();
            vfs_write("/etc/timezone", z, (uint32_t)k_strlen(z));
        }
    }
}

void echos2_users_seed(const char *user, const char *lang) {
    char home[RXFS_NAME], sub[RXFS_NAME];
    const char *folders_en[] = { "Desktop", "Documents", "Downloads", "Images", "Videos", 0 };
    const char *folders_es[] = { "Escritorio", "Documentos", "Descargas", "Imagenes", "Videos", 0 };
    const char **f;
    int i, es = 0;
    if (!user || !user[0]) user = "live";
    if (lang && (lang[0] == 'e' || lang[0] == 'E') && (lang[1] == 's' || lang[1] == 'S'))
        es = 1;
    f = es ? folders_es : folders_en;
    vfs_mkdir("/users");
    { char *d = home; const char *p = "/users/";
      while (*p) *d++ = *p++;
      p = user; while (*p && d < home + sizeof(home) - 1) *d++ = *p++;
      *d = '\0'; }
    vfs_mkdir(home);
    for (i = 0; f[i]; i++) {
        char *d = sub; const char *p = home;
        while (*p && d < sub + sizeof(sub) - 2) *d++ = *p++;
        *d++ = '/';
        p = f[i]; while (*p && d < sub + sizeof(sub) - 1) *d++ = *p++;
        *d = '\0';
        vfs_mkdir(sub);
    }
    vfs_chdir(home);
}
