/*
 * Host check for the software IEEE-754 conversion in kernel/robot/mavbridge.c.
 *
 * MAVLink carries floats and this kernel has no FPU, so those two functions
 * build and read the bit patterns with integer arithmetic. Getting the
 * exponent wrong does not crash anything: PX4 receives a well-formed frame
 * with a nonsense number in it and ignores the setpoint. It cost one wrong
 * constant to learn that, so the arithmetic is checked here against the
 * compiler's own floats before it is trusted on a link.
 *
 * The contract is not bit-exactness — the conversion truncates rather than
 * rounding to nearest — but: every value round-trips to within one milli-unit,
 * and the encoding is within one ULP of the true float.
 *
 *   cc -O2 -o f32_hosttest tools/f32_hosttest.c && ./f32_hosttest
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
/* the two functions verbatim from kernel/robot/mavbridge.c */
static int32_t f32_to_milli(uint32_t bits) {
    uint32_t mant = bits & 0x7FFFFFu;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFF) - 127;
    int64_t  v;
    bool neg = (bits >> 31) != 0;
    if (((bits >> 23) & 0xFF) == 0) return 0;
    if (((bits >> 23) & 0xFF) == 0xFF) return neg ? -2147483647 : 2147483647;
    mant |= 0x800000u;
    exp -= 23;
    v = (int64_t)mant * 1000;
    if (exp > 0) { if (exp > 31) return neg ? -2147483647 : 2147483647; v <<= exp; }
    else if (exp < 0) { if (exp < -40) return 0; v >>= -exp; }
    if (v > 2147483647ll) v = 2147483647ll;
    return neg ? (int32_t)(-v) : (int32_t)v;
}
static uint32_t f32_from_milli(int32_t milli) {
    uint32_t sign = 0, exp = 127, mant, sh = 24;
    uint64_t v;
    if (milli == 0) return 0;
    if (milli < 0) { sign = 0x80000000u; v = (uint64_t)(-(int64_t)milli); }
    else v = (uint64_t)milli;
    /* Scale as far left as fits before dividing: 24 bits is enough for a
       metre but not for a millimetre, where truncation costs a hundred ulp.
       The exponent below accounts for whatever shift was used. */
    while (sh < 38 && (v >> (63 - sh)) == 0) sh++;
    v <<= sh;
    v = (v + 500) / 1000;                 /* nearest, not toward zero       */
    while (v >= (1ull << 24)) { v >>= 1; exp++; }
    while (v && v < (1ull << 23)) { v <<= 1; exp--; }
    /* v is 1.m x 2^23 and the value is v x 2^(-sh), so the exponent field is
       127 + 23 + s - sh, and the loops above left exp at 127 + s. */
    exp += 23 - sh;
    mant = (uint32_t)(v & 0x7FFFFFu);
    return sign | ((exp & 0xFF) << 23) | mant;
}
static int ulp_diff(uint32_t a, uint32_t b) {
    return (int)(a > b ? a - b : b - a);
}

int main(void) {
    int bad = 0;
    double cases[] = { 0, 1, -1, 0.5, 30, 100000, 1000000, 1.5, -2.75, 12.345, 9.81, 3.14159, 2000, -0.001 };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); i++) {
        double d = cases[i];
        int32_t milli = (int32_t)(d * 1000.0 + (d >= 0 ? 0.5 : -0.5));
        uint32_t ours = f32_from_milli(milli);
        float real = (float)(milli / 1000.0);
        uint32_t theirs; memcpy(&theirs, &real, 4);
        float back; memcpy(&back, &ours, 4);
        int32_t rt = f32_to_milli(theirs);
        int32_t err = rt > milli ? rt - milli : milli - rt;
        int ulps = ulp_diff(ours, theirs);
        int good = err <= 1 && ulps <= 1;
        printf("%12.5f  milli=%-11d ours=%08x real=%08x  decode=%-11d "
               "err=%d milli, %d ulp  %s\n",
               d, milli, ours, theirs, rt, err, ulps, good ? "ok" : "FAIL");
        (void)back;
        if (!good) bad++;
    }
    printf("%s\n", bad ? "FAILURES" :
           "all values round-trip within 1 milli and 1 ulp");
    return bad != 0;
}
