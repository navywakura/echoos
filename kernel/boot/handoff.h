/*
 * Firmware-independent boot handoff.
 *
 * Everything the kernel needs from whoever loaded it, normalised into one
 * struct filled once, early, before any allocator runs: memory map,
 * framebuffer, ACPI root pointer, modules, command line and the firmware
 * configuration table. Consumers read this; they do not parse boot tags.
 *
 * Today the only transport is Multiboot2, and it arrives from GRUB under
 * both BIOS/CSM and UEFI — the tags differ, not the kernel. `firmware` says
 * which one actually booted, taken from the tags themselves (an EFI system
 * table pointer is present or it is not), never from a build flag. A second
 * transport (a native UEFI stub, an AArch64 entry) fills the same struct.
 */
#ifndef ECHOS_BOOT_HANDOFF_H
#define ECHOS_BOOT_HANDOFF_H

#include "rogex.h"

typedef enum {
    BOOT_FW_UNKNOWN = 0,
    BOOT_FW_BIOS,               /* legacy BIOS / CSM                        */
    BOOT_FW_UEFI                /* firmware handed us an EFI system table   */
} boot_firmware_t;

/* Normalised memory types. Multiboot2 numbers happen to match E820; that is
   a coincidence of history, so they are translated rather than passed on. */
typedef enum {
    BOOT_MEM_USABLE = 1,
    BOOT_MEM_RESERVED,
    BOOT_MEM_ACPI_RECLAIM,
    BOOT_MEM_ACPI_NVS,
    BOOT_MEM_BAD,
    BOOT_MEM_OTHER
} boot_mem_type_t;

#define BOOT_MEM_MAX      32
#define BOOT_MODULE_MAX    8
#define BOOT_CMDLINE_MAX 256
#define BOOT_MODNAME_MAX  32

typedef struct {
    uint64_t        base;
    uint64_t        length;
    boot_mem_type_t type;
    uint32_t        raw_type;   /* what the firmware called it              */
} boot_mem_range_t;

typedef struct {
    uint64_t start;             /* physical, updated if we relocate it      */
    uint64_t end;
    char     name[BOOT_MODNAME_MAX];
} boot_module_t;

typedef struct boot_handoff {
    boot_firmware_t  firmware;
    const char      *transport;         /* "multiboot2"                     */

    boot_mem_range_t mem[BOOT_MEM_MAX];
    uint32_t         mem_count;         /* stored                           */
    uint32_t         mem_reported;      /* what the firmware listed         */

    bool             fb_present;
    uint64_t         fb_addr;
    uint32_t         fb_pitch, fb_width, fb_height;
    uint8_t          fb_bpp, fb_type;

    uint64_t         acpi_rsdp;         /* 0 when the loader passed none    */
    uint8_t          acpi_revision;     /* 1 = ACPI 1.0 tag, 2 = 2.0+ tag   */

    boot_module_t    module[BOOT_MODULE_MAX];
    uint32_t         module_count;

    char             cmdline[BOOT_CMDLINE_MAX];

    uint64_t         fw_config_table;   /* EFI system table, 0 on BIOS      */
    uint64_t         fw_image_handle;
    bool             fw_services_active;/* loader did NOT exit boot services */

    uint64_t         info_start, info_end;  /* the raw block; keep it alive  */
    bool             valid;
} boot_handoff_t;

/* Parse the loader's information block. Safe to call with a bad magic: the
   handoff comes back invalid and every accessor reports nothing rather than
   reading whatever address was in the register. */
void                  boot_handoff_init(uint64_t info, uint64_t magic);
const boot_handoff_t *boot_handoff(void);

const char *boot_firmware_name(void);   /* "bios" | "uefi" | "unknown"      */
const char *boot_mode_string(void);     /* "bios-multiboot2" | "uefi-..."   */

/* Relocate the LIVE payload module above *floor so the bump allocator cannot
   hand those frames out, and raise *floor past it. Call once, from pmm_init,
   before any frame is allocated. */
void           boot_take_payload(uint64_t *floor);
const uint8_t *boot_payload(uint32_t *len);

const char *boot_cmdline(void);
bool        boot_cmdline_has(const char *key);
bool        boot_cmdline_value(const char *key, char *dst, uint32_t max);

#endif /* ECHOS_BOOT_HANDOFF_H */
