/*
 * Multiboot2 boot transport for x86_64.
 *
 * GRUB hands over the same tag block under BIOS and under UEFI; which of the
 * two it was is deduced from the tags themselves (an EFI system table
 * pointer is present or it is not), never from a build flag. This file is
 * the only thing in the tree that knows what a Multiboot2 tag looks like.
 */
#include "handoff.h"

extern boot_handoff_t g_boot;

/* Multiboot2 tag numbers (spec 3.1). Only the ones we normalise are named. */
#define MB2_TAG_END          0
#define MB2_TAG_CMDLINE      1
#define MB2_TAG_MODULE       3
#define MB2_TAG_MMAP         6
#define MB2_TAG_FRAMEBUFFER  8
#define MB2_TAG_EFI64_ST    12   /* EFI 64-bit system table pointer         */
#define MB2_TAG_ACPI_V1     14   /* copy of the ACPI 1.0 RSDP               */
#define MB2_TAG_ACPI_V2     15   /* copy of the ACPI 2.0+ RSDP              */
#define MB2_TAG_EFI_BS      18   /* present = boot services NOT terminated  */
#define MB2_TAG_EFI64_IH    20   /* EFI 64-bit image handle                 */

#define MB2_BOOTLOADER_MAGIC 0x36d76289u
#define MB2_MEM_AVAILABLE    1
#define MB2_MEM_ACPI_RECLAIM 3
#define MB2_MEM_ACPI_NVS     4
#define MB2_MEM_BAD          5


static inline uint64_t align8(uint64_t x) { return (x + 7u) & ~7ull; }

static void copy_str(char *dst, const uint8_t *src, const uint8_t *lim, uint32_t max) {
    uint32_t n = 0;
    while (src < lim && *src && n + 1 < max) dst[n++] = (char)*src++;
    dst[n] = '\0';
}

static boot_mem_type_t mem_type_of(uint32_t raw) {
    switch (raw) {
    case MB2_MEM_AVAILABLE:    return BOOT_MEM_USABLE;
    case 2:                    return BOOT_MEM_RESERVED;
    case MB2_MEM_ACPI_RECLAIM: return BOOT_MEM_ACPI_RECLAIM;
    case MB2_MEM_ACPI_NVS:     return BOOT_MEM_ACPI_NVS;
    case MB2_MEM_BAD:          return BOOT_MEM_BAD;
    default:                   return BOOT_MEM_OTHER;
    }
}

void boot_handoff_init(uint64_t info, uint64_t magic) {
    uint8_t *base, *cur, *end;
    uint32_t total;

    k_memset(&g_boot, 0, sizeof(g_boot));
    g_boot.transport = "multiboot2";
    g_boot.firmware  = BOOT_FW_UNKNOWN;

    /* A wrong magic means the register does not point at a tag block. Parsing
       it anyway is how a boot turns into a triple fault with no message. */
    if (magic != MB2_BOOTLOADER_MAGIC || !info) return;

    base  = (uint8_t *)(uintptr_t)info;
    total = *(uint32_t *)base;
    if (total < 8) return;
    cur = base + 8;
    end = base + total;
    g_boot.info_start = info;
    g_boot.info_end   = info + total;

    while (cur + 8 <= end) {
        uint32_t type = *(uint32_t *)cur;
        uint32_t size = *(uint32_t *)(cur + 4);
        if (type == MB2_TAG_END || size < 8) break;

        switch (type) {
        case MB2_TAG_CMDLINE:
            copy_str(g_boot.cmdline, cur + 8, cur + size, BOOT_CMDLINE_MAX);
            break;

        case MB2_TAG_MODULE:
            if (size >= 16 && g_boot.module_count < BOOT_MODULE_MAX) {
                boot_module_t *m = &g_boot.module[g_boot.module_count];
                m->start = *(uint32_t *)(cur + 8);
                m->end   = *(uint32_t *)(cur + 12);
                copy_str(m->name, cur + 16, cur + size, BOOT_MODNAME_MAX);
                if (m->end > m->start) g_boot.module_count++;
            }
            break;

        case MB2_TAG_MMAP:
            if (size >= 16) {
                uint32_t esize = *(uint32_t *)(cur + 8);
                uint8_t *e = cur + 16, *lim = cur + size;
                if (esize < 24) break;
                for (; e + esize <= lim; e += esize) {
                    uint32_t raw = *(uint32_t *)(e + 16);
                    g_boot.mem_reported++;
                    if (g_boot.mem_count >= BOOT_MEM_MAX) continue;
                    boot_mem_range_t *r = &g_boot.mem[g_boot.mem_count++];
                    r->base     = *(uint64_t *)e;
                    r->length   = *(uint64_t *)(e + 8);
                    r->raw_type = raw;
                    r->type     = mem_type_of(raw);
                }
            }
            break;

        case MB2_TAG_FRAMEBUFFER:
            if (size >= 32) {
                g_boot.fb_addr   = *(uint64_t *)(cur + 8);
                g_boot.fb_pitch  = *(uint32_t *)(cur + 16);
                g_boot.fb_width  = *(uint32_t *)(cur + 20);
                g_boot.fb_height = *(uint32_t *)(cur + 24);
                g_boot.fb_bpp    = *(uint8_t  *)(cur + 28);
                g_boot.fb_type   = *(uint8_t  *)(cur + 29);
                g_boot.fb_present = g_boot.fb_addr != 0 && g_boot.fb_bpp != 0;
            }
            break;

        /* The loader copies the RSDP into the tag, so this is the pointer to
           use. Under UEFI there is no BIOS area to scan for one. */
        case MB2_TAG_ACPI_V1:
            if (size > 8 && g_boot.acpi_revision < 1) {
                g_boot.acpi_rsdp = (uint64_t)(uintptr_t)(cur + 8);
                g_boot.acpi_revision = 1;
            }
            break;
        case MB2_TAG_ACPI_V2:
            if (size > 8) {
                g_boot.acpi_rsdp = (uint64_t)(uintptr_t)(cur + 8);
                g_boot.acpi_revision = 2;
            }
            break;

        case MB2_TAG_EFI64_ST:
            if (size >= 16) {
                g_boot.fw_config_table = *(uint64_t *)(cur + 8);
                g_boot.firmware = BOOT_FW_UEFI;
            }
            break;
        case MB2_TAG_EFI64_IH:
            if (size >= 16) g_boot.fw_image_handle = *(uint64_t *)(cur + 8);
            break;
        case MB2_TAG_EFI_BS:
            g_boot.fw_services_active = true;
            break;

        default:
            break;
        }
        cur += align8(size);
    }

    /* No EFI system table means nobody handed us one: legacy BIOS path. */
    if (g_boot.firmware == BOOT_FW_UNKNOWN) g_boot.firmware = BOOT_FW_BIOS;
    g_boot.valid = true;
}

