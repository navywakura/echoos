/* Software wall clock. Seeded from CMOS RTC when present, else a fixed
   boot default; advanced from the PIT. Used by the taskbar and `date`. */
#ifndef RXOS_CLOCK_H
#define RXOS_CLOCK_H

#include "rogex.h"

typedef struct {
    int year;   /* full year, e.g. 2026 */
    int month;  /* 1..12 */
    int day;    /* 1..31 */
    int hour;   /* 0..23 */
    int min;    /* 0..59 */
    int sec;    /* 0..59 */
} clock_tm;

/* Call once after pit_init(). Tries CMOS RTC; falls back to a default. */
void     clock_init(void);
bool     clock_rtc_ok(void);           /* true if CMOS seeded the clock     */

/* CMOS RTC is UTC (QEMU default, most firmware). clock_now() is local. */
void     clock_now_utc(clock_tm *out);
void     clock_now(clock_tm *out);          /* local = UTC + zone, with DST */

/* Set *local* wall time. Converted to UTC for the CMOS. */
bool     clock_set(const clock_tm *t);

/* Parse "YYYY-MM-DD" and optional "HH:MM[:SS]". */
bool     clock_parse(const char *date, const char *time, clock_tm *out);

/* Zone: id like Europe/Madrid, or an explicit offset "+2" / "+02:00".
   Europe IANA zones get EU DST (last Sunday of March to last Sunday of October). */
void        clock_tz_set(int minutes_east, const char *name);
int         clock_tz_apply(const char *id);     /* 0 ok, -1 unknown */
int         clock_tz_minutes(void);             /* effective, including DST */
int         clock_tz_std_minutes(void);         /* winter offset */
const char *clock_tz_name(void);
const char *clock_tz_list(int i);               /* NULL-terminated ids */
void        clock_tz_fmt_offset(char *out, size_t max);

/* Format helpers. out must hold the stated size. Returns out. */
char    *clock_fmt_time(char *out, size_t max);   /* "HH:MM:SS"   (9)  */
char    *clock_fmt_date(char *out, size_t max);   /* "YYYY-MM-DD" (11) */
char    *clock_fmt_stamp(char *out, size_t max);  /* "YYYY-MM-DD HH:MM:SS" (20) */

/* Seconds of wall clock that have rolled over since the last call with the
   same *seen bucket — for taskbar 1 Hz redraws. Pass &local static. */
bool     clock_second_changed(uint64_t *seen);

#endif /* RXOS_CLOCK_H */
