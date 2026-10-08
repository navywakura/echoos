/*
 * Realtek r8169-family driver. Not fully verified in-tree — no emulator has
 * this chip cleanly. Treat as experimental; report what your hardware does.
 */
#include "netif.h"
#include "pci.h"
#include "scheduler.h"

#define RTL_VENDOR 0x10EC
#define RTL_8139   0x8139        /* a different chip entirely — never claim it */

/* ------------------------------- registers -------------------------------- */
#define REG_IDR0       0x00      /* MAC address, 6 bytes                      */
#define REG_TNPDS      0x20      /* TX normal-priority descriptor start (64b)  */
#define REG_CR         0x37      /* command                                   */
#define REG_TPPOLL     0x38      /* transmit poll (doorbell)                  */
#define REG_IMR        0x3C
#define REG_ISR        0x3E
#define REG_TCR        0x40
#define REG_RCR        0x44
#define REG_CFG9346    0x50      /* register write-protect latch              */
#define REG_PHYSTATUS  0x6C
#define REG_RMS        0xDA      /* RX max packet size                        */
#define REG_CPCR       0xE0      /* C+ command                                */
#define REG_RDSAR      0xE4      /* RX descriptor start (64b)                 */
#define REG_MTPS       0xEC      /* max TX packet size, 128-byte units        */

#define CR_RST         0x10
#define CR_RE          0x08
#define CR_TE          0x04
#define TPPOLL_NPQ     0x40

#define CFG9346_UNLOCK 0xC0
#define CFG9346_LOCK   0x00

#define PHYSTATUS_LINK 0x02

/* Descriptor ownership and framing bits (opts1). */
#define DESC_OWN       (1u << 31)
#define DESC_EOR       (1u << 30)   /* end of ring                           */
#define DESC_FS        (1u << 29)   /* first segment                         */
#define DESC_LS        (1u << 28)   /* last segment                          */
#define DESC_LEN_MASK  0x00003FFFu

#define RX_DESCS   16
#define TX_DESCS   8
#define BUF_SIZE   2048            /* per descriptor                          */
#define RTL_SPINS  200000

typedef struct ROGEX_PACKED {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} rtl_desc_t;                      /* 16 bytes, rings are 256-byte aligned    */

/* ------------------------------ driver state ------------------------------ */
static volatile uint8_t *g_mmio;
static rtl_desc_t *g_rx, *g_tx;
static uint64_t    g_rx_buf, g_tx_buf;
static uint32_t    g_rx_cur, g_tx_cur;
static uint8_t     g_mac[NET_MAC_LEN];
static bool        g_up;

static inline uint8_t  rd8(uint32_t o)  { return *(volatile uint8_t  *)(g_mmio + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(g_mmio + o); }
static inline void wr8(uint32_t o, uint8_t v)   { *(volatile uint8_t  *)(g_mmio + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_mmio + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio + o) = v; }

/* The NIC reads our descriptors by DMA, so descriptor stores must be globally
   visible before the doorbell write that tells it to look. */
static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

/* --------------------------------- rings ---------------------------------- */
static void rings_init(void) {
    for (uint32_t i = 0; i < RX_DESCS; i++) {
        g_rx[i].addr  = g_rx_buf + (uint64_t)i * BUF_SIZE;
        g_rx[i].opts2 = 0;
        g_rx[i].opts1 = DESC_OWN | BUF_SIZE | (i == RX_DESCS - 1 ? DESC_EOR : 0);
    }
    for (uint32_t i = 0; i < TX_DESCS; i++) {
        g_tx[i].addr  = g_tx_buf + (uint64_t)i * BUF_SIZE;
        g_tx[i].opts2 = 0;
        g_tx[i].opts1 = (i == TX_DESCS - 1 ? DESC_EOR : 0);   /* not owned yet */
    }
    g_rx_cur = g_tx_cur = 0;
    barrier();
}

/* --------------------------------- ops ------------------------------------ */
static const uint8_t *r8169_mac(void) { return g_mac; }

static bool r8169_send(const void *frame, uint32_t len) {
    if (!g_up || len == 0 || len > NET_FRAME_MAX) return false;

    rtl_desc_t *d = &g_tx[g_tx_cur];
    /* Wait for the slot to come back to us rather than overwrite a frame the
       NIC has not sent yet. Bounded: a dead link must not hang the caller. */
    for (int i = 0; (d->opts1 & DESC_OWN) && i < RTL_SPINS; i++) sched_poll_relax(i);
    if (d->opts1 & DESC_OWN) return false;

    k_memcpy((void *)(uintptr_t)(g_tx_buf + (uint64_t)g_tx_cur * BUF_SIZE), frame, len);

    /* The chip pads runt frames itself, but the minimum Ethernet payload is
       still ours to respect for anything that counts bytes on the wire. */
    uint32_t n = len < 60 ? 60 : len;
    d->opts2 = 0;
    d->opts1 = DESC_OWN | DESC_FS | DESC_LS | n |
               (g_tx_cur == TX_DESCS - 1 ? DESC_EOR : 0);
    barrier();
    wr8(REG_TPPOLL, TPPOLL_NPQ);                 /* doorbell                  */

    g_tx_cur = (g_tx_cur + 1) % TX_DESCS;
    return true;
}

static uint32_t r8169_poll(void *buf, uint32_t max) {
    if (!g_up) return 0;
    rtl_desc_t *d = &g_rx[g_rx_cur];
    if (d->opts1 & DESC_OWN) return 0;           /* still the NIC's           */

    uint32_t len = d->opts1 & DESC_LEN_MASK;
    /* The reported length includes the 4-byte FCS the hardware appended. */
    uint32_t out = 0;
    if (len > 4) {
        len -= 4;
        out = len > max ? max : len;
        k_memcpy(buf, (const void *)(uintptr_t)(g_rx_buf + (uint64_t)g_rx_cur * BUF_SIZE), out);
    }

    d->opts2 = 0;
    d->opts1 = DESC_OWN | BUF_SIZE | (g_rx_cur == RX_DESCS - 1 ? DESC_EOR : 0);
    barrier();
    g_rx_cur = (g_rx_cur + 1) % RX_DESCS;
    return out;
}

static char g_name[40] = "r8169 (Realtek)";

static const netdrv_t R8169_OPS = {
    .name = g_name,
    .mac = r8169_mac,
    .send = r8169_send,
    .poll = r8169_poll,
    .irq_enable = NULL,      /* polled only: see the file header              */
    .irq_active = NULL,
    .irq_line = NULL,
    .take_pending = NULL,
};

/* --------------------------------- probe ---------------------------------- */
/* Realtek Ethernet controllers all sit at vendor 0x10EC, class 02:00. Device
   0x8139 is the RTL8139, an unrelated programming model with its own ring
   format — decline it rather than drive it wrongly. */
static bool r8169_visit(pci_addr_t a, uint8_t cls, uint8_t sub, void *user) {
    if (cls != 0x02 || sub != 0x00) return true;
    if (a.vendor != RTL_VENDOR || a.device == RTL_8139) return true;
    *(pci_addr_t *)user = a;
    return false;
}

static bool find_nic(pci_addr_t *out) {
    pci_addr_t found = { 0, 0, 0, 0, 0 };
    int n = pci_foreach(r8169_visit, &found);
    (void)n;
    if (!found.vendor) return false;
    *out = found;
    return true;
}

const netdrv_t *r8169_probe(void) {
    g_up = false;

    pci_addr_t a;
    if (!find_nic(&a)) return NULL;

    /* HP 15-ac195nl on-board Ethernet is RTL8106E (10EC:8136). Same
       programming model as 8168/8169. WiFi on that laptop is 10EC:8179
       (RTL8188EE, class 02:80) and is *not* this driver. */
    {
        const char *chip = "RTL81xx";
        if (a.device == 0x8136) chip = "RTL8106E/8101E (HP 10/100)";
        else if (a.device == 0x8168) chip = "RTL8168/8111";
        else if (a.device == 0x8169) chip = "RTL8169";
        else if (a.device == 0x8161) chip = "RTL8161";
        char *d = g_name;
        const char *p = "r8169 ";
        while (*p) *d++ = *p++;
        p = chip; while (*p && d < g_name + sizeof(g_name) - 1) *d++ = *p++;
        *d = '\0';
    }

    /* Registers live in a memory BAR. Which one varies across the family
       (BAR2 on the PCIe parts, BAR1 on some older ones), so try in order
       rather than assume. */
    uint64_t bar = pci_bar_mem_base(a, 2);
    if (!bar) bar = pci_bar_mem_base(a, 1);
    if (!bar) bar = pci_bar_mem_base(a, 0);
    if (!bar) return NULL;
    pci_enable_mem_and_dma(a);
    for (uint64_t off = 0; off < 0x1000; off += PAGE_SIZE)
        vmm_map_page(bar + off, bar + off, PTE_WRITABLE);
    g_mmio = (volatile uint8_t *)(uintptr_t)bar;

    /* Soft reset and wait for the chip to release it. */
    wr8(REG_CR, CR_RST);
    bool reset_done = false;
    for (int i = 0; i < RTL_SPINS; i++) {
        if (!(rd8(REG_CR) & CR_RST)) { reset_done = true; break; }
        sched_poll_relax(i);
    }
    if (!reset_done) return NULL;

    for (int i = 0; i < NET_MAC_LEN; i++) g_mac[i] = rd8(REG_IDR0 + (uint32_t)i);
    /* An all-zero or all-ones MAC means the chip never came up; refusing here
       is better than announcing a NIC that cannot be addressed. */
    bool zero = true, ones = true;
    for (int i = 0; i < NET_MAC_LEN; i++) {
        if (g_mac[i] != 0x00) zero = false;
        if (g_mac[i] != 0xFF) ones = false;
    }
    if (zero || ones) return NULL;

    /* Rings and buffers. One page holds both descriptor rings (RX 256 B, TX
       128 B) at the required 256-byte alignment; buffers get their own run. */
    uint64_t rings = pmm_alloc_contig(1);
    uint64_t rxb   = pmm_alloc_contig((RX_DESCS * BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t txb   = pmm_alloc_contig((TX_DESCS * BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!rings || !rxb || !txb) return NULL;
    g_rx = (rtl_desc_t *)(uintptr_t)rings;
    g_tx = (rtl_desc_t *)(uintptr_t)(rings + 256);
    g_rx_buf = rxb; g_tx_buf = txb;
    rings_init();

    wr8(REG_CFG9346, CFG9346_UNLOCK);       /* unlock the config registers   */

    /* RX: accept broadcast, multicast and frames addressed to us; unlimited
       DMA burst; no receive FIFO threshold. Rogex Wired is a raw L2 protocol
       that uses broadcast for discovery, so broadcast is not optional. */
    wr32(REG_RCR, 0x0000E70E);
    /* TX: unlimited DMA burst, standard interframe gap. */
    wr32(REG_TCR, 0x03000700);
    wr16(REG_CPCR, rd16(REG_CPCR));         /* keep the chip's own defaults  */
    wr16(REG_RMS, BUF_SIZE);
    wr8(REG_MTPS, 0x3B);

    wr32(REG_TNPDS,     (uint32_t)(uintptr_t)g_tx);
    wr32(REG_TNPDS + 4, 0);
    wr32(REG_RDSAR,     (uint32_t)(uintptr_t)g_rx);
    wr32(REG_RDSAR + 4, 0);

    wr8(REG_CFG9346, CFG9346_LOCK);
    wr16(REG_IMR, 0);                       /* polled: mask every interrupt  */
    wr16(REG_ISR, 0xFFFF);                  /* clear anything latched        */
    wr8(REG_CR, CR_RE | CR_TE);
    barrier();

    g_up = true;
    return &R8169_OPS;
}

/* Link state, for the Wired panel. Reported separately from "driver bound"
   because a cable can be unplugged without the NIC disappearing. */
bool r8169_link_up(void) {
    return g_up && (rd8(REG_PHYSTATUS) & PHYSTATUS_LINK) != 0;
}
