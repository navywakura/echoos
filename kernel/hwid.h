/* Hardware identity / UID probe. */
#ifndef ROGEX_HWID_H
#define ROGEX_HWID_H

#include "rogex.h"
#include "rogex_core.h"   /* ROGEX_UID_BYTES */

typedef struct {
    char     cpu_vendor[13];              /* CPUID leaf 0 vendor string       */
    uint32_t cpu_signature;               /* CPUID leaf 1 EAX (family/model)  */
    uint32_t cpu_features;                /* CPUID leaf 1 EDX                 */
    bool     tpm_present;                 /* TPM 2.0 TIS detected at 0xFED40000 */
    uint32_t tpm_did_vid;                 /* TPM DID:VID register             */
    uint8_t  mac[6];                      /* NIC MAC (placeholder until driver) */
    uint8_t  uid[ROGEX_UID_BYTES];        /* derived UID                      */
} hwid_t;

/* Gather CPU/TPM/MAC and derive the UID (via rogex_uid_derive). */
void hwid_collect(hwid_t *out);

#endif /* ROGEX_HWID_H */
