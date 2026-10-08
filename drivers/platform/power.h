/* Power / reboot / halt helpers. */
#ifndef RXOS_POWER_H
#define RXOS_POWER_H

#include "rogex.h"

/* Probe ACPI + CPU idle capabilities. Safe to call once at boot. */
void        power_init(void);

/* Park the CPU in the deepest idle state this machine actually supports:
   MWAIT with a C-state hint when available, otherwise HLT. Returns after an
   interrupt wakes the core. This is the single idle primitive the rest of
   the kernel should call. */
void        power_idle(void);

/* --- ACPI soft-off (S5) ---------------------------------------------------
   Cutting power needs SLP_TYPa/b, which live in the DSDT as AML. EchOS has no
   AML interpreter, so the values are recovered by scanning for the `_S5_`
   object and decoding the few bytes after it — a heuristic, and treated as
   one: if the shape is not exactly right, S5 reports unavailable and callers
   halt instead of writing a guess into a power-management register.
   power_soft_off() does not return on a machine that honours it. */
bool        power_soft_off_available(void);
const char *power_soft_off_status(void);
void        power_soft_off(void);

bool        power_acpi_present(void);
const char *power_acpi_oem(void);      /* FADT OEM id, or "-"                */
uint32_t    power_acpi_revision(void);
uint16_t    power_c2_latency(void);    /* firmware-declared, microseconds    */
uint16_t    power_c3_latency(void);

bool        power_mwait_available(void);
int         power_deepest_cstate(void);   /* 1 = HLT/C1 only, else 2..n      */
const char *power_idle_method(void);      /* "MWAIT C<n>" | "HLT (C1)"       */
uint64_t    power_idle_entries(void);     /* how many times we parked        */

/* --- safe MSR access ------------------------------------------------------
   RDMSR of an unimplemented MSR raises #GP. Rather than guessing which MSRs
   exist from CPU family tables (and panicking when the guess is wrong), we
   arm a guard, execute the read, and let the #GP handler skip the faulting
   instruction and report the failure. This is the same trick Linux's
   rdmsr_safe() uses, and it makes probing safe on ANY CPU — including the
   old machines this OS is meant to revive. */
bool        msr_read_safe(uint32_t msr, uint64_t *out);
bool        msr_guard_armed(void);      /* consulted by the #GP handler      */
void        msr_guard_note_fault(void);
uint64_t    msr_guard_fault_rip(void);  /* address of the guarded RDMSR      */
uint64_t    msr_guard_resume_rip(void); /* where to resume after the fault   */
const char *power_msr_guard_status(void);  /* boot self-test of the recovery */

/* --- RAPL energy metering (Intel Sandy Bridge and later) ------------------ */
bool        power_rapl_available(void);
const char *power_rapl_status(void);
const char *power_cpu_name(void);       /* "Intel fam 6 model 61" etc.       */

/* Package / core / graphics energy in microjoules. Each returns false when
   that domain is not implemented on this part. */
bool        power_rapl_pkg_uj(uint64_t *microjoules);
bool        power_rapl_core_uj(uint64_t *microjoules);
bool        power_rapl_gpu_uj(uint64_t *microjoules);
uint32_t    power_rapl_tdp_mw(void);    /* thermal design power, 0 = unknown */

/* Measure average package power over `ticks` PIT ticks while parking the CPU
   in its deepest idle state. Returns false if RAPL is unavailable. This is
   the actual instrumented measurement: energy delta over a known interval. */
bool        power_measure_mw(uint32_t ticks, uint32_t *milliwatts,
                             uint32_t *core_mw, uint32_t *gpu_mw);
const char *power_measure_status(void);   /* why the last measurement failed */

#endif /* RXOS_POWER_H */
