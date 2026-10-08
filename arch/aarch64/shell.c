/*
 * A diagnostic shell for the AArch64 board.
 *
 * This is **not** the 2.1 shell. That one is 150 commands built on a
 * framebuffer console, a package system and a PC's drivers; none of that is
 * ported. What a first ARM64 boot needs is a way to ask the machine whether
 * the things the checklist claims are really there — so every command here
 * reads live state and nothing is a canned string.
 */
#include "rogex.h"
#include "arch.h"
#include "mono.h"
#include "handoff.h"
#include "seb.h"
#include "scheduler.h"
#include "rxfs.h"
#include "vfs.h"
#include "heap0.h"
#include "heap.h"
#include "robot.h"
#include "report.h"
#include "version.h"
#include "netif.h"
#include "ipstack.h"
#include "mavbridge.h"

extern uint32_t gic_spurious(void);
extern uint64_t gic_taken(void);

static void line_prompt(void) { console_write("echos(aarch64)# "); }

static bool eq(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

static void cmd_status(void) {
    const boot_handoff_t *bh = boot_handoff();
    console_write("arch        "); console_write(arch_name());
    console_write("\ntransport   "); console_write(bh->transport);
    console_write("\nmemory      ");
    console_write_dec(pmm_used_bytes() / (1024 * 1024));
    console_write(" MiB used of ");
    console_write_dec(pmm_total_bytes() / (1024 * 1024));
    console_write(" MiB\nheap        ");
    console_write_dec(heap_used());
    console_write(" B of ");
    console_write_dec(heap_total());
    console_write(" B\nticks       ");
    console_write_dec(arch_timer_ticks());
    console_write(" at ");
    console_write_dec(arch_timer_hz());
    console_write(" Hz\nuptime      ");
    console_write_dec(arch_timer_uptime_ms());
    console_write(" ms (timer)  ");
    console_write_dec(mono_us() / 1000);
    console_write(" ms (monotonic)\nclock       ");
    console_write(mono_source());
    console_write("\ninterrupts  taken ");
    console_write_dec(gic_taken());
    console_write(", spurious ");
    console_write_dec(gic_spurious());
    console_write("\ntasks       ");
    console_write_dec((uint64_t)scheduler_task_count());
    console_write(", switches ");
    console_write_dec(sched_switches());
    console_write("\nfiles       ");
    console_write_dec((uint64_t)vfs_count());
    console_write(" in rxfs\n");
}

static void cmd_seb(void) {
    seb_stats_t st;
    seb_get_stats(&st);
    console_write("spikes      "); console_write_dec(st.total_spikes);
    console_write("\noccupancy   "); console_write_dec(st.ring_occupancy);
    console_write("\noverruns    "); console_write_dec(st.ring_overruns);
    console_write("\nself-test   ");
    console_write(seb_selftest() ? "PASS\n" : "FAIL\n");
}

static void cmd_robot(void) {
    const robot_stats_t *st;
    const safety_stats_t *sf;
    uint32_t n = robot_scenario();
    st = robot_stats();
    sf = safety_stats();
    console_write("scenario    injected "); console_write_dec(n);
    console_write(" records\nrecords     accepted ");
    console_write_dec(st->sensors_in);
    console_write(", refused "); console_write_dec(st->sensors_rejected);
    console_write("\nintents     made "); console_write_dec(st->intents_made);
    console_write(", sent "); console_write_dec(st->sent);
    console_write("\ngate        seen "); console_write_dec(sf->seen);
    console_write(", accepted "); console_write_dec(sf->accepted);
    console_write(", MODIFY "); console_write_dec(sf->modified);
    console_write(", BLOCK "); console_write_dec(sf->blocked);
    /* The accounting has to close: every record injected was either taken
       at the door, refused there, or dropped by a full queue. Printing only
       the accepted count leaves eight records unexplained, and an
       unexplained number is where a bug hides. */
    console_write("\nqueues      sensor drops ");
    console_write_dec(robot_q_sensor()->drops);
    console_write(" (high water "); console_write_dec(robot_q_sensor()->high_water);
    console_write("), gate drops "); console_write_dec(robot_q_gate()->drops);
    console_write(", out drops "); console_write_dec(robot_q_out()->drops);
    console_write("\nbalance     "); console_write_dec(n);
    console_write(" injected = "); console_write_dec(st->sensors_in);
    console_write(" accepted + "); console_write_dec(st->sensors_rejected);
    console_write(" refused + "); console_write_dec(robot_q_sensor()->drops);
    console_write(" dropped\nexpired     ");
    console_write_dec(st->intents_expired);
    console_write("\nlatency     samples ");
    console_write_dec(robot_latency()->count);
    console_write(", p50 <"); console_write_dec(robot_latency_percentile(500));
    console_write(" us, p95 <"); console_write_dec(robot_latency_percentile(950));
    console_write(" us\n");
}

static void cmd_cpu(void) {
    arch_reg_t regs[16];
    uint32_t n = arch_cpu_state(regs, 16), i;
    char vendor[13];
    arch_cpu_vendor(vendor);
    console_write("vendor      "); console_write(vendor);
    console_write("\n");
    for (i = 0; i < n; i++) {
        console_write("  ");
        console_write(regs[i].name);
        console_write("  ");
        console_write_hex(regs[i].value);
        console_write("\n");
    }
}

static void cmd_net(void) {
    const ip_cfg_t *c;
    char buf[20];
    uint32_t rtt = 0;

    if (!net_present()) { console_write("no NIC bound\n"); return; }
    console_write("driver      "); console_write(net_driver_name());
    console_write("\ntransport   "); console_write(virtio_mmio_net_transport());
    console_write("\naddress     ");
    if (!ipstack_configured()) {
        console_write("none yet, asking DHCP...\n");
        if (!ip_dhcp(4000)) { console_write("DHCP        no answer\n"); return; }
        console_write("address     ");
    }
    c = ipstack_cfg();
    ip_fmt(c->ip, buf, sizeof(buf));   console_write(buf);
    console_write(" mask ");
    ip_fmt(c->mask, buf, sizeof(buf)); console_write(buf);
    console_write(" gw ");
    ip_fmt(c->gw, buf, sizeof(buf));   console_write(buf);
    console_write("\ngateway     ");
    if (ip_icmp_ping_us(c->gw, 2000, &rtt)) {
        console_write("replies, rtt ");
        console_write_dec(rtt);
        console_write(" us\n");
    } else {
        console_write("no ICMP reply\n");
    }
}

/* The PX4 link, on the arm board, through the same bridge x86 uses: the
   point of the port is that nothing above the transport changed. The
   sub-commands match the x86 `px4` command so one script can drive both. */
/* Losses seen *during* a pump, kept apart from the running totals: between
   two commands the shell is blocked reading a key and nothing drains the
   NIC, so PX4's stream overruns the ring and the cumulative gap count says
   more about the prompt than about the link. */
static uint32_t g_gaps0, g_lost0, g_rx0;

static void mav_mark(void) {
    const mavlink_rx_stats_t *rx = mavbridge_rx_stats();
    g_gaps0 = rx->seq_gaps; g_lost0 = rx->lost_estimate; g_rx0 = rx->frames;
}

static void mav_report(void) {
    const mavbridge_stats_t *st = mavbridge_stats();
    const mavlink_rx_stats_t *rx = mavbridge_rx_stats();
    console_write("link        ");
    console_write(st->link_up ? "up" : "down");
    console_write("\nframes      rx "); console_write_dec(st->rx_frames);
    console_write(", tx "); console_write_dec(st->tx_frames);
    console_write("\nheartbeats  rx "); console_write_dec(st->heartbeats_rx);
    console_write(", tx "); console_write_dec(st->heartbeats_tx);
    console_write("\npeer        sysid "); console_write_dec(st->peer_sysid);
    console_write(", compid "); console_write_dec(st->peer_compid);
    console_write("\nsensors     "); console_write_dec(st->sensors_produced);
    console_write("\nintents     "); console_write_dec(st->intents_sent);
    console_write("\nacks        "); console_write_dec(st->command_acks);
    console_write(", last result "); console_write_dec(st->last_ack_result);
    console_write("\ndrops       "); console_write_dec(st->link_drops);
    console_write("\ncrc_errors  "); console_write_dec(rx->crc_errors);
    console_write("\nseq_gaps    "); console_write_dec(rx->seq_gaps);
    console_write(", lost "); console_write_dec(rx->lost_estimate);
    console_write("\nparser      bad_stx "); console_write_dec(rx->bad_stx);
    console_write(", unknown_id "); console_write_dec(rx->unknown_msgid);
    console_write(", incompat "); console_write_dec(rx->rejected_incompat);
    console_write("\nunverified  "); console_write_dec(rx->unverified_seq);
    console_write(" (ids");
    for (uint32_t k = 0; k < rx->unknown_ids_n; k++) {
        console_write(" "); console_write_dec(rx->unknown_ids[k]);
    }
    console_write(")");
    console_write("\nsenders     "); console_write_dec(rx->senders);
    console_write(", untracked "); console_write_dec(rx->senders_dropped);
    console_write("\nquieted     "); console_write_dec(mavbridge_quieted());
    /* Where a lost frame went: our own UDP ring counts what it could not
       take, so a sequence gap can be attributed instead of guessed at. */
    console_write("\nudp_ring    depth "); console_write_dec(ip_udp_ring_depth());
    console_write(", accepted "); console_write_dec(ip_udp_ring_accepted());
    console_write(", drops "); console_write_dec(ip_udp_ring_drops());
    {   uint32_t nf = 0, ns = 0;
        virtio_mmio_net_rx_health(&nf, &ns);
        console_write("\nnic_rx      frames "); console_write_dec(nf);
        console_write(", ring starved "); console_write_dec(ns);
    }
    console_write("\nthis_window rx "); console_write_dec(rx->frames - g_rx0);
    console_write(", gaps "); console_write_dec(rx->seq_gaps - g_gaps0);
    console_write(", lost "); console_write_dec(rx->lost_estimate - g_lost0);
    console_write("\n");
}

/* Pump the link for a wall-clock window. A spin count is the wrong unit:
   an empty poll returns in nanoseconds, so 20000 of them can pass without a
   1 Hz heartbeat ever having had the chance to arrive. */
static void mav_pump_ms(uint32_t ms) {
    uint64_t end = mono_us() + (uint64_t)ms * 1000u;
    while (mono_us() < end) { mavbridge_poll(); sched_yield(); }
}

/* The link is a task, not a command. A companion link that only runs while
   an operator is typing is not a link: PX4 streams whether or not anyone is
   looking, and a ring nobody drains overruns. This runs for as long as the
   bridge does, and the shell yields to it between keystrokes. */
static void mav_pump_task(void) {
    while (mavbridge_running()) {
        mavbridge_poll();
        sched_yield();
    }
}

static void cmd_mav(const char *arg) {
    uint8_t peer[4];

    if (eq(arg, "stop")) {
        mavbridge_stop();
        console_write("bridge stopped\n");
        return;
    }
    if (eq(arg, "start")) {
        if (!ipstack_configured() && !ip_dhcp(4000)) {
            console_write("no address; DHCP did not answer\n");
            return;
        }
        /* The host side of QEMU user networking: PX4 SITL runs outside the
           VM, so 10.0.2.2 is where it is, not an address inside the guest. */
        ip_parse("10.0.2.2", peer);
        if (!mavbridge_start(peer, 14580, 14540)) {
            console_write("bridge did not start\n");
            return;
        }
        if (sched_spawn("mavpump", mav_pump_task) < 0) {
            console_write("bridge started, but no task slot to pump it\n");
            return;
        }
        mav_pump_ms(3000);          /* let the stream requests be answered  */
        console_write("bridge started\n");
        return;
    }
    if (!mavbridge_running()) { console_write("bridge not started\n"); return; }
    /* Warm up before marking: the first frame of a window carries the whole
       gap that opened while the shell sat blocked on a keypress, and
       charging that to the link would misreport a prompt as packet loss. */
    mav_pump_ms(1000);
    mav_mark();
    if (eq(arg, "offboard")) {
        console_write(mavbridge_offboard() ? "offboard requested\n"
                                           : "offboard request failed\n");
        /* Give PX4 time to answer, and keep the stream alive while it does. */
        mav_pump_ms(2000);
    }
    mav_pump_ms(3000);
    mav_report();
}

static void cmd_help(void) {
    console_write(
        "status   what this board is doing right now\n"
        "cpu      registers this architecture actually has\n"
        "seb      spiking event bus counters and self-test\n"
        "robot    run the sensor -> intent -> safety gate scenario\n"
        "mem      memory ranges from the boot transport\n"
        "net      NIC, DHCP lease and a ping of the gateway\n"
        "report   the evidence block: every measured number, machine-readable\n"
        "mav      MAVLink 2 to PX4 on the host: mav start|offboard|stop\n"
        "ls       files in rxfs\n"
        "halt     stop the machine\n"
        " \n"
        "This is the aarch64 bring-up shell, not the 2.1 CLI: no packages\n"
        "and no framebuffer. What is absent says so.\n");
}

static void cmd_mem(void) {
    const boot_handoff_t *bh = boot_handoff();
    uint32_t i;
    for (i = 0; i < bh->mem_count; i++) {
        console_write("  ");
        console_write_hex(bh->mem[i].base);
        console_write("  ");
        console_write_dec(bh->mem[i].length / (1024 * 1024));
        console_write(" MiB  ");
        console_write(bh->mem[i].type == BOOT_MEM_USABLE ? "usable" : "reserved");
        console_write("\n");
    }
}

static void cmd_ls(void) {
    char names[16][64];
    int n = vfs_list(names, 16), i;
    for (i = 0; i < n; i++) {
        console_write("  ");
        console_write(names[i]);
        console_write("\n");
    }
    if (!n) console_write("  (empty)\n");
}

void a64_shell(void) {
    char line[80];
    console_write("\n[echOS] aarch64 bring-up shell. `help` lists what works.\n\n");
    for (;;) {
        int n = 0;
        line_prompt();
        for (;;) {
            char c = console_getc();
            if (c == '\r' || c == '\n') { console_write("\n"); break; }
            if ((c == 0x7f || c == '\b') && n > 0) {
                n--;
                console_write("\b \b");
                continue;
            }
            if (c >= 0x20 && c < 0x7f && n < (int)sizeof(line) - 1) {
                line[n++] = c;
                console_putc(c);
            }
        }
        line[n] = '\0';
        if (!n) continue;
        if (eq(line, "help"))        cmd_help();
        else if (eq(line, "status")) cmd_status();
        else if (eq(line, "cpu"))    cmd_cpu();
        else if (eq(line, "seb"))    cmd_seb();
        else if (eq(line, "robot"))  cmd_robot();
        else if (eq(line, "mem"))    cmd_mem();
        else if (eq(line, "net"))    cmd_net();
        else if (eq(line, "report")) report_emit();
        else if (eq(line, "mav"))    cmd_mav("");
        else if (line[0] == 'm' && line[1] == 'a' && line[2] == 'v' &&
                 line[3] == ' ')     cmd_mav(line + 4);
        else if (eq(line, "ls"))     cmd_ls();
        else if (eq(line, "halt")) {
            console_write("[echOS] halted.\n");
            arch_halt_forever();
        } else {
            console_write(line);
            console_write(": not in this bring-up shell (`help`)\n");
        }
    }
}
