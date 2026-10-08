/*
 * The robotic runtime: four static queues and the numbers that say what
 * happened in them.
 *
 *   sensors  →  runtime  →  intents  →  safety gate  →  autopilot transport
 *
 * No allocation on this path, no I/O from an interrupt, no floating point.
 * The producer in the middle is a reflex policy, not an intelligence: it is
 * the seam where something smarter plugs in later, and calling it a
 * placeholder is the honest description of what ships today.
 */
#ifndef ECHOS_ROBOT_H
#define ECHOS_ROBOT_H

#include "rogex.h"
#include "sensor.h"
#include "intent.h"
#include "safety.h"
#include "rqueue.h"

/* Latency is kept as a histogram of fixed buckets, not a running average: an
   average is exactly the statistic that hides a p99. Buckets are powers of
   two in microseconds, so the arithmetic is a shift. */
#define ROBOT_LAT_BUCKETS 24

typedef struct {
    uint32_t count;
    uint32_t bucket[ROBOT_LAT_BUCKETS];
    uint64_t min_us, max_us, sum_us;
} robot_latency_t;

typedef struct {
    uint32_t sensors_in;        /* records accepted at the door            */
    uint32_t sensors_rejected;  /* wrong ABI, unknown type, bad quality    */
    uint32_t intents_made;      /* the policy produced one                 */
    uint32_t intents_expired;   /* dropped: deadline passed before sending */
    uint32_t sent;              /* reached the transport queue             */
    uint32_t delivered;         /* handed to an attached transport         */
    uint32_t watchdog_intents;  /* the gate spoke because nobody did       */
} robot_stats_t;

void robot_init(void);
void robot_reset(void);

/* Feed one record in. False when it was rejected or the queue was full;
   the reason is in the counters, never in a log line only. */
bool robot_ingest(const sensor_event_t *e);

/* Move everything as far down the pipeline as it will go. Returns how many
   intents reached the transport. Call from a task or the idle path — never
   from an interrupt. */
uint32_t robot_step(uint64_t now_us);

/* Pull what is ready to be transmitted (phase E hands this to MAVLink). */
bool robot_take_outbound(intent_t *out);

/* Attach the thing that actually sends. Until one is attached the outbound
   queue fills and then drops, which is the honest picture of a machine with
   no autopilot on the other end — not a bug to be hidden by a sink that
   throws decisions away. Returns false from the callback to leave the record
   queued (a busy link is backpressure, not loss). */
typedef bool (*robot_transport_fn)(const intent_t *intent);
void robot_set_transport(robot_transport_fn fn);
bool robot_transport_attached(void);

/* Run the scripted scenario: a fixed sequence with an obstacle, a battery
   fade, a link loss, an expired intent and a burst that overflows a queue.
   Deterministic in what it exercises; the timing is whatever the machine
   does. Returns the number of records injected. */
uint32_t robot_scenario(void);

const robot_stats_t   *robot_stats(void);
const robot_latency_t *robot_latency(void);
const rqueue_t *robot_q_sensor(void);
const rqueue_t *robot_q_intent(void);
const rqueue_t *robot_q_gate(void);
const rqueue_t *robot_q_out(void);

/* Percentile of the pipeline latency, in microseconds, from the histogram.
   Returns the upper edge of the bucket the percentile falls in — a bound,
   not an interpolation that would invent precision the buckets do not have. */
uint64_t robot_latency_percentile(uint32_t per_mille);

#endif /* ECHOS_ROBOT_H */
