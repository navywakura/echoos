/* Compiled ISO version. Runtime /etc/os-release may be newer after a patch. */
#ifndef ECHOS_VERSION_H
#define ECHOS_VERSION_H

#define RXOS_VERSION        "1.0.0"          /* legacy alias, do not use   */
#define ECHOS_VENDOR        "Rogex Laboratories / Knights Labs"

/* Global CLI typeface contract: every TUI/CLI surface is JetBrains Mono,
   baked to a 10x22 antialiased cell by tools/mkfont.py. */
#define ECHOS_FONT          "JetBrains Mono"

/*
 * Edition selection. The 1.x desktop lines keep compile-time flags.
 * echOS 3.0 Universal is a shared Heap-0 kernel: one ELF, edition chosen
 * at install time from a local manifest (minimal/complete/edge/server).
 *
 *   -DECHOS_EDITION_UNIVERSAL=1  3.0 CLI ISO — installer/LIVE/memtest/recovery
 *   -DECHOS_EDITION_SERVER=1     2.x server line (CLI; daemons are flags)
 *   -DECHOS_EDITION_COMPLETE=1   1.x desktop + ECHO + IDE
 *   -DECHOS_EDITION_MINIMAL=1    1.x desktop, no ECHO AI, no IDE
 *   -DECHOS_EDITION_EDGE=1       1.x CLI-only headless
 * Default (no flag): Minimal.
 *
 * 3.0.0: a robotic edge unikernel. The surface matches the binary — no
 * Echo-in-OS, no in-OS compiler, no native httpd/sshd — and the product is
 * described by what it does: a deterministic sensor -> intent -> safety-gate
 * pipeline on static memory, talking MAVLink to an autopilot that keeps the
 * aircraft. Akida remains a PCI probe that reports itself unsupported; it is
 * not a feature and is not promised anywhere.
 */
#if defined(ECHOS_EDITION_UNIVERSAL) && ECHOS_EDITION_UNIVERSAL
#  define ECHOS_VERSION       "3.0.0"
#  define ECHOS_VERSION_NAME  "echOS 3.0"
#  define ECHOS_PRETTY_NAME   "echOS 3.0.0-honest (Heap-0 · robotic edge unikernel)"
#  define ECHOS_CODENAME      "HONEST"
#  define ECHOS_SLOGAN        "Robotic edge unikernel. Static memory, measured latency, PX4 keeps the aircraft."
#  define ECHOS_EDITION       "universal"
#  define ECHOS_SHARED_KERNEL 1
#  define ECHOS_HAS_ECHO      0
#  define ECHOS_HAS_IDE       0
#  define ECHOS_HAS_DESKTOP   0
#  define ECHOS_HAS_SERVER    0
#  define ECHOS_CLI_ONLY      1
#elif defined(ECHOS_EDITION_SERVER) && ECHOS_EDITION_SERVER
#  define ECHOS_VERSION       "2.1.0"
#  define ECHOS_VERSION_NAME  "echOS 2.1 Server"
#  define ECHOS_PRETTY_NAME   "echOS 2.1.0-honest Server (Heap-0 · CLI)"
#  define ECHOS_CODENAME      "HONEST"
#  define ECHOS_SLOGAN        "Console unikernel. Neuromorphic software. Akida when the card exists."
#  define ECHOS_EDITION       "server"
#  define ECHOS_SHARED_KERNEL 1
#  define ECHOS_HAS_ECHO      0
#  define ECHOS_HAS_IDE       0
#  define ECHOS_HAS_DESKTOP   0
#  define ECHOS_HAS_SERVER    1
#  define ECHOS_CLI_ONLY      1
#elif defined(ECHOS_EDITION_COMPLETE) && ECHOS_EDITION_COMPLETE
#  define ECHOS_VERSION       "1.0.0"
#  define ECHOS_VERSION_NAME  "EchOS 1.0"
#  define ECHOS_PRETTY_NAME   "EchOS 1.0.0 (ECLIPSE)"
#  define ECHOS_CODENAME      "ECLIPSE"
#  define ECHOS_SLOGAN        "One eclipse. Three editions. Own kernel, GRUB bootloader."
#  define ECHOS_EDITION       "complete"
#  define ECHOS_SHARED_KERNEL 0
#  define ECHOS_HAS_ECHO      1
#  define ECHOS_HAS_IDE       1
#  define ECHOS_HAS_DESKTOP   1
#  define ECHOS_HAS_SERVER    0
#  define ECHOS_CLI_ONLY      0
#elif defined(ECHOS_EDITION_EDGE) && ECHOS_EDITION_EDGE
#  define ECHOS_VERSION       "1.0.0"
#  define ECHOS_VERSION_NAME  "EchOS 1.0"
#  define ECHOS_PRETTY_NAME   "EchOS 1.0.0 (ECLIPSE)"
#  define ECHOS_CODENAME      "ECLIPSE"
#  define ECHOS_SLOGAN        "One eclipse. Three editions. Own kernel, GRUB bootloader."
#  define ECHOS_EDITION       "edge"
#  define ECHOS_SHARED_KERNEL 0
#  define ECHOS_HAS_ECHO      0
#  define ECHOS_HAS_IDE       0
#  define ECHOS_HAS_DESKTOP   0
#  define ECHOS_HAS_SERVER    0
#  define ECHOS_CLI_ONLY      1
#else
#  define ECHOS_VERSION       "1.0.0"
#  define ECHOS_VERSION_NAME  "EchOS 1.0"
#  define ECHOS_PRETTY_NAME   "EchOS 1.0.0 (ECLIPSE)"
#  define ECHOS_CODENAME      "ECLIPSE"
#  define ECHOS_SLOGAN        "One eclipse. Three editions. Own kernel, GRUB bootloader."
#  define ECHOS_EDITION       "minimal"
#  define ECHOS_SHARED_KERNEL 0
#  define ECHOS_HAS_ECHO      0
#  define ECHOS_HAS_IDE       0
#  define ECHOS_HAS_DESKTOP   1
#  define ECHOS_HAS_SERVER    0
#  define ECHOS_CLI_ONLY      0
#endif

/*
 * Developer shortcuts (t/e/b/v/i/... hotkeys, Alt+Tab, workspaces).
 * OFF in Minimal and Edge on purpose: stray keys must never open apps or
 * change system state on a user machine. ON only in the Dev edition, where
 * the keyboard drives the shell.
 */
#if defined(ECHOS_EDITION_DEV) && ECHOS_EDITION_DEV
#  define ECHOS_DEV_SHORTCUTS 1
#else
#  define ECHOS_DEV_SHORTCUTS 0
#endif

/* Legacy aliases so older subsystems keep compiling during migration.
   The build system may also pass -DRXOS_AERO; don't clash with it. */
#ifndef RXOS_AERO
#define RXOS_AERO           ECHOS_HAS_DESKTOP
#endif

#endif /* ECHOS_VERSION_H */
