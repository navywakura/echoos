/* /dev nodes (and friends). Minimal device FS surface. */
#include "devfs.h"
#include "arch.h"
#include "pmm.h"
#include "heap.h"
#include "power.h"
#include "thermal.h"
#include "framebuffer.h"
#include "disk.h"
#include "netif.h"
#include "rx_event.h"
#include "vfs.h"

/* Each generator appends into a shared scratch buffer through these, kept
   local so devfs does not depend on the shell's output builder. */
static char  *g_buf;
static size_t g_max, g_len;

static void d_str(const char *s) {
    while (*s && g_len + 1 < g_max) g_buf[g_len++] = *s++;
    g_buf[g_len] = '\0';
}
static void d_dec(uint64_t v) {
    char t[21]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && g_len + 1 < g_max) g_buf[g_len++] = t[--i];
    g_buf[g_len] = '\0';
}
static void d_kv(const char *k, uint64_t v, const char *unit) {
    d_str(k); d_str(": "); d_dec(v); d_str(unit); d_str("\n");
}

/* ------------------------------- generators ------------------------------- */
static void gen_tty(void) {
    d_str("kind: framebuffer console + VGA text + COM1 mirror\n");
    d_str("mode: "); d_str(fb_mode_string()); d_str("\n");
    d_str("double-buffered: ");
    d_str(fb_double_buffered() ? "yes\n" : "no (drawing straight to VRAM)\n");
}

static void gen_fb0(void) {
    uint32_t w = 0, h = 0;
    fb_dims(&w, &h);
    d_str("mode: "); d_str(fb_mode_string()); d_str("\n");
    d_kv("width", w, " px");
    d_kv("height", h, " px");
    d_kv("bytes", (uint64_t)w * h * 4, " (backbuffer)");
}

static void gen_rapl(void) {
    d_str("status: "); d_str(power_rapl_status()); d_str("\n");
    d_str("cpu: "); d_str(power_cpu_name()); d_str("\n");
    uint64_t uj;
    if (power_rapl_pkg_uj(&uj))  d_kv("package_uj", uj, "");
    if (power_rapl_core_uj(&uj)) d_kv("core_uj", uj, "");
    if (power_rapl_gpu_uj(&uj))  d_kv("gpu_uj", uj, "");
    if (power_rapl_tdp_mw())     d_kv("tdp_mw", power_rapl_tdp_mw(), "");
    d_str("note: counters are cumulative microjoules; power = delta / time.\n");
    d_str("      `power 5` does that division for you.\n");
}

static void gen_thermal(void) {
    d_str("status: "); d_str(thermal_status()); d_str("\n");
    uint32_t c;
    if (thermal_core_c(&c))    d_kv("core_c", c, "");
    if (thermal_package_c(&c)) d_kv("package_c", c, "");
    d_kv("tjmax_c", thermal_tjmax(), thermal_tjmax_measured() ? " (from the CPU)"
                                                              : " (assumed)");
}

static void gen_mem(void) {
    d_kv("used_bytes", pmm_used_bytes(), "");
    d_kv("free_bytes", pmm_free_bytes(), " (in the active region)");
    d_kv("addressable_bytes", pmm_usable_bytes(), " (below 4 GiB)");
    d_kv("fitted_bytes", pmm_total_bytes(), "");
    d_kv("heap_used", heap_used(), "");
    d_kv("heap_total", heap_total(), "");
    d_kv("firmware_regions", (uint64_t)pmm_region_total_seen(), "");
}

static void gen_disk0(void) {
    if (!disk_present()) { d_str("state: absent (amnesic boot)\n"); return; }
    d_str("backend: "); d_str(disk_backend()); d_str("\n");
    d_str("name: "); d_str(disk_name()); d_str("\n");
    d_str("partitioned: "); d_str(disk_partitioned() ? "yes\n" : "no\n");
}

static void gen_net0(void) {
    if (!net_present()) { d_str("state: no NIC bound\n"); return; }
    d_str("driver: "); d_str(net_driver_name()); d_str("\n");
    const uint8_t *m = net_mac();
    if (m) {
        const char *hx = "0123456789abcdef";
        d_str("mac: ");
        for (int i = 0; i < 6; i++) {
            char b[3] = { hx[m[i] >> 4], hx[m[i] & 0xF], '\0' };
            d_str(b); if (i < 5) d_str(":");
        }
        d_str("\n");
    }
    d_str("protocol: Rogex Wired L2, EtherType 0x88B5, no IP\n");
}

static void gen_fabric(void) {
    d_kv("input_events", rx_input_events(), "");
    d_kv("input_dropped", (uint64_t)rx_input_dropped(), "");
    d_kv("uptime_ticks", arch_timer_ticks(), " (PIT at 100 Hz)");
    d_str("note: there is no periodic neural tick. Actors are evaluated when\n");
    d_str("      an event arrives; the leak is reconstructed from timestamps.\n");
}

static void gen_uptime(void) {
    d_kv("seconds", arch_timer_uptime_ms() / 1000, "");
    d_kv("ticks", arch_timer_ticks(), "");
}

/* -------------------------------- the table ------------------------------- */
typedef struct { const char *path; void (*gen)(void); const char *what; } devnode_t;

static const devnode_t NODES[] = {
    { "/dev/tty",     gen_tty,     "console + framebuffer mode"      },
    { "/dev/fb0",     gen_fb0,     "framebuffer geometry"            },
    { "/dev/rapl",    gen_rapl,    "Intel energy counters"           },
    { "/dev/thermal", gen_thermal, "digital thermal sensor"          },
    { "/dev/mem",     gen_mem,     "physical + heap accounting"      },
    { "/dev/disk0",   gen_disk0,   "storage backend"                 },
    { "/dev/net0",    gen_net0,    "network interface"               },
    { "/dev/fabric",  gen_fabric,  "event fabric counters"           },
    { "/dev/uptime",  gen_uptime,  "time since boot"                 },
};
#define NODE_COUNT ((int)(sizeof(NODES) / sizeof(NODES[0])))

static bool eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

bool devfs_is_node(const char *path) {
    if (!path) return false;
    for (int i = 0; i < NODE_COUNT; i++) if (eq(path, NODES[i].path)) return true;
    return false;
}

int devfs_count(void) { return NODE_COUNT; }

const char *devfs_name(int i) {
    return (i >= 0 && i < NODE_COUNT) ? NODES[i].path : NULL;
}
const char *devfs_describe(int i) {
    return (i >= 0 && i < NODE_COUNT) ? NODES[i].what : NULL;
}

bool devfs_read(const char *path, char *out, size_t max, size_t *written) {
    if (!path || !out || max == 0) return false;
    for (int i = 0; i < NODE_COUNT; i++) {
        if (!eq(path, NODES[i].path)) continue;
        g_buf = out; g_max = max; g_len = 0; out[0] = '\0';
        NODES[i].gen();
        if (written) *written = g_len;
        return true;
    }
    return false;
}
