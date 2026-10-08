/*
 * virtio-pci block (vda). Modern path on q35. Still polled; don't expect
 * high throughput. Report hangs if a guest config confuses the feature bits.
 */
#include "virtio_blk.h"
#include "pci.h"
#include "scheduler.h"
#include "rogex.h"

/* --- virtio-pci identity ------------------------------------------------- */
#define VIRTIO_VENDOR       0x1af4
#define VIRTIO_BLK_MODERN   0x1042   /* device id 2 -> 0x1040 + 2 (modern)   */
#define VIRTIO_BLK_TRANS    0x1001   /* transitional; also speaks modern     */

/* Vendor capability (cap id 0x09) cfg_type values (spec 4.1.4). */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

/* device_status bits (spec 2.1). */
#define ST_ACK        1
#define ST_DRIVER     2
#define ST_DRIVER_OK  4
#define ST_FEATURES_OK 8
#define ST_FAILED     128

/* common_cfg register offsets (spec 4.1.4.3). */
#define CC_DEV_FEAT_SEL   0x00
#define CC_DEV_FEAT       0x04
#define CC_DRV_FEAT_SEL   0x08
#define CC_DRV_FEAT       0x0c
#define CC_MSIX_CONFIG    0x10
#define CC_NUM_QUEUES     0x12
#define CC_DEV_STATUS     0x14
#define CC_CFG_GEN        0x15
#define CC_Q_SELECT       0x16
#define CC_Q_SIZE         0x18
#define CC_Q_MSIX_VEC     0x1a
#define CC_Q_ENABLE       0x1c
#define CC_Q_NOTIFY_OFF   0x1e
#define CC_Q_DESC         0x20
#define CC_Q_DRIVER       0x28
#define CC_Q_DEVICE       0x30

#define VIRTIO_F_VERSION_1  32       /* feature bit; lives in feature word 1 */
#define NO_VECTOR           0xffff

/* Descriptor flags (spec 2.6.5). */
#define VRING_DESC_F_NEXT   1        /* buffer continues in `next`           */
#define VRING_DESC_F_WRITE  2        /* device writes it (device-writable)   */

/* virtio-blk request types + status (spec 5.2.6). */
#define VIRTIO_BLK_T_IN     0        /* read from device                     */
#define VIRTIO_BLK_T_OUT    1        /* write to device                      */
#define VIRTIO_BLK_S_OK     0

#define QSZ 8                        /* our fixed queue size (<= device max) */
#define POLL_BUDGET 4000000

/* MMIO pages for the BARs live above the boot 1 GiB identity map, so we map
   them uncached (PCD) at virt==phys, same trick as the framebuffer/TPM. */
#define PTE_PCD (1ULL << 4)

/* --- split virtqueue structures (packed, LE = native on x86) ------------- */
typedef struct ROGEX_PACKED {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vq_desc;

typedef struct ROGEX_PACKED {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSZ];
    uint16_t used_event;
} vq_avail;

typedef struct ROGEX_PACKED {
    uint32_t id;
    uint32_t len;
} vq_used_elem;

typedef struct ROGEX_PACKED {
    uint16_t     flags;
    uint16_t     idx;
    vq_used_elem ring[QSZ];
    uint16_t     avail_event;
} vq_used;

typedef struct ROGEX_PACKED {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_hdr;

/* --- driver state -------------------------------------------------------- */
static volatile uint8_t *g_common;      /* common cfg MMIO base            */
static volatile uint8_t *g_notify;      /* notify cfg MMIO base            */
static volatile uint8_t *g_device;      /* device cfg MMIO base            */
static uint32_t          g_notify_mult; /* queue_notify_off multiplier     */
static uint16_t          g_notify_off;  /* per-queue notify offset          */

static volatile vq_desc  *g_desc;
static volatile vq_avail *g_avail;
static volatile vq_used  *g_used;
static uint16_t           g_last_used;

static bool        g_present;
static uint32_t    g_capacity;          /* device size in 512 B sectors     */
static blockdev_t  g_vda;

/* Single in-flight request scratch (polled, single-threaded). */
static virtio_blk_req_hdr g_hdr;
static uint8_t            g_status;

/* --- small MMIO helpers -------------------------------------------------- */
static inline void mfence(void) { __asm__ volatile ("mfence" ::: "memory"); }

static inline uint8_t  cc_r8 (uint32_t o)            { return *(volatile uint8_t  *)(g_common + o); }
static inline uint16_t cc_r16(uint32_t o)            { return *(volatile uint16_t *)(g_common + o); }
static inline uint32_t cc_r32(uint32_t o)            { return *(volatile uint32_t *)(g_common + o); }
static inline void     cc_w8 (uint32_t o, uint8_t v) { *(volatile uint8_t  *)(g_common + o) = v; }
static inline void     cc_w16(uint32_t o, uint16_t v){ *(volatile uint16_t *)(g_common + o) = v; }
static inline void     cc_w32(uint32_t o, uint32_t v){ *(volatile uint32_t *)(g_common + o) = v; }
static inline void     cc_w64(uint32_t o, uint64_t v){ *(volatile uint64_t *)(g_common + o) = v; }

/* Map [phys, phys+len) uncached at virt==phys so MMIO/DMA pointers are the
   same value the device sees. */
static void map_mmio(uint64_t phys, uint64_t len) {
    uint64_t start = phys & ~0xfffULL;
    uint64_t end   = (phys + len + 0xfff) & ~0xfffULL;
    for (uint64_t p = start; p < end; p += 0x1000)
        vmm_map_page(p, p, PTE_WRITABLE | PTE_PCD);
}

/* --- capability discovery ------------------------------------------------ */
/* Walk the vendor caps, and for the one whose cfg_type matches, map its BAR
   region and return the mapped base (offset applied). *mult_out (optional)
   receives notify_off_multiplier for the notify cap. Returns NULL if absent. */
static volatile uint8_t *find_virtio_cfg(pci_addr_t a, uint8_t want_type,
                                         uint32_t *mult_out) {
    uint8_t cap = pci_find_cap(a, 0x09, 0);        /* 0x09 = vendor-specific */
    for (int guard = 0; cap && guard < 32; guard++) {
        uint8_t cfg_type = pci_read8(a, (uint8_t)(cap + 3));
        uint8_t bar      = pci_read8(a, (uint8_t)(cap + 4));
        uint32_t offset  = pci_read32(a, (uint8_t)(cap + 8));
        uint32_t length  = pci_read32(a, (uint8_t)(cap + 12));
        if (cfg_type == want_type) {
            uint64_t base = pci_bar_mem_base(a, bar);
            if (!base) return NULL;                /* I/O BAR / unassigned  */
            map_mmio(base + offset, length ? length : 0x1000);
            if (mult_out) *mult_out = pci_read32(a, (uint8_t)(cap + 16));
            return (volatile uint8_t *)(uintptr_t)(base + offset);
        }
        cap = pci_find_cap(a, 0x09, cap);          /* next vendor cap       */
    }
    return NULL;
}

/* --- virtqueue setup ----------------------------------------------------- */
static bool setup_queue0(void) {
    cc_w16(CC_Q_SELECT, 0);
    uint16_t max = cc_r16(CC_Q_SIZE);
    if (max == 0) return false;                    /* queue unavailable     */
    if (max < QSZ) return false;                   /* our layout needs >= 8 */
    cc_w16(CC_Q_SIZE, QSZ);                        /* shrink to our size    */
    /* Verify the device honored it: if it kept a larger size it would index
       avail/used rings past our 8-entry arrays and corrupt memory. */
    if (cc_r16(CC_Q_SIZE) != QSZ) return false;
    cc_w16(CC_Q_MSIX_VEC, NO_VECTOR);              /* we poll, no MSI-X      */

    /* One zeroed 4 KiB frame holds desc/avail/used, physically contiguous
       and (being < 1 GiB RAM) identity-mapped, so phys == our pointer. */
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return false;
    g_desc  = (volatile vq_desc  *)(uintptr_t)(frame + 0);
    g_avail = (volatile vq_avail *)(uintptr_t)(frame + 128);   /* after 8*16 */
    g_used  = (volatile vq_used  *)(uintptr_t)(frame + 256);   /* 4-aligned  */

    cc_w64(CC_Q_DESC,   frame + 0);
    cc_w64(CC_Q_DRIVER, frame + 128);
    cc_w64(CC_Q_DEVICE, frame + 256);
    g_notify_off = cc_r16(CC_Q_NOTIFY_OFF);
    cc_w16(CC_Q_ENABLE, 1);
    g_last_used = 0;
    return true;
}

/* Kick the device: write the queue index to its notify address (spec 4.1.4.4:
   notify_addr = notify_base + queue_notify_off * notify_off_multiplier). */
static void notify_queue0(void) {
    volatile uint16_t *n = (volatile uint16_t *)(g_notify + (uint32_t)g_notify_off * g_notify_mult);
    *n = 0;
}

/* --- one synchronous request (polled) ------------------------------------ */
static bool vblk_request(uint32_t type, uint32_t sector, void *buf, uint32_t count) {
    if (!g_present) return false;
    uint32_t bytes = count * BLOCKDEV_SECTOR;

    /* DMA guard: the device reads/writes physical memory directly, and we
       rely on virt==phys, which only holds inside the 1 GiB identity map. */
    uint64_t bp = (uint64_t)(uintptr_t)buf;
    if (bp + bytes > (1ULL << 30)) { klog_error("virtio-blk: buffer above identity map"); return false; }

    g_hdr.type = type; g_hdr.reserved = 0; g_hdr.sector = sector;
    g_status = 0xff;

    bool dev_writes_data = (type == VIRTIO_BLK_T_IN);
    g_desc[0].addr = (uint64_t)(uintptr_t)&g_hdr;
    g_desc[0].len  = sizeof(g_hdr);
    g_desc[0].flags = VRING_DESC_F_NEXT;           /* device reads the header */
    g_desc[0].next  = 1;
    g_desc[1].addr = bp;
    g_desc[1].len  = bytes;
    g_desc[1].flags = (uint16_t)((dev_writes_data ? VRING_DESC_F_WRITE : 0) | VRING_DESC_F_NEXT);
    g_desc[1].next  = 2;
    g_desc[2].addr = (uint64_t)(uintptr_t)&g_status;
    g_desc[2].len  = 1;
    g_desc[2].flags = VRING_DESC_F_WRITE;          /* device writes status    */
    g_desc[2].next  = 0;

    /* Publish the head into the available ring, then bump idx. The mfence
       between them ensures the device never sees the new idx before the ring
       slot that idx refers to (spec 2.6.13 ordering). */
    g_avail->ring[g_avail->idx % QSZ] = 0;
    mfence();
    g_avail->idx = (uint16_t)(g_avail->idx + 1);
    mfence();
    notify_queue0();

    /* Poll the used ring for completion, bounded so a broken device fails
       instead of hanging the boot. */
    for (int i = 0; i < POLL_BUDGET; i++) {
        if (g_used->idx != g_last_used) {
            mfence();                              /* order used-read after idx */
            g_last_used = g_used->idx;
            return g_status == VIRTIO_BLK_S_OK;
        }
        sched_poll_relax(i);          /* cooperative: yield during transfers */
    }
    klog_error("virtio-blk: request timed out");
    return false;
}

/* --- blockdev vector ----------------------------------------------------- */
/* A request always uses exactly 3 descriptors (header/data/status); `count`
   sectors travel as ONE contiguous data descriptor, so the queue size does
   NOT bound count. The cap here just rejects nonsense; the real constraint is
   the identity-map guard inside vblk_request. */
#define VBLK_MAX_SECTORS 256

static bool vblk_read(blockdev_t *d, uint32_t lba, void *buf, uint32_t count) {
    (void)d;
    if (count == 0 || count > VBLK_MAX_SECTORS || lba + count > g_capacity) return false;
    return vblk_request(VIRTIO_BLK_T_IN, lba, buf, count);
}
static bool vblk_write(blockdev_t *d, uint32_t lba, const void *buf, uint32_t count) {
    (void)d;
    if (count == 0 || count > VBLK_MAX_SECTORS || lba + count > g_capacity) return false;
    return vblk_request(VIRTIO_BLK_T_OUT, lba, (void *)buf, count);
}

/* --- bring-up ------------------------------------------------------------ */
bool virtio_blk_init(void) {
    g_present = false;

    pci_addr_t a;
    if (!pci_find(VIRTIO_VENDOR, VIRTIO_BLK_MODERN, &a) &&
        !pci_find(VIRTIO_VENDOR, VIRTIO_BLK_TRANS,  &a))
        return false;                              /* no virtio-blk on the bus */

    pci_enable_mem_and_dma(a);

    g_common = find_virtio_cfg(a, VIRTIO_PCI_CAP_COMMON_CFG, NULL);
    g_notify = find_virtio_cfg(a, VIRTIO_PCI_CAP_NOTIFY_CFG, &g_notify_mult);
    g_device = find_virtio_cfg(a, VIRTIO_PCI_CAP_DEVICE_CFG, NULL);
    if (!g_common || !g_notify) return false;      /* not a modern device    */

    /* Device-status handshake (spec 3.1.1). */
    cc_w8(CC_DEV_STATUS, 0);                        /* reset                  */
    for (int i = 0; cc_r8(CC_DEV_STATUS) != 0; i++) {   /* bounded: no hang  */
        if (i > 1000000) return false;
        __asm__ volatile ("pause");
    }
    cc_w8(CC_DEV_STATUS, ST_ACK);
    cc_w8(CC_DEV_STATUS, ST_ACK | ST_DRIVER);

    /* Require VIRTIO_F_VERSION_1 (bit 32 -> feature word 1, bit 0). A modern
       device offers it; setting it is what selects the non-legacy layout. */
    cc_w32(CC_DEV_FEAT_SEL, 1);
    if (!(cc_r32(CC_DEV_FEAT) & (1u << (VIRTIO_F_VERSION_1 - 32)))) {
        cc_w8(CC_DEV_STATUS, ST_FAILED);
        return false;
    }
    cc_w32(CC_DRV_FEAT_SEL, 0); cc_w32(CC_DRV_FEAT, 0);          /* no word-0 features */
    cc_w32(CC_DRV_FEAT_SEL, 1); cc_w32(CC_DRV_FEAT, 1u << (VIRTIO_F_VERSION_1 - 32));

    cc_w8(CC_DEV_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(cc_r8(CC_DEV_STATUS) & ST_FEATURES_OK)) {  /* device rejected subset */
        cc_w8(CC_DEV_STATUS, ST_FAILED);
        return false;
    }

    cc_w16(CC_MSIX_CONFIG, NO_VECTOR);
    if (!setup_queue0()) { cc_w8(CC_DEV_STATUS, ST_FAILED); return false; }

    /* Capacity: virtio_blk_config.capacity (le64 sectors) at device cfg +0. */
    if (g_device) {
        uint64_t cap = *(volatile uint64_t *)g_device;
        g_capacity = cap > 0xffffffffULL ? 0xffffffffu : (uint32_t)cap;
    }
    if (g_capacity == 0) { cc_w8(CC_DEV_STATUS, ST_FAILED); return false; }

    cc_w8(CC_DEV_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    /* Polled I/O only — mask INTx so a shared line with virtio-net cannot
       storm the PIC (q35 puts both virtio functions on the same pin). */
    {
        uint16_t cmd = pci_read16(a, 0x04);
        pci_write16(a, 0x04, (uint16_t)(cmd | (1u << 10)));
    }

    g_vda.name    = "vda";
    g_vda.sectors = g_capacity;
    g_vda.read    = vblk_read;
    g_vda.write   = vblk_write;
    g_vda.ctx     = NULL;
    g_present     = blockdev_register(&g_vda);
    return g_present;
}

bool virtio_blk_present(void)    { return g_present; }
blockdev_t *virtio_blk_dev(void) { return g_present ? &g_vda : NULL; }
