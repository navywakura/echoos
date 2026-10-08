#include "akida.h"
#include "pci.h"
#include "device_manager.h"

static akida_info_t g_ak;
static const char  *g_why = "not probed";

void akida_init(void) {
    pci_addr_t a;

    g_ak.present = false;
    g_ak.bar0_phys = 0;
    g_ak.bar0_size = 0;
    g_why = "absent (PCI 1e7c:bca1 not enumerated)";

    if (!pci_find(AKIDA_PCI_VENDOR, AKIDA_PCI_DEVICE, &a)) {
        devmgr_register("akida", "npu", g_why);
        return;
    }

    g_ak.vendor = AKIDA_PCI_VENDOR;
    g_ak.device = AKIDA_PCI_DEVICE;
    g_ak.bus = a.bus;
    g_ak.dev = a.dev;
    g_ak.fn  = a.fn;
    g_ak.bar0_phys = pci_bar_mem_base(a, 0);
    g_ak.present = true;

    /* Detected, not supported: nothing here can drive the part, so we do not
       enable bus mastering, map the window or claim an offload path. */
    g_why = g_ak.bar0_phys
                ? "detected on PCI, unsupported (no driver; software LIF runs)"
                : "detected on PCI with BAR0 unprogrammed, unsupported";
    devmgr_register("akida", "npu", g_why);
}

bool akida_present(void) { return g_ak.present; }
const akida_info_t *akida_info(void) { return &g_ak; }
const char *akida_status(void) { return g_why; }
