/* Network interface API. */
#ifndef RXOS_NETIF_H
#define RXOS_NETIF_H

#include "rogex.h"

#define NET_MAC_LEN    6
#define NET_FRAME_MAX  1514        /* max Ethernet payload frame (no CRC)    */

/* One NIC backend. A driver's probe() either claims its hardware and returns
   its ops table, or returns NULL. Optional entries (irq_*) may be NULL. */
typedef struct {
    const char *name;                              /* "virtio-net", "e1000"  */
    const uint8_t *(*mac)(void);
    bool     (*send)(const void *frame, uint32_t len);
    uint32_t (*poll)(void *buf, uint32_t max);
    bool     (*irq_enable)(void);
    bool     (*irq_active)(void);
    int      (*irq_line)(void);
    bool     (*take_pending)(void);
} netdrv_t;

const netdrv_t *virtio_net_probe(void);
const netdrv_t *e1000_probe(void);
/* Realtek RTL810x/8168 — the on-board NIC in consumer laptops. UNVERIFIED:
   QEMU emulates no chip of this family, so it cannot be exercised by any test
   here. See the header of drivers/net/r8169.c. */
const netdrv_t *r8169_probe(void);
bool            r8169_link_up(void);
const netdrv_t *rtl8139_probe(void);
/* virtio-mmio transport (aarch64 virt); drivers/aarch64/virtio_mmio_net.c. */
const netdrv_t *virtio_mmio_net_probe(void);
const char     *virtio_mmio_net_transport(void);
/* Frames taken off the ring, and how often the ring was found empty of
   buffers — the device drops what it cannot store, so this is where an
   unexplained gap gets attributed. */
void            virtio_mmio_net_rx_health(uint32_t *frames, uint32_t *starved);

/* PCI Ethernet/WiFi inventory (even chips we do not drive). */
#define NET_PCI_MAX 8
typedef struct {
    uint16_t vendor, device;
    uint8_t  cls, subcls;
    const char *note;     /* bound driver, or why we skip it */
} net_pci_dev_t;
int  net_pci_list(net_pci_dev_t *out, int max);
const char *net_bound_name(void); /* alias of net_driver_name */

/* Bring up the NIC (probe PCI, negotiate, set up RX/TX queues, pre-fill RX).
   Returns true if a NIC is present and up. */
bool        net_init(void);
bool        net_present(void);
const char *net_driver_name(void); /* bound backend name, or "none"         */
const uint8_t *net_mac(void);      /* our 6-byte hardware MAC               */

/* Transmit one complete Ethernet frame (14-byte header + payload). Returns
   true on success. Synchronous/polled. */
bool        net_send(const void *frame, uint32_t len);

/* Poll for one received frame. On a frame, copies up to `max` bytes into
   `buf` and returns its length (>0); returns 0 if nothing is waiting. */
uint32_t    net_poll(void *buf, uint32_t max);

/* --- interrupt mode (optional; polled fallback if this returns false) -----
   Wire the NIC's INTx line into the interrupt controller so RX no longer
   needs busy polling: the handler acks the device and raises a "pending" flag
   that the event loop drains. Only registers an IRQ when a NIC is present, so
   non-networked boots are unaffected.

   STATUS: UNVERIFIED — the INTx path cannot be exercised in the build sandbox
   (no RX traffic without QEMU networking). Falls back to polling on any
   failure. Confirm with a two-node run. */
bool        net_irq_enable(void);      /* try to switch to IRQ-driven RX     */
bool        net_irq_active(void);      /* true once IRQ mode is engaged      */
int         net_irq_line(void);        /* the INTx IRQ number, or -1         */
bool        net_take_pending(void);    /* read+clear the RX-pending flag     */

/* --- event-fabric bridge (drivers/net/virtio_net_actor.c) -----------------
   The network RX path can run through the typed kernel event fabric
   (kernel/event/): the virtio ISR produces RX_EVENT_NET_RX_READY into the
   "net_rx" actor's SPSC ring, and the UI idle hook becomes a pump that adds
   a 1-PIT-tick deadline stimulus (covering e1000, which has no IRQ, and q35
   where INTx delivery is UNVERIFIED) and dispatches the actor. The actor is
   DETERMINISTIC on purpose — every event fires the drain callback — per the
   critical boundary: thresholds never gate correctness paths. */
void net_actor_online(void);           /* register actor + hooks (shell init) */
void net_actor_stats(uint64_t *received, uint64_t *hints,
                     uint64_t *deadlines, uint64_t *spikes,
                     uint32_t *dropped);
/* virtio backend: register an ISR-safe producer invoked on INTx RX events. */
void virtio_net_set_irq_event_hook(void (*fn)(void));

#endif /* RXOS_NETIF_H */
