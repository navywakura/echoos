#include "rqueue.h"

void rqueue_init(rqueue_t *q, const char *name, void *storage,
                 uint32_t elem_size, uint32_t capacity) {
    q->slots = (uint8_t *)storage;
    q->elem_size = elem_size;
    q->capacity = capacity;
    q->head = q->tail = 0;
    q->high_water = 0;
    q->pushed = q->popped = q->drops = q->expired = 0;
    q->name = name;
}

uint32_t rqueue_depth(const rqueue_t *q) { return q->head - q->tail; }
bool rqueue_empty(const rqueue_t *q) { return q->head == q->tail; }

bool rqueue_push(rqueue_t *q, const void *elem) {
    uint32_t depth = rqueue_depth(q);
    uint8_t *dst;
    if (!q->slots || !q->capacity) return false;
    if (depth >= q->capacity) { q->drops++; return false; }
    dst = q->slots + (q->head % q->capacity) * q->elem_size;
    k_memcpy(dst, elem, q->elem_size);
    q->head++;
    depth++;
    if (depth > q->high_water) q->high_water = depth;
    q->pushed++;
    return true;
}

bool rqueue_pop(rqueue_t *q, void *out) {
    if (!q->slots || rqueue_empty(q)) return false;
    k_memcpy(out, q->slots + (q->tail % q->capacity) * q->elem_size, q->elem_size);
    q->tail++;
    q->popped++;
    return true;
}

void rqueue_reset(rqueue_t *q) {
    q->head = q->tail = 0;
    q->high_water = 0;
    q->pushed = q->popped = q->drops = q->expired = 0;
}

void rqueue_count_expired(rqueue_t *q) { q->expired++; }
