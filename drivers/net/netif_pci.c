/* PCI Ethernet inventory: what the bus has, including chips we cannot drive.
 *
 * Split out of netif.c so the interface layer itself carries no bus: a board
 * whose NICs hang off a device tree builds netif.c and leaves this behind.
 */
#include "netif.h"
#include "pci.h"

static bool nic_visit(pci_addr_t a, uint8_t cls, uint8_t sub, void *user) {
    struct { net_pci_dev_t *out; int max; int n; } *st = user;
    if (cls != 0x02) return true;          /* not a network controller */
    if (st->n >= st->max) return false;
    net_pci_dev_t *e = &st->out[st->n++];
    e->vendor = a.vendor;
    e->device = a.device;
    e->cls = cls;
    e->subcls = sub;
    if (sub == 0x80) {
        e->note = "802.11 WiFi — no driver (needs firmware + 802.11 stack)";
    } else if (a.vendor == 0x10EC && a.device == 0x8139) {
        e->note = "rtl8139";
    } else if (a.vendor == 0x10EC) {
        e->note = "r8169 family (RTL810x/8168 — HP 15-ac195nl Ethernet is 8136)";
    } else if (a.vendor == 0x8086) {
        e->note = "Intel e1000/e1000e";
    } else if (a.vendor == 0x1AF4) {
        e->note = "virtio-net";
    } else {
        e->note = "Ethernet class — no matching driver";
    }
    return true;
}

int net_pci_list(net_pci_dev_t *out, int max) {
    struct { net_pci_dev_t *out; int max; int n; } st = { out, max, 0 };
    if (!out || max <= 0) return 0;
    pci_foreach(nic_visit, &st);
    return st.n;
}
