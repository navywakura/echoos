"""Certify the storage path on a real QEMU NVMe controller.

Three boots against one image, plus a host-side audit between them:

  1. Write a GPT and let the guest verify its own table on read-back.
     Then check the same bytes with util-linux (fdisk, sfdisk, partx),
     which know nothing about this kernel. A table our own code accepts and
     fdisk rejects is a table that is wrong.
  2. Format RXFS, write a sentinel, save, halt.
  3. Boot the same image again: the sentinel has to still be there.

The disk is an NVMe namespace, driven by the NVMe driver — reset, admin
queues, Identify, one I/O queue and NVM read/write commands. Nothing here
passes if the driver is only a PCI class match.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from smoke_universal import Guest, PROMPT, plain

SENTINEL = "NVME_PERSIST_SENTINEL"


class Args:
    """The Guest class takes an argparse namespace; this is that shape."""
    def __init__(self, iso, qemu, timeout, output, extra):
        self.iso, self.qemu, self.timeout = iso, qemu, timeout
        self.output, self.firmware, self.extra = output, "bios", extra


def boot(args, image, name):
    # -boot d: once the namespace carries a protective MBR the firmware
    # considers it bootable and would try it before the ISO. We are testing
    # the disk as storage, not as a boot device.
    g = Guest(Args(args.iso, args.qemu, args.timeout, args.output,
                   ["-boot", "d",
                    "-drive", f"file={image},if=none,id=nvm,format=raw",
                    "-device", "nvme,serial=cert21,drive=nvm"]))
    try:
        g.wait(r"q LIVE \(Minimal\)")
        g.send("q")
        g.wait(PROMPT)
        g.command("verbose off")
        return g
    except Exception:
        (args.output / f"{name}.log").write_bytes(g.raw)
        g.close()
        raise


def host_gpt_audit(image, report):
    """Ask tools that never saw this kernel whether the table is valid."""
    audits = []
    for name, argv in (("fdisk -l",       ["fdisk", "-l", str(image)]),
                       ("sfdisk --verify", ["sfdisk", "--verify", str(image)]),
                       ("partx --show",    ["partx", "--show", str(image)])):
        exe = shutil.which(argv[0])
        if not exe:
            continue
        p = subprocess.run([exe] + argv[1:], capture_output=True, text=True)
        audits.append({"tool": name, "rc": p.returncode,
                       "output": (p.stdout + p.stderr).strip()})
    report["host_audit"] = audits
    return audits


def run(args):
    report = {"phase": "CERT-22", "subject": "nvme+gpt", "checks": [], "green": False}
    args.output.mkdir(parents=True, exist_ok=True)
    image = args.output / "nvme.img"
    guest = None

    def check(name, passed, detail=None):
        entry = {"name": name, "passed": bool(passed)}
        if detail is not None:
            entry["detail"] = detail
        report["checks"].append(entry)
        if not passed:
            raise AssertionError(name)

    try:
        with open(image, "wb") as f:
            f.truncate(args.size_mib * 1024 * 1024)

        # ---- boot 1: the driver, then the partition table ------------------
        guest = boot(args, image, "boot1")
        banner = plain(guest.raw)
        check("nvme_identified", "QEMU NVMe Ctrl" in banner)
        check("nvme_is_the_disk", "nvme0n1" in banner)
        check("nvme_namespace_size", re.search(r"ns1 \d+ MiB, 512 B blocks", banner) is not None)

        wrote = guest.command("partition nvme0n1 gpt")
        check("gpt_written", "wrote GPT" in wrote, wrote)
        check("gpt_self_verified", "CRC32 and GUIDs verified on read-back" in wrote, wrote)
        guest.send("halt\n")
        guest.wait(r"\[echOS\] halted\.")
        (args.output / "boot1.log").write_bytes(guest.raw)
        guest.close()
        guest = None

        # Keep the table as it was written. Boot 2 formats RXFS over sector 0
        # on purpose, so after that the GPT is gone and auditing the same file
        # later would audit the wrong thing — which is exactly the mistake a
        # copy makes impossible to repeat.
        shutil.copyfile(image, args.output / "nvme-gpt.img")

        # ---- host audit: tools that know nothing about this kernel ---------
        audits = host_gpt_audit(image, report)
        check("external_gpt_tools_available", len(audits) >= 2,
              "util-linux fdisk/sfdisk/partx are the external validators")
        # Positive evidence, not a blacklist of scary words: a corrupted table
        # makes these tools go quiet rather than shout, so "no error text" is
        # not proof of anything. Each tool has to say what it read.
        out = {a["tool"]: a["output"] for a in audits}
        listing = out.get("fdisk -l", "")
        check("host_fdisk_reads_gpt_label", "Disklabel type: gpt" in listing, listing)
        check("host_fdisk_reads_disk_guid",
              re.search(r"Disk identifier: [0-9A-F]{8}-[0-9A-F]{4}-", listing) is not None, listing)
        check("host_fdisk_sees_one_partition",
              re.search(r"^\S+1\s+34\s+\d+\s+\d+\s", listing, re.M) is not None, listing)
        verify = out.get("sfdisk --verify", "")
        # "No errors detected" alone is not evidence: sfdisk says that about a
        # disk with no partition table at all. Require it to name the GPT it
        # actually parsed.
        check("host_sfdisk_verifies",
              "No errors detected" in verify and "Header version: 1.0" in verify
              and "Using 1 out of 128 partitions" in verify, verify)
        table = out.get("partx --show", "")
        check("host_partx_lists_partition",
              re.search(r"^\s*1\s+34\s+\d+\s+\d+\s+\S+\s+echOS\s+"
                        r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-", table, re.M) is not None,
              table)

        # ---- boot 2: filesystem on the namespace, then persist -------------
        guest = boot(args, image, "boot2")
        fmt = guest.command("format nvme0n1 yes")
        check("rxfs_format", "disk formatted" in fmt, fmt)
        wr = guest.command(f"write persist.txt {SENTINEL}")
        check("write_file", "wrote" in wr, wr)
        saved = guest.command("save")
        check("save_to_nvme", "will survive a reboot" in saved, saved)
        guest.send("halt\n")
        guest.wait(r"\[echOS\] halted\.")
        (args.output / "boot2.log").write_bytes(guest.raw)
        guest.close()
        guest = None

        # ---- boot 3: the reboot the whole thing is for ---------------------
        guest = boot(args, image, "boot3")
        back = guest.command("cat persist.txt")
        check("sentinel_survived_reboot", SENTINEL in back, back)
        stats = guest.command("hwprobe")
        check("nvme_reported_by_hwprobe", "nvme" in stats.lower(), stats)
        # The counters are the evidence that commands ran, not decoration: a
        # restore that read the image had to complete dozens of them.
        counters = re.search(r"commands completed (\d+)\s+errors (\d+)\s+timeouts (\d+)", stats)
        check("nvme_counters_present", counters is not None, stats)
        done, errs, tmo = (int(x) for x in counters.groups())
        check("nvme_commands_completed", done > 0, f"completed={done}")
        check("nvme_no_errors", errs == 0, f"errors={errs}")
        check("nvme_no_timeouts", tmo == 0, f"timeouts={tmo}")
        guest.send("halt\n")
        guest.wait(r"\[echOS\] halted\.")
        (args.output / "boot3.log").write_bytes(guest.raw)

        report["green"] = all(c["passed"] for c in report["checks"])
    except (AssertionError, RuntimeError, OSError) as exc:
        report["error"] = str(exc)
    finally:
        if guest is not None:
            guest.close()
        (args.output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["green"] else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--iso", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--qemu", default="qemu-system-x86_64")
    p.add_argument("--size-mib", type=int, default=128)
    p.add_argument("--timeout", type=float, default=120)
    raise SystemExit(run(p.parse_args()))


if __name__ == "__main__":
    main()
