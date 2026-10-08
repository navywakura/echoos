# EchOS 1.0 — Architecture Documentation

**Rogex Laboratories / Knights Labs · EchOS 1.0 "ECLIPSE" · rev 1.0 · 2026-08-22**

This document is the master architectural reference for EchOS 1.0 and its
three editions. It follows the laboratory rule: *what is not built is labeled
PLAN, never implied.*

---

## 1. Product lines

| Edition | Kernel flag | Desktop | ECHO AI | IDE/SDK | Purpose |
|---|---|---|---|---|---|
| Complete | `-DECHOS_EDITION_COMPLETE=1` | Eclipse Shell | Navi 10 (Lang/Code/Sys) | Yes | Full workstation |
| Minimal  | `-DECHOS_EDITION_MINIMAL=1`  | Eclipse Shell | none | No | Daily desktop |
| Edge     | `-DECHOS_EDITION_EDGE=1`     | none (CLI)    | Navi Mini | No | IoT / robotics / drones / cameras |

Selection happens at build time (`make edition-complete|minimal|edge`);
`kernel/version.h` resolves the flags into `ECHOS_HAS_*` capability macros so
subsystems compile out cleanly. Manifests: `editions/*/manifest.yaml`.

---

## 2. Kernel (neuromorphic unikernel)

Layered tree: `boot/ → arch/x86_64 → kernel/ → drivers/ → fs/ → ui/ → userland/`.

- **Boot**: NASM Multiboot2 → long mode → freestanding C17 kernel + Rust
  `no_std` crypto core (`rogex-core/`), single ELF.
- **Memory**: PMM, 4-level paging, `kmalloc/kfree` heap.
- **Scheduling**: cooperative (`switch.asm`) with spawn/yield/block/wake;
  preemption is deliberately out of scope.
- **Event fabric** (`kernel/event/`): fixed 64-byte `rx_event_t`, SPSC rings,
  LIF neuron model in Q16.16 fixed point, local STDP. This fabric is the
  substrate every neuromorphic feature shares — the dock's animation loop,
  input handling and Navi all consume the same event types.
- **NPU support (Complete)**: universal kernel path for standard CPUs and
  neuromorphic accelerators (BrainChip Akida via PCIe/SPI). Status:
  software backend closed and measured; silicon offload is **PLAN**
  (`docs/AKIDA.md`, cianotipo phases A–G). The kernel refuses to claim an
  NPU that is absent — `neurocpu akida` errors out without silicon.

## 3. Drivers

Modular driver model registered into a device manager at boot
(`drivers/device_manager.h`): framebuffer video, PS/2 + USB-HID-class mouse,
PIT timer, ATA/virtio-blk storage, virtio-net/e1000/r8169/rtl8139 network.
Display path is double-buffered framebuffer with damage rectangles
(`fb_damage*`), which the window server composes from.

## 4. Window server — Eclipse Shell (`ui/`)

The shell renders a dark/black flat-metal aesthetic over the Eclipse
wallpaper. Three global interaction contracts are implemented as shared
modules any surface can join:

### 4.1 Dock (`ui/dock.c`) — macOS-style, not a taskbar

- Bottom-centred bar of app tiles; running apps show an indicator dot.
- **Real-time magnification**: per-frame scale factor
  `s(i) = 1 + (MAX−1)·R²/(R²+d²)` where `d` is cursor distance. Pure Q16.16
  integer math — no FPU, safe inside the kernel.
- Pinned tiles are managed by `dock_pin()/dock_unpin()`; the global context
  menu and drag-to-dock both call them.

### 4.2 Global right-click (`ui/ctxmenu.c` + legacy menu in `window.c`)

Right-click works on **every** item class:

| Target | Items |
|---|---|
| Desktop icon | Open / Rename / Delete / Properties / Pin to Dock |
| Dock tile | Open / Unpin from Dock / Properties |
| Window | Copy / Paste / Select All / Maximize / Close / Tile-Float |
| Explorer row | Open / Copy / Cut / Paste / Vanish / New note/folder / Up / Refresh |
| Empty desktop | Settings / Wallpaper / Terminal / Explorer |

`ctxmenu.c` exposes the canonical API (`ctxmenu_open/click/render`,
`CTX_CUSTOM+n` extension slots) for new surfaces.

### 4.3 Global drag & drop (`ui/dragdrop.c` + icon slots in `window.c`)

One state machine: press arms a payload, movement past a 6 px threshold fires
ON_DRAG_START, surfaces register drop zones each frame, release delivers
ON_DRAG_DROP. Implemented today for desktop icons (slot grid reorder,
swap-on-collide) and drop-onto-dock (pin + launch). The module generalizes to
files and windows behind the same four calls.

## 5. Browser engine — Rogex Nova (proprietary)

Stack, bottom-up, all first-party:

1. **Network**: EchOS TCP/IP socket stack + own TLS 1.2 client
   (ECDHE-RSA-AES128-GCM-SHA256, SNI). Verified live from the host harness
   against rogexlaboratories.com, google.com, example.com and httpbin.org;
   in-OS Nova fetched the real rogexlaboratories.com over HTTPS (HTTP 200).
   Body delivery inside the OS is intermittent under QEMU slirp — being
   hardened for 1.0.0 (record reassembly + connection reuse).
2. **HTML parser**: token stream → styled line model (`userland/shell/
   browse.c`): headings (h1–h3 sizes), paragraphs, lists, links, bold.
3. **CSS subset**: `<style>` blocks and inline `style=""` — color (named +
   #hex/#rgb), font-size (px buckets), font-weight, text-align. Rendered
   per-line onto the framebuffer.
4. **JS runtime**: staged. React/Vite SPAs ship an empty `<div id="root">`
   and build the DOM from JavaScript, so they render their no-JS fallback
   until the single-pass interpreter lands (roadmap: ES subset → DOM API →
   event loop). Honest status: no JS today.
5. **Local pages**: `rgx://` scheme served by built-in pages and `.rgxsite`
   content.

Browser chrome: back/forward/reload buttons, editable URL bar (auto-https),
history, fullscreen window — the standard set users expect.

## 6. Media pipeline

- **Images**: native decoders in `drivers/video/imgdec.*`; JPEG/PNG/QOI
  paths power Photos, Explorer previews and wallpapers.
- **Video (PLAN → in progress)**: container parsers (MP4/MKV), H.264 baseline
  software decode, YUV420p→RGB32 conversion written straight into the video
  buffer for windowed/fullscreen playback. Nothing here is claimed shipped
  until it plays in QEMU capture.

## 7. EchOS IDE (Complete only)

Editor with syntax highlighting grammars for C, Rust, ASM, Python, HTML, CSS,
JS; integrated toolchain (assembler, compiler drivers, interactive debugger
with register/memory inspection). The IDE shells out to the same toolchain
the SDK ships, so CLI and GUI behave identically.

## 8. ECHO (Navi 10) subsystem

See `docs/ECHO_MODELS.md` — engine, A/B training contract, specialized heads
(Echo-Lang / Echo-Code / Echo-Sys), inference APIs. Edge ships Navi Mini
(retrainable, operator datasets, no cloud).

## 9. Package system

`.rxp` binary packages + `rx-pkg` manager + ten proprietary core tools.
Specification: `packages/SPEC.md`. Per-tool docs: `packages/docs/`.

## 10. Build matrix

```sh
make edition-complete   # build/EchOS-1.0.0-complete-{vm,metal}.iso
make edition-minimal    # build/EchOS-1.0.0-minimal-{vm,metal}.iso
make edition-edge       # build/EchOS-1.0.0-edge-{vm,metal}.iso
```

Freestanding C17 (`clang -target x86_64-none-elf`), NASM boot objects,
`ld.lld` static link against `linker.ld`.
