/*
 * Realtek RTL8139 — the common cheap 10/100 chip (QEMU `-device rtl8139`,
 * many 2000s NICs). I/O-port programming model; not the r8169 family.
 */
#include "netif.h"
#include "pci.h"
#include "ports.h"
#include "pmm.h"
#include "scheduler.h"

#define RTL_VENDOR 0x10EC
#define RTL_8139   0x8139

#define IDR0     0x00
#define TSD0     0x10
#define TSAD0    0x20
#define RBSTART  0x30
#define CR       0x37
#define CAPR     0x38
#define IMR      0x3C
#define ISR      0x3E
#define TCR      0x40
#define RCR      0x44
#define CONFIG1  0x52

#define CR_BUFE  0x01
#define CR_TE    0x04
#define CR_RE    0x08
#define CR_RST   0x10

#define RCR_AAP  0x01
#define RCR_APM  0x02
#define RCR_AM   0x04
#define RCR_AB   0x08
#define RCR_WRAP 0x80
#define RCR_8K   0x00

#define RX_BUF   (8192 + 16 + 1536)
#define TX_SLOTS 4
#define TX_SIZE  1536

static uint16_t g_io;
static uint8_t  g_mac[NET_MAC_LEN];
static uint8_t *g_rx;
static uint64_t g_rx_phys;
static uint8_t *g_tx[TX_SLOTS];
static uint64_t g_tx_phys[TX_SLOTS];
static uint32_t g_tx_cur, g_rx_off;
static bool     g_up;

static inline void wr8(uint16_t o, uint8_t v)  { outb((uint16_t)(g_io + o), v); }
static inline void wr16(uint16_t o, uint16_t v){ outw((uint16_t)(g_io + o), v); }
static inline void wr32(uint16_t o, uint32_t v){
    outw((uint16_t)(g_io + o), (uint16_t)v);
    outw((uint16_t)(g_io + o + 2), (uint16_t)(v >> 16));
}
static inline uint8_t  rd8(uint16_t o)  { return inb((uint16_t)(g_io + o)); }
static inline uint16_t rd16(uint16_t o) { return inw((uint16_t)(g_io + o)); }
static inline uint32_t rd32(uint16_t o) {
    uint16_t lo = inw((uint16_t)(g_io + o));
    uint16_t hi = inw((uint16_t)(g_io + o + 2));
    return ((uint32_t)hi << 16) | lo;
}

static const uint8_t *rtl8139_mac(void) { return g_mac; }

static bool rtl8139_send(const void *frame, uint32_t len) {
    if (!g_up || !frame || len == 0 || len > NET_FRAME_MAX) return false;
    uint32_t slot = g_tx_cur % TX_SLOTS;
    uint32_t tsd = rd32((uint16_t)(TSD0 + slot * 4));
    /* OWN (bit 13) clear means the NIC still owns the slot. */
    if (!(tsd & (1u << 13)) && tsd != 0) {
        for (int i = 0; i < 10000 && !(rd32((uint16_t)(TSD0 + slot * 4)) & (1u << 13)); i++)
            sched_poll_relax(i);
    }
    uint32_t n = len < 60 ? 60 : len;
    k_memset(g_tx[slot], 0, TX_SIZE);
    k_memcpy(g_tx[slot], frame, len);
    wr32((uint16_t)(TSAD0 + slot * 4), (uint32_t)g_tx_phys[slot]);
    wr32((uint16_t)(TSD0 + slot * 4), n);   /* writing length hands OWN to NIC */
    g_tx_cur++;
    return true;
}

static uint32_t rtl8139_poll(void *buf, uint32_t max) {
    if (!g_up) return 0;
    if (rd8(CR) & CR_BUFE) return 0;
    uint32_t off = g_rx_off % 8192;
    /* Packet header: status (16) + size (16), then the frame. */
    uint16_t status = (uint16_t)(g_rx[off] | (g_rx[off + 1] << 8));
    uint16_t size   = (uint16_t)(g_rx[off + 2] | (g_rx[off + 3] << 8));
    (void)status;
    if (size < 4 || size > 1600) {
        /* Desync: drop the ring and let the chip refill. */
        g_rx_off = 0;
        wr16(CAPR, 0xFFF0);
        return 0;
    }
    uint32_t framelen = (uint32_t)size - 4; /* minus CRC */
    uint32_t out = framelen > max ? max : framelen;
    uint32_t src = (off + 4) % 8192;
    uint8_t *d = (uint8_t *)buf;
    for (uint32_t i = 0; i < out; i++) d[i] = g_rx[(src + i) % 8192];
    uint32_t next = off + size + 4;
    next = (next + 3) & ~3u;
    g_rx_off = next;
    wr16(CAPR, (uint16_t)(g_rx_off - 16));
    wr16(ISR, 0x0001); /* ROK */
    return out;
}

static const netdrv_t RTL8139_OPS = {
    .name = "rtl8139 (Realtek 10/100)",
    .mac  = rtl8139_mac,
    .send = rtl8139_send,
    .poll = rtl8139_poll,
};

const netdrv_t *rtl8139_probe(void) {
    g_up = false;
    pci_addr_t a;
    if (!pci_find(RTL_VENDOR, RTL_8139, &a)) return NULL;
    uint16_t io = pci_bar_io_base(a, 0);
    if (!io) io = pci_bar_io_base(a, 1);
    if (!io) return NULL;
    pci_enable_io_and_dma(a);
    g_io = io;

    wr8(CONFIG1, 0x00);                 /* wake from sleep / LWAKE off */
    wr8(CR, CR_RST);
    for (int i = 0; i < 200000; i++) {
        if (!(rd8(CR) & CR_RST)) break;
        sched_poll_relax(i);
    }
    if (rd8(CR) & CR_RST) return NULL;

    for (int i = 0; i < NET_MAC_LEN; i++) g_mac[i] = rd8((uint16_t)(IDR0 + i));

    uint64_t rxp = pmm_alloc_contig((RX_BUF + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!rxp) return NULL;
    g_rx = (uint8_t *)(uintptr_t)rxp;
    g_rx_phys = rxp;
    k_memset(g_rx, 0, RX_BUF);

    for (int i = 0; i < TX_SLOTS; i++) {
        uint64_t p = pmm_alloc_contig(1);
        if (!p) return NULL;
        g_tx[i] = (uint8_t *)(uintptr_t)p;
        g_tx_phys[i] = p;
    }
    g_tx_cur = 0;
    g_rx_off = 0;

    wr32(RBSTART, (uint32_t)g_rx_phys);
    wr16(IMR, 0);
    wr16(ISR, 0xFFFF);
    wr32(RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | RCR_8K);
    wr32(TCR, 0x03000700);
    wr8(CR, CR_RE | CR_TE);
    g_up = true;
    return &RTL8139_OPS;
}
