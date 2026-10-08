/* Device-tree boot transport for AArch64.
 *
 * QEMU's -kernel hands us a flattened device tree in x0. This reads the one
 * thing the kernel cannot start without — where RAM is — and fills the same
 * handoff struct the Multiboot2 transport fills on x86. Everything above it
 * reads that struct and has no idea which transport ran.
 *
 * It is a reader, not a parser: it walks the structure block looking for a
 * /memory node and its `reg`, and ignores everything else. When there is no
 * usable DTB it falls back to what QEMU virt always has, and says so, rather
 * than booting with a memory map of zero bytes.
 */
#include "handoff.h"

extern boot_handoff_t g_boot;
extern uint8_t _kernel_end[];

#define FDT_MAGIC       0xD00DFEEDu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

/* The UEFI stub's handoff. Under UEFI there may be no device tree at all —
   ArmVirtQemu publishes ACPI — so the memory map comes from the firmware's
   own, which the stub captured before leaving boot services. */
#define ECHOS_EFI_MAGIC 0x4543484F53454649ull
#define EFI_CONVENTIONAL_MEMORY 7

typedef struct {
    uint64_t magic;
    uint64_t map;           /* EFI_MEMORY_DESCRIPTOR array                  */
    uint64_t bytes;         /* its length                                   */
    uint64_t desc_size;     /* stride, which is not sizeof the struct       */
    uint64_t dtb;           /* if the firmware did publish one              */
} efi_handoff_t;

/* Only EfiConventionalMemory is taken. Loader and boot-services regions are
   reclaimable in principle, but this image is *in* one of them and the code
   deciding that is running from it. */
static uint32_t parse_efi_map(const efi_handoff_t *h) {
    uint32_t n = 0;
    uint64_t off;
    if (!h->map || !h->desc_size || h->desc_size < 40) return 0;
    for (off = 0; off + h->desc_size <= h->bytes; off += h->desc_size) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)(h->map + off);
        uint32_t type = *(const uint32_t *)d;
        uint64_t base = *(const uint64_t *)(d + 8);
        uint64_t pages = *(const uint64_t *)(d + 24);
        if (type != EFI_CONVENTIONAL_MEMORY || !pages) continue;
        if (n >= BOOT_MEM_MAX) break;
        g_boot.mem[n].base = base;
        g_boot.mem[n].length = pages * 4096ull;
        g_boot.mem[n].type = BOOT_MEM_USABLE;
        g_boot.mem[n].raw_type = type;
        n++;
    }
    return n;
}

/* QEMU virt: RAM starts here and the DTB says how much. */
#define VIRT_RAM_BASE   0x40000000ull
#define VIRT_RAM_FALLBK (512ull * 1024 * 1024)

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}
static bool starts_with(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return false; }
    return true;
}

static bool parse_memory(uint64_t dtb, uint64_t *base, uint64_t *size) {
    const uint8_t *fdt = (const uint8_t *)(uintptr_t)dtb;
    const uint8_t *structs, *strings, *p;
    uint32_t total, off_struct, off_strings, size_struct;
    bool in_memory = false;

    if (!dtb || be32(fdt) != FDT_MAGIC) return false;
    total       = be32(fdt + 4);
    off_struct  = be32(fdt + 8);
    off_strings = be32(fdt + 12);
    size_struct = be32(fdt + 36);
    if (!total || off_struct >= total || off_strings >= total) return false;

    structs = fdt + off_struct;
    strings = fdt + off_strings;
    p = structs;

    while (p < structs + size_struct) {
        uint32_t token = be32(p);
        p += 4;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            in_memory = starts_with(name, "memory");
            while (*p) p++;                       /* skip the name          */
            p = (const uint8_t *)(((uintptr_t)p + 4) & ~3ull);
        } else if (token == FDT_PROP) {
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4);
            const uint8_t *val = p + 8;
            const char *pname = (const char *)(strings + nameoff);
            if (in_memory && starts_with(pname, "reg") && len >= 16) {
                *base = be64(val);
                *size = be64(val + 8);
                return true;
            }
            p = val + ((len + 3) & ~3u);
        } else if (token == FDT_END_NODE) {
            in_memory = false;
        } else if (token == FDT_END) {
            break;
        } else if (token != FDT_NOP) {
            break;                                 /* not a tree we know     */
        }
    }
    return false;
}

void boot_handoff_init(uint64_t info, uint64_t magic) {
    uint64_t base = 0, size = 0;
    bool from_dtb;

    k_memset(&g_boot, 0, sizeof(g_boot));
    g_boot.transport = "device-tree";
    g_boot.firmware  = BOOT_FW_UNKNOWN;

    if (magic == ECHOS_EFI_MAGIC && info) {
        const efi_handoff_t *h = (const efi_handoff_t *)(uintptr_t)info;
        uint32_t n = parse_efi_map(h);
        if (n) {
            g_boot.transport = "UEFI memory map";
            g_boot.firmware = BOOT_FW_UEFI;
            g_boot.mem_count = g_boot.mem_reported = n;
            g_boot.info_start = h->map;
            g_boot.info_end = h->map + h->bytes;
            g_boot.fb_present = false;
            g_boot.acpi_rsdp = 0;
            g_boot.module_count = 0;
            g_boot.cmdline[0] = '\0';
            g_boot.valid = true;
            return;
        }
        /* A UEFI boot whose map we could not read is not a device-tree boot;
           say which transport failed rather than falling through silently. */
        g_boot.transport = "UEFI (memory map unreadable; assuming QEMU virt)";
        g_boot.firmware = BOOT_FW_UEFI;
        info = h->dtb;
    }

    from_dtb = parse_memory(info, &base, &size);
    if (!from_dtb || !size) {
        base = VIRT_RAM_BASE;
        size = VIRT_RAM_FALLBK;
        g_boot.transport = "device-tree (unreadable; assuming QEMU virt)";
    }

    g_boot.mem_count = g_boot.mem_reported = 1;
    g_boot.mem[0].base = base;
    g_boot.mem[0].length = size;
    g_boot.mem[0].type = BOOT_MEM_USABLE;
    g_boot.mem[0].raw_type = 1;

    /* The DTB itself sits in that RAM and is still being read after this, so
       it has to be protected from the allocator the same way the Multiboot2
       block is on x86. */
    g_boot.info_start = info;
    g_boot.info_end = info ? info + 0x100000ull : 0;   /* 1 MiB is generous  */

    g_boot.fb_present = false;          /* no GOP yet: the console is serial */
    g_boot.acpi_rsdp = 0;               /* ARM uses the DTB, not ACPI, here  */
    g_boot.module_count = 0;
    g_boot.cmdline[0] = '\0';
    g_boot.valid = true;
}
