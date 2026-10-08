# Contributing to RXos

RXos is open source research software by **Roger Navarro**
([Rogex Laboratories / Knights Labs](https://www.rogexlaboratories.com/rx-os)).

Pull requests are welcome. You do not need permission to open one.

## What this project is

An experimental bare-metal x86_64 OS foundation: boot, drivers, RXFS, desktop,
`rgx://` local routing, a small app runtime, and a post-quantum crypto core.
It is **not** a finished product. Status labels in the code and docs
(**IMPLEMENTED / PARTIAL / STUB / TODO**) are intentional.

Project page: <https://www.rogexlaboratories.com/rx-os>

## Bugs are expected

This is research software with real edges:

- Drivers are polled or partially interrupt-driven; edge cases hang or misbehave.
- The WM, editor, and Roxenite parser are incomplete on purpose.
- Crypto passes KATs and boot self-tests but has **no external audit**.
- Network (Rogex Wired) is raw L2; multi-node demos can still flake.
- Persistence is opt-in and only as solid as the virtio-blk / ATA paths.

If something breaks, that is useful signal — not a failure of the contribution
model. Prefer a small repro (`make run` steps, serial log, guest config) over
a vague “it doesn’t work”.

Known themes and honesty rules: `README.md`, `docs/STATUS.md`, `ROADMAP.md`.

## How to contribute

1. Fork the repo (or branch off the default branch).
2. Build and smoke-test locally:
   ```bash
   tools/check_toolchain.sh   # or tools/setup-fedora.sh once
   make && make test          # 26 QEMU assertions
   # optional:
   make test-disk             # persistence
   make net-test-host         # Wired wire-format checks
   ```
3. Keep changes focused. One concern per PR when you can.
4. Match the surrounding style: freestanding C, NASM, Rust `no_std` in
   `rogex-core/`. No libc. No silent “everything is fine” when a stage fails.
5. Open a pull request against the default branch. Describe:
   - what you changed and why
   - how you tested it
   - any leftover risk or follow-up

Issues and draft PRs are fine for design talk. Large rewrites without prior
discussion are harder to land — open an issue first if you plan one.

## What we will not pretend

- Do not add marketing claims (“unbreakable”, “production-ready PQ mesh”, etc.).
- Do not mark STUB/TODO code as done without a verified path (QEMU + notes).
- Do not strip honesty banners or self-tests to make boots look greener.

## License

RXos is licensed under the **GNU GPLv3** (or later) — see `LICENSE`.

By contributing you agree your changes are licensed under the same GPLv3 (or
later) terms as the rest of the tree, unless a file says otherwise. That keeps
the project free: edits are welcome; closed proprietary products based on this
codebase are not.

## Contact / credit

Author: **Roger Navarro** — Rogex Laboratories (Knights Labs).  
Site: <https://www.rogexlaboratories.com/rx-os>
