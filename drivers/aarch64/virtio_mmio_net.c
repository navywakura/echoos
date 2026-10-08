/*
 * virtio-net over the MMIO transport (aarch64 `virt`).
 *
 * Same device as drivers/net/virtio_net.c, different transport: no PCI
 * configuration space, no capability walk, just a window of registers the
 * device tree points at. QEMU's virt board lays 32 slots at 0x0a000000, one
 * every 512 bytes, and the empty ones read back device id 0 — which is the
 * whole enumeration.
 *
 * Both transport versions are implemented because which one answers is not
 * ours to choose: `virt` decides, and a driver that only speaks the modern
 * one silently finds no network on an older QEMU.
 */
#include "netif.h"
#include "arch.h"

#define MMIO_BASE   0x0a000000ull
#define MMIO_STRIDE 0x200
#define MMIO_SLOTS  32

#define R_MAGIC        0x000
#define R_VERSION      0x004
#define R_DEVICE_ID    0x008
#define R_DEV_FEAT     0x010
#define R_DEV_FEAT_SEL 0x014
#define R_DRV_FEAT     0x020
#define R_DRV_FEAT_SEL 0x024
#define R_GUEST_PGSZ   0x028      /* legacy only                            */
#define R_Q_SEL        0x030
#define R_Q_NUM_MAX    0x034
#define R_Q_NUM        0x038
#define R_Q_ALIGN      0x03c      /* legacy only                            */
#define R_Q_PFN        0x040      /* legacy only                            */
#define R_Q_READY      0x044      /* modern only                            */
#define R_Q_NOTIFY     0x050
#define R_IRQ_STATUS   0x060
#define R_IRQ_ACK      0x064
#define R_STATUS       0x070
#define R_Q_DESC_LO    0x080
#define R_Q_DESC_HI    0x084
#define R_Q_DRV_LO     0x090
#define R_Q_DRV_HI     0x094
#define R_Q_DEV_LO     0x0a0
#define R_Q_DEV_HI     0x0a4
#define R_CONFIG       0x100

#define MAGIC_VIRT     0x74726976u        /* "virt"                         */
#define DEV_ID_NET     1

#define ST_ACK         1
#define ST_DRIVER      2
#define ST_DRIVER_OK   4
#define ST_FEATURES_OK 8
#define ST_FAILED      128

#define F_NET_MAC      5                  /* feature word 0                 */
#define F_VERSION_1    32                 /* feature word 1, bit 0          */

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define QSZ_RX 64
#define QSZ_TX 8
#define BUF_SZ 2048                       /* >= hdr + 1514                  */
#define VRING_ALIGN 4096

/* The IRQ the virt board wires slot n to: SPI 16+n, i.e. GIC line 32+16+n.
   Unused until RX moves off polling; kept here so the number has one home. */
#define SPI_BASE 16

typedef struct ROGEX_PACKED {
    uint64_t addr; uint32_t len; uint16_t flags; uint16_t next;
} vq_desc;

typedef struct {
    vq_desc  *desc;
    uint16_t *avail;                      /* flags, idx, ring[qsz]          */
    uint16_t *used_hdr;                   /* flags, idx, then used elems    */
    uint32_t *used_ring;                  /* {id, len} pairs                */
    uint16_t  qsz, last_used;
} mvq;

/* Rings and buffers are static: Heap-0 means a driver may not allocate, and
   a NIC that can fail to come up for lack of memory is a NIC that fails at
   the worst moment. Each ring is its own 4 KiB-aligned block so the legacy
   transport's single-PFN layout works without arithmetic games. */
#define RING_BYTES 8192   /* desc+avail fit under the 4 KiB used offset */
static uint8_t g_ring_rx[RING_BYTES] __attribute__((aligned(VRING_ALIGN)));
static uint8_t g_ring_tx[RING_BYTES] __attribute__((aligned(VRING_ALIGN)));
static uint8_t g_rxbuf[QSZ_RX][BUF_SZ] __attribute__((aligned(16)));
static uint8_t g_txbuf[QSZ_TX][BUF_SZ] __attribute__((aligned(16)));

static volatile uint8_t *g_base;
static bool     g_legacy;
static uint32_t g_hdr_len = 12;
static mvq      g_rx, g_tx;
static uint8_t  g_mac[NET_MAC_LEN];
static bool     g_up;
/* How often the device was left with no buffer to write into. A NIC drops
   silently when that happens, so without this counter a lost frame has no
   address: it could be the wire, the host, or us. */
static uint32_t g_starved;
static uint32_t g_rx_frames;

static uint32_t rd(uint32_t off) {
    return *(volatile uint32_t *)(g_base + off);
}
static void wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_base + off) = v;
}

static uint64_t feat_read(void) {
    uint64_t f;
    wr(R_DEV_FEAT_SEL, 0); f = rd(R_DEV_FEAT);
    wr(R_DEV_FEAT_SEL, 1); f |= (uint64_t)rd(R_DEV_FEAT) << 32;
    return f;
}
static void feat_write(uint64_t f) {
    wr(R_DRV_FEAT_SEL, 0); wr(R_DRV_FEAT, (uint32_t)f);
    wr(R_DRV_FEAT_SEL, 1); wr(R_DRV_FEAT, (uint32_t)(f >> 32));
}

/* desc | avail | pad to VRING_ALIGN | used — the layout the legacy transport
   requires and the modern one accepts, so one builder serves both. */
static void vring_lay(mvq *q, uint8_t *mem, uint16_t qsz) {
    uintptr_t p = (uintptr_t)mem;
    uintptr_t avail = p + (uintptr_t)qsz * sizeof(vq_desc);
    uintptr_t used  = (avail + 6 + 2 * (uintptr_t)qsz + (VRING_ALIGN - 1))
                      & ~(uintptr_t)(VRING_ALIGN - 1);
    for (uint32_t i = 0; i < RING_BYTES; i++) mem[i] = 0;
    q->desc      = (vq_desc *)p;
    q->avail     = (uint16_t *)avail;
    q->used_hdr  = (uint16_t *)used;
    q->used_ring = (uint32_t *)(used + 4);
    q->qsz       = qsz;
    q->last_used = 0;
}

static bool q_setup(int idx, mvq *q, uint8_t *mem, uint16_t want) {
    uint32_t max;
    wr(R_Q_SEL, (uint32_t)idx);
    max = rd(R_Q_NUM_MAX);
    if (!max) return false;
    if (want > max) want = (uint16_t)max;
    vring_lay(q, mem, want);
    wr(R_Q_NUM, want);
    if (g_legacy) {
        wr(R_Q_ALIGN, VRING_ALIGN);
        wr(R_Q_PFN, (uint32_t)((uintptr_t)mem / VRING_ALIGN));
    } else {
        wr(R_Q_DESC_LO, (uint32_t)(uintptr_t)q->desc);
        wr(R_Q_DESC_HI, (uint32_t)((uint64_t)(uintptr_t)q->desc >> 32));
        wr(R_Q_DRV_LO,  (uint32_t)(uintptr_t)q->avail);
        wr(R_Q_DRV_HI,  (uint32_t)((uint64_t)(uintptr_t)q->avail >> 32));
        wr(R_Q_DEV_LO,  (uint32_t)(uintptr_t)q->used_hdr);
        wr(R_Q_DEV_HI,  (uint32_t)((uint64_t)(uintptr_t)q->used_hdr >> 32));
        wr(R_Q_READY, 1);
    }
    return true;
}

static const uint8_t *mmio_mac(void);
static bool     mmio_send(const void *frame, uint32_t len);
static uint32_t mmio_poll(void *buf, uint32_t max);

static void rx_fill(void) {
    for (uint16_t i = 0; i < g_rx.qsz; i++) {
        g_rx.desc[i].addr  = (uint64_t)(uintptr_t)g_rxbuf[i];
        g_rx.desc[i].len   = BUF_SZ;
        g_rx.desc[i].flags = VRING_DESC_F_WRITE;
        g_rx.desc[i].next  = 0;
        g_rx.avail[2 + i]  = i;
    }
    arch_barrier_write();
    g_rx.avail[1] = g_rx.qsz;             /* avail->idx                     */
    arch_barrier_write();
    wr(R_Q_NOTIFY, 0);
}

static const netdrv_t ops = {
    .name = "virtio-net (mmio)",
    .mac  = mmio_mac,
    .send = mmio_send,
    .poll = mmio_poll,
    .irq_enable = NULL,
    .irq_active = NULL,
    .irq_line   = NULL,
    .take_pending = NULL,
};

const netdrv_t *virtio_mmio_net_probe(void) {
    volatile uint8_t *base;
    uint64_t feat, want;
    uint32_t status, ver;

    if (g_up) return &ops;

    for (int slot = 0; slot < MMIO_SLOTS; slot++) {
        base = (volatile uint8_t *)(uintptr_t)(MMIO_BASE + (uint64_t)slot * MMIO_STRIDE);
        if (*(volatile uint32_t *)(base + R_MAGIC) != MAGIC_VIRT) continue;
        if (*(volatile uint32_t *)(base + R_DEVICE_ID) != DEV_ID_NET) continue;
        g_base = base;
        break;
    }
    if (!g_base) return NULL;

    ver = rd(R_VERSION);
    if (ver != 1 && ver != 2) { g_base = NULL; return NULL; }
    g_legacy = (ver == 1);

    wr(R_STATUS, 0);                       /* reset                          */
    status = ST_ACK;             wr(R_STATUS, status);
    status |= ST_DRIVER;         wr(R_STATUS, status);

    feat = feat_read();
    want = 0;
    if (feat & (1ull << F_NET_MAC)) want |= 1ull << F_NET_MAC;
    if (!g_legacy) {
        if (!(feat & (1ull << F_VERSION_1))) { wr(R_STATUS, ST_FAILED); g_base = NULL; return NULL; }
        want |= 1ull << F_VERSION_1;
        g_hdr_len = 12;                    /* virtio_net_hdr_v1              */
    } else {
        g_hdr_len = 10;                    /* no num_buffers without MRG_RXBUF */
        wr(R_GUEST_PGSZ, VRING_ALIGN);
    }
    feat_write(want);

    if (!g_legacy) {
        status |= ST_FEATURES_OK; wr(R_STATUS, status);
        if (!(rd(R_STATUS) & ST_FEATURES_OK)) {
            wr(R_STATUS, ST_FAILED); g_base = NULL; return NULL;
        }
    }

    if (!q_setup(0, &g_rx, g_ring_rx, QSZ_RX) ||
        !q_setup(1, &g_tx, g_ring_tx, QSZ_TX)) {
        wr(R_STATUS, ST_FAILED); g_base = NULL; return NULL;
    }

    if (want & (1ull << F_NET_MAC)) {
        for (int i = 0; i < NET_MAC_LEN; i++)
            g_mac[i] = *(volatile uint8_t *)(g_base + R_CONFIG + i);
    } else {
        /* Locally administered, unicast: the bit that says "not a vendor's". */
        g_mac[0] = 0x02; g_mac[1] = 0x00; g_mac[2] = 0x00;
        g_mac[3] = 0xec; g_mac[4] = 0x05; g_mac[5] = 0x01;
    }

    status |= ST_DRIVER_OK; wr(R_STATUS, status);
    rx_fill();
    g_up = true;
    return &ops;
}

static const uint8_t *mmio_mac(void) { return g_mac; }

static bool mmio_send(const void *frame, uint32_t len) {
    static uint16_t next;
    uint16_t i;
    uint8_t *b;
    uint64_t spin;

    if (!g_up || !frame || !len || len > NET_FRAME_MAX) return false;
    i = next % g_tx.qsz;
    next++;
    b = g_txbuf[i];
    for (uint32_t k = 0; k < g_hdr_len; k++) b[k] = 0;
    for (uint32_t k = 0; k < len; k++) b[g_hdr_len + k] = ((const uint8_t *)frame)[k];

    g_tx.desc[i].addr  = (uint64_t)(uintptr_t)b;
    g_tx.desc[i].len   = g_hdr_len + len;
    g_tx.desc[i].flags = 0;
    g_tx.desc[i].next  = 0;
    g_tx.avail[2 + (g_tx.avail[1] % g_tx.qsz)] = i;
    arch_barrier_write();
    g_tx.avail[1] = (uint16_t)(g_tx.avail[1] + 1);
    arch_barrier_write();
    wr(R_Q_NOTIFY, 1);

    /* Wait for the device to hand the buffer back before its slot is reused:
       the ring is 8 deep and a send that returns "sent" while the frame is
       still queued is a lie the caller cannot check. */
    for (spin = 0; spin < 2000000; spin++) {
        arch_barrier_read();
        if ((uint16_t)(g_tx.used_hdr[1] - g_tx.last_used) != 0) {
            g_tx.last_used++;
            return true;
        }
        arch_relax();
    }
    return false;
}

static uint32_t mmio_poll(void *buf, uint32_t max) {
    uint32_t id, len;
    uint16_t slot;

    if (!g_up || !buf) return 0;
    arch_barrier_read();
    if (g_rx.used_hdr[1] == g_rx.last_used) return 0;

    g_rx_frames++;
    slot = (uint16_t)(g_rx.last_used % g_rx.qsz);
    id   = g_rx.used_ring[slot * 2];
    len  = g_rx.used_ring[slot * 2 + 1];
    g_rx.last_used++;
    if (id >= g_rx.qsz) return 0;

    if (len <= g_hdr_len) len = 0; else len -= g_hdr_len;
    if (len > max) len = max;
    for (uint32_t k = 0; k < len; k++)
        ((uint8_t *)buf)[k] = g_rxbuf[id][g_hdr_len + k];

    /* Give the buffer straight back: RX starves if the ring is not refilled. */
    g_rx.avail[2 + (g_rx.avail[1] % g_rx.qsz)] = (uint16_t)id;
    arch_barrier_write();
    g_rx.avail[1] = (uint16_t)(g_rx.avail[1] + 1);
    arch_barrier_write();
    wr(R_Q_NOTIFY, 0);
    if ((uint16_t)(g_rx.avail[1] - g_rx.used_hdr[1]) == 0) g_starved++;
    return len;
}

void virtio_mmio_net_rx_health(uint32_t *frames, uint32_t *starved) {
    if (frames)  *frames = g_rx_frames;
    if (starved) *starved = g_starved;
}

const char *virtio_mmio_net_transport(void) {
    if (!g_up) return "absent";
    return g_legacy ? "virtio-mmio v1 (legacy)" : "virtio-mmio v2 (modern)";
}

