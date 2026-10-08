/* duf — superior df for EchOS.
 * Parses /proc/mounts directly (no getmntent), stats each mount with
 * statvfs, prints filesystem/size/used/avail/use% in a table with
 * ASCII bars. Non-device pseudo filesystems are hidden unless -a.
 * EchOS coretools — proprietary, from scratch. C99.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

static int opt_all = 0;

struct mnt {
    char dev[256];
    char dir[4096];
    char type[64];
};

static void human(unsigned long long b, double *val, const char **unit)
{
    static const char *u[] = { "B", "Ki", "Mi", "Gi", "Ti" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    *val = v;
    *unit = u[i];
}

int main(int argc, char **argv)
{
    FILE *f;
    char line[8192];
    struct mnt mounts[512];
    size_t n = 0;
    size_t i;
    long li;
    size_t wdev = 3, wdir = 4, wtype = 4;

    for (li = 1; li < argc; li++) {
        if (strcmp(argv[li], "-a") == 0) opt_all = 1;
        else { fprintf(stderr, "usage: duf [-a]\n"); return 2; }
    }

    f = fopen("/proc/mounts", "r");
    if (!f) {
        fprintf(stderr, "duf: cannot read /proc/mounts\n");
        return 1;
    }
    while (fgets(line, sizeof line, f) && n < sizeof mounts / sizeof *mounts) {
        char dev[256], dir[4096], type[64];
        if (sscanf(line, "%255s %4095s %63s", dev, dir, type) != 3)
            continue;
        if (!opt_all && strncmp(dev, "/dev/", 5) != 0 &&
            strcmp(type, "tmpfs") != 0)
            continue;
        snprintf(mounts[n].dev, sizeof mounts[n].dev, "%s", dev);
        snprintf(mounts[n].dir, sizeof mounts[n].dir, "%s", dir);
        snprintf(mounts[n].type, sizeof mounts[n].type, "%s", type);
        n++;
    }
    fclose(f);

    for (i = 0; i < n; i++) {
        size_t l;
        l = strlen(mounts[i].dev);  if (l > wdev)  wdev = l;
        l = strlen(mounts[i].dir);  if (l > wdir)  wdir = l;
        l = strlen(mounts[i].type); if (l > wtype) wtype = l;
    }

    printf("%-*s  %-*s  %-*s  %10s %10s %10s %6s  %-14s\n",
           (int)wdev, "FS", (int)wdir, "MOUNT", (int)wtype, "TYPE",
           "SIZE", "USED", "AVAIL", "USE%", "USAGE");

    for (i = 0; i < n; i++) {
        struct statvfs vfs;
        unsigned long long total, avail, freeb, used;
        double tv, uv, av;
        const char *tu, *uu, *au;
        int pct;
        char bar[13];

        if (statvfs(mounts[i].dir, &vfs) != 0) {
            printf("%-*s  %-*s  %-*s  %10s\n",
                   (int)wdev, mounts[i].dev, (int)wdir, mounts[i].dir,
                   (int)wtype, mounts[i].type, "?");
            continue;
        }
        total = (unsigned long long)vfs.f_blocks * (unsigned long long)vfs.f_frsize;
        freeb = (unsigned long long)vfs.f_bfree  * (unsigned long long)vfs.f_frsize;
        avail = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
        used  = total - freeb;
        pct   = total ? (int)((used * 100 + total / 200) / total) : 0;
        if (pct > 100) pct = 100;

        human(total, &tv, &tu);
        human(used,  &uv, &uu);
        human(avail, &av, &au);
        {
            int filled = pct / 10, bi;
            if (filled < 1) filled = 1;
            for (bi = 0; bi < 12; bi++) bar[bi] = bi < filled ? '#' : '-';
            bar[12] = '\0';
        }
        printf("%-*s  %-*s  %-*s  %6.1f%-4s %6.1f%-4s %6.1f%-4s %5d%%  [%s]\n",
               (int)wdev, mounts[i].dev,
               (int)wdir, mounts[i].dir,
               (int)wtype, mounts[i].type,
               tv, tu, uv, uu, av, au, pct, bar);
    }
    return 0;
}
