/* NIC abstraction. One active interface at a time today.
 *
 * Which NICs can exist is a property of the machine, not of the stack, so
 * the probe chain is the only part of this file that knows the target: the
 * PCI inventory lives in netif_pci.c, which the aarch64 board does not build
 * because it has no PCI to enumerate.
 */
#include "netif.h"

static const netdrv_t *g_drv;

bool net_init(void) {
    if (g_drv) return true;
#if defined(__aarch64__)
    /* virt has no PCI NIC wired up by default; the device tree advertises
       virtio-mmio slots instead. */
    g_drv = virtio_mmio_net_probe();
#else
    /* Order: proven VM chips first, then the laptop Realteks. PCI scan
       walks bridges — q35 and the HP put NICs off bus 0. */
    g_drv = virtio_net_probe();
    if (!g_drv) g_drv = e1000_probe();
    if (!g_drv) g_drv = r8169_probe();
    if (!g_drv) g_drv = rtl8139_probe();
#endif
    return g_drv != NULL;
}

bool net_present(void)          { return g_drv != NULL; }
const char *net_driver_name(void) { return g_drv ? g_drv->name : "none"; }
const char *net_bound_name(void) { return net_driver_name(); }
const uint8_t *net_mac(void)    { return g_drv ? g_drv->mac() : NULL; }

bool net_send(const void *frame, uint32_t len) {
    return g_drv ? g_drv->send(frame, len) : false;
}

uint32_t net_poll(void *buf, uint32_t max) {
    return g_drv ? g_drv->poll(buf, max) : 0;
}

/* Optional INTx wake-hint: only the virtio backend implements it today; a
   backend without it simply stays polled (the idle loop drains RX anyway). */
bool net_irq_enable(void)  { return g_drv && g_drv->irq_enable ? g_drv->irq_enable() : false; }
bool net_irq_active(void)  { return g_drv && g_drv->irq_active ? g_drv->irq_active() : false; }
int  net_irq_line(void)    { return g_drv && g_drv->irq_line   ? g_drv->irq_line()   : -1; }
bool net_take_pending(void){ return g_drv && g_drv->take_pending ? g_drv->take_pending() : false; }
