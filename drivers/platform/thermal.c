/* Thermal sensor probe. Best-effort; many machines just say "unknown". */
#include "thermal.h"
#include "power.h"

#define MSR_IA32_THERM_STATUS         0x19C
#define MSR_IA32_PACKAGE_THERM_STATUS 0x1B1
#define MSR_TEMPERATURE_TARGET        0x1A2

#define THERM_VALID   (1u << 31)   /* the readout field is meaningful         */
#define THERM_READOUT_SHIFT 16
#define THERM_READOUT_MASK  0x7F

static bool     g_present;
static bool     g_pkg_present;
static uint32_t g_tjmax = 100;     /* conservative default if the MSR is absent */
static bool     g_tjmax_read;
static const char *g_why = "not probed";

static inline void cpuid(uint32_t leaf, uint32_t sub,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(sub));
}

void thermal_init(void) {
    g_present = g_pkg_present = false;

    /* CPUID leaf 6, EAX bit 0: digital thermal sensor. Bit 6: package thermal
       interrupt/status, which is what gates the package MSR. */
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 6) { g_why = "unavailable (no CPUID leaf 6)"; return; }
    cpuid(6, 0, &a, &b, &c, &d);
    if (!(a & 1u)) { g_why = "unavailable (no digital thermal sensor)"; return; }

    uint64_t v;
    if (!msr_read_safe(MSR_IA32_THERM_STATUS, &v)) {
        g_why = "unavailable (THERM_STATUS faulted)";
        return;
    }
    /* An emulator that returns 0 for absent MSRs would look like "valid bit
       clear", which is the correct answer for it anyway — no reading. */
    g_present = (v & THERM_VALID) != 0;
    if (!g_present) { g_why = "unavailable (sensor reports no valid reading)"; return; }

    if (msr_read_safe(MSR_TEMPERATURE_TARGET, &v)) {
        uint32_t t = (uint32_t)((v >> 16) & 0xFF);
        /* Sanity: real parts report 70..110. Anything outside that is the MSR
           not existing and reading as something arbitrary. */
        if (t >= 70 && t <= 110) { g_tjmax = t; g_tjmax_read = true; }
    }

    g_pkg_present = msr_read_safe(MSR_IA32_PACKAGE_THERM_STATUS, &v)
                    && (v & THERM_VALID) != 0;
    g_why = g_pkg_present ? "available (core + package)" : "available (core)";
}

static bool read_celsius(uint32_t msr, uint32_t *out) {
    uint64_t v;
    if (!msr_read_safe(msr, &v)) return false;
    if (!(v & THERM_VALID)) return false;
    uint32_t below = (uint32_t)((v >> THERM_READOUT_SHIFT) & THERM_READOUT_MASK);
    /* The readout counts degrees BELOW TjMax, so a big number is a cold CPU. */
    *out = (below >= g_tjmax) ? 0 : g_tjmax - below;
    return true;
}

bool thermal_core_c(uint32_t *celsius) {
    return g_present && celsius && read_celsius(MSR_IA32_THERM_STATUS, celsius);
}

bool thermal_package_c(uint32_t *celsius) {
    return g_pkg_present && celsius &&
           read_celsius(MSR_IA32_PACKAGE_THERM_STATUS, celsius);
}

bool        thermal_available(void) { return g_present; }
const char *thermal_status(void)    { return g_why; }
uint32_t    thermal_tjmax(void)     { return g_tjmax; }
bool        thermal_tjmax_measured(void) { return g_tjmax_read; }
