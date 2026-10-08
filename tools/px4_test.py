"""Fly the phase-E link: PX4 SITL on the host, echOS in QEMU, one wire.

The eight steps the prompt asks for, in order:

  1. echOS in QEMU x86_64            5. an intent sent
  2. PX4 SITL outside the VM         6. COMMAND_ACK or a state change
  3. a controlled virtual network    7. the link cut on purpose
  4. telemetry received              8. PX4's safe behaviour

PX4 is the authority on the other side. This end sends high-level intent and
reads what comes back; nothing here arms, mixes or actuates. The link is
QEMU user networking: the guest reaches the host at 10.0.2.2, and PX4 replies
to the address it saw, which is what makes a NAT'd UDP flow work at all.

PX4 SITL is a host tool. It is not in the ISO and never will be.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke_universal import Guest, PROMPT, plain   # noqa: E402

PX4_ROOT = Path("/home/roger/px4/PX4-Autopilot")
PX4_ONBOARD_PORT = 14580          # PX4's "onboard"/companion MAVLink instance


class Args:
    def __init__(self, iso, qemu, timeout, output, extra):
        self.iso, self.qemu, self.timeout = iso, qemu, timeout
        self.output, self.firmware, self.extra = output, "bios", extra


class Px4:
    """PX4 SITL with SIH: the flight stack plus its own physics, no Gazebo."""

    def __init__(self, root, logfile):
        env = dict(os.environ, HEADLESS="1", PX4_SIM_MODEL="sihsim_quadx")
        self.log = open(logfile, "wb")
        self.proc = subprocess.Popen(
            ["make", "px4_sitl", "sihsim_quadx"], cwd=str(root), env=env,
            stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT,
            start_new_session=True)
        self.logpath = Path(logfile)

    def wait_ready(self, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("PX4 exited early; see its log")
            text = self.logpath.read_bytes().decode("utf-8", "replace")
            if "Simulation loop with" in text and "mode: Onboard" in text:
                return text
            time.sleep(1)
        raise RuntimeError("PX4 did not come up")

    def text(self):
        return self.logpath.read_bytes().decode("utf-8", "replace")

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
        self.log.close()


def counters(text):
    """Parse `px4 status` into a dict of the numbers it prints."""
    out = {}
    m = re.search(r"state (\w+)   drops to down (\d+)", text)
    if m:
        out["state"], out["link_drops"] = m.group(1), int(m.group(2))
    m = re.search(r"frames    rx (\d+)  tx (\d+)   heartbeats rx (\d+) tx (\d+)", text)
    if m:
        out["rx"], out["tx"], out["hb_rx"], out["hb_tx"] = (int(x) for x in m.groups())
    m = re.search(r"telemetry heartbeat (\d+)  sys_status (\d+)  battery (\d+)  "
                  r"attitude (\d+)", text)
    if m:
        out["t_hb"], out["t_sys"], out["t_bat"], out["t_att"] = (int(x) for x in m.groups())
    m = re.search(r"local_pos (\d+)  global_pos (\d+)  distance (\d+)  command_ack (\d+)", text)
    if m:
        out["t_lpos"], out["t_gpos"], out["t_dist"], out["t_ack"] = (int(x) for x in m.groups())
    m = re.search(r"errors    crc (\d+)  unknown id (\d+)  rejected incompat (\d+)  "
                  r"stray bytes (\d+)", text)
    if m:
        out["crc"], out["unknown"], out["incompat"], out["stray"] = (int(x) for x in m.groups())
    m = re.search(r"loss      sequence gaps (\d+)  frames missed (\d+)   "
                  r"our own ring drops (\d+)", text)
    if m:
        out["gaps"], out["missed"], out["ring_drops"] = (int(x) for x in m.groups())
    m = re.search(r"pipeline  sensor records (\d+)   intents sent (\d+)   acks (\d+)", text)
    if m:
        out["sensors"], out["intents"], out["acks"] = (int(x) for x in m.groups())
    m = re.search(r"last cmd (\d+) result (\d+)", text)
    if m:
        out["ack_cmd"], out["ack_result"] = int(m.group(1)), int(m.group(2))
    m = re.search(r"heartbeat gap  last (\d+) ms   worst (\d+) ms", text)
    if m:
        out["hb_last_ms"], out["hb_worst_ms"] = int(m.group(1)), int(m.group(2))
    m = re.search(r"streams   silenced (\d+)", text)
    if m:
        out["silenced"] = int(m.group(1))
    m = re.search(r"peer      sysid (\d+) compid (\d+)", text)
    if m:
        out["peer_sysid"], out["peer_compid"] = int(m.group(1)), int(m.group(2))
    return out


def run(args):
    report = {"phase": "CERT-E", "subject": "px4-mavlink", "checks": [], "green": False}
    args.output.mkdir(parents=True, exist_ok=True)
    guest = px4 = None

    def check(name, passed, detail=None):
        entry = {"name": name, "passed": bool(passed)}
        if detail is not None:
            entry["detail"] = str(detail)[:400]
        report["checks"].append(entry)
        if not passed:
            raise AssertionError(name)

    try:
        # ---- steps 1-3: PX4 outside, echOS inside, one virtual wire -------
        check("px4_sitl_present", (PX4_ROOT / "build/px4_sitl_default/bin/px4").exists(),
              str(PX4_ROOT))
        px4 = Px4(PX4_ROOT, args.output / "px4.log")
        boot = px4.wait_ready()
        check("px4_sitl_running", "Simulation loop with" in boot,
              [l for l in boot.splitlines() if "Simulation loop" in l])
        check("px4_onboard_link_open", f"remote port {14540}" in boot or
              "mode: Onboard" in boot, [l for l in boot.splitlines() if "Onboard" in l])

        guest = Guest(Args(args.iso, args.qemu, args.timeout, args.output, []))
        guest.wait(r"q LIVE \(Minimal\)")
        guest.send("q")
        guest.wait(PROMPT)
        guest.command("verbose off")

        started = guest.command(f"px4 start 10.0.2.2 {PX4_ONBOARD_PORT}")
        check("bridge_started", "MAVLink 2 over UDP" in started, started)

        # ---- step 4: telemetry actually arrives ---------------------------
        # The guest is left idle on purpose: that is when its input loop runs
        # the idle hook, which is what pumps the link. Typing commands at it
        # would measure the test's own polling, not the link.
        time.sleep(args.settle)
        seen = counters(guest.command("px4"))
        check("heartbeat_received", seen.get("hb_rx", 0) >= 3, seen)
        check("link_up", seen.get("state") == "UP", seen)
        check("peer_identified", seen.get("peer_sysid", 0) > 0, seen)
        check("crc_clean", seen.get("crc", 1) == 0,
              "PX4 is the independent implementation here: a wrong CRC_EXTRA "
              "would show up as every frame failing")
        check("no_incompat_frames", seen.get("incompat", -1) == 0, seen)
        check("attitude_received", seen.get("t_att", 0) > 0, seen)
        check("position_received", seen.get("t_lpos", 0) > 0 or
              seen.get("t_gpos", 0) > 0, seen)
        check("battery_received", seen.get("t_bat", 0) > 0 or
              seen.get("t_sys", 0) > 0, seen)
        check("telemetry_became_sensor_records", seen.get("sensors", 0) > 0, seen)
        check("heartbeat_gap_within_timeout",
              seen.get("hb_worst_ms", 99999) < 3000, seen)
        check("loss_measured", "gaps" in seen and "missed" in seen, seen)

        # ---- step 5 and 6: an intent leaves, PX4 answers ------------------
        check("intent_sent", seen.get("intents", 0) > 0, seen)
        check("command_acknowledged", seen.get("acks", 0) > 0, seen)
        check("ack_was_accepted", seen.get("ack_result", -1) == 0,
              f"last ack result {seen.get('ack_result')} (0 = MAV_RESULT_ACCEPTED)")

        # ---- step 8 setup: ask PX4 to take its setpoints from us ---------
        # Offboard is what makes the far end depend on this stream. PX4 may
        # refuse — that is a real answer and the check records which it was.
        guest.command("px4 offboard")
        time.sleep(6)
        engaged = counters(guest.command("px4"))
        px4_log = px4.text()
        offboard_seen = ("OFFBOARD" in px4_log.upper() or
                         engaged.get("acks", 0) > seen.get("acks", 0))
        check("offboard_request_answered", offboard_seen,
              [l for l in px4_log.splitlines() if "ffboard" in l][-3:] or
              f"acks {seen.get('acks')} -> {engaged.get('acks')}")

        # ---- step 7a: cut OUR side, with PX4 alive to notice -------------
        # Killing PX4 first and then reading its log, as an earlier version of
        # this test did, asks a dead process how it felt about dying. To see
        # the autopilot react, the autopilot has to still be running.
        before_cut = px4.text()
        guest.command("px4 stop")
        time.sleep(10)
        px4_after = px4.text()[len(before_cut):]
        px4_noticed = [l for l in px4_after.splitlines()
                       if re.search(r"lost|failsafe|offboard|timeout", l, re.I)]
        report["px4_lines_after_our_silence"] = px4_noticed[-8:]
        check("px4_noticed_our_silence", bool(px4_noticed),
              px4_noticed[-3:] or "PX4 logged nothing; see the note in the record")

        # ---- step 7b and 8: the link back, then PX4 gone -----------------
        guest.command(f"px4 start 10.0.2.2 {PX4_ONBOARD_PORT}")
        time.sleep(8)
        back = counters(guest.command("px4"))
        check("link_recovered", back.get("state") == "UP", back)

        before = back
        px4.stop()
        px4 = None
        time.sleep(8)
        after = counters(guest.command("px4"))
        check("link_loss_detected", after.get("state") == "DOWN", after)
        check("link_drop_counted", after.get("link_drops", 0) >= 1,
              f"{before.get('link_drops')} -> {after.get('link_drops')}")
        gate = guest.command("robot")
        check("gate_saw_link_loss", "link-lost" in gate or "link=" in gate, gate)
        check("gate_degraded_to_hold", re.search(r"HOLD=[1-9]", gate) is not None, gate)

        report["counters_before_cut"] = before
        report["counters_after_cut"] = after

        guest.send("halt\n")
        guest.wait(r"\[echOS\] halted\.")
        report["green"] = all(c["passed"] for c in report["checks"])
    except (AssertionError, RuntimeError, OSError) as exc:
        report["error"] = str(exc)
    finally:
        if guest is not None:
            (args.output / "guest.log").write_bytes(guest.raw)
            guest.close()
        if px4 is not None:
            px4.stop()
        (args.output / "report.json").write_text(json.dumps(report, indent=2,
                                                            sort_keys=True) + "\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["green"] else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--iso", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--qemu", default="qemu-system-x86_64")
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--settle", type=float, default=25,
                   help="seconds the guest idles while the link runs")
    raise SystemExit(run(p.parse_args()))


if __name__ == "__main__":
    main()
