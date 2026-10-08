#include "hwprobe.h"
#include "nvme.h"
#include "pci.h"
#include "blockdev.h"
#include "partition.h"
#include "disk.h"
#include "akida.h"
#include "device_manager.h"
#include "echos_arch.h"
#include "netif.h"
#include "power.h"
#include "framebuffer.h"
void commands_v(const char *s);

static char  *o_buf;
static size_t o_max, o_len;
static void o_reset(char *b, size_t m) {
    o_buf = b; o_max = m; o_len = 0; if (m) b[0] = '\0';
}
static void o_s(const char *s) {
    if (!s) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = '\0';
}
static void o_d(uint64_t v) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v && i < 20) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) { char c[2] = { t[--i], 0 }; o_s(c); }
}
static void o_nl(void) { o_s("\n"); }
static void o_hex16(uint16_t v) {
    const char *h = "0123456789abcdef";
    char b[5] = { h[(v >> 12) & 15], h[(v >> 8) & 15], h[(v >> 4) & 15], h[v & 15], 0 };
    o_s(b);
}

static const char *cls_name(uint8_t cls, uint8_t sub) {
    if (cls == 0x01 && sub == 0x01) return "IDE";
    if (cls == 0x01 && sub == 0x06) return "AHCI/SATA (HDD/SSD)";
    if (cls == 0x01 && sub == 0x08) return "NVMe (SSD)";
    if (cls == 0x01) return "storage";
    if (cls == 0x02) return "network";
    if (cls == 0x03) return "display";
    if (cls == 0x0C && sub == 0x03) return "USB";
    if (cls == 0x0C) return "serial-bus";
    if (cls == 0x12) return "accelerator/NPU";
    return "other";
}

static bool pci_row(pci_addr_t a, uint8_t cls, uint8_t sub, void *user) {
    int *n = (int *)user;
    o_s("  "); o_d(a.bus); o_s(":"); o_d(a.dev); o_s("."); o_d(a.fn);
    o_s("  "); o_hex16(a.vendor); o_s(":"); o_hex16(a.device);
    o_s("  "); o_s(cls_name(cls, sub));
    if (a.vendor == AKIDA_PCI_VENDOR && a.device == AKIDA_PCI_DEVICE)
        o_s("  AKD1000");
    o_nl();
    (*n)++;
    return true;
}

static const char *disk_kind(blockdev_t *d) {
    const char *b;
    if (!d) return "?";
    if (is_ram(d)) return "ramdisk (LIVE)";
    if (part_is_view(d)) return "partition";
    b = disk_backend();
    if (b && b[0] == 'v') return "virtio (VM disk)";
    if (b && (b[0] == 'A' || b[0] == 'a')) return "AHCI SATA (HDD/SSD)";
    if (b && (b[0] == 'I' || b[0] == 'i')) return "IDE (HDD)";
    return "block";
}

void hwprobe_cmd(char *out, size_t outmax) {
    int i, n, pci_n = 0;
    o_reset(out, outmax);
    commands_v("walking PCI + blockdevs + Akida 1e7c:bca1 (probe, not a driver)");
    o_s("EchOS hardware probe  arch="); o_s(ECHOS_ARCH);
    o_s("  cpu="); o_s(power_cpu_name()); o_nl(); o_nl();

    o_s("[CPU]\n  "); o_s(power_cpu_name());
    o_s("  family="); o_s(ECHOS_ARCH_FAMILY);
    o_s("  bits="); o_d(ECHOS_ARCH_BITS); o_nl();
    o_s("  targets: "); o_s(ECHOS_TARGET_X86_64); o_s("  ");
    o_s(ECHOS_TARGET_AARCH64); o_s("  "); o_s(ECHOS_TARGET_X32); o_nl();

    o_s("\n[RAM]\n  total "); o_d(pmm_total_bytes() / (1024 * 1024));
    o_s(" MiB  free "); o_d(pmm_free_bytes() / (1024 * 1024)); o_s(" MiB\n");

    o_s("\n[PCI]\n");
    n = pci_foreach(pci_row, &pci_n);
    if (!pci_n) o_s("  (no functions walked)\n");
    (void)n;

    o_s("\n[block]\n");
    n = blockdev_count();
    for (i = 0; i < n; i++) {
        blockdev_t *d = blockdev_at(i);
        if (!d) continue;
        o_s("  "); o_s(d->name); o_s("  ");
        { uint64_t kib = (uint64_t)d->sectors / 2;
          if (kib >= 1024) { o_d(kib / 1024); o_s(" MiB  "); }
          else { o_d(kib); o_s(" KiB  "); } }
        o_s(disk_kind(d)); o_nl();
    }
    if (!n) o_s("  (none — LIVE ramdisk only)\n");

    /* NVMe answers for itself: either why it is absent, or the model it
       reported plus what its queues have actually done. */
    o_s("\n[NVMe]\n  "); o_s(nvme_status()); o_nl();
    if (nvme_present()) {
        o_s("  commands completed "); o_d(nvme_completions());
        o_s("   errors "); o_d(nvme_errors());
        o_s("   timeouts "); o_d(nvme_timeouts());
        if (nvme_last_error()) { o_s("   last status 0x"); o_d(nvme_last_error()); }
        o_nl();
    }

    o_s("\n[net]\n  "); o_s(net_present() ? net_driver_name() : "none"); o_nl();

    o_s("\n[NPU Akida AKD1000]\n  "); o_s(akida_status()); o_nl();
    if (akida_present()) {
        const akida_info_t *inf = akida_info();
        o_s("  BAR0 0x");
        { const char *h = "0123456789abcdef"; uint64_t v = inf->bar0_phys; int sh;
          for (sh = 60; sh >= 0; sh -= 4) {
              char c[2] = { h[(v >> sh) & 15], 0 }; o_s(c);
          } }
        o_s("  (reported by firmware; not mapped, not driven)\n");
    } else {
        o_s("  software LIF runs on the CPU. No offload path exists either way.\n");
    }

    o_s("\n[video]\n  ");
    o_s(fb_active() ? fb_mode_string() : "VGA text / serial"); o_nl();

    o_s("\n[registry]\n");
    n = devmgr_count();
    for (i = 0; i < n; i++) {
        const device_info_t *e = devmgr_at(i);
        if (!e) continue;
        o_s("  "); o_s(e->name); o_s("  "); o_s(e->class_); o_s("  ");
        o_s(e->status); o_nl();
    }
}
