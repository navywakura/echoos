/*
 * Monotonic microsecond clock.
 *
 * The PIT gives 10 ms ticks, which is useless for a pipeline whose whole
 * point is to report a median and a p99. This is the interface the robotic
 * runtime timestamps with; arch/<target>/ implements it. On x86_64 that is
 * the TSC, calibrated against the PIT at boot.
 *
 * Monotonic, never wall clock: an NTP-less box with a CMOS RTC must not be
 * able to make a latency negative.
 */
#ifndef ECHOS_MONO_H
#define ECHOS_MONO_H

#include "rogex.h"

/* Calibrate. Call after pit_init(); returns false when no usable source was
   found, and mono_us() then returns PIT milliseconds scaled to us — coarse
   and honest rather than absent. */
bool     mono_init(void);
uint64_t mono_us(void);

/* What the clock actually is, for the report: "TSC calibrated at N MHz" or
   "PIT 10 ms granularity". Resolution in nanoseconds, so a reader can tell a
   real measurement from a rounded one. */
const char *mono_source(void);
uint32_t    mono_resolution_ns(void);

/* The TSC is disciplined against the PIT, which is the authority for elapsed
   time. These say how often that correction was needed and how far off the
   last estimate was, because a clock that keeps being corrected is a clock
   whose numbers deserve a caveat. */
uint32_t    mono_resyncs(void);
uint32_t    mono_drift_permille(void);

#endif /* ECHOS_MONO_H */
