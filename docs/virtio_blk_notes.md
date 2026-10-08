# virtio-blk driver notes (v4.2)

Status: **IMPLEMENTED** — modern virtio-pci block driver, polled, verified on
QEMU q35 by the double-boot persistence test (`make test-disk`).

## Why virtio-blk

q35 (the modern QEMU machine, and what `make run` uses) exposes no legacy IDE,
so the v4.1 ATA PIO driver never appeared there. virtio-blk is the native,
efficient storage path for q35 and real virtualized hosts. The disk facade
(`drivers/storage/disk.c`) now probes **virtio-blk ("vda") first, then ATA
("hda")**, so persistence "just works" on both machines and nothing above the
facade names a backend.

## Layers

```
drivers/pci/pci.c        PCI config access (mechanism #1: 0xCF8/0xCFC),
                         BAR decode (32/64-bit), capability-list walk,
                         enable memory-space + bus-master (DMA).
drivers/storage/virtio_blk.c   modern virtio-pci: vendor caps ->
                         common/notify/device cfg; VIRTIO_F_VERSION_1
                         negotiation; split virtqueue; 3-descriptor request.
drivers/storage/disk.c   facade: disk_init/present/dev/name/backend.
```

## How a request works (VirtIO 1.1, split virtqueue)

1. **Discovery**: find PCI `1af4:1042` (modern) or `:1001` (transitional),
   enable memory + bus-master, walk the vendor capabilities (id 0x09) to
   locate the common/notify/device config structures inside a BAR. Each
   region is mapped **uncached at virt==phys** (the BARs sit above the boot
   1 GiB identity map — same handling as the framebuffer/TPM).
2. **Handshake** (device_status): reset → ACKNOWLEDGE → DRIVER → require and
   set `VIRTIO_F_VERSION_1` → FEATURES_OK (verified) → set up queue 0 →
   DRIVER_OK.
3. **Virtqueue**: one zeroed 4 KiB PMM frame holds the descriptor table
   (8 entries), the available ring and the used ring at fixed sub-offsets
   (physically contiguous, identity-mapped → the device and we use the same
   addresses). `queue_size` is written down to 8 **and read back** to be
   sure the device honored it (otherwise it would index our rings out of
   bounds).
4. **A read/write** builds 3 descriptors — header (device-reads) → data
   (device-writes for reads / reads for writes) → status (device-writes) —
   publishes the head in the available ring, bumps `avail->idx` (with an
   `mfence` between, per spec ordering), and **notifies** the device at
   `notify_base + queue_notify_off * notify_off_multiplier`.
5. **Completion**: poll `used->idx` (bounded so a broken device fails instead
   of hanging), `mfence`, then read the status byte.

The whole `count` of sectors travels as **one contiguous data descriptor**, so
the queue size does not bound transfer length — only the caller's buffer does.

## Deliberate limits (honest)

- **Polled, no MSI-X / no interrupt.** The used ring is polled with a bounded
  budget. Wiring virtio's IRQ into the existing IPC pipeline (as keyboard/
  mouse already are) is a clean later step.
- **Single in-flight request**, no indirect descriptors, no multiqueue —
  matches the small, synchronous RXFS image I/O and the polled ATA driver.
- **DMA relies on virt==phys** inside the 1 GiB identity map; buffers above
  it are rejected (guard), not silently corrupted. A real DMA/bounce
  allocator is future work when RXFS images outgrow that window.
- **queue_size fixed at 8**; requires the device to offer ≥ 8 (QEMU offers
  256). The 8-entry rings fit in one frame with room to spare.

## Errors caught during implementation (internal review passes)

1. **`count > QSZ` guard was wrong** and blocked real saves: a request always
   uses 3 descriptors regardless of sector count (data is one descriptor), so
   the 9-sector RXFS record writes were being rejected. Replaced with a
   buffer-oriented cap. (Caught by the first q35 save returning "save
   FAILED".)
2. **`queue_size` honor check**: if a device ignored the shrink to 8 and kept
   256, it would index the available/used rings past our 8-entry arrays and
   corrupt memory. Added a mandatory read-back that fails cleanly.
3. **Unbounded reset wait** could hang boot on a broken device; bounded it.
4. **BARs above 1 GiB**: the virtio BAR is outside the boot identity map, so
   MMIO would fault without an explicit `vmm_map_page`; added uncached
   mapping of each cfg region.
