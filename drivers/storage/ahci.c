/*
 * AHCI SATA. Lets real machines persist without legacy IDE.
 * Still young — timeouts and odd controllers will bite.
 */
#include "ahci.h"
#include "pci.h"
#include "scheduler.h"

/* ------------------------------ HBA registers ----------------------------- */
#define HBA_CAP        0x00
#define HBA_GHC        0x04
#define HBA_IS         0x08
#define HBA_PI         0x0C     /* ports implemented, one bit per port       */
#define HBA_CAP2       0x24
#define HBA_BOHC       0x28     /* BIOS/OS handoff control                   */

#define GHC_HR         (1u << 0)    /* HBA reset                             */
#define GHC_AE         (1u << 31)   /* AHCI enable                           */
#define CAP2_BOH       (1u << 0)    /* BIOS/OS handoff supported             */
#define BOHC_BOS       (1u << 0)    /* BIOS owned                            */
#define BOHC_OOS       (1u << 1)    /* OS owned                              */
#define BOHC_BB        (1u << 4)    /* BIOS busy                             */

/* Port registers, at 0x100 + port*0x80. */
#define PORT_BASE(n)   (0x100 + (n) * 0x80)
#define PxCLB          0x00
#define PxCLBU         0x04
#define PxFB           0x08
#define PxFBU          0x0C
#define PxIS           0x10
#define PxIE           0x14
#define PxCMD          0x18
#define PxTFD          0x20
#define PxSIG          0x24
#define PxSSTS         0x28
#define PxSERR         0x30
#define PxCI           0x38

#define PxCMD_ST       (1u << 0)    /* start                                 */
#define PxCMD_FRE      (1u << 4)    /* FIS receive enable                    */
#define PxCMD_FR       (1u << 14)   /* FIS receive running                   */
#define PxCMD_CR       (1u << 15)   /* command list running                  */

#define PxIS_TFES      (1u << 30)   /* task file error                       */
#define TFD_BSY        (1u << 7)
#define TFD_DRQ        (1u << 3)
#define TFD_ERR        (1u << 0)

#define SIG_SATA       0x00000101u  /* a plain SATA disk (not ATAPI/SEMB)    */

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_IDENTIFY       0xEC
#define ATA_FLUSH_EXT      0xEA

/* Bounded spins, same policy as the ATA PIO driver: a wedged controller must
   not be able to hang the boot. */
#define AHCI_SPINS 2000000

/* ----------------------------- DMA structures ----------------------------- */
typedef struct ROGEX_PACKED {
    uint16_t flags;             /* [4:0] cmd FIS length in dwords, bit6 = W  */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t rsv[4];
} ahci_cmd_header_t;            /* 32 bytes                                  */

typedef struct ROGEX_PACKED {
    uint32_t dba, dbau, rsv;
    uint32_t dbc;               /* [21:0] byte count - 1, bit31 = interrupt  */
} ahci_prdt_t;

typedef struct ROGEX_PACKED {
    uint8_t     cfis[64];
    uint8_t     acmd[16];
    uint8_t     rsv[48];
    ahci_prdt_t prdt[8];
} ahci_cmd_tbl_t;

typedef struct ROGEX_PACKED {
    uint8_t fis_type;           /* 0x27 = host to device                     */
    uint8_t pmport_c;           /* bit7 set = this is a command              */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, featureh;
    uint8_t countl, counth, icc, control;
    uint8_t rsv[4];
} fis_h2d_t;

/* ------------------------------ driver state ------------------------------ */
static volatile uint8_t *g_abar;
static int        g_port = -1;
static uint32_t   g_sectors;
static bool       g_present;
static blockdev_t g_sda;
static char       g_model[44] = "-";

/* One page holds everything the port needs, at the alignments AHCI demands:
   command list 1 KiB-aligned, FIS area 256-byte aligned, command table
   128-byte aligned. A page start satisfies all three. */
static uint64_t   g_dma;            /* command list + FIS + command table    */
static uint64_t   g_bounce;         /* transfer staging buffer               */
#define BOUNCE_SECTORS 64
#define BOUNCE_BYTES   (BOUNCE_SECTORS * BLOCKDEV_SECTOR)

#define CL_OFF   0
#define FIS_OFF  1024
#define CT_OFF   1280

static inline uint32_t hba_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_abar + off);
}
static inline void hba_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_abar + off) = v;
}
static inline uint32_t prd(uint32_t reg) { return hba_rd(PORT_BASE(g_port) + reg); }
static inline void pwr(uint32_t reg, uint32_t v) { hba_wr(PORT_BASE(g_port) + reg, v); }

/* ------------------------------ port control ------------------------------ */
static bool port_stop(void) {
    pwr(PxCMD, prd(PxCMD) & ~PxCMD_ST);
    for (int i = 0; i < AHCI_SPINS; i++) {
        if (!(prd(PxCMD) & PxCMD_CR)) break;
        sched_poll_relax(i);
        if (i == AHCI_SPINS - 1) return false;
    }
    pwr(PxCMD, prd(PxCMD) & ~PxCMD_FRE);
    for (int i = 0; i < AHCI_SPINS; i++) {
        if (!(prd(PxCMD) & PxCMD_FR)) return true;
        sched_poll_relax(i);
    }
    return false;
}

static bool port_start(void) {
    for (int i = 0; i < AHCI_SPINS; i++) {
        if (!(prd(PxCMD) & PxCMD_CR)) break;
        sched_poll_relax(i);
        if (i == AHCI_SPINS - 1) return false;
    }
    pwr(PxCMD, prd(PxCMD) | PxCMD_FRE);
    pwr(PxCMD, prd(PxCMD) | PxCMD_ST);
    return true;
}

static bool wait_ready(void) {
    for (int i = 0; i < AHCI_SPINS; i++) {
        if (!(prd(PxTFD) & (TFD_BSY | TFD_DRQ))) return true;
        sched_poll_relax(i);
    }
    return false;
}

/* -------------------------- issue one command ----------------------------
   Slot 0 only. `bytes` may be 0 for commands with no data (FLUSH). Returns
   false on timeout or a task-file error, never blocks forever. */
static bool issue(uint8_t command, uint64_t lba, uint32_t count,
                  uint64_t buf_phys, uint32_t bytes, bool write) {
    if (!wait_ready()) return false;

    pwr(PxIS, prd(PxIS));                    /* clear stale status           */
    pwr(PxSERR, prd(PxSERR));

    ahci_cmd_header_t *hdr = (ahci_cmd_header_t *)(uintptr_t)(g_dma + CL_OFF);
    ahci_cmd_tbl_t    *tbl = (ahci_cmd_tbl_t *)(uintptr_t)(g_dma + CT_OFF);

    k_memset(tbl, 0, sizeof(*tbl));
    k_memset(hdr, 0, sizeof(*hdr));
    hdr->flags = (uint16_t)((sizeof(fis_h2d_t) / 4) | (write ? (1u << 6) : 0));
    hdr->prdtl = bytes ? 1 : 0;
    hdr->ctba  = (uint32_t)(g_dma + CT_OFF);
    hdr->ctbau = 0;
    hdr->prdbc = 0;

    if (bytes) {
        tbl->prdt[0].dba  = (uint32_t)buf_phys;
        tbl->prdt[0].dbau = 0;
        tbl->prdt[0].dbc  = bytes - 1;       /* AHCI stores count minus one  */
    }

    fis_h2d_t *fis = (fis_h2d_t *)tbl->cfis;
    fis->fis_type = 0x27;
    fis->pmport_c = 0x80;                    /* this FIS is a command        */
    fis->command  = command;
    fis->device   = 0x40;                    /* LBA mode                     */
    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)(count);
    fis->counth = (uint8_t)(count >> 8);

    pwr(PxCI, 1u);
    for (int i = 0; i < AHCI_SPINS; i++) {
        if (!(prd(PxCI) & 1u)) break;
        if (prd(PxIS) & PxIS_TFES) return false;
        sched_poll_relax(i);                 /* yield: a transfer must not
                                                freeze the whole machine     */
        if (i == AHCI_SPINS - 1) return false;
    }
    if (prd(PxIS) & PxIS_TFES) return false;
    return !(prd(PxTFD) & TFD_ERR);
}

/* ------------------------------ block device ------------------------------ */
/* Everything goes through a page-aligned bounce buffer. A PRD data address
   has alignment rules and must be physically contiguous; a caller's buffer
   satisfies neither by construction, and copying 32 KiB is nothing next to a
   5400 rpm seek. */
/* Subtraction form: `lba + count` wraps at 32 bits, which would let a request
   past the end of the disk pass a naive bounds check. */
static bool in_range(uint32_t lba, uint32_t count) {
    return count != 0 && count <= g_sectors && lba <= g_sectors - count;
}

static bool ahci_read(blockdev_t *dev, uint32_t lba, void *buf, uint32_t count) {
    (void)dev;
    if (!g_present || !in_range(lba, count)) return false;
    uint8_t *out = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        if (!issue(ATA_READ_DMA_EXT, lba, n, g_bounce, n * BLOCKDEV_SECTOR, false))
            return false;
        k_memcpy(out, (const void *)(uintptr_t)g_bounce, n * BLOCKDEV_SECTOR);
        out += n * BLOCKDEV_SECTOR; lba += n; count -= n;
    }
    return true;
}

static bool ahci_write(blockdev_t *dev, uint32_t lba, const void *buf, uint32_t count) {
    (void)dev;
    if (!g_present || !in_range(lba, count)) return false;
    const uint8_t *in = (const uint8_t *)buf;
    while (count) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        k_memcpy((void *)(uintptr_t)g_bounce, in, n * BLOCKDEV_SECTOR);
        if (!issue(ATA_WRITE_DMA_EXT, lba, n, g_bounce, n * BLOCKDEV_SECTOR, true))
            return false;
        in += n * BLOCKDEV_SECTOR; lba += n; count -= n;
    }
    /* Durability: the drive may still be holding the data in its cache. */
    return issue(ATA_FLUSH_EXT, 0, 0, 0, 0, false);
}

/* ------------------------------- IDENTIFY --------------------------------- */
/* Words 100..103 hold the LBA48 sector count; 60..61 the LBA28 one. The
   blockdev layer addresses sectors with a uint32, so a disk larger than 2 TiB
   is reported clamped rather than wrapped — the HP's 1 TB disk is ~1.95e9
   sectors and fits. */
static bool identify(void) {
    if (!issue(ATA_IDENTIFY, 0, 0, g_bounce, 512, false)) return false;
    const uint16_t *id = (const uint16_t *)(uintptr_t)g_bounce;

    uint64_t s48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                   ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    uint32_t s28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    uint64_t sectors = s48 ? s48 : s28;
    if (sectors == 0) return false;
    g_sectors = sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sectors;

    /* Words 27..46: model string, ASCII, byte-swapped within each word. */
    int n = 0;
    for (int w = 27; w <= 46 && n < (int)sizeof(g_model) - 1; w++) {
        g_model[n++] = (char)(id[w] >> 8);
        if (n < (int)sizeof(g_model) - 1) g_model[n++] = (char)(id[w] & 0xFF);
    }
    while (n > 0 && (g_model[n-1] == ' ' || g_model[n-1] == '\0')) n--;
    g_model[n] = '\0';
    if (n == 0) { g_model[0] = '-'; g_model[1] = '\0'; }
    return true;
}

/* --------------------------------- probe ---------------------------------- */
/* Firmware may still own the controller when we take over from a CSM boot.
   The spec's handshake is short and skipping it risks the BIOS and the kernel
   both driving the same port. */
static void bios_handoff(void) {
    if (!(hba_rd(HBA_CAP2) & CAP2_BOH)) return;
    hba_wr(HBA_BOHC, hba_rd(HBA_BOHC) | BOHC_OOS);
    for (int i = 0; i < AHCI_SPINS; i++) {
        uint32_t b = hba_rd(HBA_BOHC);
        if (!(b & BOHC_BOS) && !(b & BOHC_BB)) return;
        sched_poll_relax(i);
    }
}

bool ahci_init(void) {
    g_present = false;
    g_port = -1;

    pci_addr_t a;
    if (!pci_find_class(0x01, 0x06, 0x01, &a)) return false;
    uint64_t abar = pci_bar_mem_base(a, 5);          /* ABAR is always BAR5  */
    if (!abar) return false;
    pci_enable_mem_and_dma(a);

    /* The register block is 0x1100 bytes for 32 ports; map two pages. */
    for (uint64_t off = 0; off < 0x2000; off += PAGE_SIZE)
        vmm_map_page(abar + off, abar + off, PTE_WRITABLE);
    g_abar = (volatile uint8_t *)(uintptr_t)abar;

    bios_handoff();
    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);

    /* First implemented port carrying a SATA disk with an established link.
       DET==3 means device present and communication established; IPM==1 means
       the interface is in active power state. */
    uint32_t pi = hba_rd(HBA_PI);
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t ssts = hba_rd(PORT_BASE(p) + PxSSTS);
        if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1) continue;
        if (hba_rd(PORT_BASE(p) + PxSIG) != SIG_SATA) continue;   /* skip ATAPI */
        g_port = p;
        break;
    }
    if (g_port < 0) return false;

    g_dma = pmm_alloc_contig(1);
    g_bounce = pmm_alloc_contig((BOUNCE_BYTES + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!g_dma || !g_bounce) return false;

    if (!port_stop()) return false;
    pwr(PxCLB, (uint32_t)(g_dma + CL_OFF));  pwr(PxCLBU, 0);
    pwr(PxFB,  (uint32_t)(g_dma + FIS_OFF)); pwr(PxFBU, 0);
    pwr(PxSERR, prd(PxSERR));
    pwr(PxIS, prd(PxIS));
    pwr(PxIE, 0);                            /* polled: no interrupts wanted */
    if (!port_start()) return false;

    /* IDENTIFY has to succeed before we claim the disk exists — a port that
       links up but will not answer commands is not a block device. issue()
       does not consult g_present, so this runs before the flag is set. */
    if (!identify()) { port_stop(); return false; }

    g_sda.name    = "sda";
    g_sda.sectors = g_sectors;
    g_sda.read    = ahci_read;
    g_sda.write   = ahci_write;
    g_sda.ctx     = NULL;
    g_present = blockdev_register(&g_sda);
    if (!g_present) port_stop();
    return g_present;
}

bool        ahci_present(void) { return g_present; }
blockdev_t *ahci_dev(void)     { return g_present ? &g_sda : NULL; }
const char *ahci_model(void)   { return g_model; }
