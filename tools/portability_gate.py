"""Fail when a generic module reaches for the machine directly.

Phase F's requirement is not "we wrote a HAL", it is "nothing generic
depends on x86 any more". A header full of good intentions does not enforce
that; this does, and it runs as part of the certificate.

What counts as generic: the kernel proper, the filesystem, the userland and
the console — the code that has to compile on AArch64 unchanged. What does
not: arch/, and device drivers, because a PS/2 controller, a PIC and a PCI
configuration port *are* x86 and their drivers are allowed to know it.

The patterns are code shapes, not words: `__asm__`, a port access, a named
x86 register or chip. A comment that mentions the PIT is not a violation —
prose explaining why something moved is exactly what should survive here.
"""
import argparse
import json
from pathlib import Path
import re
import sys

# Generic trees: these must build for another architecture with no edits.
GENERIC = ["kernel", "fs", "userland", "ui"]
# Machine trees: allowed to know what machine this is.
EXEMPT = ["arch", "drivers", "legacy", "build", "backup"]

VIOLATIONS = [
    ("inline assembly",   re.compile(r"\b__asm__\b|\basm\s+volatile\b")),
    ("port I/O",          re.compile(r"\b(inb|outb|inw|outw|inl|outl)\s*\(")),
    ("x86 instruction",   re.compile(r"\b(invlpg|rdtsc|rdmsr|wrmsr|cpuid)\s*\(")),
    ("Multiboot2",        re.compile(r"\bMB2_TAG_|\bMULTIBOOT2_")),
    ("PIC/PIT/APIC regs", re.compile(r"\b(PIC1_COMMAND|PIC2_COMMAND|PIT_CH0|"
                                     r"PIT_HZ|APIC_BASE|LAPIC_)\b")),
    # An include is a dependency even when no line of code shows it: pulling
    # in pit.h is how "generic" quietly becomes "x86" again.
    ("chip header",       re.compile(r'#\s*include\s+"(ports|pit|irq|gdt|idt|'
                                     r'apic|i8042)\.h"')),
]


def code_only(line):
    """Strip comments and string literals.

    Prose is not a violation: a comment saying why the PIT moved behind the
    HAL, or a help line telling the user that idle is `hlt`, is exactly what
    should survive a port. Only what the compiler turns into instructions
    counts, so strings and comments come out before matching — otherwise the
    gate flags `#include "cli.h"` for containing an x86 mnemonic, which is
    how a check like this stops being believed."""
    line = re.sub(r"/\*.*?\*/", "", line)
    line = re.sub(r"//.*$", "", line)
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    return line


def scan(root):
    findings = []
    for tree in GENERIC:
        base = root / tree
        if not base.exists():
            continue
        for path in sorted(base.rglob("*.[ch]")):
            rel = path.relative_to(root)
            if any(str(rel).startswith(e + "/") for e in EXEMPT):
                continue
            for n, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
                line = code_only(raw)
                # An #include is code even though it looks like a string.
                if raw.lstrip().startswith("#include"):
                    line = raw
                if not line.strip():
                    continue
                for what, pattern in VIOLATIONS:
                    if pattern.search(line):
                        findings.append({"file": str(rel), "line": n,
                                         "kind": what, "text": raw.strip()[:120]})
    return findings


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, default=Path("."))
    p.add_argument("--json", type=Path)
    args = p.parse_args()

    findings = scan(args.root.resolve())
    report = {"generic_trees": GENERIC, "violations": findings,
              "clean": not findings}
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if findings:
        print(f"[portability] {len(findings)} generic modules still reach for "
              f"the machine:")
        for f in findings:
            print(f"  {f['file']}:{f['line']}  {f['kind']}  {f['text']}")
        return 1
    print("[portability] generic trees are architecture-neutral "
          f"({', '.join(GENERIC)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
