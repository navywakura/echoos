/* e1000 scaffold. Not the primary path; virtio-net is what we test. */
#include "netif.h"
#include "pci.h"
#include "rogex.h"

/* --- identity: 8254x family members with this exact programming model ----- */
#define E1000_VENDOR   0x8086
#define DEV_82540EM    0x100e      /* VirtualBox "MT Desktop", QEMU e1000    */
#define DEV_82545EM    0x100f      /* VirtualBox/VMware "MT Server"          */
#define DEV_82543GC    0x1004      /* VirtualBox "T Server"                  */
#define DEV_82541PI    0x107c
#define DEV_82541GI    0x1076
#define DEV_82546EB    0x1010
#define DEV_82545GM    0x1026
#define DEV_82573L     0x109a      /* common 2006–2010 laptop Intel NIC      */
#define DEV_82574L     0x10d3      /* 82574L — same legacy rings             */
#define DEV_82567V     0x10de
#define DEV_82579LM    0x1502
#define DEV_82579V     0x1503

/* --- register map (byte offsets into BAR0) -------------------------------- */
#define R_CTRL    0x0000
#define R_STATUS  0x0008
#define R_EERD    0x0014           /* EEPROM read port                       */
#define R_ICR     0x00c0           /* interrupt cause (read clears)          */
#define R_IMC     0x00d8           /* interrupt mask clear (write 1s = off)  */
#define R_RCTL    0x0100
#define R_TCTL    0x0400
#define R_TIPG    0x0410
#define R_RDBAL   0x2800
#define R_RDBAH   0x2804
#define R_RDLEN   0x2808
#define R_RDH     0x2810
#define R_RDT     0x2818
#define R_TDBAL   0x3800
#define R_TDBAH   0x3804
#define R_TDLEN   0x3808
#define R_TDH     0x3810
#define R_TDT     0x3818
#define R_MTA     0x5200           /* multicast table array: 128 dwords      */
#define R_RAL0    0x5400           /* receive address 0 low                  */
#define R_RAH0    0x5404           /* receive address 0 high + AV bit        */

#define CTRL_LRST     (1u << 3)    /* link reset (must be 0 for link up)     */
#define CTRL_ASDE     (1u << 5)    /* auto speed detection                   */
#define CTRL_SLU      (1u << 6)    /* set link up                            */
#define CTRL_ILOS     (1u << 7)    /* invert loss-of-signal (keep 0)         */
#define CTRL_RST      (1u << 26)   /* device reset (self-clearing)           */
#define CTRL_VME      (1u << 30)   /* VLAN mode (keep 0)                     */
#define CTRL_PHY_RST  (1u << 31)

#define RCTL_EN       (1u << 1)
#define RCTL_BAM      (1u << 15)   /* accept broadcast (Wired PINGs)         */
#define RCTL_SECRC    (1u << 26)   /* strip CRC (lengths = frame only)       */
/* BSIZE bits 17:16 left 00 = 2048-byte buffers                              */

#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)    /* pad short packets to 64 bytes on wire  */
#define TCTL_CT_15    (0x0fu << 4) /* collision threshold (spec value)       */
#define TCTL_COLD_FD  (0x40u << 12)/* collision distance, full duplex        */

#define EERD_START    (1u << 0)    /* 82540EM-class EERD: DONE=bit4, addr<<8 */
#define EERD_DONE     (1u << 4)

#define RAH_AV        (1u << 31)   /* address valid                          */

/* Legacy descriptors (8254x SDM 3.2.3 / 3.3.3). Exactly 16 bytes each. */
typedef struct ROGEX_PACKED {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;               /* bit0 DD, bit1 EOP                      */
    uint8_t  errors;
    uint16_t special;
} rx_desc;

typedef struct ROGEX_PACKED {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;                  /* bit0 EOP, bit1 IFCS, bit3 RS           */
    uint8_t  status;               /* bit0 DD                                */
    uint8_t  css;
    uint16_t special;
} tx_desc;

#define DESC_DD   0x01
#define TX_EOP    0x01
#define TX_IFCS   0x02             /* NIC appends the Ethernet CRC           */
#define TX_RS     0x08             /* report status -> DD when sent          */

#define NRX 32                     /* RDLEN 512 B (must be 128-aligned)      */
#define NTX 8                      /* TDLEN 128 B                            */
#define BUF_SZ 2048
#define POLL_BUDGET 4000000
#define PTE_PCD (1ULL << 4)

static volatile uint8_t *g_mmio;
static volatile rx_desc *g_rxr;    /* the two rings share one PMM frame      */
static volatile tx_desc *g_txr;
static uintptr_t g_rxbuf[NRX], g_txbuf;
static uint32_t  g_rx_next, g_tx_next;
static uint8_t   g_mac[NET_MAC_LEN];
static bool      g_present;

/* --- MMIO access: volatile always, or the optimizer breaks the hardware --- */
static inline uint32_t rr(uint32_t off)             { return *(volatile uint32_t *)(g_mmio + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_mmio + off) = v; }
static inline void     mfence(void)                 { __asm__ volatile ("mfence" ::: "memory"); }

static void map_mmio(uint64_t phys, uint64_t len) {
    uint64_t s = phys & ~0xfffULL, e = (phys + len + 0xfff) & ~0xfffULL;
    for (uint64_t p = s; p < e; p += 0x1000) vmm_map_page(p, p, PTE_WRITABLE | PTE_PCD);
}

/* One EEPROM word via EERD. The 82540EM class signals completion in bit 4
   (newer chips moved it — those are not in our probe list). */
static bool eeprom_read(uint8_t addr, uint16_t *out) {
    wr(R_EERD, EERD_START | ((uint32_t)addr << 8));
    for (int i = 0; i < POLL_BUDGET; i++) {
        uint32_t v = rr(R_EERD);
        if (v & EERD_DONE) { *out = (uint16_t)(v >> 16); return true; }
        __asm__ volatile ("pause");
    }
    return false;
}

/* --- backend ops ---------------------------------------------------------- */
static const uint8_t *e1000_mac(void) { return g_mac; }

static bool e1000_send(const void *frame, uint32_t len) {
    if (!g_present || len == 0 || len > NET_FRAME_MAX) return false;
    /* Zero-pad runts to the 60-byte minimum (TCTL.PSP pads on the wire too,
       but padding the DMA buffer keeps the descriptor length honest). */
    uint32_t wire = len < 60 ? 60 : len;
    k_memset((void *)g_txbuf, 0, wire);
    k_memcpy((void *)g_txbuf, frame, len);

    uint32_t i = g_tx_next;
    volatile tx_desc *d = &g_txr[i];
    d->addr = g_txbuf; d->length = (uint16_t)wire;
    d->cso = 0; d->css = 0; d->special = 0;
    d->status = 0;
    d->cmd = TX_EOP | TX_IFCS | TX_RS;
    mfence();                                   /* descriptor before doorbell */
    g_tx_next = (i + 1) % NTX;
    wr(R_TDT, g_tx_next);                       /* doorbell: NIC DMAs + sends */

    for (int spin = 0; spin < POLL_BUDGET; spin++) {
        if (g_txr[i].status & DESC_DD) return true;   /* sent                 */
        __asm__ volatile ("pause");
    }
    return false;                               /* timed out                  */
}

static uint32_t e1000_poll(void *out, uint32_t max) {
    if (!g_present) return 0;
    uint32_t i = g_rx_next;
    volatile rx_desc *d = &g_rxr[i];
    if (!(d->status & DESC_DD)) return 0;       /* nothing received           */
    mfence();

    uint32_t len = d->length;                   /* SECRC: no CRC included     */
    if (len > max) len = max;
    if (len) k_memcpy(out, (void *)g_rxbuf[i], len);

    /* Return the slot to hardware: clear status, then advance the tail onto
       it. RDT always trails the slot being consumed by exactly the one-slot
       gap the 8254x requires between head and tail. */
    d->status = 0;
    mfence();
    wr(R_RDT, i);
    g_rx_next = (i + 1) % NRX;
    return len;
}

static bool e1000_init(pci_addr_t a) {
    pci_enable_mem_and_dma(a);
    uint64_t bar0 = pci_bar_mem_base(a, 0);
    if (!bar0) return false;
    map_mmio(bar0, 0x20000);                    /* 128 KiB register window    */
    g_mmio = (volatile uint8_t *)(uintptr_t)bar0;

    /* Reset: mask interrupts, whole-device reset, mask again (RST clears the
       mask), drain any latched cause. RST is self-clearing. */
    wr(R_IMC, 0xffffffffu);
    wr(R_CTRL, rr(R_CTRL) | CTRL_RST);
    for (int i = 0; i < POLL_BUDGET && (rr(R_CTRL) & CTRL_RST); i++)
        __asm__ volatile ("pause");
    if (rr(R_CTRL) & CTRL_RST) return false;
    wr(R_IMC, 0xffffffffu);
    (void)rr(R_ICR);

    /* Link up: SLU + auto speed detect; LRST/ILOS/VME/PHY_RST must be 0. */
    uint32_t ctrl = rr(R_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_ILOS | CTRL_VME | CTRL_PHY_RST);
    wr(R_CTRL, ctrl);

    /* MAC: EEPROM words 0..2 (little-endian pairs); if the emulated EEPROM
       does not answer, fall back to whatever firmware left in RAL0/RAH0. */
    uint16_t w0, w1, w2;
    if (eeprom_read(0, &w0) && eeprom_read(1, &w1) && eeprom_read(2, &w2)) {
        g_mac[0] = (uint8_t)w0; g_mac[1] = (uint8_t)(w0 >> 8);
        g_mac[2] = (uint8_t)w1; g_mac[3] = (uint8_t)(w1 >> 8);
        g_mac[4] = (uint8_t)w2; g_mac[5] = (uint8_t)(w2 >> 8);
    } else {
        uint32_t ral = rr(R_RAL0), rah = rr(R_RAH0);
        if (!ral && !(rah & 0xffff)) return false;     /* no MAC anywhere     */
        g_mac[0] = (uint8_t)ral;         g_mac[1] = (uint8_t)(ral >> 8);
        g_mac[2] = (uint8_t)(ral >> 16); g_mac[3] = (uint8_t)(ral >> 24);
        g_mac[4] = (uint8_t)rah;         g_mac[5] = (uint8_t)(rah >> 8);
    }
    /* Program the exact-match filter with our MAC and clear the MTA. */
    wr(R_RAL0, (uint32_t)g_mac[0] | ((uint32_t)g_mac[1] << 8) |
               ((uint32_t)g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24));
    wr(R_RAH0, (uint32_t)g_mac[4] | ((uint32_t)g_mac[5] << 8) | RAH_AV);
    for (int i = 0; i < 128; i++) wr(R_MTA + (uint32_t)i * 4, 0);

    /* Rings: one zeroed PMM frame holds both (RX 32*16=512 B at +0, TX
       8*16=128 B at +2048; both 16-aligned, lengths 128-multiples). */
    uint64_t ring = pmm_alloc_frame();
    if (!ring) return false;
    k_memset((void *)(uintptr_t)ring, 0, 4096);
    g_rxr = (volatile rx_desc *)(uintptr_t)ring;
    g_txr = (volatile tx_desc *)(uintptr_t)(ring + 2048);

    /* RX buffers: 2048 bytes each, two per PMM frame. */
    for (int i = 0; i < NRX; i += 2) {
        uint64_t f = pmm_alloc_frame();
        if (!f) return false;
        g_rxbuf[i] = (uintptr_t)f;
        if (i + 1 < NRX) g_rxbuf[i + 1] = (uintptr_t)f + BUF_SZ;
    }
    for (int i = 0; i < NRX; i++) { g_rxr[i].addr = g_rxbuf[i]; g_rxr[i].status = 0; }
    uint64_t txf = pmm_alloc_frame();
    if (!txf) return false;
    g_txbuf = (uintptr_t)txf;

    mfence();                                   /* rings visible before enable */
    wr(R_RDBAL, (uint32_t)ring); wr(R_RDBAH, (uint32_t)(ring >> 32));
    wr(R_RDLEN, NRX * 16);
    wr(R_RDH, 0); wr(R_RDT, NRX - 1);           /* all but one slot to the NIC */
    g_rx_next = 0;

    wr(R_TDBAL, (uint32_t)(ring + 2048)); wr(R_TDBAH, (uint32_t)((ring + 2048) >> 32));
    wr(R_TDLEN, NTX * 16);
    wr(R_TDH, 0); wr(R_TDT, 0);
    g_tx_next = 0;

    wr(R_TIPG, 10 | (8u << 10) | (6u << 20));   /* SDM copper defaults        */
    wr(R_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT_15 | TCTL_COLD_FD);
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    g_present = true;
    return true;
}

/* --- probe entry (no INTx ops: this backend is polled-only today) --------- */
static const netdrv_t E1000_OPS = {
    .name = "e1000",
    .mac  = e1000_mac,
    .send = e1000_send,
    .poll = e1000_poll,
};

const netdrv_t *e1000_probe(void) {
    static const uint16_t IDS[] = {
        DEV_82540EM, DEV_82545EM, DEV_82543GC,
        DEV_82541PI, DEV_82541GI, DEV_82546EB, DEV_82545GM,
        DEV_82573L, DEV_82574L, DEV_82567V, DEV_82579LM, DEV_82579V,
    };
    pci_addr_t a;
    for (unsigned i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++)
        if (pci_find(E1000_VENDOR, IDS[i], &a))
            return e1000_init(a) ? &E1000_OPS : NULL;
    return NULL;
}
