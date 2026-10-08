/*
 * Bounded static queue with the numbers a robot report needs.
 *
 * No allocation, ever: the storage is the caller's array and the capacity is
 * fixed at init. A full queue drops the new record and counts the drop —
 * blocking a sensor producer is how a control loop turns into a pile-up.
 *
 * Every queue keeps what the phase asks for: capacity, depth, high-water
 * mark, pushes, pops, drops and expiries. High water is the number that says
 * whether the pipeline ever nearly failed, and it is the one a summary
 * average always hides.
 */
#ifndef ECHOS_RQUEUE_H
#define ECHOS_RQUEUE_H

#include "rogex.h"

typedef struct {
    uint8_t *slots;
    uint32_t elem_size;
    uint32_t capacity;
    uint32_t head, tail;        /* free-running; depth = head - tail       */
    uint32_t high_water;
    uint32_t pushed, popped, drops, expired;
    const char *name;
} rqueue_t;

void     rqueue_init(rqueue_t *q, const char *name, void *storage,
                     uint32_t elem_size, uint32_t capacity);
bool     rqueue_push(rqueue_t *q, const void *elem);
bool     rqueue_pop(rqueue_t *q, void *out);
uint32_t rqueue_depth(const rqueue_t *q);
bool     rqueue_empty(const rqueue_t *q);
void     rqueue_reset(rqueue_t *q);
/* Count a record thrown away downstream because its deadline had passed. */
void     rqueue_count_expired(rqueue_t *q);

#endif /* ECHOS_RQUEUE_H */
