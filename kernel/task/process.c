/* Process table stub. One kernel flow, no fork/exec yet. */
#include "process.h"

static const process_t g_kernel_proc = { 0, "kernel (rxos unikernel)", "running" };

void process_init(void) {
    klog_info("process table: STUB (single kernel task, pid 0)");
}

int process_count(void) { return 1; }

const process_t *process_at(int i) { return i == 0 ? &g_kernel_proc : NULL; }
