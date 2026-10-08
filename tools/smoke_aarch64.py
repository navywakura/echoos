"""CERT-G: what the AArch64 board actually does, asked of the board itself.

Phase G's claim is not "it compiles for ARM". It is that the same kernel,
the same filesystem, the same event bus, the same robotic runtime and the
same PX4 bridge run on a machine that shares no chip with the PC — and that
every one of those is verified on the ARM board rather than inferred from
the x86 run.

So this boots the image under QEMU virt and reads live state out of the
bring-up shell: the boot checklist, the registers the CPU really has, a DHCP
lease from QEMU's server, an ICMP reply from the gateway, the robotic
pipeline's own counters, and — when PX4 SITL is available — a MAVLink 2 link
whose frames PX4 acknowledges.

Every check names what would make it red. A check that cannot fail is not a
check, so the numeric ones assert values, not the presence of a word.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke_universal import Guest, plain          # noqa: E402

PROMPT = re.compile(r"echos\(aarch64\)# ")
QEMU = "qemu-system-aarch64"


class ArmGuest(Guest):
    """The same driver, a different machine.

    Not a subclass for its own sake: wait/send/command are transport logic
    and have nothing x86 in them. Only the command line and the prompt are
    the board's, which is the whole point of the phase.
    """

    def __init__(self, image, timeout, netdev=True, esp=None,
                 aavmf_code=None, aavmf_vars=None, output=None):
        import selectors
        import shutil
        self.raw = bytearray()
        cmd = [QEMU, "-machine", "virt", "-cpu", "cortex-a72", "-m", "512M",
               "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-no-reboot"]
        if esp:
            # UEFI: the firmware finds the image on the EFI system partition
            # by the removable-media path and loads it as a PE application.
            # The variable store is copied per run so one boot cannot leave
            # the next a different NVRAM.
            varfile = Path(output or ".") / "aavmf-vars.fd"
            shutil.copyfile(aavmf_vars, varfile)
            cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={aavmf_code}",
                    "-drive", f"if=pflash,format=raw,file={varfile}",
                    "-drive", f"file={Path(esp).resolve()},format=raw,if=virtio"]
        else:
            cmd += ["-kernel", str(Path(image).resolve())]
        if netdev:
            cmd += ["-netdev", "user,id=n0",
                    "-device", "virtio-net-device,netdev=n0"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.timeout = timeout

    def command(self, command):
        start = len(plain(self.raw))
        self.send(command + "\r")
        piece = self.wait(PROMPT.pattern, start)
        first, sep, rest = piece.partition("\n")
        if not sep or first.strip() != command:
            raise RuntimeError(f"command echo mismatch for {command!r}: {first!r}")
        return PROMPT.sub("", rest).strip()


def num(text, pattern, default=None):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="build/aarch64/universal/rxos.img")
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--output", type=Path, default=Path("build/aarch64/cert"))
    ap.add_argument("--px4", action="store_true",
                    help="also fly the MAVLink link against PX4 SITL")
    ap.add_argument("--firmware", choices=["none", "uefi"], default="none",
                    help="none: QEMU's -kernel loader. uefi: boot the same "
                         "image as a PE application from an ESP.")
    ap.add_argument("--esp")
    ap.add_argument("--aavmf-code")
    ap.add_argument("--aavmf-vars")
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    report = {"phase": "CERT-G", "subject": "aarch64",
              "firmware": args.firmware, "checks": [], "green": False}

    def check(name, ok, detail=""):
        report["checks"].append({"name": name, "passed": bool(ok),
                                 "detail": detail if not ok else ""})
        return bool(ok)

    image = Path(args.image)
    check("image_present", image.exists(), f"{image} not built")
    if not image.exists():
        print(json.dumps(report, indent=2))
        return 1

    head = image.read_bytes()[:64]
    check("arm64_boot_header", head[56:60] == b"ARM\x64",
          "no arm64 image magic: a loader will not pass a device tree")
    check("text_offset_matches_link",
          int.from_bytes(head[8:16], "little") == 0x80000,
          "text_offset disagrees with linker-aarch64.ld")

    guest = px4 = None
    try:
        uefi = args.firmware == "uefi"
        guest = ArmGuest(image, args.timeout,
                         esp=args.esp if uefi else None,
                         aavmf_code=args.aavmf_code, aavmf_vars=args.aavmf_vars,
                         output=args.output)
        boot = guest.wait(PROMPT.pattern)

        # --- the checklist, item by item, as the board reported it ---------
        for name, pattern in [
            ("boot_stage",      r"Boot stage: OK"),
            ("heap0",           r"Heap-0: OK"),
            ("memory",          r"Memory: OK"),
            ("interrupts_gic",  r"Interrupts: OK \(GICv2"),
            ("timer",           r"Timer: OK \(ARM generic timer"),
            ("mono_clock",      r"Monotonic clock: ARM generic timer"),
            ("scheduler",       r"Scheduler: OK \(spawn/yield/block/wake\)"),
            ("seb",             r"SEB: OK"),
            ("storage",         r"Storage: OK"),
            ("rxfs",            r"RXFS: OK"),
            ("network",         r"Network: OK \(virtio-net over MMIO\)"),
            ("robot_runtime",   r"Robot runtime: OK"),
            ("selftests_pass",  r"self-tests: scheduler PASS, SEB PASS, "
                                r"RXFS PASS, heap PASS"),
        ]:
            check(f"boot_{name}", re.search(pattern, boot) is not None,
                  f"missing from the boot log: {pattern}")

        if uefi:
            # The firmware loaded a PE application and we left boot services
            # under our own steam. The memory map is the firmware's, not an
            # assumption, and it is the one thing that proves which path ran.
            check("uefi_stub_entered", "echOS: UEFI entry" in boot,
                  "the UEFI stub never printed; this was not a UEFI boot")
            check("uefi_memory_map_used",
                  "boot=UEFI memory map" in boot,
                  "fell back to an assumed map: the firmware's was unusable")
            ranges = num(boot, r"memory map: (\d+) range\(s\)") or 0
            check("uefi_memory_map_detailed", ranges > 1,
                  f"{ranges} range(s): a real firmware map is fragmented, "
                  "a guessed one is a single block")
            total = sum(int(m) for m in
                        re.findall(r"\+ (\d+) MiB  usable", boot))
            check("uefi_memory_plausible", 300 <= total <= 512,
                  f"{total} MiB of conventional memory out of the 512 given")
        else:
            # The device tree is read, not assumed. QEMU was given 512 MiB; a
            # board inventing its own map would still print a number, so the
            # check is that the number is the one we asked QEMU for.
            check("dtb_parsed",
                  re.search(r"boot=device-tree(?! \()", boot) is not None,
                  "fell back to an assumed memory map")
            check("dtb_memory_size", "+ 512 MiB  usable" in boot,
                  "memory map does not match the -m 512M the machine was given")

        # --- the machine, asked directly -----------------------------------
        status = guest.command("status")
        check("status_arch", "aarch64" in status, status[:200])
        check("status_switches", (num(status, r"switches (\d+)") or 0) > 0,
              "no context switch ever happened")

        cpu = guest.command("cpu")
        check("cpu_vendor_arm", "vendor      ARM" in cpu, cpu[:200])
        for reg in ["midr", "sctlr", "cntfrq", "daif", "ttbr0", "elr_el1"]:
            check(f"cpu_{reg}", re.search(rf"\b{reg}\s+0x", cpu) is not None,
                  f"{reg} not readable; this is not an ARM64 in EL1")
        el = re.search(r"\bEL\s+0x0*([0-9a-f]+)", cpu)
        check("cpu_el1", el is not None and int(el.group(1), 16) == 1,
              "not running at EL1")
        sctlr = re.search(r"\bsctlr\s+0x([0-9a-f]+)", cpu)
        check("cpu_mmu_on", sctlr is not None and (int(sctlr.group(1), 16) & 1),
              "SCTLR_EL1.M is clear: the MMU is off")
        check("cpu_align_check_off",
              sctlr is not None and not (int(sctlr.group(1), 16) & 2),
              "SCTLR_EL1.A is set: unaligned access will abort")
        daif = re.search(r"\bdaif\s+0x([0-9a-f]+)", cpu)
        check("cpu_irqs_unmasked",
              daif is not None and not (int(daif.group(1), 16) & 0x80),
              "PSTATE.I is set: interrupts are masked in the shell")
        freq = re.search(r"\bcntfrq\s+0x([0-9a-f]+)", cpu)
        check("cpu_cntfrq_sane",
              freq is not None and 1_000_000 <= int(freq.group(1), 16) <= 1_000_000_000,
              "CNTFRQ_EL0 is not a believable counter frequency")

        # A clock is proven by advancing, not by being printed. Two reads a
        # known wall-clock apart: the tick count must grow at the rate the
        # board claims, and the monotonic clock must agree with the timer.
        t0 = num(status, r"ticks\s+(\d+)") or 0
        m0 = num(status, r"(\d+) ms \(monotonic\)") or 0
        u0 = num(status, r"uptime\s+(\d+) ms") or 0
        time.sleep(3.0)
        status2 = guest.command("status")
        t1 = num(status2, r"ticks\s+(\d+)") or 0
        m1 = num(status2, r"(\d+) ms \(monotonic\)") or 0
        u1 = num(status2, r"uptime\s+(\d+) ms") or 0
        hz = num(status, r"at (\d+) Hz") or 0
        elapsed_ms = u1 - u0
        expected = hz * elapsed_ms / 1000.0
        report["clock"] = {"ticks": [t0, t1], "hz": hz,
                           "timer_ms": [u0, u1], "mono_ms": [m0, m1]}
        check("timer_advances", t1 > t0, "the tick count did not move")
        check("timer_rate_matches_claim",
              expected > 0 and abs((t1 - t0) - expected) <= 0.10 * expected,
              f"{t1 - t0} ticks in {elapsed_ms} ms, {hz} Hz claimed")
        check("mono_advances", m1 > m0, "the monotonic clock did not move")
        check("mono_agrees_with_timer",
              elapsed_ms > 0 and abs((m1 - m0) - elapsed_ms) <= 0.05 * elapsed_ms,
              f"monotonic moved {m1 - m0} ms while the timer moved {elapsed_ms} ms")

        seb = guest.command("seb")
        check("seb_selftest", "self-test   PASS" in seb, seb[:200])
        check("seb_no_overruns", num(seb, r"overruns\s+(\d+)") == 0,
              "the event ring overran")

        # --- the robotic runtime, on ARM -----------------------------------
        # The scenario is adversarial on purpose: it feeds malformed records,
        # an intent over the speed envelope, an already-expired intent and a
        # burst larger than the queue. So "nothing was refused" would be the
        # failure, and the accounting closing is the proof nothing vanished.
        robot = guest.command("robot")
        report["robot"] = robot
        injected = num(robot, r"injected (\d+)") or 0
        accepted = num(robot, r"accepted (\d+)") or 0
        refused = num(robot, r"refused (\d+)")
        dropped = num(robot, r"sensor drops (\d+)")
        made = num(robot, r"made (\d+)") or 0
        sent = num(robot, r"sent (\d+)") or 0
        seen = num(robot, r"gate\s+seen (\d+)") or 0
        g_ok = num(robot, r"seen \d+, accepted (\d+)")
        modified = num(robot, r"MODIFY (\d+)")
        blocked = num(robot, r"BLOCK (\d+)")
        expired = num(robot, r"expired\s+(\d+)")
        samples = num(robot, r"samples (\d+)") or 0
        p50 = num(robot, r"p50 <(\d+)")
        p95 = num(robot, r"p95 <(\d+)")
        check("robot_injected", injected > 0, robot[:200])
        check("robot_balance_closes",
              None not in (refused, dropped) and
              accepted + refused + dropped == injected,
              f"{injected} injected but {accepted}+{refused}+{dropped} accounted")
        check("robot_abi_refused_the_malformed", (refused or 0) > 0,
              "nothing was refused at the door; the ABI check cannot go red")
        check("robot_queue_dropped_the_burst", (dropped or 0) > 0,
              "the oversized burst did not overflow the queue it should")
        check("robot_intents_made", made > 0, "no intent was produced")
        check("robot_gate_balance",
              None not in (g_ok, modified, blocked) and
              g_ok + modified + blocked == seen,
              f"gate saw {seen} but ruled on {g_ok}+{modified}+{blocked}")
        check("robot_gate_modified", (modified or 0) > 0,
              "the gate never clamped anything; MODIFY is untested")
        check("robot_gate_blocked", (blocked or 0) > 0,
              "the gate never blocked anything; BLOCK is untested")
        check("robot_expired_counted", (expired or 0) > 0,
              "the already-expired intent was not counted as expired")
        check("robot_sent_matches_gate", sent == g_ok + modified,
              f"{sent} sent but the gate approved {g_ok}+{modified}")
        check("robot_latency_measured", samples > 0, "no latency sample")
        check("robot_latency_nonzero", (p50 or 0) > 0,
              "p50 is zero: the clock is not measuring the pipeline")
        check("robot_latency_ordered", (p95 or 0) >= (p50 or 0),
              f"p95 {p95} below p50 {p50}")
        report["robot_latency_us"] = {"p50": p50, "p95": p95, "samples": samples}

        # --- storage and filesystem ----------------------------------------
        # Cross-check rather than count: the shell's file listing and the
        # kernel's own count must agree, whatever the number is.
        files = guest.command("ls")
        listed = 0 if "(empty)" in files else len(
            [l for l in files.splitlines() if l.strip()])
        check("rxfs_count_agrees_with_listing",
              (num(status, r"files\s+(\d+)") or 0) == listed,
              f"status says {num(status, r'files\s+(\d+)')}, ls shows {listed}")

        # --- the network, proven by someone else ---------------------------
        net = guest.command("net")
        check("net_driver", "virtio-net (mmio)" in net, net[:200])
        check("net_transport_known",
              re.search(r"virtio-mmio v[12]", net) is not None, net[:200])
        check("net_dhcp_lease", "10.0.2.15" in net,
              "QEMU's DHCP server did not lease us the address it always leases")
        rtt = num(net, r"rtt (\d+) us")
        check("net_gateway_replies", "replies" in net, net[:300])
        check("net_rtt_measured", rtt is not None and rtt > 0,
              "an ICMP round trip of zero is not a measurement")

        # --- PX4, if it is here --------------------------------------------
        if args.px4:
            from px4_test import PX4_ROOT, Px4
            check("px4_sitl_present",
                  (PX4_ROOT / "build/px4_sitl_default/bin/px4").exists(),
                  "PX4 SITL is not built")
            px4 = Px4(PX4_ROOT, args.output / "px4.log")
            boot_px4 = px4.wait_ready()
            check("px4_sitl_running", "Simulation loop with" in boot_px4,
                  "PX4 did not start")
            check("px4_onboard_link", "remote port 14540" in boot_px4,
                  "PX4 opened no onboard link")

            guest.command("mav start")
            m = guest.command("mav")
            check("mav_link_up", re.search(r"link        up", m) is not None, m[:400])
            rx = num(m, r"frames      rx (\d+)") or 0
            tx = num(m, r"rx \d+, tx (\d+)") or 0
            hb = num(m, r"heartbeats  rx (\d+)") or 0
            sysid = num(m, r"sysid (\d+)")
            sensors = num(m, r"sensors     (\d+)") or 0
            intents = num(m, r"intents     (\d+)") or 0
            acks = num(m, r"acks        (\d+)") or 0
            crc = num(m, r"crc_errors  (\d+)")
            gaps = num(m, r"this_window rx \d+, gaps (\d+)")
            lost = num(m, r"gaps \d+, lost (\d+)")
            starved = num(m, r"ring starved (\d+)")
            drops = num(m, r"drops (\d+)")
            check("mav_telemetry_received", rx > 0, "no MAVLink frame arrived")
            check("mav_we_transmitted", tx > 0, "we sent nothing")
            check("mav_heartbeats", hb > 0, "PX4's heartbeat never arrived")
            check("mav_peer_is_px4", sysid == 1, f"peer sysid {sysid}, expected 1")
            check("mav_became_sensors", sensors > 0,
                  "telemetry never reached the robotic pipeline")
            check("mav_intent_sent", intents > 0, "no intent left the machine")
            check("mav_command_acked", acks > 0,
                  "PX4 acknowledged nothing we sent")
            check("mav_crc_clean", crc == 0, f"{crc} CRC errors")
            check("mav_no_loss_in_window", gaps == 0 and lost == 0,
                  f"{lost} frames lost in the measured window")
            check("mav_ring_never_starved", starved == 0,
                  f"the NIC ring ran dry {starved} times")
            check("mav_udp_ring_no_drops", drops == 0,
                  f"{drops} datagrams dropped by the receiver")
            report["mav"] = {"rx": rx, "tx": tx, "heartbeats": hb,
                             "sensors": sensors, "intents": intents,
                             "acks": acks}

            # Ask PX4 to take its setpoints from this link. It is a request,
            # not a takeover: what makes it a check is PX4's answer.
            before_off = px4.text()
            off = guest.command("mav offboard")
            acks_after = num(off, r"acks\s+(\d+)") or 0
            log_off = px4.text()[len(before_off):]
            check("mav_offboard_answered",
                  acks_after > acks or "OFFBOARD" in log_off.upper(),
                  f"acks {acks} -> {acks_after}, PX4 said nothing about offboard")

            # Cut our side while PX4 is still alive. Killing it first and
            # then reading its log asks a dead process how it felt about
            # dying; the autopilot has to be running to react.
            before_cut = px4.text()
            guest.command("mav stop")
            time.sleep(10)
            after = px4.text()[len(before_cut):]
            noticed = [l for l in after.splitlines()
                       if re.search(r"lost|failsafe|offboard|timeout", l, re.I)]
            report["px4_after_our_silence"] = noticed[-8:]
            check("px4_noticed_our_silence", bool(noticed),
                  "PX4 logged nothing when we went quiet")

            # And the link comes back: a bridge that cannot be restarted has
            # not survived the cut, it has merely stopped.
            guest.command("mav start")
            back = guest.command("mav")
            check("mav_link_recovered",
                  re.search(r"link        up", back) is not None, back[:300])

        # `halt` never returns a prompt — that is the point of it — so this
        # waits for the machine to say it stopped instead.
        start = len(plain(guest.raw))
        guest.send("halt\r")
        check("halt", "halted" in guest.wait(r"halted", start),
              "the board did not acknowledge halt")
        report["green"] = all(c["passed"] for c in report["checks"])
    except Exception as exc:                     # noqa: BLE001
        check("run_completed", False, f"{type(exc).__name__}: {exc}")
    finally:
        if px4 is not None:
            px4.stop()
        if guest is not None:
            (args.output / "console.txt").write_text(plain(guest.raw))
            guest.close()

    # On disk as well as on stdout: report30.py cites this verdict, and a
    # verdict that only ever existed in a terminal cannot be cited.
    (args.output / "report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["green"] else 1


if __name__ == "__main__":
    sys.exit(main())
