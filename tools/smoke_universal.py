"""Exercise the actual Universal LIVE shell in a bounded, diskless QEMU.

Never pre-feed input to GRUB or the account wizard. Each command waits for
its own response and prompt; an echoed command is not evidence of execution.
Serial output and the deterministic assertion report are retained on failure.

The same certificate runs under both firmwares: `--firmware bios` boots the
hybrid ISO through SeaBIOS, `--firmware uefi` boots the same file through
OVMF. The guest is asked which one it thinks it got, and the answer has to
match the one we started.

The network check is a real round trip: a host listener the guest reaches
through its own NIC, ARP, IPv4 and TCP. QEMU user networking maps the guest
gateway to the host, so nothing here needs the internet or a privileged port.

The limits check reads the ceilings the running kernel declares and compares
them with the contract below. That catches a constant drifting away from
docs2.0/limites.md; it does not prove the kernel refuses at the ceiling.

This is a BIOS/Multiboot2 certificate, not a UEFI or robotics certificate.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import threading
import time


ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
PROMPT = re.compile(r"live@echos [^\r\n]*# ")
# Output longer than the console pages. That is the shell being right, so the
# certificate drives the pager instead of hanging on it.
PAGER = re.compile(r"-- more --")
STOP = re.compile(PROMPT.pattern + "|" + PAGER.pattern)

# Fixed so the guest's reading of its own NIC is an assertion, not a sample.
GUEST_MAC = "52:54:00:ce:21:01"
GUEST_IPV4 = "10.0.2.15"        # QEMU user-mode DHCP lease
HOST_IPV4 = "10.0.2.2"          # the host, from inside the guest

# The ceilings this ISO must declare. Prose lives in docs2.0/limites.md; the
# numbers live in the headers the kernel compiles against.
LIMITS = {
    "tasks": 4,
    "task stack": 65536,
    "seb ring": 1024,
    "seb channels": 8,
    "wsp packet": 16,
    "heap-0 wsp pool": 65536,
    "devices": 64,
    "kmalloc arena": 524288,
    "rxfs files": 64,
    "rxfs file size": 65536,
    "rxfs dirs": 64,
    "rxfs path depth": 8,
    "ramdisk": 524288,
}

# The console contract: a modern proportionally-spaced-free face, three real
# weights, and the special characters a robotics console prints. Each of
# these is answered by the font module in the guest, not by this list.
CONSOLE_FACE = "JetBrains Mono"
GLYPH_CHECKS = {
    "00F1": "n with tilde",       "20AC": "euro",
    "2500": "box drawing rule",   "2588": "full block",
    "2192": "right arrow",        "03C0": "pi",
    "2713": "check mark",         "28FF": "braille, all eight dots",
    "FFFD": "replacement",
}
STYLED_GLYPH = "0041"             # 'A' must have its own bold and italic

# A version string that is no longer this product must not survive anywhere in
# the image: not in the kernel, the GRUB menu, the seeded /etc or the manuals.
RETIRED_VERSION_STRINGS = (b"echOS 2.0", b"EchOS 2.0")

# The retired in-kernel chatbot: its name, its symbols and its GRUB weight
# modules. Matched as a name, never as a substring — "navigation" is an
# English word this shell is allowed to use, "navi2" is a product that left.
RETIRED_AI = (
    ("chatbot_name", re.compile(rb"(?i)navi(?![a-z])")),
    ("chatbot_project", re.compile(rb"(?i)monad")),
    ("weight_modules", re.compile(rb"_weights\.bin")),
)


def plain(raw):
    text = ANSI.sub("", raw.decode("utf-8", errors="replace")).replace("\r", "")
    # A pipe read can split the prompt's trailing reset. Do not count an
    # unfinished escape as visible text, or the next read moves our cursor.
    return re.sub(r"\x1b(?:\[[0-?]*[ -/]*)?$", "", text)


class HostHttp:
    """A listener on the host that the guest has to reach over the wire."""

    BODY = b"CERT21_NET_SENTINEL\n"

    def __init__(self):
        body = self.BODY

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"   # the guest client is HTTP/1.0

            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


# OVMF keeps its variables in a writable flash image; each run gets a fresh
# copy so a boot cannot inherit the previous one's boot entries.
OVMF_CODE = Path("/usr/share/edk2/ovmf/OVMF_CODE.fd")
OVMF_VARS = Path("/usr/share/edk2/ovmf/OVMF_VARS.fd")


def firmware_args(firmware, workdir):
    if firmware == "bios":
        return []
    if not OVMF_CODE.exists() or not OVMF_VARS.exists():
        raise RuntimeError(f"OVMF not installed: {OVMF_CODE} (dnf install edk2-ovmf)")
    varfile = workdir / "OVMF_VARS.fd"
    varfile.write_bytes(OVMF_VARS.read_bytes())
    return ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={varfile}"]


class Guest:
    def __init__(self, args):
        self.raw = bytearray()
        self.proc = subprocess.Popen(
            [args.qemu, "-accel", "tcg", "-machine", "q35", "-m", "512M"]
            + firmware_args(args.firmware, args.output) +
            ["-cdrom", str(args.iso.resolve()), "-display", "none", "-vga", "std",
             "-serial", "stdio", "-monitor", "none",
             "-nic", f"user,model=virtio-net-pci,mac={GUEST_MAC}",
             "-no-reboot", "-no-shutdown"]
            # Storage certification reuses this guest with a disk attached.
            + list(getattr(args, "extra", []) or []),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.timeout = args.timeout

    def wait(self, pattern, start=0):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            text = plain(self.raw)
            match = re.search(pattern, text[start:])
            if match:
                return text[start:start + match.end()]
            if self.proc.poll() is not None:
                raise RuntimeError(f"QEMU exited: {self.proc.returncode}")
            for key, _ in self.selector.select(min(0.2, max(0, deadline - time.monotonic()))):
                chunk = os.read(key.fd, 65536)
                if chunk:
                    self.raw.extend(chunk)
        raise RuntimeError(f"timeout waiting for {pattern!r}")

    def send(self, line):
        self.proc.stdin.write(line.encode())
        self.proc.stdin.flush()

    def command(self, command):
        """Type one command, answer the pager if it appears, return the rest.

        The search origin moves past each piece: looking again from the same
        offset finds the same "-- more --" for ever, which is a hang that
        looks exactly like a broken guest."""
        start = len(plain(self.raw))
        self.send(command + "\n")
        pos, chunks = start, []
        while True:
            piece = self.wait(STOP, pos)
            chunks.append(piece)
            pos += len(piece)
            if not PAGER.search(piece.rsplit("\n", 1)[-1]):
                break
            self.send(" ")          # next page
        response = "".join(chunks)
        # Remove the shell echo and the next prompt before checking results.
        first, sep, rest = response.partition("\n")
        if not sep or first.strip() != command:
            raise RuntimeError(f"command echo mismatch for {command!r}: {first!r}")
        rest = PAGER.sub("", rest)
        rest = re.sub(r"\s*space next   b back   q quit\n?", "", rest)
        return PROMPT.sub("", rest).strip()
    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.selector.close()
        self.proc.stdin.close()
        self.proc.stdout.close()


def run(args):
    if args.edition != "universal":
        raise ValueError("this certificate tests Universal; use test-universal-x86_64")
    iso_bytes = args.iso.read_bytes()
    boot_mode = f"{args.firmware}-multiboot2"
    report = {"phase": "CERT-21", "architecture": "x86_64",
              "boot_mode": boot_mode, "edition": args.edition,
              "version": args.version, "iso_sha256": hashlib.sha256(iso_bytes).hexdigest(),
              "checks": [], "green": False}
    args.output.mkdir(parents=True, exist_ok=True)
    guest = None
    http = None

    def check(name, passed, detail=None):
        entry = {"name": name, "passed": bool(passed)}
        if detail is not None:
            entry["detail"] = str(detail)[:400]
        report["checks"].append(entry)
        if not passed:
            raise AssertionError(name)

    try:
        # ---- the source itself: no generic module may name this machine ---
        # A host-side check, like the GPT audit in the storage certificate:
        # the guest cannot tell you whether the port is still possible.
        gate = subprocess.run([sys.executable, str(Path(__file__).with_name(
            "portability_gate.py")), "--root", str(Path(__file__).resolve().parents[1])],
            capture_output=True, text=True)
        check("generic_trees_are_portable", gate.returncode == 0,
              gate.stdout.strip()[-400:])

        for retired in RETIRED_VERSION_STRINGS:
            check("iso_free_of_" + retired.decode().replace(" ", "_").replace(".", ""),
                  retired not in iso_bytes)
        for name, pattern in RETIRED_AI:
            # The ELF (symbols included) and the GRUB menu are inside the ISO,
            # so one byte scan covers "not linked", "not staged", "not booted".
            check("iso_free_of_" + name, pattern.search(iso_bytes) is None)
        http = HostHttp()
        guest = Guest(args)
        guest.wait(r"q LIVE \(Minimal\)")
        guest.send("q")
        guest.wait(PROMPT)
        boot = plain(guest.raw)
        check("compiled_identity",
              f"identity version={args.version} edition=universal arch=x86_64 "
              f"boot={boot_mode}" in boot)
        # The guest names the firmware it was actually handed, and it has to be
        # the one QEMU was started with. A build flag could not tell them apart.
        check("firmware_reported",
              re.search(rf"firmware: {args.firmware}\b", boot) is not None)
        check("firmware_config_table",
              ("config table: present" in boot) == (args.firmware == "uefi"))
        check("boot_services_exited", "boot services: exited" in boot)
        check("handoff_memory_map",
              re.search(r"memory map: (\d+) of \1 ranges", boot) is not None)
        check("handoff_module", re.search(r"modules: [1-9]", boot) is not None)
        check("handoff_acpi", "acpi: RSDP v" in boot)
        for label in ("GDT", "Memory", "Timer", "IDT", "SEB", "RXFS", "Shell"):
            check("boot_" + label.lower(), re.search(rf"{label}[^\n]*\bOK\b", boot) is not None)
        for label in ("scheduler self-test (spawn/yield/block/wake)",
                      "ipc queue self-test", "event fabric self-test (SPSC ring + Q16.16 LIF + input)",
                      "AEAD self-test (RFC 8439 KAT)"):
            check(label, label + ": PASS" in boot)
        guest.command("verbose off")
        check("uname", args.version in guest.command("uname -a"))
        listed = guest.command("help")
        check("help", "echos-install" in listed)
        check("help_free_of_retired_ai",
              not any(p.search(listed.encode()) for _, p in RETIRED_AI))
        check("write", "wrote 15 B to cert21.txt" in guest.command("write cert21.txt CERT21_SENTINEL"))
        check("read_written_bytes", guest.command("cat cert21.txt") == "-- cert21.txt (15 B) --\nCERT21_SENTINEL")
        check("copy", "copied cert21.txt -> cert21-copy.txt" in guest.command("copy cert21.txt cert21-copy.txt"))
        check("read_copied_bytes", guest.command("cat cert21-copy.txt") == "-- cert21-copy.txt (15 B) --\nCERT21_SENTINEL")
        check("rename", "renamed cert21-copy.txt -> cert21-moved.txt" in guest.command("rename cert21-copy.txt cert21-moved.txt"))
        check("rename_removed_source", "cat: not found: cert21-copy.txt" == guest.command("cat cert21-copy.txt"))
        check("read_renamed_bytes", guest.command("cat cert21-moved.txt") == "-- cert21-moved.txt (15 B) --\nCERT21_SENTINEL")
        check("delete", "deleted cert21-moved.txt" == guest.command("rm cert21-moved.txt"))
        check("delete_removed_bytes", "cat: not found: cert21-moved.txt" == guest.command("cat cert21-moved.txt"))
        guest.command("rm cert21.txt")

        # Limits: what the running kernel says its ceilings are.
        declared = guest.command("limits")
        for name, want in LIMITS.items():
            check("limit_" + name.replace(" ", "_").replace("-", "_"),
                  re.search(rf"^{re.escape(name)}\s+{want}\b", declared, re.M) is not None)
        card = guest.command("status")
        check("status_firmware_row", re.search(rf"^firmware\s+{boot_mode}\b", card, re.M) is not None)
        running = re.search(r"tasks=(\d+)", card)
        check("scheduler_within_declared_limit",
              running is not None and int(running.group(1)) <= LIMITS["tasks"])

        # ---- robotic runtime: the pipeline, the gate, the queues ----------
        abi = guest.command("robot abi")
        check("robot_sensor_abi", "Sensor ABI v1" in abi and "record 64 B fixed" in abi, abi)
        check("robot_intent_abi", "Intent ABI v1" in abi and "record 72 B fixed" in abi, abi)
        check("robot_intent_has_no_actuator",
              "No PWM, no servo, no throttle, no channel" in abi, abi)
        for kind in ("HOLD", "CONTINUE", "APPROACH", "AVOID", "RETURN_HOME",
                     "LAND", "ABORT", "NONE"):
            check("intent_kind_" + kind.lower(), re.search(rf"\b{kind}\b", abi) is not None)

        # Allocation is counted, not inferred: a transient malloc that freed
        # itself would leave heap_used at zero and still be a violation.
        before = guest.command("robot")
        allocs_before = int(re.search(r"kmalloc calls since boot (\d+)", before).group(1))
        ran = guest.command("robot run")
        check("robot_scenario_ran", "injected 61 records" in ran, ran.splitlines()[:2])
        allocs_after = int(re.search(r"kmalloc calls since boot (\d+)", ran).group(1))
        check("robot_zero_allocation_on_path", allocs_after == allocs_before,
              f"kmalloc calls {allocs_before} -> {allocs_after}")

        rows = dict(re.findall(r"^  (\S+)\s+cap (\d+)\s+depth \d+\s+high (\d+)\s+"
                               r"in (\d+)\s+out (\d+)\s+drops (\d+)\s+expired (\d+)",
                               ran, re.M) and
                    [(m[0], m[1:]) for m in
                     re.findall(r"^  (\S+)\s+cap (\d+)\s+depth \d+\s+high (\d+)\s+"
                                r"in (\d+)\s+out (\d+)\s+drops (\d+)\s+expired (\d+)",
                                ran, re.M)])
        check("robot_four_queues", len(rows) == 4, list(rows))
        for name in ("sensor->runtime", "runtime->intents", "intents->gate",
                     "gate->autopilot"):
            check("queue_" + name.replace("->", "_to_"), name in rows, list(rows))
        cap, high, qin, qout, drops, expired = (int(x) for x in rows["sensor->runtime"])
        check("queue_high_water_measured", high == cap, f"high {high} of cap {cap}")
        check("queue_drops_counted", drops > 0, f"drops {drops}")
        check("queue_conservation", qin == qout, f"in {qin} out {qout}")
        check("queue_expiry_counted", int(rows["intents->gate"][5]) > 0, rows["intents->gate"])

        seen, ok, mod, blocked = (int(x) for x in re.search(
            r"seen (\d+)   accepted unchanged (\d+)   MODIFY (\d+)   BLOCK (\d+)", ran).groups())
        check("gate_saw_everything", seen == ok + mod + blocked,
              f"{seen} != {ok}+{mod}+{blocked}")
        check("gate_accepted_some", ok > 0, f"accepted {ok}")
        check("gate_modified_some", mod > 0, f"modified {mod}")
        check("gate_blocked_some", blocked > 0, f"blocked {blocked}")
        reasons = re.search(r"reasons:(.*)", ran).group(1)
        for r in ("expired", "battery", "limit"):
            check("gate_reason_" + r, f"{r}=" in reasons, reasons)
        degraded = re.search(r"sent as:(.*)", ran).group(1)
        for k in ("RETURN_HOME", "LAND", "HOLD"):
            check("gate_degraded_to_" + k.lower(), k + "=" in degraded, degraded)

        lat = re.search(r"samples (\d+)   min (\d+)  p50 <(\d+)  p95 <(\d+)  "
                        r"p99 <(\d+)  max (\d+) us", ran)
        check("latency_measured", lat is not None, ran)
        n, lo, p50, p95, p99, hi = (int(x) for x in lat.groups())
        check("latency_samples", n > 0, f"{n} samples")
        check("latency_is_not_zero", lo > 0,
              f"min {lo} us — a pipeline that always measures 0 is measuring itself")
        check("latency_ordered", p50 <= p95 <= p99, f"p50 {p50} p95 {p95} p99 {p99}")
        check("latency_bounded_by_max", p50 <= hi, f"p50 {p50} max {hi}")
        check("clock_is_calibrated", "TSC calibrated against the PIT" in ran, ran)

        # ---- console: the face, the characters, the weights ---------------
        spec = guest.command("font")
        check("font_face", CONSOLE_FACE in spec, spec.splitlines()[:2])
        check("font_cell", re.search(r"\b10x22 px cell", spec) is not None, spec)
        check("font_antialiased", "4-bit antialiased" in spec, spec)
        m = re.search(r"(\d+) glyphs", spec)
        check("font_glyph_count", m is not None and int(m.group(1)) > 1500, spec)

        drawn = guest.command("font check " + " ".join(GLYPH_CHECKS))
        for cp, what in GLYPH_CHECKS.items():
            row = re.search(rf"^U\+{cp}\s+(\S+)\s+ink (\d+)", drawn, re.M)
            check("glyph_" + cp,
                  row is not None and row.group(1) == "drawable" and int(row.group(2)) > 0,
                  f"{what}: {row.group(0) if row else 'no row'}")
        styled = guest.command("font check " + STYLED_GLYPH)
        check("font_bold_is_its_own_face", "bold own" in styled, styled)
        check("font_italic_is_its_own_face", "italic own" in styled, styled)

        # ---- console: scrollback moves and comes back ----------------------
        base = guest.command("pane")
        check("scroll_starts_at_live", re.search(r"^scroll  0 back of", base, re.M) is not None, base)
        m = re.search(r"back of (\d+) rows", base)
        check("scrollback_has_history", m is not None and int(m.group(1)) > 0, base)
        moved = guest.command("pane scroll up 6")
        m2 = re.search(r"now (\d+) rows back", moved)
        check("scroll_moves_back", m2 is not None and int(m2.group(1)) > 0, moved)
        # Typing returns to the live tail: that is why the offset above cannot
        # be read from a later command, and it is worth asserting on its own.
        after = guest.command("pane")
        check("typing_returns_to_live",
              re.search(r"^scroll  0 back of", after, re.M) is not None, after)
        back_down = guest.command("pane scroll down 3")
        check("scroll_down_clamps_at_live", "now 0 rows back" in back_down, back_down)

        # ---- console: split panes -----------------------------------------
        guest.command("pane split")
        two = guest.command("pane")
        check("pane_split", re.search(r"^panes  2 of 4", two, re.M) is not None, two)
        widths = re.findall(r"#\d+  (\d+)x(\d+)", two)
        check("pane_geometry_halved",
              len(widths) == 2 and abs(int(widths[0][0]) - int(widths[1][0])) <= 1
              and int(widths[0][0]) < 100, two)
        guest.command("pane monitor")
        first = guest.command("pane")
        guest.command("uptime")           # do something else in the shell
        guest.command("uptime")
        second = guest.command("pane")
        r1 = int(re.search(r"monitor refreshes (\d+)", first).group(1))
        r2 = int(re.search(r"monitor refreshes (\d+)", second).group(1))
        check("monitor_pane_keeps_refreshing", r2 > r1, f"{r1} -> {r2}")
        guest.command("pane shell")
        guest.command("pane close")
        one = guest.command("pane")
        check("pane_close", re.search(r"^panes  1 of 4", one, re.M) is not None, one)

        # The kept neuromorphic surface has to work, not just exist: one frame
        # out on the spike bus and the same eight bytes back off the ring.
        loop = guest.command("wsp-tool loopback")
        check("wsp_frame_round_trip", re.search(r"result\s+PASS", loop) is not None)

        # Akida: detected-or-absent, never an active intelligence. QEMU has no
        # card, so the honest answer is absent and the LIF stays on the CPU.
        backend = guest.command("neurocpu")
        check("snn_backend_is_software",
              re.search(r"active:\s+software", backend, re.I) is not None)
        check("akida_not_claimed_active", "[absent]" in backend and "[ready]  akida" not in backend)
        check("no_offload_claimed", "offload" not in backend.replace(
            "akida is a PCI probe. no offload without a card. no Loihi in 2.1.", ""))

        # Network: the guest's own NIC, its own stack, a host it must reach.
        inventory = guest.command("nics")
        check("nic_enumerated", "1af4:" in inventory)
        check("nic_bound", "bound driver: virtio-net" in inventory)
        guest.command("www on")
        iface = guest.command("ipconf")
        check("nic_link_up", "state    up" in iface)
        check("nic_mac_read_from_device", GUEST_MAC in iface)
        check("nic_ipv4_leased", GUEST_IPV4 in iface)
        fetched = guest.command(f"curl http://{HOST_IPV4}:{http.port}/cert21")
        check("http_round_trip", HostHttp.BODY.decode().strip() in fetched)
        guest.command("www off")

        # Everything above ran without the guest ever naming the experiment.
        # Snapshot before probing, or the probe's own echo answers the scan.
        unprompted = plain(guest.raw)

        # The names are gone from the shell, not merely hidden from help.
        for retired in ("navi", "navi2", "monad"):
            check("retired_command_unknown_" + retired,
                  guest.command(retired) == retired + ": command not found")

        start = len(plain(guest.raw))
        guest.send("halt\n")
        guest.wait(r"\[echOS\] halted\.", start)
        check("halt", True)
        session = plain(guest.raw)
        check("no_guest_failure",
              re.search(r"\bFAIL\b|\bPANIC\b|triple fault|SECURITY BUG", session) is None)
        check("session_free_of_retired_version",
              not any(r.decode() in session for r in RETIRED_VERSION_STRINGS))
        # Nothing the guest printed on its own named the retired experiment.
        # Checked before the probe below types the name itself.
        check("session_free_of_retired_ai",
              not any(p.search(unprompted.encode()) for _, p in RETIRED_AI))
        report["green"] = all(c["passed"] for c in report["checks"])
    except (AssertionError, RuntimeError, OSError) as exc:
        report["error"] = str(exc)
    finally:
        if http is not None:
            http.close()
        if guest is not None:
            guest.close()
            (args.output / "serial.log").write_bytes(guest.raw)
        (args.output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["green"] else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--iso", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--edition", default="universal")
    p.add_argument("--version", default="2.1.0")
    p.add_argument("--firmware", choices=("bios", "uefi"), default="bios")
    p.add_argument("--qemu", default="qemu-system-x86_64")
    p.add_argument("--timeout", type=float, default=90)
    raise SystemExit(run(p.parse_args()))


if __name__ == "__main__":
    main()
