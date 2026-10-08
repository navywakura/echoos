/* x86 platform power: ACPI, RAPL, MWAIT/HLT idle, reboot.
 *
 * This lives under drivers/ and not under kernel/ because every line of it
 * is about this machine: MSRs, PM1a control ports, CPUID leaves. Generic
 * code calls power_idle() the way it calls disk_read() — through an
 * interface, without knowing what is behind it. Another architecture brings
 * its own driver with the same shape. */
#include "power.h"
#include "pit.h"
#include "rx_backend.h"
#include "handoff.h"

/* --------------------------- ACPI table layout ---------------------------- */
typedef struct ROGEX_PACKED {
    char     sig[8];            /* "RSD PTR " */
    uint8_t  checksum;
    char     oem[6];
    uint8_t  revision;          /* 0 = ACPI 1.0 (RSDT), 2+ = XSDT present    */
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

typedef struct ROGEX_PACKED {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_hdr_t;

/* Only the FADT fields we actually use. Offsets per ACPI spec. */
#define FADT_DSDT_OFF     40
#define FADT_PM1A_CNT_OFF 64
#define FADT_PM1B_CNT_OFF 68
#define FADT_P_LVL2_LAT   96
#define FADT_P_LVL3_LAT   98
#define FADT_X_DSDT_OFF   140       /* 64-bit DSDT, ACPI 2.0+                */

static bool     g_acpi;
static char     g_oem[7];
static uint32_t g_acpi_rev;
static uint16_t g_c2_lat, g_c3_lat;

static bool     g_mwait;
static int      g_deepest = 1;
static uint32_t g_mwait_hint;
static char     g_idle_desc[24];
static uint64_t g_idle_entries;

static bool     g_rapl;
static bool     g_rapl_core, g_rapl_gpu;
static uint32_t g_energy_unit = 16;   /* 1/2^unit joules per counter tick    */
static uint32_t g_tdp_mw;
static const char *g_rapl_why = "not probed";
static const char *g_guard_why = "not probed";
static const char *g_measure_why = "not run";
static char     g_cpu_name[52];

/* ------------------------------ primitives -------------------------------- */
static inline void cpuid(uint32_t leaf, uint32_t sub,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(sub));
}

/* --------------------------- safe MSR access ------------------------------
   A bare RDMSR of an MSR the part does not implement raises #GP. The old code
   dodged that by refusing to touch RAPL under a hypervisor, which also meant
   refusing to measure anything on real hardware whose model we had not
   tabulated. The robust answer — the one Linux uses in rdmsr_safe() — is to
   let the fault happen and recover from it:

     1. arm a guard flag,
     2. execute RDMSR,
     3. the #GP vector sees the guard, notes the fault, and steps rip past the
        two-byte 0F 32 opcode so execution resumes on the next instruction,
     4. we read the flag and report failure instead of a garbage value.

   Rather than guess how many bytes to step over, the asm block records the
   exact address of the RDMSR and the exact address to resume at, both taken
   with RIP-relative LEA so they are whatever the assembler actually emitted.
   The handler only acts when the faulting RIP matches the recorded one, so a
   #GP raised anywhere else in the kernel is still a fatal, loudly reported
   fault. Interrupts are masked across the window so nothing can observe a
   half-armed guard. */
uint64_t rx_msr_fault_rip;      /* written from inline asm: keep the symbol   */
uint64_t rx_msr_resume_rip;
static volatile bool g_msr_fault;

bool msr_guard_armed(void)      { return rx_msr_fault_rip != 0; }
void msr_guard_note_fault(void) { g_msr_fault = true; }

bool msr_read_safe(uint32_t msr, uint64_t *out) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");

    uint32_t lo = 0, hi = 0;
    g_msr_fault = false;
    __asm__ volatile (
        "leaq .Lrxmsr_f%=(%%rip), %%r10\n\t"
        "movq %%r10, rx_msr_fault_rip(%%rip)\n\t"
        "leaq .Lrxmsr_r%=(%%rip), %%r10\n\t"
        "movq %%r10, rx_msr_resume_rip(%%rip)\n\t"
        ".Lrxmsr_f%=: rdmsr\n\t"
        ".Lrxmsr_r%=:"
        : "=a"(lo), "=d"(hi)
        : "c"(msr)
        : "r10", "memory");
    rx_msr_fault_rip = 0;
    bool ok = !g_msr_fault;

    if (flags & (1u << 9)) __asm__ volatile ("sti");
    if (ok && out) *out = ((uint64_t)hi << 32) | lo;
    return ok;
}

uint64_t msr_guard_fault_rip(void)  { return rx_msr_fault_rip; }
uint64_t msr_guard_resume_rip(void) { return rx_msr_resume_rip; }

static uint8_t sum_bytes(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
}

/* ------------------------------ ACPI probe -------------------------------- */
/* Under UEFI there is no BIOS area to scan: the RSDP is reached through the
   firmware configuration table, and the loader already copied it into the
   handoff. Use that when it is there; fall back to the legacy scan, which is
   the only way on a BIOS machine whose loader passed no ACPI tag. */
static const acpi_rsdp_t *find_rsdp(void) {
    const char sig[8] = { 'R','S','D',' ','P','T','R',' ' };
    const boot_handoff_t *bh = boot_handoff();

    if (bh->acpi_rsdp) {
        const acpi_rsdp_t *r = (const acpi_rsdp_t *)(uintptr_t)bh->acpi_rsdp;
        bool m = true;
        for (int i = 0; i < 8; i++) if (r->sig[i] != sig[i]) { m = false; break; }
        if (m && sum_bytes(r, 20) == 0) return r;
    }

    /* EBDA segment pointer lives at 0x40E (BIOS data area). */
    uint64_t ebda = (uint64_t)(*(volatile uint16_t *)0x40E) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        for (uint64_t a = ebda; a < ebda + 1024; a += 16) {
            const acpi_rsdp_t *r = (const acpi_rsdp_t *)a;
            bool m = true;
            for (int i = 0; i < 8; i++) if (r->sig[i] != sig[i]) { m = false; break; }
            if (m && sum_bytes(r, 20) == 0) return r;
        }
    }
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        const acpi_rsdp_t *r = (const acpi_rsdp_t *)a;
        bool m = true;
        for (int i = 0; i < 8; i++) if (r->sig[i] != sig[i]) { m = false; break; }
        if (m && sum_bytes(r, 20) == 0) return r;
    }
    return NULL;
}

static const acpi_hdr_t *find_table(const acpi_rsdp_t *rsdp, const char sig[4]) {
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        const acpi_hdr_t *x = (const acpi_hdr_t *)(uintptr_t)rsdp->xsdt_addr;
        uint32_t n = (x->length - sizeof(acpi_hdr_t)) / 8;
        const uint64_t *e = (const uint64_t *)((const uint8_t *)x + sizeof(acpi_hdr_t));
        for (uint32_t i = 0; i < n; i++) {
            const acpi_hdr_t *t = (const acpi_hdr_t *)(uintptr_t)e[i];
            if (t && t->sig[0]==sig[0] && t->sig[1]==sig[1] &&
                     t->sig[2]==sig[2] && t->sig[3]==sig[3]) return t;
        }
        return NULL;
    }
    if (!rsdp->rsdt_addr) return NULL;
    const acpi_hdr_t *r = (const acpi_hdr_t *)(uintptr_t)rsdp->rsdt_addr;
    uint32_t n = (r->length - sizeof(acpi_hdr_t)) / 4;
    const uint32_t *e = (const uint32_t *)((const uint8_t *)r + sizeof(acpi_hdr_t));
    for (uint32_t i = 0; i < n; i++) {
        const acpi_hdr_t *t = (const acpi_hdr_t *)(uintptr_t)e[i];
        if (t && t->sig[0]==sig[0] && t->sig[1]==sig[1] &&
                 t->sig[2]==sig[2] && t->sig[3]==sig[3]) return t;
    }
    return NULL;
}

/* --------------------------- ACPI soft-off (S5) ---------------------------
   Turning the machine off is one OUT instruction — the hard part is finding
   the two numbers it needs, and they are not in any table with a fixed
   layout. The sleep-type values for S5 live in the DSDT, as an AML object
   named `_S5_` holding a package whose first two bytes are SLP_TYPa and
   SLP_TYPb; the register to write them to is in the FADT.

   A full AML interpreter is a large subsystem and EchOS does not have one, so
   this does what every small kernel does: scan the DSDT for the four-byte
   name and decode the handful of bytes that follow. That is a heuristic, and
   it is treated as one — if the shape is not exactly what is expected, S5 is
   reported unavailable and the caller falls back to halting, rather than
   writing a guessed value into a power-management register. */
static uint16_t g_pm1a_cnt, g_pm1b_cnt;
static uint16_t g_slp_typ_a, g_slp_typ_b;
static bool     g_s5;
static const char *g_s5_why = "not probed";

static inline void outw_(uint16_t p, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p));
}

static void s5_probe(const acpi_hdr_t *fadt) {
    const uint8_t *f = (const uint8_t *)fadt;
    if (fadt->length <= FADT_PM1B_CNT_OFF + 4) { g_s5_why = "FADT too short"; return; }
    g_pm1a_cnt = (uint16_t)*(const uint32_t *)(f + FADT_PM1A_CNT_OFF);
    g_pm1b_cnt = (uint16_t)*(const uint32_t *)(f + FADT_PM1B_CNT_OFF);
    if (!g_pm1a_cnt) { g_s5_why = "no PM1a control block"; return; }

    uint64_t dsdt = *(const uint32_t *)(f + FADT_DSDT_OFF);
    if (fadt->length > FADT_X_DSDT_OFF + 8) {
        uint64_t x = *(const uint64_t *)(f + FADT_X_DSDT_OFF);
        if (x) dsdt = x;
    }
    if (!dsdt) { g_s5_why = "no DSDT"; return; }

    const acpi_hdr_t *d = (const acpi_hdr_t *)(uintptr_t)dsdt;
    if (d->sig[0] != 'D' || d->sig[1] != 'S' || d->sig[2] != 'D' || d->sig[3] != 'T') {
        g_s5_why = "DSDT signature mismatch"; return;
    }
    if (d->length < sizeof(acpi_hdr_t) || d->length > (16u << 20)) {
        g_s5_why = "implausible DSDT length"; return;
    }

    const uint8_t *p   = (const uint8_t *)d;
    uint32_t       len = d->length;
    for (uint32_t i = sizeof(acpi_hdr_t); i + 8 < len; i++) {
        if (p[i] != '_' || p[i+1] != 'S' || p[i+2] != '5' || p[i+3] != '_') continue;
        /* Name, then optionally 0x12 (PackageOp), a PkgLength byte and an
           element count, then the two values — each either a small integer
           opcode (0x00/0x01) or a BytePrefix (0x0A) followed by the byte. */
        const uint8_t *q = p + i + 4;
        if (*q == 0x12) {
            q++;                       /* PackageOp                          */
            q += ((*q & 0xC0) >> 6) + 1;   /* PkgLength: 1 + follow bytes    */
            q++;                       /* NumElements                        */
        }
        int a = -1, b = -1;
        if (*q == 0x0A) { q++; a = *q++; } else if (*q <= 0x01) { a = *q++; }
        if (*q == 0x0A) { q++; b = *q++; } else if (*q <= 0x01) { b = *q++; }
        if (a < 0) { g_s5_why = "_S5_ package not in the expected shape"; return; }
        g_slp_typ_a = (uint16_t)a;
        g_slp_typ_b = (uint16_t)(b < 0 ? 0 : b);
        g_s5 = true;
        g_s5_why = "available";
        return;
    }
    g_s5_why = "_S5_ not found in the DSDT";
}

bool        power_soft_off_available(void) { return g_s5; }
const char *power_soft_off_status(void)    { return g_s5_why; }

void power_soft_off(void) {
    if (!g_s5) return;
    /* SLP_TYP in bits 10..12, SLP_EN in bit 13. Writing this is the last
       instruction that runs on a healthy machine, so there is no error path
       to take afterwards — the caller halts if we return. */
    outw_(g_pm1a_cnt, (uint16_t)((g_slp_typ_a << 10) | (1u << 13)));
    if (g_pm1b_cnt)
        outw_(g_pm1b_cnt, (uint16_t)((g_slp_typ_b << 10) | (1u << 13)));
}

static void acpi_probe(void) {
    g_oem[0] = '-'; g_oem[1] = '\0';
    const acpi_rsdp_t *rsdp = find_rsdp();
    if (!rsdp) return;
    const char fadt_sig[4] = { 'F','A','C','P' };
    const acpi_hdr_t *fadt = find_table(rsdp, fadt_sig);
    if (!fadt) return;

    g_acpi     = true;
    g_acpi_rev = fadt->revision;
    for (int i = 0; i < 6; i++) g_oem[i] = fadt->oem[i];
    g_oem[6] = '\0';

    const uint8_t *f = (const uint8_t *)fadt;
    if (fadt->length > FADT_P_LVL3_LAT + 2) {
        g_c2_lat = *(const uint16_t *)(f + FADT_P_LVL2_LAT);
        g_c3_lat = *(const uint16_t *)(f + FADT_P_LVL3_LAT);
    }
    s5_probe(fadt);
}

/* --------------------------- idle capability ------------------------------ */
static void idle_probe(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    bool monitor = (c & (1u << 3)) != 0;

    if (monitor) {
        /* Leaf 5: EDX holds the number of sub-states per C-state, 4 bits per
           C-state (C0..C7). Pick the deepest that reports any sub-state. */
        uint32_t a5, b5, c5, d5;
        cpuid(5, 0, &a5, &b5, &c5, &d5);
        bool ext = (c5 & 0x1) && (c5 & 0x2);   /* MWAIT extensions + break-on-IRQ */
        int deepest = 1;
        for (int cs = 1; cs <= 7; cs++)
            if (((d5 >> (cs * 4)) & 0xF) != 0) deepest = cs;
        if (ext && deepest >= 1) {
            g_mwait   = true;
            g_deepest = deepest;
            /* hint = (C-state - 1) << 4, sub-state 0 */
            g_mwait_hint = (uint32_t)((deepest - 1) << 4);
        }
    }

    char *p = g_idle_desc;
    if (g_mwait) {
        const char *s = "MWAIT C"; while (*s) *p++ = *s++;
        *p++ = (char)('0' + g_deepest); *p = '\0';
    } else {
        const char *s = "HLT (C1)"; while (*s) *p++ = *s++;
        *p = '\0';
    }
}

/* ------------------------------- CPU name --------------------------------- */
/* Leaves 0x80000002..4 return the 48-character brand string when leaf
   0x80000000 reports them. Every Intel part since Pentium 4 has it, so on the
   machines this OS targets we can print the real model instead of a family
   number. Fall back to family/model when the leaves are absent. */
static void cpu_name_probe(void) {
    uint32_t a, b, c, d;
    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        uint32_t regs[12];
        for (int i = 0; i < 3; i++)
            cpuid(0x80000002u + (uint32_t)i, 0,
                  &regs[i*4+0], &regs[i*4+1], &regs[i*4+2], &regs[i*4+3]);
        const char *src = (const char *)regs;
        /* The string is space-padded on the left by some parts; trim it. */
        int s = 0;
        while (s < 48 && src[s] == ' ') s++;
        int n = 0;
        for (int i = s; i < 48 && src[i]; i++) g_cpu_name[n++] = src[i];
        while (n > 0 && g_cpu_name[n-1] == ' ') n--;
        g_cpu_name[n] = '\0';
        if (n > 0) return;
    }

    cpuid(1, 0, &a, &b, &c, &d);
    uint32_t fam = (a >> 8) & 0xF, mod = (a >> 4) & 0xF;
    if (fam == 0xF) fam += (a >> 20) & 0xFF;
    if (fam == 0x6 || fam == 0xF) mod |= ((a >> 16) & 0xF) << 4;

    char *p = g_cpu_name;
    const char *s = "CPU family "; while (*s) *p++ = *s++;
    if (fam >= 10) *p++ = (char)('0' + fam / 10);
    *p++ = (char)('0' + fam % 10);
    s = " model "; while (*s) *p++ = *s++;
    if (mod >= 100) *p++ = (char)('0' + mod / 100);
    if (mod >= 10)  *p++ = (char)('0' + (mod / 10) % 10);
    *p++ = (char)('0' + mod % 10);
    *p = '\0';
}

/* ------------------------------ RAPL probe -------------------------------- */
#define MSR_RAPL_POWER_UNIT    0x606
#define MSR_PKG_POWER_INFO     0x614
#define MSR_PKG_ENERGY_STATUS  0x611
#define MSR_PP0_ENERGY_STATUS  0x639   /* cores                              */
#define MSR_PP1_ENERGY_STATUS  0x641   /* integrated graphics                */

static void rapl_probe(void) {
    /* RAPL MSR numbers are Intel-specific; AMD uses a different set. */
    uint32_t v0, v1, v2, v3;
    cpuid(0, 0, &v0, &v1, &v2, &v3);
    if (!(v1 == 0x756E6547 && v3 == 0x49656E69 && v2 == 0x6C65746E)) {
        g_rapl_why = "unavailable (non-Intel CPU)";
        return;
    }

    /* No CPU-model table and no hypervisor gate: just try the read. If the
       part (or the hypervisor's MSR emulation) does not implement 0x606 the
       #GP guard catches it and we report that honestly. Sandy Bridge and
       later answer; anything older does not. */
    uint64_t unit;
    if (!msr_read_safe(MSR_RAPL_POWER_UNIT, &unit)) {
        g_rapl_why = "unavailable (no RAPL MSRs: pre-Sandy-Bridge CPU)";
        return;
    }
    /* Not every emulator raises #GP for an MSR it does not implement: QEMU's
       TCG interpreter quietly returns zero, which would have us announce RAPL
       on a machine that has none and then "measure" 0 mW. Validate the unit
       word instead of trusting the read. Bits 12:8 are the energy unit
       exponent; real parts report 13..16 (Sandy Bridge 16, Haswell onward 14),
       never 0 — 2^0 would mean one joule per counter tick. */
    g_energy_unit = (uint32_t)((unit >> 8) & 0x1F);
    if (unit == 0 || g_energy_unit < 8) {
        g_energy_unit = 16;
        g_rapl_why = "unavailable (RAPL MSRs read as zero: emulated CPU)";
        return;
    }

    uint64_t pkg;
    if (!msr_read_safe(MSR_PKG_ENERGY_STATUS, &pkg)) {
        g_rapl_why = "unavailable (package energy domain not implemented)";
        return;
    }
    g_rapl = true;
    g_rapl_why = "available (pkg";

    uint64_t tmp;
    g_rapl_core = msr_read_safe(MSR_PP0_ENERGY_STATUS, &tmp);
    g_rapl_gpu  = msr_read_safe(MSR_PP1_ENERGY_STATUS, &tmp);
    if (g_rapl_core && g_rapl_gpu)  g_rapl_why = "available (pkg + cores + gpu)";
    else if (g_rapl_core)           g_rapl_why = "available (pkg + cores)";
    else                            g_rapl_why = "available (pkg only)";

    /* Thermal spec power: bits 14:0 in power units of 1/2^(unit & 0xF) W. */
    uint64_t info;
    if (msr_read_safe(MSR_PKG_POWER_INFO, &info)) {
        uint32_t pu = (uint32_t)(unit & 0xF);
        g_tdp_mw = (uint32_t)(((info & 0x7FFF) * 1000u) >> pu);
    }
}

/* Boot self-test for the #GP recovery path itself. 0xDEADBEEF is not an MSR on
   any x86 part, so a CPU that follows the architecture must fault on it. If we
   come back with a value, this machine silently returns zero for unimplemented
   MSRs (QEMU's TCG interpreter does exactly that) and the guard is merely
   unexercised — which is worth saying out loud, because it means "RAPL
   available" here would be an emulator artefact, not a capability. */
#define MSR_DEFINITELY_ABSENT 0xDEADBEEFu

static void guard_selftest(void) {
    uint64_t junk;
    if (msr_read_safe(MSR_DEFINITELY_ABSENT, &junk))
        g_guard_why = "untested (this CPU returns 0 for unknown MSRs)";
    else
        g_guard_why = "OK (#GP raised and recovered)";
}

/* -------------------------------- public ---------------------------------- */
void power_init(void) {
    cpu_name_probe();
    acpi_probe();
    idle_probe();
    guard_selftest();
    rapl_probe();
}

void power_idle(void) {
    g_idle_entries++;
    rx_backend_note_idle();
    if (g_mwait) {
        /* MONITOR needs an address to watch; any writable kernel address
           works since we only care about the interrupt break. */
        static volatile uint32_t monitor_line __attribute__((aligned(64)));
        __asm__ volatile ("monitor" :: "a"(&monitor_line), "c"(0), "d"(0));
        /* ECX bit 0 = treat interrupts as break events even with IF=0. */
        __asm__ volatile ("mwait" :: "a"(g_mwait_hint), "c"(1));
    } else {
        __asm__ volatile ("hlt");
    }
}

bool        power_acpi_present(void) { return g_acpi; }
const char *power_acpi_oem(void)     { return g_oem; }
uint32_t    power_acpi_revision(void){ return g_acpi_rev; }
uint16_t    power_c2_latency(void)   { return g_c2_lat; }
uint16_t    power_c3_latency(void)   { return g_c3_lat; }

bool        power_mwait_available(void) { return g_mwait; }
int         power_deepest_cstate(void)  { return g_mwait ? g_deepest : 1; }
const char *power_idle_method(void)     { return g_idle_desc; }
uint64_t    power_idle_entries(void)    { return g_idle_entries; }

bool        power_rapl_available(void)  { return g_rapl; }
const char *power_rapl_status(void)     { return g_rapl_why; }
const char *power_cpu_name(void)        { return g_cpu_name; }
uint32_t    power_rapl_tdp_mw(void)     { return g_tdp_mw; }
const char *power_msr_guard_status(void){ return g_guard_why; }
const char *power_measure_status(void)  { return g_measure_why; }

/* Raw 32-bit counter reads. Deltas must be taken on the raw value, not on the
   converted energy, so the counter's 32-bit wraparound cancels naturally. */
static bool rapl_raw(uint32_t msr, uint32_t *raw) {
    uint64_t v;
    if (!g_rapl || !msr_read_safe(msr, &v)) return false;
    *raw = (uint32_t)(v & 0xFFFFFFFFull);
    return true;
}

static uint64_t raw_to_uj(uint32_t raw) {
    return ((uint64_t)raw * 1000000ull) >> g_energy_unit;
}

static bool domain_uj(uint32_t msr, bool present, uint64_t *out) {
    uint32_t raw;
    if (!present || !out || !rapl_raw(msr, &raw)) return false;
    *out = raw_to_uj(raw);
    return true;
}

bool power_rapl_pkg_uj(uint64_t *uj)  { return domain_uj(MSR_PKG_ENERGY_STATUS, g_rapl, uj); }
bool power_rapl_core_uj(uint64_t *uj) { return domain_uj(MSR_PP0_ENERGY_STATUS, g_rapl_core, uj); }
bool power_rapl_gpu_uj(uint64_t *uj)  { return domain_uj(MSR_PP1_ENERGY_STATUS, g_rapl_gpu, uj); }

/* --------------------------- the measurement ------------------------------
   Average power = energy consumed / time elapsed. We sample the package (and,
   where implemented, core and graphics) energy counters, park the core in its
   deepest idle state for a known number of PIT ticks, and sample again. The
   PIT runs at a fixed 100 Hz, so the interval is exact to the tick — no
   calibration and no wall clock needed. */
bool power_measure_mw(uint32_t ticks, uint32_t *milliwatts,
                      uint32_t *core_mw, uint32_t *gpu_mw) {
    if (!g_rapl || !milliwatts || ticks == 0) {
        g_measure_why = "no energy counters on this machine";
        return false;
    }

    uint32_t p0 = 0, c0 = 0, g0 = 0;
    if (!rapl_raw(MSR_PKG_ENERGY_STATUS, &p0)) {
        g_measure_why = "package counter faulted on read";
        return false;
    }
    bool have_c = g_rapl_core && rapl_raw(MSR_PP0_ENERGY_STATUS, &c0);
    bool have_g = g_rapl_gpu  && rapl_raw(MSR_PP1_ENERGY_STATUS, &g0);

    uint64_t start = pit_ticks();
    while (pit_ticks() - start < ticks) power_idle();
    uint64_t elapsed_ticks = pit_ticks() - start;

    uint32_t p1 = 0, c1 = 0, g1 = 0;
    if (!rapl_raw(MSR_PKG_ENERGY_STATUS, &p1)) {
        g_measure_why = "package counter faulted mid-run";
        return false;
    }
    if (have_c) have_c = rapl_raw(MSR_PP0_ENERGY_STATUS, &c1);
    if (have_g) have_g = rapl_raw(MSR_PP1_ENERGY_STATUS, &g1);

    /* Unsigned 32-bit subtraction wraps correctly across the counter rollover
       (~60 s at 15 W with a 2^-14 J unit), so no special case is needed. */
    uint64_t us = elapsed_ticks * (1000000ull / PIT_HZ);
    if (us == 0) { g_measure_why = "interval too short"; return false; }

    /* A real package counter advances thousands of ticks per second even in
       deep idle. Exactly zero over a full interval means the MSR is a constant,
       so refuse to dress it up as a measurement of 0 mW. */
    if (p1 == p0) {
        g_measure_why = "energy counter never advanced (emulated MSR, not real RAPL)";
        return false;
    }
    g_measure_why = "OK";

    *milliwatts = (uint32_t)((raw_to_uj(p1 - p0) * 1000ull) / us);
    if (core_mw) *core_mw = have_c ? (uint32_t)((raw_to_uj(c1 - c0) * 1000ull) / us) : 0;
    if (gpu_mw)  *gpu_mw  = have_g ? (uint32_t)((raw_to_uj(g1 - g0) * 1000ull) / us) : 0;
    return true;
}
