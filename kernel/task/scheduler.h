/*
 * Cooperative scheduler (v4.7+). Real stacks + ctx switch; not preemptive.
 * A task that never yields keeps the CPU. That is intentional for now.
 */
#ifndef RXOS_SCHEDULER_H
#define RXOS_SCHEDULER_H

#include "rogex.h"
#include "arch.h"

#define SCHED_MAX_TASKS  4
/* 64 KiB. It was 16384, which is smaller than a single rx_actor_t (16704
   bytes, thanks to its 256-slot event inbox) — so one actor as a local blew
   the whole stack. The size is now larger than any single object in the tree
   by a wide margin, and sched_stack_headroom() below makes an overflow
   announce itself instead of quietly eating the neighbouring globals. */
#define SCHED_STACK_SIZE 65536      /* per task; interrupts run on it too    */

typedef enum {
    TASK_FREE = 0,
    TASK_READY,        /* runnable, waiting for the CPU                      */
    TASK_RUNNING,      /* currently executing                                */
    TASK_BLOCKED,      /* waiting on I/O or an event; skipped by the picker  */
    TASK_DONE          /* returned from its entry point                      */
} task_state_t;

typedef void (*task_entry_t)(void);

/* Take over the current flow of control as task 0 ("boot"). Must be called
   before any spawn; the boot task keeps the bootstrap stack. */
void        scheduler_init(void);

/* Create a runnable task with its own stack. Returns the task id, or -1. */
int         sched_spawn(const char *name, task_entry_t entry);

/* Give up the CPU to the next runnable task. Returns when rescheduled. If no
   other task is runnable this returns immediately (cheap no-op). */
void        sched_yield(void);

/* Mark the current task BLOCKED and yield. It will not be picked again until
   someone calls sched_wake() on it. Returns once woken. */
void        sched_block(void);
void        sched_wake(int task_id);
int         sched_current(void);

/* Cooperative wait used by polled drivers: yield while a condition is false,
   giving the rest of the system CPU during a hardware transfer instead of
   spinning. Bounded by `budget` yields; returns true if the condition became
   true, false on timeout. */
bool        sched_wait_until(bool (*ready)(void), int budget);

/* Relax inside a driver's bounded polling loop. Spins cheaply most of the
   time, but every 256th iteration hands the CPU to other runnable tasks, so a
   slow hardware transfer no longer freezes the machine — this is what makes
   the polled disk drivers *cooperative* without rewriting them around
   completion interrupts.
   Constraint (documented rather than locked): disk access is confined to the
   shell task, so a yield here cannot re-enter a disk driver. If a second task
   ever needs the disk, add a mutex before relying on this. */
/* --- stack overflow detection ---------------------------------------------
   Each spawned stack is stamped with a guard pattern. A task that overruns
   writes through it on its way into whatever BSS follows — which is how the
   `bench` command corrupted the heap free list on one machine and the
   physical allocator's counters on another, producing two faults that looked
   unrelated. Checking the stamp turns silent corruption into a statement. */
bool        sched_stacks_intact(void);      /* false if a guard was clobbered */
int         sched_stack_worst_used(void);   /* deepest use seen, in bytes     */
const char *sched_stack_worst_task(void);

static inline void sched_poll_relax(int i) {
    if ((i & 0xFF) == 0xFF) sched_yield();
    else arch_relax();
}

/* Telemetry. */
int         scheduler_task_count(void);
const char *scheduler_status(void);
uint64_t    sched_switches(void);
const char *sched_task_name(int i);
int         sched_task_state(int i);

bool        scheduler_selftest(void);   /* spawn/yield/block/wake round-trip */

#endif /* RXOS_SCHEDULER_H */
