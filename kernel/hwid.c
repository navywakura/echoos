/*
 * Hardware UID: CPUID (and TPM probe if present) hashed into a stable id.
 * Not per-unit unique without a TPM — CPUID alone has no serial. Known limit.
 */
#include "hwid.h"
#include "arch.h"

static inline uint32_t mmio_r32(uint64_t phys) {
    return *(volatile uint32_t *)(uintptr_t)phys;
}

/* TPM 2.0 TIS: locality-0 register base and the DID/VID register offset. */
#define TPM_TIS_BASE   0xFED40000ULL
#define TPM_DID_VID    (TPM_TIS_BASE + 0xF00)

void hwid_collect(hwid_t *out) {
    k_memset(out, 0, sizeof(*out));

    /* Vendor string, signature and feature word come from the arch layer:
       on x86 that is CPUID, elsewhere it is whatever that machine has. */
    arch_cpu_vendor(out->cpu_vendor);
    out->cpu_signature = arch_cpu_signature();
    out->cpu_features  = arch_cpu_features();

    /* --- TPM 2.0 TIS presence: a real device gives a non-empty DID/VID. ---
       The TIS is at 0xFED40000, above the boot's 1 GiB identity map, so map its
       page first (else the MMIO read faults). On QEMU with no TPM this reads
       0xFFFFFFFF -> treated as absent. */
    vmm_map_page(TPM_TIS_BASE, TPM_TIS_BASE, PTE_WRITABLE);
    uint32_t didvid = mmio_r32(TPM_DID_VID);
    out->tpm_did_vid = didvid;
    out->tpm_present = (didvid != 0x00000000u && didvid != 0xFFFFFFFFu);

    /* --- NIC MAC: placeholder until the mesh NIC driver lands. --- */
    static const uint8_t placeholder_mac[6] = { 0x02, 'R', 'G', 'X', 0x00, 0x01 };
    k_memcpy(out->mac, placeholder_mac, 6);

    /* --- Derive the UID. TPM material is the DID/VID when present. --- */
    uint8_t tpm_material[4];
    k_memcpy(tpm_material, &out->tpm_did_vid, 4);
    uint8_t cpu_material[12];
    k_memcpy(&cpu_material[0], &out->cpu_signature, 4);
    k_memcpy(&cpu_material[4], &out->cpu_features, 4);
    k_memcpy(&cpu_material[8], out->cpu_vendor, 4);

    rogex_uid_derive(out->tpm_present ? tpm_material : NULL,
                     out->tpm_present ? 4 : 0,
                     cpu_material, sizeof(cpu_material),
                     out->mac, sizeof(out->mac),
                     out->uid);
}
