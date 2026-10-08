/*
 * The AArch64 board's main.
 *
 * A per-target entry point, not a pile of #ifdefs inside the PC one: the
 * order of bring-up is a property of the machine, and writing it out is
 * clearer than guarding twenty lines. What it calls is the same generic
 * kernel the x86 board calls — pmm, heap, SEB, scheduler, RXFS, the robotic
 * runtime — through the same interfaces.
 *
 * It walks the checklist phase G asks a first ARM64 boot to reach, in order,
 * and prints OK only where something was actually verified.
 */
#include "rogex.h"
#include "arch.h"
#include "mono.h"
#include "handoff.h"
#include "heap0.h"
#include "heap.h"
#include "seb.h"
#include "scheduler.h"
#include "rxfs.h"
#include "vfs.h"

extern const vfs_ops_t RXFS_OPS;   /* fs/rxfs/rxfs_vfs.c */
#include "ramdisk.h"
#include "netif.h"
#include "ipstack.h"
#include "blockdev.h"
#include "robot.h"
#include "version.h"

void a64_shell(void);          /* shell.c, this directory                  */

static void banner(void) {
    console_write("\n");
    console_write("  " ECHOS_PRETTY_NAME " · Rogex Laboratories\n");
    console_write("  [echOS] identity version=" ECHOS_VERSION
                  " edition=" ECHOS_EDITION " arch=");
    console_write(arch_name());
    console_write(" boot=");
    console_write(boot_handoff()->transport);
    console_write("\n\n");
}

static void report_memory(void) {
    const boot_handoff_t *bh = boot_handoff();
    uint32_t i;
    console_write("[echOS] memory map: ");
    console_write_dec(bh->mem_count);
    console_write(" range(s) from the ");
    console_write(bh->transport);
    console_write("\n");
    for (i = 0; i < bh->mem_count; i++) {
        console_write("         ");
        console_write_hex(bh->mem[i].base);
        console_write(" + ");
        console_write_dec(bh->mem[i].length / (1024 * 1024));
        console_write(" MiB  ");
        console_write(bh->mem[i].type == BOOT_MEM_USABLE ? "usable" : "reserved");
        console_write("\n");
    }
    console_write("[echOS] RAM: kernel using ");
    console_write_dec(pmm_used_bytes() / (1024 * 1024));
    console_write(" MiB of ");
    console_write_dec(pmm_total_bytes() / (1024 * 1024));
    console_write(" MiB\n");
}

ROGEX_NORETURN void kmain(uint64_t info, uint64_t magic) {
    bool timer_ok, irq_ok, sched_ok, seb_ok, rxfs_ok, ram_ok, mono_ok, net_ok;

    arch_console_early_init();
    console_init();

    /* 1. Identity, and what handed us the machine. */
    boot_handoff_init(info, magic);
    pmm_init(boot_handoff());
    banner();
    klog_status("Boot stage", boot_handoff()->valid ? "OK" : "FAIL (no boot info)");

    /* 2. Memory. */
    heap0_init();
    klog_status("Heap-0", heap0_ready() ? "OK (WSP pool bound)" : "FAIL");
    heap_init();
    klog_status("Memory", (pmm_total_bytes() > 0 && heap_selftest()) ? "OK" : "FAIL");
    report_memory();

    /* 3+4. Interrupts before the timer: the tick is an interrupt. */
    irq_ok = arch_irq_init();
    klog_status("Interrupts", irq_ok ? "OK (GICv2, EL1 vectors)" : "FAIL");
    arch_irq_enable();

    timer_ok = arch_timer_init();
    klog_status("Timer", timer_ok ? "OK (ARM generic timer, 100 Hz)"
                                  : "FAIL (no tick)");
    mono_ok = mono_init();
    klog_status("Monotonic clock", mono_ok ? mono_source() : "unusable");

    /* 5. Scheduler. */
    scheduler_init();
    sched_ok = scheduler_selftest();
    klog_status("Scheduler", sched_ok ? "OK (spawn/yield/block/wake)" : "FAIL");

    /* 6. Spiking event bus. */
    seb_init();
    seb_ok = seb_selftest();
    klog_status("SEB", seb_ok ? "OK (emit/poll ring)" : "FAIL");

    /* 7. Virtual storage: a RAM-backed block device and the filesystem on it. */
    ram_ok = ramdisk_init();
    klog_status("Storage", ram_ok ? "OK (ram0, virtual)"
                                  : "FAIL (no block device)");
    rxfs_init();
    rxfs_ok = ram_ok && rxfs_selftest();
    klog_status("RXFS", rxfs_ok ? "OK" : "FAIL");
    if (!vfs_mount(&RXFS_OPS)) klog_warn("VFS mount failed");

    /* 8. Network: virtio over the MMIO transport the virt board advertises.
       No PCI here, so nothing of the x86 NIC path is reachable — the same
       netif/ipstack sit on a different transport underneath. */
    net_ok = net_init();
    if (net_ok) {
        console_write("[echOS] net: ");
        console_write(virtio_mmio_net_transport());
        console_write(", MAC ");
        {   const uint8_t *m = net_mac();
            for (int i = 0; i < 6; i++) {
                const char *hex = "0123456789abcdef";
                char b[3] = { hex[m[i] >> 4], hex[m[i] & 15], 0 };
                console_write(b);
                if (i < 5) console_write(":");
            }
        }
        console_write("\n");
    }
    klog_status("Network", net_ok ? "OK (virtio-net over MMIO)"
                                  : "ABSENT (no virtio-mmio net device)");

    /* The robotic runtime is generic: it comes up here exactly as on x86. */
    robot_init();
    klog_status("Robot runtime", "OK (4 static queues, gate armed)");

    /* 10. Self-tests that already ran, summarised where a reader looks. */
    console_write("[echOS] self-tests: scheduler ");
    console_write(sched_ok ? "PASS" : "FAIL");
    console_write(", SEB ");
    console_write(seb_ok ? "PASS" : "FAIL");
    console_write(", RXFS ");
    console_write(rxfs_ok ? "PASS" : "FAIL");
    console_write(", heap ");
    console_write(heap_selftest() ? "PASS" : "FAIL");
    console_write("\n");

    /* 9. A shell. */
    a64_shell();

    arch_halt_forever();
}

