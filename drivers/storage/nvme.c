#include "nvme.h"
#include "pci.h"
#include "pit.h"
#include "rogex.h"

/* --- controller registers (NVMe 1.4 section 3.1) -------------------------- */
#define NVME_REG_CAP      0x00   /* 64: capabilities                        */
#define NVME_REG_VS       0x08   /* 32: version                             */
#define NVME_REG_INTMS    0x0C
#define NVME_REG_INTMC    0x10
#define NVME_REG_CC       0x14   /* 32: controller configuration            */
#define NVME_REG_CSTS     0x1C   /* 32: controller status                   */
#define NVME_REG_AQA      0x24   /* 32: admin queue attributes              */
#define NVME_REG_ASQ      0x28   /* 64: admin submission queue base         */
#define NVME_REG_ACQ      0x30   /* 64: admin completion queue base         */
#define NVME_REG_SQ0TDBL  0x1000 /* doorbells start here                    */

#define CC_EN        (1u << 0)
#define CC_CSS_NVM   (0u << 4)
#define CC_MPS_4K    (0u << 7)
#define CC_AMS_RR    (0u << 11)
#define CC_SHN_NONE  (0u << 14)
#define CC_IOSQES_64 (6u << 16)  /* 2^6 = 64 B submission entry             */
#define CC_IOCQES_16 (4u << 20)  /* 2^4 = 16 B completion entry             */

#define CSTS_RDY  (1u << 0)
#define CSTS_CFS  (1u << 1)      /* controller fatal status                 */

/* Admin opcodes */
#define ADM_DELETE_SQ 0x00
#define ADM_CREATE_SQ 0x01
#define ADM_DELETE_CQ 0x04
#define ADM_CREATE_CQ 0x05
#define ADM_IDENTIFY  0x06
/* NVM command set opcodes */
#define NVM_WRITE     0x01
#define NVM_READ      0x02

#define QUEUE_DEPTH   8          /* entries per queue; we keep one in flight */
#define NVME_TIMEOUT_MS 5000     /* per command; CAP.TO is a 500 ms unit     */
#define PTE_PCD (1ULL << 4)      /* uncached: these are device registers     */

/* A submission entry is 64 bytes of dwords; naming the ones we set keeps the
   command builders readable without a struct per opcode. */
typedef struct ROGEX_PACKED {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_sqe_t;

typedef struct ROGEX_PACKED {
    uint32_t dw0, dw1;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    uint16_t status;             /* phase in bit 0, status code above it    */
} nvme_cqe_t;

_Static_assert(sizeof(nvme_sqe_t) == 64, "NVMe submission entry is 64 B");
_Static_assert(sizeof(nvme_cqe_t) == 16, "NVMe completion entry is 16 B");

typedef struct {
    volatile nvme_sqe_t *sq;
    volatile nvme_cqe_t *cq;
    uint16_t sq_tail, cq_head;
    uint8_t  phase;              /* expected phase bit for the next entry   */
    uint16_t id;
} nvme_queue_t;

static volatile uint8_t *g_regs;
static uint32_t          g_db_stride;    /* doorbell stride in bytes        */
static nvme_queue_t      g_admin, g_io;
static bool              g_present;
static const char       *g_why = "not probed";
static char              g_status_line[128];
static char              g_model[41];
static uint64_t          g_ns_blocks;
static uint32_t          g_block_bytes = 512;
static uint32_t          g_nsid = 1;
static uint16_t          g_cid;
static uint32_t          g_completions, g_errors, g_timeouts, g_last_err;
static blockdev_t        g_dev;

/* DMA scratch: queues and one data page, all identity-mapped physical RAM. */
static uint64_t g_dma_base;
static uint8_t *g_data;                  /* 4 KiB bounce buffer             */

#define ADMIN_SQ_PHYS (g_dma_base + 0 * PAGE_SIZE)
#define ADMIN_CQ_PHYS (g_dma_base + 1 * PAGE_SIZE)
#define IO_SQ_PHYS    (g_dma_base + 2 * PAGE_SIZE)
#define IO_CQ_PHYS    (g_dma_base + 3 * PAGE_SIZE)

static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(g_regs + off); }
static inline void     w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_regs + off) = v; }
static inline uint64_t r64(uint32_t off) { return *(volatile uint64_t *)(g_regs + off); }
static inline void     w64(uint32_t off, uint64_t v) { *(volatile uint64_t *)(g_regs + off) = v; }
static inline void     mfence(void) { __asm__ volatile ("mfence" ::: "memory"); }

static void set_status(const char *what) {
    char *d = g_status_line;
    const char *e = g_status_line + sizeof(g_status_line) - 1;
    const char *p = what;
    while (*p && d < e) *d++ = *p++;
    *d = '\0';
    g_why = g_status_line;
}

/* Doorbells: SQ y at 0x1000 + (2y) * stride, CQ y at 0x1000 + (2y + 1). */
static void ring_sq(nvme_queue_t *q) {
    mfence();
    w32(NVME_REG_SQ0TDBL + (2u * q->id) * g_db_stride, q->sq_tail);
}
static void ring_cq(nvme_queue_t *q) {
    mfence();
    w32(NVME_REG_SQ0TDBL + (2u * q->id + 1u) * g_db_stride, q->cq_head);
}

/* Submit one command and wait for its completion. Returns the NVMe status
   field (0 = success), or 0xFFFF on timeout — a timeout is not a status code
   and must not be mistaken for one. */
static uint16_t submit_sync(nvme_queue_t *q, nvme_sqe_t *cmd) {
    uint64_t deadline;
    uint16_t status;

    cmd->cid = ++g_cid;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % QUEUE_DEPTH);
    ring_sq(q);

    deadline = pit_uptime_ms() + NVME_TIMEOUT_MS;
    for (;;) {
        volatile nvme_cqe_t *c = &q->cq[q->cq_head];
        if ((c->status & 1u) == q->phase) {
            status = (uint16_t)(c->status >> 1);
            q->cq_head = (uint16_t)((q->cq_head + 1) % QUEUE_DEPTH);
            if (q->cq_head == 0) q->phase ^= 1u;
            ring_cq(q);
            g_completions++;
            if (status) { g_errors++; g_last_err = status; }
            return status;
        }
        if (r32(NVME_REG_CSTS) & CSTS_CFS) {
            g_errors++;
            g_last_err = 0xFFFEu;
            return 0xFFFEu;                  /* controller fatal status      */
        }
        if (pit_uptime_ms() > deadline) {
            g_timeouts++;
            return 0xFFFFu;
        }
    }
}

static bool wait_ready(bool want, const char *what) {
    uint64_t deadline = pit_uptime_ms() + NVME_TIMEOUT_MS;
    while (pit_uptime_ms() <= deadline) {
        uint32_t csts = r32(NVME_REG_CSTS);
        if (csts & CSTS_CFS) { set_status(what); return false; }
        if (((csts & CSTS_RDY) != 0) == want) return true;
    }
    set_status(what);
    return false;
}

static void queue_init(nvme_queue_t *q, uint16_t id, uint64_t sq_phys, uint64_t cq_phys) {
    q->sq = (volatile nvme_sqe_t *)(uintptr_t)sq_phys;
    q->cq = (volatile nvme_cqe_t *)(uintptr_t)cq_phys;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;                    /* controller writes phase 1 first     */
    q->id = id;
    k_memset((void *)q->sq, 0, QUEUE_DEPTH * sizeof(nvme_sqe_t));
    k_memset((void *)q->cq, 0, QUEUE_DEPTH * sizeof(nvme_cqe_t));
}

static bool identify(uint32_t cns, uint32_t nsid, uint64_t buf_phys) {
    nvme_sqe_t cmd;
    uint16_t st;
    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADM_IDENTIFY;
    cmd.nsid   = nsid;
    cmd.prp1   = buf_phys;
    cmd.cdw10  = cns;
    st = submit_sync(&g_admin, &cmd);
    return st == 0;
}

static bool create_io_queues(uint64_t sq_phys, uint64_t cq_phys) {
    nvme_sqe_t cmd;
    uint16_t st;

    /* Completion queue first: the submission queue references it. */
    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADM_CREATE_CQ;
    cmd.prp1   = cq_phys;
    cmd.cdw10  = (uint32_t)1 | ((QUEUE_DEPTH - 1u) << 16);   /* qid 1, size  */
    cmd.cdw11  = 1;                                          /* physically contiguous */
    st = submit_sync(&g_admin, &cmd);
    if (st) { set_status("create I/O completion queue rejected"); return false; }

    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADM_CREATE_SQ;
    cmd.prp1   = sq_phys;
    cmd.cdw10  = (uint32_t)1 | ((QUEUE_DEPTH - 1u) << 16);
    cmd.cdw11  = 1u | (1u << 16);                            /* contiguous, cqid 1 */
    st = submit_sync(&g_admin, &cmd);
    if (st) { set_status("create I/O submission queue rejected"); return false; }
    return true;
}

/* --- block device -------------------------------------------------------- */
/* One 512-byte sector per command through a 4 KiB bounce page. Slow and
   obvious; correctness first, and the callers here are an installer and a
   filesystem that writes a sector at a time anyway. */
static bool nvme_rw_one(uint32_t lba, void *buf, bool write) {
    nvme_sqe_t cmd;
    uint16_t st;
    uint32_t blk_per_sec = g_block_bytes / BLOCKDEV_SECTOR;
    uint64_t nlba;

    if (!g_present || !blk_per_sec) return false;
    /* The device counts in its own block size; callers count in 512 B. */
    nlba = lba / blk_per_sec;
    if (g_block_bytes != BLOCKDEV_SECTOR) return false;   /* 4 Kn: unsupported */
    if (nlba >= g_ns_blocks) return false;

    if (write) k_memcpy(g_data, buf, BLOCKDEV_SECTOR);
    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = write ? NVM_WRITE : NVM_READ;
    cmd.nsid   = g_nsid;
    cmd.prp1   = (uint64_t)(uintptr_t)g_data;
    cmd.cdw10  = (uint32_t)nlba;
    cmd.cdw11  = (uint32_t)(nlba >> 32);
    cmd.cdw12  = 0;                       /* NLB is zero-based: one block    */
    st = submit_sync(&g_io, &cmd);
    if (st) return false;
    if (!write) k_memcpy(buf, g_data, BLOCKDEV_SECTOR);
    return true;
}

static bool dev_read(blockdev_t *dev, uint32_t lba, void *buf, uint32_t count) {
    uint8_t *p = (uint8_t *)buf;
    (void)dev;
    while (count--) {
        if (!nvme_rw_one(lba, p, false)) return false;
        p += BLOCKDEV_SECTOR;
        lba++;
    }
    return true;
}

static bool dev_write(blockdev_t *dev, uint32_t lba, const void *buf, uint32_t count) {
    const uint8_t *p = (const uint8_t *)buf;
    (void)dev;
    while (count--) {
        if (!nvme_rw_one(lba, (void *)(uintptr_t)p, true)) return false;
        p += BLOCKDEV_SECTOR;
        lba++;
    }
    return true;
}

/* --- init ---------------------------------------------------------------- */
bool nvme_init(void) {
    pci_addr_t a;
    uint64_t bar, cap;
    uint32_t cc;
    uint8_t *idbuf;

    g_present = false;
    g_model[0] = '\0';
    set_status("absent (no PCI class 01:08:02 controller)");

    /* NVMe is a class-coded controller: 01 mass storage, 08 NVM, 02 NVMe. */
    if (!pci_find_class(0x01, 0x08, 0x02, &a)) return false;

    pci_enable_mem_and_dma(a);
    bar = pci_bar_mem_base(a, 0);
    if (!bar) { set_status("controller found but BAR0 is unprogrammed"); return false; }

    /* Registers are device memory: map uncached, virt == phys. */
    for (uint64_t off = 0; off < 0x2000; off += PAGE_SIZE)
        if (!vmm_map_page(bar + off, bar + off, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) {
            set_status("BAR0 present but the VMM refused to map it");
            return false;
        }
    g_regs = (volatile uint8_t *)(uintptr_t)bar;

    cap = r64(NVME_REG_CAP);
    g_db_stride = 4u << ((uint32_t)(cap >> 32) & 0xFu);   /* CAP.DSTRD       */
    if (((cap >> 37) & 1u) == 0) {                        /* CAP.CSS bit 0   */
        set_status("controller does not support the NVM command set");
        return false;
    }

    /* Five pages, one structure each: admin SQ, admin CQ, I/O SQ, I/O CQ and
       a data bounce page. Every queue base has to be 4 KiB aligned — the low
       12 bits of ASQ/ACQ and of a PC=1 PRP1 are reserved, and a controller
       handed a half-page-aligned admin queue simply never sets CSTS.RDY. */
    g_dma_base = pmm_alloc_contig(5);
    if (!g_dma_base) { set_status("no contiguous DMA memory for the queues"); return false; }
    k_memset((void *)(uintptr_t)g_dma_base, 0, 5 * PAGE_SIZE);
    g_data = (uint8_t *)(uintptr_t)(g_dma_base + 4 * PAGE_SIZE);

    /* Reset: clear CC.EN and wait for CSTS.RDY to fall before touching AQA. */
    cc = r32(NVME_REG_CC);
    if (cc & CC_EN) {
        w32(NVME_REG_CC, cc & ~CC_EN);
        if (!wait_ready(false, "controller did not clear CSTS.RDY on reset")) return false;
    }

    queue_init(&g_admin, 0, ADMIN_SQ_PHYS, ADMIN_CQ_PHYS);
    w32(NVME_REG_AQA, (QUEUE_DEPTH - 1u) | ((QUEUE_DEPTH - 1u) << 16));
    w64(NVME_REG_ASQ, ADMIN_SQ_PHYS);
    w64(NVME_REG_ACQ, ADMIN_CQ_PHYS);

    w32(NVME_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR | CC_SHN_NONE |
                     CC_IOSQES_64 | CC_IOCQES_16);
    if (!wait_ready(true, "controller did not set CSTS.RDY after enable")) return false;

    /* Identify Controller (CNS 1): model string at bytes 24..63. */
    idbuf = g_data;
    k_memset(idbuf, 0, PAGE_SIZE);
    if (!identify(1, 0, (uint64_t)(uintptr_t)idbuf)) {
        set_status("Identify Controller failed");
        return false;
    }
    {
        int n = 0, i;
        for (i = 0; i < 40; i++) g_model[i] = (char)idbuf[24 + i];
        g_model[40] = '\0';
        for (i = 39; i >= 0 && (g_model[i] == ' ' || g_model[i] == '\0'); i--) g_model[i] = '\0';
        (void)n;
    }

    /* Identify Namespace (CNS 0) for nsid 1: size and block size. */
    k_memset(idbuf, 0, PAGE_SIZE);
    if (!identify(0, g_nsid, (uint64_t)(uintptr_t)idbuf)) {
        set_status("Identify Namespace failed");
        return false;
    }
    {
        uint64_t nsze = *(uint64_t *)idbuf;                 /* blocks         */
        uint8_t  flbas = idbuf[26] & 0x0F;                  /* formatted LBA  */
        uint32_t lbaf  = *(uint32_t *)(idbuf + 128 + 4 * flbas);
        uint8_t  lbads = (uint8_t)((lbaf >> 16) & 0xFF);
        g_ns_blocks = nsze;
        g_block_bytes = lbads ? (1u << lbads) : 512u;
    }
    if (g_ns_blocks == 0) { set_status("namespace 1 reports zero blocks"); return false; }
    if (g_block_bytes != BLOCKDEV_SECTOR) {
        set_status("namespace is not 512-byte formatted (4 Kn unsupported)");
        return false;
    }

    queue_init(&g_io, 1, IO_SQ_PHYS, IO_CQ_PHYS);
    if (!create_io_queues(IO_SQ_PHYS, IO_CQ_PHYS)) return false;

    g_dev.name    = "nvme0n1";
    g_dev.sectors = (uint32_t)(g_ns_blocks > 0xFFFFFFFFull ? 0xFFFFFFFFu : g_ns_blocks);
    g_dev.read    = dev_read;
    g_dev.write   = dev_write;
    g_dev.ctx     = NULL;
    if (!blockdev_register(&g_dev)) {
        set_status("block device table is full");
        return false;
    }

    g_present = true;
    {
        /* One line that names what was actually read from the device. */
        char *d = g_status_line;
        const char *e = g_status_line + sizeof(g_status_line) - 1;
        const char *p = g_model[0] ? g_model : "NVMe controller";
        uint64_t mib = ((uint64_t)g_dev.sectors * BLOCKDEV_SECTOR) / (1024 * 1024);
        char num[24];
        int n = 0;
        while (*p && d < e) *d++ = *p++;
        p = "  ns1 ";
        while (*p && d < e) *d++ = *p++;
        if (!mib) num[n++] = '0';
        while (mib) { num[n++] = (char)('0' + (mib % 10)); mib /= 10; }
        while (n > 0 && d < e) *d++ = num[--n];
        p = " MiB, 512 B blocks, 1 I/O queue (polled)";
        while (*p && d < e) *d++ = *p++;
        *d = '\0';
        g_why = g_status_line;
    }
    return true;
}

bool        nvme_present(void)     { return g_present; }
blockdev_t *nvme_dev(void)         { return g_present ? &g_dev : NULL; }
const char *nvme_status(void)      { return g_why; }
const char *nvme_model(void)       { return g_model; }
uint64_t    nvme_ns_blocks(void)   { return g_ns_blocks; }
uint32_t    nvme_block_bytes(void) { return g_block_bytes; }
uint32_t    nvme_completions(void) { return g_completions; }
uint32_t    nvme_errors(void)      { return g_errors; }
uint32_t    nvme_timeouts(void)    { return g_timeouts; }
uint32_t    nvme_last_error(void)  { return g_last_err; }
