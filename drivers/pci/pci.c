/* PCI config space, BARs, capabilities, bus-master.
   Walks every bus the firmware programmed (PCI bridges, class 06:04).
   q35 and real laptops put NICs behind a root port — bus 0 is not enough. */
#include "pci.h"
#include "ports.h"

#define PCI_ADDR 0xcf8
#define PCI_DATA 0xcfc

static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static uint32_t cfg_addr(pci_addr_t a, uint8_t off) {
    return 0x80000000u | ((uint32_t)a.bus << 16) | ((uint32_t)(a.dev & 0x1f) << 11)
                       | ((uint32_t)(a.fn & 0x07) << 8) | (off & 0xfc);
}

uint32_t pci_read32(pci_addr_t a, uint8_t off) {
    outl(PCI_ADDR, cfg_addr(a, off));
    return inl(PCI_DATA);
}
uint16_t pci_read16(pci_addr_t a, uint8_t off) {
    return (uint16_t)(pci_read32(a, off) >> ((off & 2) * 8));
}
uint8_t pci_read8(pci_addr_t a, uint8_t off) {
    return (uint8_t)(pci_read32(a, off) >> ((off & 3) * 8));
}
void pci_write32(pci_addr_t a, uint8_t off, uint32_t v) {
    outl(PCI_ADDR, cfg_addr(a, off));
    outl(PCI_DATA, v);
}
void pci_write16(pci_addr_t a, uint8_t off, uint16_t v) {
    uint32_t cur = pci_read32(a, off);
    int shift = (off & 2) * 8;
    cur = (cur & ~(0xffffu << shift)) | ((uint32_t)v << shift);
    pci_write32(a, off, cur);
}

typedef struct { uint16_t vendor, device; pci_addr_t *out; bool found; } pci_id_seek;
typedef struct { uint8_t cls, subcls; int progif; pci_addr_t *out; bool found; } pci_cls_seek;

static bool seek_id(pci_addr_t a, uint8_t cls, uint8_t sub, void *user) {
    pci_id_seek *s = user;
    (void)cls; (void)sub;
    if (a.vendor == s->vendor && a.device == s->device) {
        *s->out = a;
        s->found = true;
        return false;
    }
    return true;
}

static bool seek_cls(pci_addr_t a, uint8_t cls, uint8_t sub, void *user) {
    pci_cls_seek *s = user;
    uint32_t rc;
    uint8_t pi;
    if (cls != s->cls || sub != s->subcls) return true;
    rc = pci_read32(a, 0x08);
    pi = (uint8_t)(rc >> 8);
    if (s->progif >= 0 && pi != (uint8_t)s->progif) return true;
    *s->out = a;
    s->found = true;
    return false;
}

bool pci_find(uint16_t vendor, uint16_t device, pci_addr_t *out) {
    pci_id_seek s = { vendor, device, out, false };
    if (!out) return false;
    pci_foreach(seek_id, &s);
    return s.found;
}

/* Find by class/subclass/prog-if instead of vendor:device. Storage and network
   controllers are defined by what they implement, not by who made them: every
   AHCI controller answers 01:06:01 regardless of whether it is the Intel
   Wildcat Point in a 2015 laptop or QEMU's ich9-ahci, and tabulating device
   IDs would mean the driver only works on hardware we happened to list. */
bool pci_find_class(uint8_t cls, uint8_t subcls, int progif, pci_addr_t *out) {
    pci_cls_seek s = { cls, subcls, progif, out, false };
    if (!out) return false;
    pci_foreach(seek_cls, &s);
    return s.found;
}

uint64_t pci_bar_mem_base(pci_addr_t a, int bar) {
    if (bar < 0 || bar > 5) return 0;
    uint8_t off = (uint8_t)(0x10 + bar * 4);
    uint32_t lo = pci_read32(a, off);
    if (lo & 0x1) return 0;                        /* I/O BAR — not ours   */
    uint64_t base = lo & ~0xfULL;
    if (((lo >> 1) & 0x3) == 0x2) {                /* 64-bit memory BAR    */
        if (bar > 4) return 0;
        base |= (uint64_t)pci_read32(a, (uint8_t)(off + 4)) << 32;
    }
    return base;
}

void pci_enable_mem_and_dma(pci_addr_t a) {
    uint16_t cmd = pci_read16(a, 0x04);
    cmd |= 0x0002 | 0x0004;        /* bit1 memory space, bit2 bus master */
    pci_write16(a, 0x04, cmd);
}

uint16_t pci_bar_io_base(pci_addr_t a, int bar) {
    if (bar < 0 || bar > 5) return 0;
    uint8_t off = (uint8_t)(0x10 + bar * 4);
    uint32_t lo = pci_read32(a, off);
    if (!(lo & 0x1)) return 0;
    return (uint16_t)(lo & ~0x3u);
}

void pci_enable_io_and_dma(pci_addr_t a) {
    uint16_t cmd = pci_read16(a, 0x04);
    cmd |= 0x0001 | 0x0004;        /* bit0 I/O space, bit2 bus master */
    pci_write16(a, 0x04, cmd);
}

/* Recurse into PCI-PCI bridges. `seen` is a 256-bit bus bitmap. */
static int scan_bus(uint8_t bus, pci_visit_fn fn, void *user, uint8_t *seen) {
    int n = 0;
    uint8_t dev, fn_i;
    if (seen[bus >> 3] & (uint8_t)(1u << (bus & 7))) return 0;
    seen[bus >> 3] |= (uint8_t)(1u << (bus & 7));

    for (dev = 0; dev < 32; dev++) {
        for (fn_i = 0; fn_i < 8; fn_i++) {
            pci_addr_t a = { bus, dev, fn_i, 0, 0 };
            uint32_t id = pci_read32(a, 0x00);
            uint32_t rc;
            uint8_t c, sc, hdr, sec;
            if ((id & 0xffff) == 0xffff) {
                if (fn_i == 0) break;
                continue;
            }
            a.vendor = (uint16_t)(id & 0xffff);
            a.device = (uint16_t)(id >> 16);
            rc = pci_read32(a, 0x08);
            c  = (uint8_t)(rc >> 24);
            sc = (uint8_t)(rc >> 16);
            n++;
            if (fn && !fn(a, c, sc, user)) return n;

            hdr = pci_read8(a, 0x0e);
            /* Type-1 bridge: class 06:04, secondary bus at 0x19. */
            if ((hdr & 0x7f) == 0x01 || (c == 0x06 && sc == 0x04)) {
                sec = pci_read8(a, 0x19);
                if (sec && sec != bus)
                    n += scan_bus(sec, fn, user, seen);
            }
            if (fn_i == 0 && !(hdr & 0x80)) break;
        }
    }
    return n;
}

int pci_foreach(pci_visit_fn fn, void *user) {
    uint8_t seen[32];
    int i;
    for (i = 0; i < 32; i++) seen[i] = 0;
    return scan_bus(0, fn, user, seen);
}

uint8_t pci_find_cap(pci_addr_t a, uint8_t cap_id, uint8_t after) {
    if (!(pci_read16(a, 0x06) & 0x0010)) return 0;   /* no cap list       */
    uint8_t off = pci_read8(a, 0x34) & 0xfc;
    bool skipping = after != 0;
    for (int guard = 0; off && guard < 48; guard++) {  /* bounded walk    */
        uint8_t id = pci_read8(a, off);
        if (id == cap_id) {
            if (!skipping) return off;
            if (off == after) skipping = false;      /* resume after this */
        }
        off = pci_read8(a, (uint8_t)(off + 1)) & 0xfc;
    }
    return 0;
}
