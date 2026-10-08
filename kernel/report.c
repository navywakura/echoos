/*
 * The evidence block: one machine-readable dump of what this boot measured.
 *
 * Generic on purpose. Both boards call the same function, so the report is a
 * contract rather than two texts that happen to look alike, and a field that
 * only one architecture can fill is visibly absent instead of quietly
 * different. tools/report30.py parses it; §7 of the Astra brief lists what it
 * has to contain.
 *
 * Determinism rules, enforced by what this file is allowed to print:
 *   - no addresses, no timestamps, no host-specific strings;
 *   - counters, which the scenario makes repeatable;
 *   - latency as bucket bounds, which are powers of two and therefore stable
 *     across runs, alongside the raw min/max, which are not and are labelled
 *     so. A number that moves every run must not be presented as if it were
 *     part of the fixed record.
 */
#include "report.h"

#include "arch.h"

#include "version.h"
#include "handoff.h"
#include "heap.h"
#include "heap0.h"
#include "pmm.h"
#include "scheduler.h"
#include "robot.h"
#include "safety.h"
#include "rqueue.h"
#include "seb.h"
#include "mavbridge.h"

static void kv(const char *key, uint64_t v) {
    console_write(key);
    console_write(" ");
    console_write_dec(v);
    console_write("\n");
}

static void ks(const char *key, const char *v) {
    console_write(key);
    console_write(" ");
    console_write(v ? v : "-");
    console_write("\n");
}

static void queue_line(const char *key, const rqueue_t *q) {
    console_write(key);
    console_write(" pushed ");   console_write_dec(q->pushed);
    console_write(" popped ");   console_write_dec(q->popped);
    console_write(" drops ");    console_write_dec(q->drops);
    console_write(" expired ");  console_write_dec(q->expired);
    console_write(" high_water ");
    console_write_dec(q->high_water);
    console_write(" capacity "); console_write_dec(q->capacity);
    console_write("\n");
}

void report_emit(void) {
    /* Snapshots, not live pointers. If a PX4 link ran before this, the
       cumulative counters already carry its telemetry, and the scenario's
       balance (injected = accepted + refused + dropped) would not close
       through no fault of the pipeline. The scenario is reported as a delta
       across its own run; anything genuinely cumulative says so in its name. */
    robot_stats_t   rs0;
    safety_stats_t  sf0;
    uint32_t        qs_drops0, qg_expired0, qo_drops0;
    const robot_stats_t *rs;
    const safety_stats_t *sf;
    const robot_latency_t *lat;
    const boot_handoff_t *bh = boot_handoff();
    uint64_t before_calls, after_calls;
    uint32_t injected;

    console_write("--- echos-report v1\n");
    ks("version", ECHOS_VERSION);
    ks("edition", ECHOS_EDITION);
    ks("arch", arch_name());
    ks("boot_transport", bh->transport);
    /* Transport and firmware are different questions: the same Multiboot2
       handoff arrives from a BIOS and from UEFI, and a report that prints
       only one of them cannot tell the two boots apart. */
    ks("boot_firmware", boot_firmware_name());
    kv("boot_info_valid", bh->valid ? 1 : 0);
    kv("mem_ranges", bh->mem_count);

    /* --- memory ---------------------------------------------------------
       Totals and high-water marks, not addresses. */
    kv("mem_total_bytes", pmm_total_bytes());
    /* Not "free RAM". pmm_free_bytes() is what is left in the one region the
       bump allocator picked, and the gap to the total is RAM it simply did
       not choose — pmm.c says so in its own comment. Reporting it as free
       memory would invite the reading that the difference is in use. */
    kv("mem_frames_handed_out_bytes", pmm_used_bytes());
    kv("mem_bump_region_remaining_bytes", pmm_free_bytes());
    kv("heap_total_bytes", (uint64_t)heap_total());
    kv("heap_used_bytes", (uint64_t)heap_used());
    kv("heap0_total_bytes", (uint64_t)heap0_total_bytes());
    kv("heap0_reserved_bytes", (uint64_t)heap0_reserved_bytes());
    kv("heap0_regions", heap0_region_count());

    /* --- the robotic pipeline, run now so the numbers are this boot's ---
       The allocation counter is read either side of the scenario: "no
       dynamic allocation on the sensor->intent path" is a claim only a
       measurement can carry, and a claim nothing counts is a slogan. */
    rs0 = *robot_stats();
    sf0 = *safety_stats();
    qs_drops0   = robot_q_sensor()->drops;
    qg_expired0 = robot_q_gate()->expired;
    qo_drops0   = robot_q_out()->drops;

    before_calls = heap_alloc_calls();
    injected = robot_scenario();
    after_calls = heap_alloc_calls();

    rs = robot_stats();
    sf = safety_stats();
    lat = robot_latency();

    kv("pipeline_alloc_calls", after_calls - before_calls);
    kv("records_injected", injected);
    kv("records_accepted", rs->sensors_in - rs0.sensors_in);
    kv("records_refused", rs->sensors_rejected - rs0.sensors_rejected);
    kv("records_dropped", robot_q_sensor()->drops - qs_drops0);
    kv("intents_made", rs->intents_made - rs0.intents_made);
    kv("intents_sent", rs->sent - rs0.sent);
    kv("intents_delivered", rs->delivered - rs0.delivered);
    kv("intents_expired", rs->intents_expired - rs0.intents_expired);
    /* One event, one count. The gate queue's `expired` column records the
       same intent from the queue's side (robot.c increments both on the same
       line), so adding them would report a single missed deadline as two. */
    kv("deadlines_missed", rs->intents_expired - rs0.intents_expired);
    kv("deadlines_missed_seen_by_gate_queue",
       robot_q_gate()->expired - qg_expired0);
    kv("outbound_dropped", robot_q_out()->drops - qo_drops0);
    kv("watchdog_intents", rs->watchdog_intents - rs0.watchdog_intents);
    kv("gate_seen", sf->seen - sf0.seen);
    kv("gate_accepted", sf->accepted - sf0.accepted);
    kv("gate_modified", sf->modified - sf0.modified);
    kv("gate_blocked", sf->blocked - sf0.blocked);

    queue_line("queue_sensor", robot_q_sensor());
    queue_line("queue_intent", robot_q_intent());
    queue_line("queue_gate",   robot_q_gate());
    queue_line("queue_out",    robot_q_out());

    /* --- latency ---------------------------------------------------------
       The percentiles are bucket upper bounds: quantised, and therefore the
       same number on a fast host and a slow one. min/max are raw microseconds
       and will move; they are named apart for that reason. */
    /* Cumulative across the boot: telemetry-driven samples are as real as
       the scenario's, and throwing them away would flatter the numbers. */
    kv("latency_samples_total", lat->count);
    kv("latency_p50_us_lt", robot_latency_percentile(500));
    kv("latency_p95_us_lt", robot_latency_percentile(950));
    kv("latency_p99_us_lt", robot_latency_percentile(990));
    kv("latency_jitter_us_lt", robot_latency_percentile(950) -
                               robot_latency_percentile(500));
    kv("latency_min_us_raw", lat->count ? lat->min_us : 0);
    kv("latency_max_us_raw", lat->max_us);
    kv("latency_mean_us_raw", lat->count ? lat->sum_us / lat->count : 0);

    /* --- tasks and stacks ------------------------------------------------ */
    kv("tasks", (uint64_t)scheduler_task_count());
    kv("context_switches", sched_switches());
    kv("stack_guards_intact", sched_stacks_intact() ? 1 : 0);
    kv("stack_worst_used_bytes", (uint64_t)sched_stack_worst_used());
    ks("stack_worst_task", sched_stack_worst_task());

    /* --- the link, if one was ever opened -------------------------------- */
    if (mavbridge_running() || mavbridge_stats()->rx_frames) {
        const mavbridge_stats_t *mb = mavbridge_stats();
        const mavlink_rx_stats_t *rx = mavbridge_rx_stats();
        kv("link_open", mavbridge_running() ? 1 : 0);
        kv("link_up", mb->link_up ? 1 : 0);
        kv("link_drops", mb->link_drops);
        kv("mav_rx_frames", mb->rx_frames);
        kv("mav_tx_frames", mb->tx_frames);
        kv("mav_heartbeats_rx", mb->heartbeats_rx);
        kv("mav_command_acks", mb->command_acks);
        kv("mav_sensors_produced", mb->sensors_produced);
        kv("mav_intents_sent", mb->intents_sent);
        kv("mav_crc_errors", rx->crc_errors);
        kv("mav_bad_stx", rx->bad_stx);
        kv("mav_rejected_incompat", rx->rejected_incompat);
        kv("mav_seq_gaps", rx->seq_gaps);
        kv("mav_packets_lost", rx->lost_estimate);
        kv("mav_undecodable", rx->unknown_msgid);
        kv("mav_senders", rx->senders);
    } else {
        kv("link_open", 0);
    }

    /* The boot-time result, not a fresh run: see seb.c. */
    kv("seb_selftest_at_boot", (uint64_t)(seb_selftest_at_boot() > 0 ? 1 : 0));
    console_write("--- end\n");
}
