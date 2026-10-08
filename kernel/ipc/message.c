/* Tiny IRQ-safe message queue. Keyboard/mouse ISRs produce; UI pump consumes. */
#include "message.h"
#include "arch.h"

static rx_message_t g_q[MSG_SLOTS];
static int g_head, g_tail, g_count;

/* Enter/leave a critical section, preserving the caller's IF state. */
static inline uint64_t ipc_lock(void) {
    uint64_t flags;
    flags = arch_irq_save();
    return flags;
}
static inline void ipc_unlock(uint64_t flags) {
    arch_irq_restore(flags);
}

void msg_init(void) {
    uint64_t f = ipc_lock();
    g_head = g_tail = g_count = 0;
    ipc_unlock(f);
}

bool msg_send(uint32_t from, const void *data, uint32_t len) {
    if (len > MSG_PAYLOAD) return false;
    uint64_t f = ipc_lock();
    if (g_count >= MSG_SLOTS) { ipc_unlock(f); return false; }
    rx_message_t *m = &g_q[g_tail];
    m->from = from;
    m->len  = len;
    k_memcpy(m->data, data, len);
    g_tail = (g_tail + 1) % MSG_SLOTS;
    g_count++;
    ipc_unlock(f);
    return true;
}

bool msg_recv(rx_message_t *out) {
    uint64_t f = ipc_lock();
    if (g_count == 0) { ipc_unlock(f); return false; }
    *out = g_q[g_head];
    g_head = (g_head + 1) % MSG_SLOTS;
    g_count--;
    ipc_unlock(f);
    return true;
}

int msg_pending(void) {
    uint64_t f = ipc_lock();
    int n = g_count;
    ipc_unlock(f);
    return n;
}

bool msg_selftest(void) {
    msg_init();
    for (int i = 0; i < MSG_SLOTS; i++) {
        uint8_t b = (uint8_t)i;
        if (!msg_send(1, &b, 1)) return false;
    }
    uint8_t b = 0xff;
    if (msg_send(1, &b, 1)) return false;          /* must reject when full  */
    rx_message_t m;
    for (int i = 0; i < MSG_SLOTS; i++) {
        if (!msg_recv(&m) || m.len != 1 || m.data[0] != (uint8_t)i) return false;
    }
    if (msg_recv(&m)) return false;                /* must be empty now      */
    return msg_pending() == 0;
}
