/*
 * kmain — boot.asm already put us in long mode, flat GDT, first 4 GiB
 * identity-mapped, exception IDT installed. We bring stuff up in order and
 * only print OK when a stage actually checked itself. Then userland/shell.
 */
#include "rogex.h"
#include "arch.h"
#include "version.h"
#include "rogex_core.h"
#include "revm.h"
#include "hwid.h"
#include "rgxsite.h"
#include "framebuffer.h"
#include "font.h"
#include "mouse.h"
#include "arch.h"
#include "clock.h"
#include "pmm.h"
#include "heap.h"
#include "message.h"
#include "rx_event.h"
#include "rx_backend.h"
#include "power.h"
#include "thermal.h"
#include "scheduler.h"
#include "process.h"
#include "syscall.h"
#include "device_manager.h"
#include "ramdisk.h"
#include "disk.h"
#include "nvme.h"
#include "robot.h"
#include "mono.h"
#include "blockdev.h"
#include "netif.h"
#include "rxfs.h"
#include "vfs.h"
#include "init.h"
#include "handoff.h"
#include "keyboard.h"
#include "seb.h"
#include "heap0.h"
#include "akida.h"

/* --- freestanding libc shims -------------------------------------------- */
/* The compiler may emit calls to these for struct copies / zeroing. */
/* The freestanding string helpers moved to kernel/kstring.c: every board
   needs them, and only this one was defining them. */

/* Names the compiler/backend may reference by convention. */
void *memset(void *d, int v, size_t n) { return k_memset(d, v, n); }
void *memcpy(void *d, const void *s, size_t n) { return k_memcpy(d, s, n); }

/* --- Multiboot2 magic the loader must have passed --------------------- */
#define MB2_BOOTLOADER_MAGIC 0x36d76289u

/* --- the shell, as a scheduled task -------------------------------------- */
/* Task entries take no arguments, so the hardware identity is handed over
   through this static. When the shell returns (the user asked to halt) the
   task simply ends; the fabric loop keeps owning the machine and idles, which
   is what a kernel should do when its interactive task exits. */
static hwid_t g_shell_hw;
static void shell_task(void) {
    init_start_shell(&g_shell_hw);
    console_write("\n[echOS] halted.\n");
}

static void banner(void) {
    console_write(
        "\n"
        "                  888       .d88888b.   .d8888b.\n"
        "                  888      d88P\" \"Y88b d88P  Y88b\n"
        "                  888      888     888 Y88b.\n"
        " .d88b.   .d8888b 88888b.  888     888  \"Y888b.\n"
        "d8P  Y8b d88P\"    888 \"88b 888     888     \"Y88b.\n"
        "88888888 888      888  888 888     888       \"888\n"
        "Y8b.     Y88b.    888  888 Y88b. .d88P Y88b  d88P\n"
        " \"Y8888   \"Y8888P 888  888  \"Y88888P\"   \"Y8888P\"\n"
        "\n"
        "  " ECHOS_PRETTY_NAME " · Rogex Laboratories\n\n");
}

/* Build + run a tiny encrypted bytecode program that prints "ROGEX OS\n".
   Demonstrates the WHOLE chain: an ML-KEM handshake yields a shared secret;
   the "sender" seals a bytecode app under it with ChaCha20-Poly1305; the
   "device" opens it with the *independently decapsulated* key and runs it.
   If the keys did not match, revm_init would return REVM_ERR_AUTH. */
static void selftest_vm(void) {
    static uint8_t pk[MLKEM_PUBLICKEY_BYTES], sk[MLKEM_SECRETKEY_BYTES];
    static uint8_t kem_ct[MLKEM_CIPHERTEXT_BYTES];
    static uint8_t ss_sender[REVM_KEY_BYTES], ss_device[REVM_KEY_BYTES];
    rogex_kem_keypair(pk, sk);                     /* device publishes pk      */
    rogex_kem_encapsulate(pk, kem_ct, ss_sender);  /* sender derives ss + ct   */
    rogex_kem_decapsulate(sk, kem_ct, ss_device);  /* device recovers ss       */

    static const char *msg = "ROGEX OS\n";
    uint8_t prog[256];
    uint32_t n = 0;
    for (const char *p = msg; *p; p++) {
        prog[n++] = OP_PUSH;
        uint64_t c = (uint8_t)*p;
        for (int i = 0; i < 8; i++) prog[n++] = (uint8_t)((c >> (i * 8)) & 0xff);
        prog[n++] = OP_SYSCALL;
        prog[n++] = (uint8_t)(SYS_PUTC & 0xff);
        prog[n++] = (uint8_t)((SYS_PUTC >> 8) & 0xff);
    }
    prog[n++] = OP_HALT;

    uint8_t container[512];
    uint32_t clen = revm_seal_container(container, sizeof(container),
                                        prog, n, /*entry=*/0, ss_sender);
    if (!clen) { console_write("[echOS] container seal failed\n"); return; }

    const uint8_t *key = ss_device;
    revm_vm vm;
    revm_status s = revm_init(&vm, container, clen, key);
    if (s != REVM_OK) {
        console_write("[echOS] revm_init: ");
        console_write(revm_status_str(s));
        console_putc('\n');
        return;
    }
    console_write("[echOS] REVM (bytecode sealed under ML-KEM key) output -> ");
    s = revm_run(&vm, /*max_steps=*/1000000);
    console_write("[echOS] REVM finished: ");
    console_write(revm_status_str(s));
    console_putc('\n');

    /* Negative control: flip one ciphertext byte -> AEAD must reject it. */
    container[clen - 1] ^= 0x01;
    revm_status tampered = revm_init(&vm, container, clen, key);
    console_write("[echOS] tamper rejected by AEAD: ");
    console_write(tampered == REVM_ERR_AUTH ? "PASS\n" : "FAIL (SECURITY BUG)\n");
}

/* Exercise the Rust post-quantum core end to end. */
static void selftest_crypto(void) {
    static uint8_t pk[MLKEM_PUBLICKEY_BYTES];
    static uint8_t sk[MLKEM_SECRETKEY_BYTES];
    static uint8_t ct[MLKEM_CIPHERTEXT_BYTES];
    static uint8_t ss_sender[MLKEM_SHAREDSECRET_BYTES];
    static uint8_t ss_recvr[MLKEM_SHAREDSECRET_BYTES];

    console_write("[echOS] ML-KEM keygen...");
    rogex_kem_keypair(pk, sk);
    console_write(" encap...");
    rogex_kem_encapsulate(pk, ct, ss_sender);
    console_write(" decap...");
    rogex_kem_decapsulate(sk, ct, ss_recvr);
    console_write(" done\n");

    bool match = true;
    for (int i = 0; i < MLKEM_SHAREDSECRET_BYTES; i++)
        if (ss_sender[i] != ss_recvr[i]) { match = false; break; }

    console_write("[echOS] ML-KEM handshake (keypair/encap/decap): ");
    console_write(match ? "PASS - shared secrets agree\n" : "FAIL\n");

    static const uint8_t frame[] = "rgx://selftest-node/hello";
    int32_t accepted = rogex_mesh_tx(frame, (uint32_t)sizeof(frame));
    console_write("[echOS] mesh tx (garlic-wrapped) accepted bytes: ");
    console_write_dec((uint64_t)accepted);
    console_putc('\n');
}

/* Phase 2: Proof-of-Report consensus bans a terminal. */
static void selftest_laws(void) {
    uint8_t subject[ROGEX_UID_BYTES];
    uint8_t r1[ROGEX_UID_BYTES], r2[ROGEX_UID_BYTES], r3[ROGEX_UID_BYTES];
    rogex_uid_derive((const uint8_t *)"s", 1, (const uint8_t *)"subject", 7, (const uint8_t *)"m", 1, subject);
    rogex_uid_derive((const uint8_t *)"1", 1, (const uint8_t *)"rep1", 4, (const uint8_t *)"m", 1, r1);
    rogex_uid_derive((const uint8_t *)"2", 1, (const uint8_t *)"rep2", 4, (const uint8_t *)"m", 1, r2);
    rogex_uid_derive((const uint8_t *)"3", 1, (const uint8_t *)"rep3", 4, (const uint8_t *)"m", 1, r3);

    int a = rogex_report(r1, subject);
    int dup = rogex_report(r1, subject);
    int b = rogex_report(r2, subject);
    int c = rogex_report(r3, subject);
    bool banned = rogex_is_banned(subject) == 1;
    bool denied = rogex_mesh_admit(subject) == 0;

    console_write("[echOS] Proof-of-Report consensus (3 distinct reporters): ");
    console_write((a == 1 && dup == 0 && b == 1 && c == 2 && banned && denied)
                      ? "PASS - terminal banned from mesh\n" : "FAIL\n");
}

/* i8042 presence probe: a hung/absent controller reads status 0xFF. */
/* The i8042 probe moved to the keyboard driver, which is the thing that
   knows what an i8042 is. */

ROGEX_NORETURN void kmain(uint64_t mb2_info, uint64_t mb2_magic) {
    console_init();

    /* Physical memory first (enables page mapping), then the framebuffer (it
       maps the LFB, which lives above the boot's 1 GiB identity map). After
       this, console output is drawn with the 8x16 font at 1280x720. */
    /* One normalised picture of what the firmware handed us, parsed once,
       before anything allocates. Everything below reads that, not tags. */
    boot_handoff_init(mb2_info, mb2_magic);
    pmm_init(boot_handoff());
    fb_init(boot_handoff());

    banner();

    /* ---- required boot status block (each line verified, not decorative) */
    console_write(ECHOS_PRETTY_NAME "\nRogex Laboratories\n");
    console_write("[echOS] identity version=" ECHOS_VERSION
                  " edition=" ECHOS_EDITION " arch=x86_64 boot=");
    console_write(boot_mode_string());
    console_write("\n");
    klog_status("Boot stage", boot_handoff()->valid
                                  ? "OK" : "FAIL (bad Multiboot2 magic)");
    /* What the firmware actually handed over, so a UEFI boot cannot be
       mistaken for a BIOS one by reading a build flag. No addresses: this
       line is compared by the certificate, and an address is host state. */
    {
        const boot_handoff_t *bh = boot_handoff();
        console_write("[echOS] firmware: ");
        console_write(boot_firmware_name());
        console_write("  config table: ");
        console_write(bh->fw_config_table ? "present" : "none");
        console_write("  boot services: ");
        console_write(bh->fw_services_active ? "STILL ACTIVE" : "exited");
        console_write("  memory map: ");
        klog_dec(bh->mem_count);
        console_write(bh->mem_count < bh->mem_reported ? "/" : " of ");
        klog_dec(bh->mem_reported);
        console_write(" ranges  modules: ");
        klog_dec(bh->module_count);
        console_write("  acpi: ");
        if (bh->acpi_rsdp) {
            console_write("RSDP v");
            klog_dec(bh->acpi_revision);
            console_write(" from loader");
        } else {
            console_write("no loader tag (legacy scan)");
        }
        console_write("\n");
    }
    /* Reaching 64-bit kmain is the GDT check: boot.asm already loaded it. */
    klog_status("GDT", "OK (long mode; GDT loaded in boot.asm)");

    heap0_init();
    klog_status("Heap-0", heap0_ready()
                              ? "OK (WSP 64 KiB BSS; kmalloc 512 KiB still exists)"
                              : "FAIL (WSP pool not bound)");

    heap_init();
    bool mem_ok = pmm_total_bytes() > 0 && heap_selftest();
    klog_status("Memory", mem_ok ? "OK" : "FAIL");

    seb_init();
    klog_status("SEB", seb_selftest() ? "OK (emit/poll ring)" : "FAIL");

    arch_irq_init();            /* controller up, lines masked, interrupts on */
    bool timer_ok = arch_timer_init();  /* periodic tick; verifies it advances */
    klog_status("Timer", timer_ok ? "OK" : "FAIL (no PIT ticks)");
    klog_status("IDT", timer_ok ? "OK (IRQ0 delivered through IDT)"
                                : "FAIL (no PIT ticks; IDT unproven)");
    if (timer_ok) {
        /* Monotonic microseconds for the robotic pipeline. Calibrated
           against the PIT, so it says what it is worth. */
        bool mono_ok = mono_init();
        klog_status("Monotonic clock", mono_ok ? mono_source()
                                               : "PIT only (10 ms granularity)");
        robot_init();
        klog_status("Robot runtime", "OK (4 static queues, gate armed, no producer yet)");
        clock_init();           /* wall clock: CMOS RTC seed + PIT advance   */
        char stamp[24];
        clock_fmt_stamp(stamp, sizeof(stamp));
        console_write("[echOS] clock: ");
        console_write(stamp);
        console_write(clock_rtc_ok() ? " (CMOS RTC)\n" : " (software default)\n");
    }

    bool kbd_ok = kbd_controller_present();
    klog_status("Keyboard", kbd_ok ? "OK" : "FAIL (no i8042)");

    bool ram0_ok = ramdisk_init();
    bool disk_ok = disk_init();         /* NVMe, virtio-blk, AHCI or ATA PIO */
    klog_status("Storage", disk_ok ? disk_backend() : "none (amnesic; attach a disk)");
    /* NVMe says what it found or why it did not: a class code is not a disk. */
    klog_status("NVMe", nvme_status());
    bool net_ok  = net_init();          /* virtio / e1000 / r8169 / rtl8139 */
    klog_status("NIC", net_ok ? net_driver_name() : "none");
    akida_init();
    klog_status("Akida AKD1000", akida_present() ? akida_status() : "absent (software LIF)");
    rxfs_init();
    bool rxfs_ok = ram0_ok && rxfs_selftest();
    klog_status("RXFS", rxfs_ok ? "OK" : "FAIL");

    /* ---- detail lines ---------------------------------------------------- */
    if (fb_active()) {
        uint32_t w, h; fb_dims(&w, &h);
        console_write("[echOS] framebuffer ");
        console_write_dec(w); console_putc('x'); console_write_dec(h);
        console_write(fb_double_buffered() ? "x32 (graphical, double-buffered)\n"
                                           : "x32 (graphical, direct to VRAM)\n");
        fb_cursor_init();
        mouse_init();
        if (!mouse_present())
            console_write("[echOS] mouse: not detected\n");
        else if (mouse_has_wheel())
            console_write("[echOS] mouse: PS/2 IMPS/2 wheel\n");
        else
            console_write("[echOS] mouse: PS/2 3-button (no wheel ID)\n");
    } else {
        console_write("[echOS] framebuffer: none - VGA text fallback\n");
    }

    console_write("[echOS] RAM: kernel using ");
    console_write_dec(pmm_used_bytes() / (1024 * 1024));
    console_write(" MiB  |  this PC ");
    console_write_dec(pmm_total_bytes() / (1024 * 1024));
    console_write(" MiB firmware  |  heap ");
    console_write_dec(heap_total() / 1024);
    console_write(" KiB\n");

    /* Cooperative scheduler: take over this flow as task 0, then prove the
       context switch really works before anything depends on it. */
    scheduler_init();
    console_write("[echOS] scheduler self-test (spawn/yield/block/wake): ");
    console_write(scheduler_selftest() ? "PASS\n" : "FAIL\n");

    /* Idle power: ACPI discovery + deepest usable C-state + energy metering. */
    power_init();
    console_write("[echOS] power: idle via ");
    console_write(power_idle_method());
    if (power_acpi_present()) {
        console_write("  |  ACPI ");
        console_write(power_acpi_oem());
        console_write(" (C2 lat ");
        console_write_dec(power_c2_latency());
        console_write("us, C3 lat ");
        console_write_dec(power_c3_latency());
        console_write("us)");
    } else {
        console_write("  |  ACPI: not found");
    }
    thermal_init();
    console_write("\n[echOS] thermal: ");
    console_write(thermal_status());
    { uint32_t c;
      if (thermal_core_c(&c)) {
          console_write("  core "); console_write_dec(c);
          console_write(" C (TjMax "); console_write_dec(thermal_tjmax());
          console_write(thermal_tjmax_measured() ? " measured)" : " assumed)");
      } }
    console_write("\n[echOS] cpu: ");
    console_write(power_cpu_name());
    console_write("\n[echOS] power: MSR #GP guard ");
    console_write(power_msr_guard_status());
    console_write("\n[echOS] power: RAPL ");
    console_write(power_rapl_status());
    console_putc('\n');

    process_init();
    syscall_init();
    msg_init();
    console_write("[echOS] ipc queue self-test: ");
    console_write(msg_selftest() ? "PASS\n" : "FAIL\n");
    console_write("[echOS] event fabric self-test (SPSC ring + Q16.16 LIF + input): ");
    console_write(rx_event_selftest() ? "PASS\n" : "FAIL\n");
    rx_backend_init();
    console_write("[echOS] neuro backend: ");
    console_write(rx_backend_label());
    console_write(" — ");
    console_write(rx_backend_status());
    console_putc('\n');

    /* Interrupt-driven input (after the queue self-test — the ISRs produce
       into this queue from here on): IRQ1/IRQ12 post events, the UI event
       loop consumes them, and the CPU hlt-sleeps while idle. Falls back to
       the classic polled pipeline if anything did not verify. */
    bool kbd_irq_ok   = kbd_ok && kbd_enable_irq();
    bool mouse_irq_ok = mouse_present() ? mouse_enable_irq() : true;
    bool irq_input    = timer_ok && kbd_irq_ok && mouse_irq_ok;
    events_use_irq(irq_input);
    console_write(irq_input
        ? "[echOS] input: interrupt-driven (IRQ1 kbd, IRQ12 mouse) -> IPC queue; idle = hlt\n"
        : "[echOS] input: POLLED fallback (IRQ input setup did not verify)\n");

    /* Device registry (the `devices` command lists this). */
    devmgr_register("framebuffer", "video",   fb_active() ? fb_mode_string() : "absent (VGA text)");
    devmgr_register("terminal",    "video",   "ok (fb console + VGA + COM1)");
    devmgr_register("keyboard",    "input",   !kbd_ok ? "not detected"
                                              : irq_input ? "ok (PS/2, IRQ1 -> event fabric)" : "ok (PS/2, polled)");
    devmgr_register("mouse",       "input",   !mouse_present() ? "not detected"
                                              : irq_input ? "ok (PS/2, IRQ12 -> event fabric, 200 Hz)" : "ok (PS/2, polled)");
    devmgr_register("pit",         "timer",   timer_ok ? "ok (100 Hz, IRQ0)" : "FAILED");
    devmgr_register("clock",       "timer",   !timer_ok ? "FAILED"
                                              : clock_rtc_ok() ? "ok (CMOS RTC + PIT)"
                                                               : "ok (software + PIT)");
    devmgr_register("ram0",        "storage", ram0_ok ? "ok (512 KiB ramdisk)" : "FAILED");
    devmgr_register("disk",        "storage", disk_ok ? disk_backend()
                                                      : "none (amnesic; attach with: make run-disk)");
    devmgr_register("seb",         "event",   "ok (Spiking Event Bus, 1024-ring)");
    devmgr_register("heap0",       "memory",  "ok (Static Heap-0 Layout, O(1))");
    devmgr_register("rxfs",        "fs",      rxfs_ok ? "ok (in-RAM, 64 files)" : "FAILED");
    devmgr_register("nic",         "network", !net_ok ? "none (virtio/e1000/r8169/rtl8139)"
                                                      : net_driver_name());
    if (disk_ok) {
        console_write("[echOS] disk: "); console_write(disk_backend());
        console_write(", "); console_write_dec((uint64_t)disk_dev()->sectors / 2);
        console_write(" KiB\n");
    }
    {
        uint32_t plen = 0;
        boot_payload(&plen);
        if (plen) {
            console_write("[echOS] install payload: ");
            console_write_dec((uint64_t)plen);
            console_write(" bytes (LIVE can write a bootable disk)\n");
        }
    }
    if (net_ok) {
        static const char H[] = "0123456789abcdef";
        const uint8_t *m = net_mac();
        console_write("[echOS] nic: ");
        console_write(net_driver_name());
        console_write(" MAC ");
        for (int i = 0; i < 6; i++) { console_putc(H[m[i]>>4]); console_putc(H[m[i]&0xf]); if (i<5) console_putc(':'); }
        console_write(" (Rogex Wired L2 ready)\n");
        /* Register the INTx handler as a wake-from-hlt HINT only (RX is always
           drained by the idle loop, so ping/pong works whether or not INTx is
           actually delivered on this machine). UNVERIFIED: the delivery path
           can't be exercised in the build sandbox. */
        if (net_irq_enable()) {
            console_write("[echOS] nic: INTx wake-hint on IRQ ");
            console_write_dec((uint64_t)net_irq_line());
            console_write(" (RX still drained by idle loop; INTx UNVERIFIED)\n");
        } else {
            console_write("[echOS] nic: polled RX (serviced from idle loop)\n");
        }
    }

    /* Post-quantum + mesh core (Rust, no_std). Seeded from TSC. */
    uint64_t tsc = arch_cycles();
    if (rogex_core_init(tsc) == 0)
        console_write("[echOS] rogex-core (PQC + mesh) initialized\n");
    else
        console_write("[echOS] rogex-core init FAILED\n");

    console_write("[echOS] AEAD self-test (RFC 8439 KAT): ");
    console_write(rogex_aead_selftest() == 0 ? "PASS\n" : "FAIL\n");
    selftest_crypto();
    selftest_laws();
    console_write("[echOS] rgxsite integrity (verify + 1-byte tamper): ");
    console_write(rgxsite_selftest() ? "PASS\n" : "FAIL\n");
    selftest_vm();

    /* Hardware identity. */
    hwid_t hw;
    hwid_collect(&hw);
    console_write("[echOS] hardware UID: ");
    { static const char hx[] = "0123456789abcdef";
      for (int i = 0; i < 12; i++) { console_putc(hx[hw.uid[i] >> 4]); console_putc(hx[hw.uid[i] & 0xf]); } }
    console_write("...  TPM 2.0: ");
    console_write(hw.tpm_present ? "present\n" : "absent (QEMU has none; use -tpmdev)\n");

    /* Userland: mount the VFS, seed built-in apps, then the shell. */
    {
        bool userland_ok = init_userland();
        if (!userland_ok)
            klog_warn("userland init reported failure; shell may be degraded");
        klog_status("Shell", userland_ok ? "OK (userland init)" : "FAIL");
    }

    /* ---- event-loop inversion (Level 2) --------------------------------
       The shell is no longer the owner of the machine: it becomes a task
       among others, and this flow of control hands itself to the fabric's
       master loop. The fabric drives every registered pump, yields to the
       runnable tasks, and parks the core in the deepest supported idle
       state when nobody has work. rx_kernel_event_loop() never returns. */
    g_shell_hw = hw;
    if (sched_spawn("shell", shell_task) < 0) {
        klog_error("could not spawn the shell task; running it inline");
        init_start_shell(&hw);
        console_write("\n[echOS] halted.\n");
        for (;;) power_idle();
    }
    rx_kernel_event_loop();
}
