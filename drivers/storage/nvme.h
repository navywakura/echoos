/*
 * NVMe 1.x over PCI — one controller, one namespace, one I/O queue pair.
 *
 * Polled, single request in flight, no interrupts and no MSI-X. That is the
 * whole driver: a detection is not a driver, so this one resets the
 * controller, builds admin and I/O queues, identifies the controller and the
 * namespace, and moves sectors with real NVM read/write commands. Anything
 * it cannot do it reports; it never pretends.
 */
#ifndef ECHOS_NVME_H
#define ECHOS_NVME_H

#include "rogex.h"
#include "blockdev.h"

/* Probe PCI class 01:08:02, initialise, register the namespace as a block
   device. False when there is no controller or init failed — nvme_status()
   then says which step, in words. */
bool        nvme_init(void);
bool        nvme_present(void);
blockdev_t *nvme_dev(void);

/* One line of truth for `hwprobe` / `devices`: either why it is absent, or
   the model, namespace size and queue geometry that were actually read. */
const char *nvme_status(void);

/* Identify data worth showing: model string and namespace block count. */
const char *nvme_model(void);
uint64_t    nvme_ns_blocks(void);
uint32_t    nvme_block_bytes(void);

/* Counters, so an error is a number and not a feeling. */
uint32_t nvme_completions(void);
uint32_t nvme_errors(void);
uint32_t nvme_timeouts(void);
/* Status code of the last failed command (NVMe SCT<<8 | SC), 0 if none. */
uint32_t nvme_last_error(void);

#endif /* ECHOS_NVME_H */
