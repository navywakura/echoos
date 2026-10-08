/*
 * virtio-net. Enough for Rogex Wired (raw Ethernet). No IP stack on purpose.
 * Bugs welcome — two-node demos still flaky under load sometimes.
 */
#include "netif.h"
#include "pci.h"
#include "irq.h"
#include "rogex.h"

/* --- identity ------------------------------------------------------------ */
#define VIRTIO_VENDOR      0x1af4
#define VIRTIO_NET_MODERN  0x1041      /* device id 1 -> 0x1040 + 1          */
#define VIRTIO_NET_TRANS   0x1000      /* transitional; also speaks modern   */

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8
#define ST_FAILED 128

#define CC_DEV_FEAT_SEL 0x00
#define CC_DEV_FEAT     0x04
#define CC_DRV_FEAT_SEL 0x08
#define CC_DRV_FEAT     0x0c
#define CC_MSIX_CONFIG  0x10
#define CC_NUM_QUEUES   0x12
#define CC_DEV_STATUS   0x14
#define CC_Q_SELECT     0x16
#define CC_Q_SIZE       0x18
#define CC_Q_MSIX_VEC   0x1a
#define CC_Q_ENABLE     0x1c
#define CC_Q_NOTIFY_OFF 0x1e
#define CC_Q_DESC       0x20
#define CC_Q_DRIVER     0x28
#define CC_Q_DEVICE     0x30

#define F_VERSION_1     32             /* feature word 1, bit 0              */
#define F_NET_MAC       5              /* feature word 0, bit 5              */
#define NO_VECTOR       0xffff

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define PTE_PCD (1ULL << 4)

#define NET_HDR_LEN  12                /* virtio_net_hdr_v1 (VERSION_1)      */
#define RXQ 0
#define TXQ 1
#define QSZ_RX 16
#define QSZ_TX 8
#define BUF_SZ 2048                    /* per RX/TX buffer (>= 12 + 1514)    */
#define POLL_BUDGET 4000000

typedef struct ROGEX_PACKED { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } vq_desc;

/* A split virtqueue accessed by raw offsets (rings are variable-length). */
typedef struct {
    uintptr_t desc, avail, used;       /* physical == virtual bases          */
    uint16_t  qsz, last_used, notify_off;
} vnet_vq;

static volatile uint8_t *g_common, *g_notify, *g_device, *g_isr;
static uint32_t g_notify_mult;
static vnet_vq  g_rx, g_tx;
static uintptr_t g_rxbuf[QSZ_RX], g_txbuf[QSZ_TX];
static uint8_t  g_mac[NET_MAC_LEN];
static bool     g_present;
static pci_addr_t g_pci;               /* saved for IRQ setup                */
static volatile bool g_pending;        /* RX signalled by the INTx handler   */
static bool     g_irq_active;
static int      g_irq_line = -1;

/* --- MMIO helpers -------------------------------------------------------- */
static inline void mfence(void){ __asm__ volatile ("mfence" ::: "memory"); }
static inline uint8_t  cc_r8 (uint32_t o){ return *(volatile uint8_t  *)(g_common+o); }
static inline uint16_t cc_r16(uint32_t o){ return *(volatile uint16_t *)(g_common+o); }
static inline uint32_t cc_r32(uint32_t o){ return *(volatile uint32_t *)(g_common+o); }
static inline void cc_w8 (uint32_t o,uint8_t v){ *(volatile uint8_t  *)(g_common+o)=v; }
static inline void cc_w16(uint32_t o,uint16_t v){ *(volatile uint16_t *)(g_common+o)=v; }
static inline void cc_w32(uint32_t o,uint32_t v){ *(volatile uint32_t *)(g_common+o)=v; }
static inline void cc_w64(uint32_t o,uint64_t v){ *(volatile uint64_t *)(g_common+o)=v; }

/* ring accessors (variable qsz -> raw offsets, spec 2.6) */
static inline volatile vq_desc *desc_at(vnet_vq *q, int i){ return (volatile vq_desc*)(q->desc + (uintptr_t)i*16); }
static inline volatile uint16_t *avail_idx(vnet_vq *q){ return (volatile uint16_t*)(q->avail + 2); }
static inline volatile uint16_t *avail_ring(vnet_vq *q,int i){ return (volatile uint16_t*)(q->avail + 4 + (uintptr_t)i*2); }
static inline volatile uint16_t *used_idx(vnet_vq *q){ return (volatile uint16_t*)(q->used + 2); }
static inline uint32_t used_id(vnet_vq *q,int i){ return *(volatile uint32_t*)(q->used + 4 + (uintptr_t)i*8); }
static inline uint32_t used_len(vnet_vq *q,int i){ return *(volatile uint32_t*)(q->used + 4 + (uintptr_t)i*8 + 4); }

static void map_mmio(uint64_t phys, uint64_t len){
    uint64_t s = phys & ~0xfffULL, e = (phys+len+0xfff) & ~0xfffULL;
    for (uint64_t p=s; p<e; p+=0x1000) vmm_map_page(p,p, PTE_WRITABLE|PTE_PCD);
}

static volatile uint8_t *find_cfg(pci_addr_t a, uint8_t want, uint32_t *mult){
    uint8_t cap = pci_find_cap(a, 0x09, 0);
    for (int g=0; cap && g<32; g++){
        uint8_t type = pci_read8(a,(uint8_t)(cap+3));
        uint8_t bar  = pci_read8(a,(uint8_t)(cap+4));
        uint32_t off = pci_read32(a,(uint8_t)(cap+8));
        uint32_t len = pci_read32(a,(uint8_t)(cap+12));
        if (type==want){
            uint64_t base = pci_bar_mem_base(a,bar);
            if (!base) return NULL;
            map_mmio(base+off, len?len:0x1000);
            if (mult) *mult = pci_read32(a,(uint8_t)(cap+16));
            return (volatile uint8_t*)(uintptr_t)(base+off);
        }
        cap = pci_find_cap(a, 0x09, cap);
    }
    return NULL;
}

/* Set up virtqueue `sel` with size `qsz` in one zeroed PMM frame. */
static bool vq_setup(vnet_vq *q, uint16_t sel, uint16_t qsz){
    cc_w16(CC_Q_SELECT, sel);
    uint16_t max = cc_r16(CC_Q_SIZE);
    if (max == 0 || max < qsz) return false;
    cc_w16(CC_Q_SIZE, qsz);
    if (cc_r16(CC_Q_SIZE) != qsz) return false;    /* must honor our size    */
    cc_w16(CC_Q_MSIX_VEC, NO_VECTOR);

    uint64_t frame = pmm_alloc_frame();
    if (!frame) return false;
    uint32_t avail_off = (uint32_t)qsz * 16;
    uint32_t used_off  = (avail_off + 4 + (uint32_t)qsz*2 + 2 + 3) & ~3u;  /* 4-align */
    q->desc  = (uintptr_t)frame;
    q->avail = (uintptr_t)frame + avail_off;
    q->used  = (uintptr_t)frame + used_off;
    q->qsz = qsz; q->last_used = 0;

    cc_w64(CC_Q_DESC,   q->desc);
    cc_w64(CC_Q_DRIVER, q->avail);
    cc_w64(CC_Q_DEVICE, q->used);
    q->notify_off = cc_r16(CC_Q_NOTIFY_OFF);
    cc_w16(CC_Q_ENABLE, 1);
    return true;
}

static void notify(vnet_vq *q){
    *(volatile uint16_t *)(g_notify + (uint32_t)q->notify_off * g_notify_mult) =
        (q == &g_rx) ? RXQ : TXQ;
}

/* --- backend ops (bound by drivers/net/netif.c on successful probe) ------- */
static const uint8_t *vnet_mac(void){ return g_mac; }

static bool vnet_send(const void *frame, uint32_t len){
    if (!g_present || len == 0 || len > NET_FRAME_MAX) return false;
    /* Ethernet minimum frame is 60 bytes (before CRC); a real switch drops
       shorter "runt" frames, so zero-pad up to 60. */
    uint32_t wire = len < 60 ? 60 : len;
    uintptr_t buf = g_txbuf[0];                     /* one in-flight TX       */
    k_memset((void*)buf, 0, NET_HDR_LEN + wire);    /* zero hdr + pad tail    */
    k_memcpy((void*)(buf + NET_HDR_LEN), frame, len);

    volatile vq_desc *d = desc_at(&g_tx, 0);
    d->addr = buf; d->len = NET_HDR_LEN + wire; d->flags = 0; d->next = 0;  /* device reads */

    uint16_t ai = *avail_idx(&g_tx);
    *avail_ring(&g_tx, ai % g_tx.qsz) = 0;
    mfence();
    *avail_idx(&g_tx) = (uint16_t)(ai + 1);
    mfence();
    notify(&g_tx);

    for (int i=0;i<POLL_BUDGET;i++){
        if (*used_idx(&g_tx) != g_tx.last_used){ mfence(); g_tx.last_used = *used_idx(&g_tx); return true; }
        __asm__ volatile ("pause");
    }
    return false;                                    /* timed out            */
}

static uint32_t vnet_poll(void *out, uint32_t max){
    if (!g_present) return 0;
    if (*used_idx(&g_rx) == g_rx.last_used) return 0;   /* nothing received  */
    mfence();
    int slot = g_rx.last_used % g_rx.qsz;
    uint32_t id  = used_id(&g_rx, slot);
    uint32_t tot = used_len(&g_rx, slot);               /* hdr + frame bytes */
    g_rx.last_used = (uint16_t)(g_rx.last_used + 1);

    /* Memory-safety guard: a used id outside our buffer table would make the
       re-post below index g_rxbuf[]/desc out of bounds. Drop it. */
    if (id >= QSZ_RX) return 0;

    uint32_t flen = 0;
    if (tot > NET_HDR_LEN) {
        flen = tot - NET_HDR_LEN;
        if (flen > max) flen = max;
        k_memcpy(out, (void*)(g_rxbuf[id] + NET_HDR_LEN), flen);
    }
    /* Re-post this buffer so the device can receive into it again. */
    volatile vq_desc *d = desc_at(&g_rx, (int)id);
    d->addr = g_rxbuf[id]; d->len = BUF_SZ; d->flags = VRING_DESC_F_WRITE; d->next = 0;
    uint16_t ai = *avail_idx(&g_rx);
    *avail_ring(&g_rx, ai % g_rx.qsz) = (uint16_t)id;
    mfence();
    *avail_idx(&g_rx) = (uint16_t)(ai + 1);
    mfence();
    notify(&g_rx);
    return flen;
}

static bool vnet_init(void){
    g_present = false;

    pci_addr_t a;
    if (!pci_find(VIRTIO_VENDOR, VIRTIO_NET_MODERN, &a) &&
        !pci_find(VIRTIO_VENDOR, VIRTIO_NET_TRANS,  &a))
        return false;
    g_pci = a;
    pci_enable_mem_and_dma(a);

    g_common = find_cfg(a, VIRTIO_PCI_CAP_COMMON_CFG, NULL);
    g_notify = find_cfg(a, VIRTIO_PCI_CAP_NOTIFY_CFG, &g_notify_mult);
    g_device = find_cfg(a, VIRTIO_PCI_CAP_DEVICE_CFG, NULL);
    g_isr    = find_cfg(a, VIRTIO_PCI_CAP_ISR_CFG,    NULL);   /* for INTx ack */
    if (!g_common || !g_notify || !g_device) return false;

    cc_w8(CC_DEV_STATUS, 0);
    for (int i=0; cc_r8(CC_DEV_STATUS)!=0; i++){ if (i>1000000) return false; __asm__ volatile("pause"); }
    cc_w8(CC_DEV_STATUS, ST_ACK);
    cc_w8(CC_DEV_STATUS, ST_ACK|ST_DRIVER);

    /* Need VERSION_1; want NET_MAC so the device hands us a stable MAC. */
    cc_w32(CC_DEV_FEAT_SEL, 1);
    if (!(cc_r32(CC_DEV_FEAT) & (1u << (F_VERSION_1-32)))) { cc_w8(CC_DEV_STATUS, ST_FAILED); return false; }
    cc_w32(CC_DEV_FEAT_SEL, 0);
    uint32_t feat0 = cc_r32(CC_DEV_FEAT);
    bool has_mac = (feat0 & (1u << F_NET_MAC)) != 0;
    cc_w32(CC_DRV_FEAT_SEL, 0); cc_w32(CC_DRV_FEAT, has_mac ? (1u << F_NET_MAC) : 0);
    cc_w32(CC_DRV_FEAT_SEL, 1); cc_w32(CC_DRV_FEAT, 1u << (F_VERSION_1-32));

    cc_w8(CC_DEV_STATUS, ST_ACK|ST_DRIVER|ST_FEATURES_OK);
    if (!(cc_r8(CC_DEV_STATUS) & ST_FEATURES_OK)) { cc_w8(CC_DEV_STATUS, ST_FAILED); return false; }

    cc_w16(CC_MSIX_CONFIG, NO_VECTOR);
    if (!vq_setup(&g_rx, RXQ, QSZ_RX)) { cc_w8(CC_DEV_STATUS, ST_FAILED); return false; }
    if (!vq_setup(&g_tx, TXQ, QSZ_TX)) { cc_w8(CC_DEV_STATUS, ST_FAILED); return false; }

    /* MAC from device config (offset 0), or a locally-administered fallback. */
    if (has_mac) for (int i=0;i<NET_MAC_LEN;i++) g_mac[i] = *(volatile uint8_t*)(g_device + i);
    else { g_mac[0]=0x02; for (int i=1;i<NET_MAC_LEN;i++) g_mac[i]=(uint8_t)(0x30+i); }

    /* Allocate RX/TX buffers (one PMM frame each; < 1 GiB so identity-mapped
       and DMA-addressable as virt==phys). Pre-post all RX buffers. */
    for (int i=0;i<QSZ_RX;i++){
        uint64_t f = pmm_alloc_frame(); if (!f) return false; g_rxbuf[i] = (uintptr_t)f;
        volatile vq_desc *d = desc_at(&g_rx, i);
        d->addr = g_rxbuf[i]; d->len = BUF_SZ; d->flags = VRING_DESC_F_WRITE; d->next = 0;
        *avail_ring(&g_rx, i) = (uint16_t)i;
    }
    for (int i=0;i<QSZ_TX;i++){ uint64_t f=pmm_alloc_frame(); if(!f) return false; g_txbuf[i]=(uintptr_t)f; }
    mfence();
    *avail_idx(&g_rx) = QSZ_RX;                      /* publish all RX buffers */
    mfence();

    cc_w8(CC_DEV_STATUS, ST_ACK|ST_DRIVER|ST_FEATURES_OK|ST_DRIVER_OK);
    notify(&g_rx);                                   /* tell it RX is armed    */

    g_present = true;
    return true;
}

/* --- interrupt-driven RX (INTx) ------------------------------------------ */
/* Optional event-fabric producer: when registered (virtio_net_actor.c), the
   ISR also enqueues a typed RX_EVENT_NET_RX_READY descriptor into the net
   actor's SPSC ring. The hook must be ISR-safe: construct + enqueue + return,
   no loops, no waiting (rx_ring_enqueue drops on a full ring by design). */
static void (*g_irq_event_hook)(void);
void virtio_net_set_irq_event_hook(void (*fn)(void)) { g_irq_event_hook = fn; }

/* INTx handler. Reading the virtio ISR status register acks the device and
   deasserts the (level-triggered) line — REQUIRED, or the line stays high and
   the PIC re-fires forever. We only raise a flag here; the event loop drains
   net_poll(), so the handler stays tiny (interrupt context). */
static void vnet_irq(void) {
    uint8_t isr = g_isr ? *g_isr : 1;               /* read = ack/deassert    */
    if (isr & 0x1) {
        g_pending = true;                           /* bit0 = virtqueue event */
        if (g_irq_event_hook) g_irq_event_hook();   /* typed event (fabric)   */
    }
}

static bool vnet_irq_active(void) { return g_irq_active; }
static int  vnet_irq_line_no(void){ return g_irq_active ? g_irq_line : -1; }

/* Read+clear the RX-pending flag under a short critical section (the handler
   may set it between the read and the clear otherwise). */
static bool vnet_take_pending(void) {
    uint64_t fl;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    bool p = g_pending; g_pending = false;
    if (fl & 0x200) __asm__ volatile ("sti");
    return p;
}

static bool vnet_irq_enable(void) {
    if (!g_present || !g_isr) return false;         /* need the ISR reg to ack */
    uint8_t line = pci_read8(g_pci, 0x3c);          /* PCI Interrupt Line     */
    if (line == 0xff || line > 15) return false;    /* no legacy IRQ assigned */

    /* INTx must not be disabled (PCI Command bit 10). */
    uint16_t cmd = pci_read16(g_pci, 0x04);
    if (cmd & (1u << 10)) pci_write16(g_pci, 0x04, (uint16_t)(cmd & ~(1u << 10)));

    irq_register(line, vnet_irq);                   /* unmasks the PIC line   */
    g_irq_line = line;
    g_irq_active = true;
    g_pending = true;                               /* drain once at start    */
    return true;
}

/* --- probe entry ---------------------------------------------------------- */
static const netdrv_t VNET_OPS = {
    .name         = "virtio-net",
    .mac          = vnet_mac,
    .send         = vnet_send,
    .poll         = vnet_poll,
    .irq_enable   = vnet_irq_enable,
    .irq_active   = vnet_irq_active,
    .irq_line     = vnet_irq_line_no,
    .take_pending = vnet_take_pending,
};

const netdrv_t *virtio_net_probe(void) {
    return vnet_init() ? &VNET_OPS : NULL;
}
