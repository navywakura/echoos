/* Wall clock: CMOS RTC seed + PIT-driven advance. Super simple on purpose. */
#include "clock.h"
#include "pit.h"
#include "ports.h"

static clock_tm  g_base;
static uint64_t  g_base_ticks;
static bool      g_rtc_ok;
static int       g_tz_std;          /* winter minutes east of UTC */
static int       g_dst_rule;        /* 0 none, 1 EU, 2 US */
static char      g_tz_name[40] = "Europe/Madrid";

typedef struct { const char *id; int std; int dst; } tz_zone;
static const tz_zone ZONES[] = {
    { "UTC",                    0, 0 },
    { "Europe/London",          0, 1 },
    { "Europe/Madrid",         60, 1 },
    { "Europe/Paris",          60, 1 },
    { "Europe/Berlin",         60, 1 },
    { "Atlantic/Canary",        0, 1 },
    { "America/New_York",    -300, 2 },
    { "America/Chicago",     -360, 2 },
    { "America/Mexico_City", -360, 0 },
    { "America/Bogota",      -300, 0 },
    { "America/Argentina/Buenos_Aires", -180, 0 },
    { "America/Sao_Paulo",   -180, 0 },
    { "Asia/Tokyo",           540, 0 },
    { 0, 0, 0 }
};

/* ---- calendar helpers --------------------------------------------------- */
static const int DAYS_IN_MONTH[] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int dim(int y, int m) {
    if (m == 2 && is_leap(y)) return 29;
    if (m < 1 || m > 12) return 30;
    return DAYS_IN_MONTH[m];
}

static bool valid_tm(const clock_tm *t) {
    if (!t) return false;
    if (t->year < 2000 || t->year > 2099) return false;
    if (t->month < 1 || t->month > 12) return false;
    if (t->day < 1 || t->day > dim(t->year, t->month)) return false;
    if (t->hour < 0 || t->hour > 23) return false;
    if (t->min < 0 || t->min > 59) return false;
    if (t->sec < 0 || t->sec > 59) return false;
    return true;
}

static int starts(const char *s, const char *p) {
    if (!s || !p) return 0;
    while (*p) { if (*s != *p) return 0; s++; p++; }
    return 1;
}

static int dow(int y, int m, int d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int last_sun(int y, int m) {
    int d = dim(y, m);
    while (d > 1 && dow(y, m, d) != 0) d--;
    return d;
}

static int nth_sun(int y, int m, int n) {
    int d = 1, seen = 0;
    int md = dim(y, m);
    for (; d <= md; d++) {
        if (dow(y, m, d) == 0) {
            seen++;
            if (seen == n) return d;
        }
    }
    return d;
}

static int cmp_mdh(const clock_tm *t, int m, int d, int h) {
    if (t->month != m) return t->month - m;
    if (t->day != d) return t->day - d;
    return t->hour - h;
}

static int in_eu_dst(const clock_tm *utc) {
    int ms = last_sun(utc->year, 3);
    int os = last_sun(utc->year, 10);
    return cmp_mdh(utc, 3, ms, 1) >= 0 && cmp_mdh(utc, 10, os, 1) < 0;
}

static int in_us_dst(const clock_tm *utc) {
    /* Second Sunday of March 07:00 UTC ≈ 02:00 EST, first Sunday of Nov. */
    int ms = nth_sun(utc->year, 3, 2);
    int ns = nth_sun(utc->year, 11, 1);
    return cmp_mdh(utc, 3, ms, 7) >= 0 && cmp_mdh(utc, 11, ns, 6) < 0;
}

static int offset_for(const clock_tm *utc) {
    int extra = 0;
    if (g_dst_rule == 1 && in_eu_dst(utc)) extra = 60;
    if (g_dst_rule == 2 && in_us_dst(utc)) extra = 60;
    return g_tz_std + extra;
}

static void add_minutes(clock_tm *t, int mins);

/* Add whole seconds to a broken-down time. */
static void add_seconds(clock_tm *t, uint64_t secs) {
    uint64_t s = (uint64_t)t->sec + secs;
    t->sec = (int)(s % 60);
    uint64_t m = (uint64_t)t->min + s / 60;
    t->min = (int)(m % 60);
    uint64_t h = (uint64_t)t->hour + m / 60;
    t->hour = (int)(h % 24);
    uint64_t days = h / 24;
    while (days > 0) {
        int md = dim(t->year, t->month);
        if (t->day < md) {
            t->day++;
            days--;
        } else {
            t->day = 1;
            t->month++;
            if (t->month > 12) {
                t->month = 1;
                t->year++;
            }
            days--;
        }
    }
}

static void add_minutes(clock_tm *t, int mins) {
    if (!t || !mins) return;
    if (mins > 0) {
        add_seconds(t, (uint64_t)mins * 60u);
        return;
    }
    {
        int64_t total = (int64_t)t->hour * 60 + t->min + mins;
        int days = 0;
        while (total < 0) { total += 24 * 60; days--; }
        t->hour = (int)((total / 60) % 24);
        t->min  = (int)(total % 60);
        while (days < 0) {
            t->day--;
            if (t->day < 1) {
                t->month--;
                if (t->month < 1) { t->month = 12; t->year--; }
                t->day = dim(t->year, t->month);
            }
            days++;
        }
    }
}

/* ---- CMOS RTC (MC146818) ------------------------------------------------ */
static uint8_t cmos_read(uint8_t reg) {
    /* NMI disabled while selecting (bit 7). */
    outb(0x70, (uint8_t)(reg | 0x80));
    return inb(0x71);
}

static uint8_t bcd2bin(uint8_t v) {
    return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

static uint8_t bin2bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void cmos_write(uint8_t reg, uint8_t val) {
    outb(0x70, (uint8_t)(reg | 0x80));
    outb(0x71, val);
}

static bool cmos_write_time(const clock_tm *t) {
    /* Hold the update (reg B bit 7), write BCD 24-hour, then release. */
    uint8_t b = cmos_read(0x0B);
    cmos_write(0x0B, (uint8_t)(b | 0x80));
    cmos_write(0x00, bin2bcd((uint8_t)t->sec));
    cmos_write(0x02, bin2bcd((uint8_t)t->min));
    cmos_write(0x04, bin2bcd((uint8_t)t->hour));
    cmos_write(0x07, bin2bcd((uint8_t)t->day));
    cmos_write(0x08, bin2bcd((uint8_t)t->month));
    cmos_write(0x09, bin2bcd((uint8_t)(t->year % 100)));
    cmos_write(0x32, bin2bcd((uint8_t)(t->year / 100)));
    /* 24-hour (bit 1), BCD (bit 2 clear), SET off. */
    cmos_write(0x0B, (uint8_t)((b | 0x02) & ~(uint8_t)0x84));
    return true;
}

static bool cmos_read_time(clock_tm *out) {
    /* Wait until Update-In-Progress clears (reg A bit 7). Bound the spin. */
    for (int i = 0; i < 100000; i++) {
        if (!(cmos_read(0x0A) & 0x80)) break;
    }
    uint8_t s  = cmos_read(0x00);
    uint8_t m  = cmos_read(0x02);
    uint8_t h  = cmos_read(0x04);
    uint8_t d  = cmos_read(0x07);
    uint8_t mo = cmos_read(0x08);
    uint8_t y  = cmos_read(0x09);
    uint8_t b  = cmos_read(0x0B);

    /* Re-read once more if an update may have crossed mid-read. */
    for (int i = 0; i < 100000; i++) {
        if (!(cmos_read(0x0A) & 0x80)) break;
    }
    uint8_t s2 = cmos_read(0x00);
    if (s2 != s) {
        s  = s2;
        m  = cmos_read(0x02);
        h  = cmos_read(0x04);
        d  = cmos_read(0x07);
        mo = cmos_read(0x08);
        y  = cmos_read(0x09);
    }

    /* Status B bit 2 = binary mode; clear = BCD (the usual case). */
    if (!(b & 0x04)) {
        s  = bcd2bin(s);
        m  = bcd2bin(m);
        h  = bcd2bin(h & 0x7f);
        d  = bcd2bin(d);
        mo = bcd2bin(mo);
        y  = bcd2bin(y);
    } else {
        h &= 0x7f;
    }
    /* 12-hour mode: bit 1 of B clear, and hour has PM bit. */
    if (!(b & 0x02)) {
        bool pm = (cmos_read(0x04) & 0x80) != 0;
        if (h == 12) h = 0;
        if (pm) h = (uint8_t)(h + 12);
    }

    out->sec   = (int)s;
    out->min   = (int)m;
    out->hour  = (int)h;
    out->day   = (int)d;
    out->month = (int)mo;
    /* Century: assume 2000+. QEMU / most firmwares store year as 00..99. */
    out->year  = 2000 + (int)y;

    return valid_tm(out);
}

/* ---- public API --------------------------------------------------------- */
static void tz_copy_name(const char *name) {
    uint32_t i = 0;
    if (!name || !name[0]) { g_tz_name[0] = 'U'; g_tz_name[1] = 'T';
                             g_tz_name[2] = 'C'; g_tz_name[3] = '\0'; return; }
    while (name[i] && i + 1 < sizeof(g_tz_name)) { g_tz_name[i] = name[i]; i++; }
    g_tz_name[i] = '\0';
}

static int dst_from_id(const char *id) {
    if (!id) return 0;
    if (starts(id, "UTC")) return 0;
    if (starts(id, "Europe/")) return 1;
    if (starts(id, "Atlantic/Canary")) return 1;
    if (starts(id, "America/New_York") || starts(id, "America/Chicago")) return 2;
    return 0;
}

void clock_init(void) {
    g_base_ticks = pit_ticks();
    g_rtc_ok = false;
    g_tz_std = 60;
    g_dst_rule = 1;
    tz_copy_name("Europe/Madrid");
    clock_tm rtc;
    if (cmos_read_time(&rtc) && valid_tm(&rtc)) {
        g_base = rtc;
        g_rtc_ok = true;
    } else {
        g_base.year  = 2026;
        g_base.month = 8;
        g_base.day   = 12;
        g_base.hour  = 12;
        g_base.min   = 0;
        g_base.sec   = 0;
    }
}

bool clock_rtc_ok(void) { return g_rtc_ok; }

void clock_now_utc(clock_tm *out) {
    if (!out) return;
    *out = g_base;
    uint64_t now = pit_ticks();
    if (now > g_base_ticks) {
        uint64_t elapsed = now - g_base_ticks;
        uint64_t secs = elapsed / (uint64_t)PIT_HZ;
        add_seconds(out, secs);
    }
}

void clock_now(clock_tm *out) {
    clock_tm utc;
    clock_now_utc(&utc);
    if (!out) return;
    *out = utc;
    add_minutes(out, offset_for(&utc));
}

bool clock_set(const clock_tm *t) {
    clock_tm utc, probe;
    int off;
    if (!valid_tm(t)) return false;
    /* `t` is local. CMOS stores UTC. */
    utc = *t;
    probe = *t;
    off = offset_for(&probe);
    add_minutes(&utc, -off);
    g_base = utc;
    g_base_ticks = pit_ticks();
    if (cmos_write_time(&utc)) g_rtc_ok = true;
    return true;
}

void clock_tz_set(int minutes_east, const char *name) {
    g_tz_std = minutes_east;
    tz_copy_name(name);
    g_dst_rule = dst_from_id(g_tz_name);
}

int clock_tz_apply(const char *id) {
    int i, sign = 1, v = 0, n = 0;
    if (!id || !id[0]) return -1;
    while (*id == ' ') id++;
    for (i = 0; ZONES[i].id; i++) {
        const char *a = ZONES[i].id, *b = id;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && (!*b || *b == '\n' || *b == '\r' || *b == ' ')) {
            g_tz_std = ZONES[i].std;
            g_dst_rule = ZONES[i].dst;
            tz_copy_name(ZONES[i].id);
            return 0;
        }
    }
    if (*id == '+' || *id == '-') {
        if (*id == '-') sign = -1;
        id++;
        while (*id >= '0' && *id <= '9') { v = v * 10 + (*id - '0'); n++; id++; }
        if (n < 1) return -1;
        if (*id == ':') {
            int m = 0;
            id++;
            if (*id < '0' || *id > '9') return -1;
            m = (*id++ - '0') * 10;
            if (*id < '0' || *id > '9') return -1;
            m += *id - '0';
            g_tz_std = sign * (v * 60 + m);
        } else if (v >= -14 && v <= 14 && n <= 2) {
            g_tz_std = sign * v * 60;
        } else {
            g_tz_std = sign * v;
        }
        g_dst_rule = 0;
        tz_copy_name("offset");
        return 0;
    }
    return -1;
}

int clock_tz_minutes(void) {
    clock_tm utc;
    clock_now_utc(&utc);
    return offset_for(&utc);
}
int         clock_tz_std_minutes(void) { return g_tz_std; }
const char *clock_tz_name(void)        { return g_tz_name; }
const char *clock_tz_list(int i) {
    int n = 0;
    while (ZONES[n].id) n++;
    if (i < 0 || i >= n) return 0;
    return ZONES[i].id;
}
void clock_tz_fmt_offset(char *out, size_t max) {
    int m, h, mm;
    if (!out || max < 7) { if (out && max) out[0] = '\0'; return; }
    m = clock_tz_minutes();
    out[0] = (m < 0) ? '-' : '+';
    if (m < 0) m = -m;
    h = m / 60; mm = m % 60;
    out[1] = (char)('0' + (h / 10) % 10);
    out[2] = (char)('0' + (h % 10));
    out[3] = ':';
    out[4] = (char)('0' + (mm / 10) % 10);
    out[5] = (char)('0' + (mm % 10));
    out[6] = '\0';
}

bool clock_parse(const char *date, const char *time, clock_tm *out) {
    if (!date || !date[0] || !out) return false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, n = 0;
    for (const char *p = date; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            if (n < 4) y = y * 10 + (*p - '0');
            else if (n < 6) mo = mo * 10 + (*p - '0');
            else if (n < 8) d = d * 10 + (*p - '0');
            else return false;
            n++;
        } else if (*p == '-' && (n == 4 || n == 6)) {
            continue;
        } else return false;
    }
    if (n != 8) return false;
    if (time && time[0]) {
        int tn = 0;
        for (const char *p = time; *p; p++) {
            if (*p >= '0' && *p <= '9') {
                if (tn < 2) h = h * 10 + (*p - '0');
                else if (tn < 4) mi = mi * 10 + (*p - '0');
                else if (tn < 6) s = s * 10 + (*p - '0');
                else return false;
                tn++;
            } else if (*p == ':' && (tn == 2 || tn == 4)) {
                continue;
            } else return false;
        }
        if (tn != 6 && tn != 4) return false;
    }
    out->year = y; out->month = mo; out->day = d;
    out->hour = h; out->min = mi; out->sec = s;
    return valid_tm(out);
}

static void pad2(char *d, int v) {
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + (v % 10));
}

char *clock_fmt_time(char *out, size_t max) {
    if (!out || max < 9) { if (out && max) out[0] = '\0'; return out; }
    clock_tm t; clock_now(&t);
    pad2(out + 0, t.hour);
    out[2] = ':';
    pad2(out + 3, t.min);
    out[5] = ':';
    pad2(out + 6, t.sec);
    out[8] = '\0';
    return out;
}

char *clock_fmt_date(char *out, size_t max) {
    if (!out || max < 11) { if (out && max) out[0] = '\0'; return out; }
    clock_tm t; clock_now(&t);
    out[0] = (char)('0' + (t.year / 1000) % 10);
    out[1] = (char)('0' + (t.year / 100) % 10);
    out[2] = (char)('0' + (t.year / 10) % 10);
    out[3] = (char)('0' + t.year % 10);
    out[4] = '-';
    pad2(out + 5, t.month);
    out[7] = '-';
    pad2(out + 8, t.day);
    out[10] = '\0';
    return out;
}

char *clock_fmt_stamp(char *out, size_t max) {
    if (!out || max < 20) { if (out && max) out[0] = '\0'; return out; }
    clock_fmt_date(out, max);
    out[10] = ' ';
    clock_fmt_time(out + 11, max - 11);
    return out;
}

bool clock_second_changed(uint64_t *seen) {
    if (!seen) return false;
    clock_tm t; clock_now(&t);
    uint64_t bucket = (uint64_t)t.hour * 3600u + (uint64_t)t.min * 60u + (uint64_t)t.sec;
    /* Fold date so midnight still triggers. */
    bucket += (uint64_t)t.day * 86400u + (uint64_t)t.month * 2678400u;
    if (bucket == *seen) return false;
    *seen = bucket;
    return true;
}
