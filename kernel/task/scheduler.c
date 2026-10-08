/*
 * Cooperative round-robin with real context switches (switch.asm).
 * Yields only at explicit points — never from an ISR. Stacks are static BSS.
 * Not preemptive on purpose (keeps single-threaded drivers sane).
 */
#include "scheduler.h"

#include "arch.h"

typedef struct {
    uint64_t     rsp;              /* saved stack pointer while not running  */
    task_state_t state;
    const char  *name;
    uint64_t     switches;
    uint8_t     *stack;            /* NULL for the boot task                 */
} task_t;

static task_t   g_task[SCHED_MAX_TASKS];
/* Task 0 keeps the 256 KiB bootstrap stack, so only spawned tasks need
   storage here. 16-byte aligned as the ABI requires. */
static uint8_t  g_stack[SCHED_MAX_TASKS - 1][SCHED_STACK_SIZE]
                __attribute__((aligned(16)));
static int      g_cur;             /* currently running task id              */
static int      g_ntasks;
static uint64_t g_switches;
static bool     g_active;

/* A pattern no real stack content would spell, stamped across the low end of
   every spawned stack. A task that overruns must write through it. */
#define STACK_GUARD 0x5247584F53544B21ULL   /* "RGXOSTK!" */
#define GUARD_WORDS 8                        /* 64 bytes of tripwire          */

/* The whole stack is stamped, not only the low end: the guard words at the
   bottom are the tripwire, and the pattern further up is what lets
   sched_stack_worst_used() find the high-water mark by looking for the first
   word a task has never written. */
static void stamp_guard(uint8_t *base) {
    uint64_t *g = (uint64_t *)base;
    for (int i = 0; i < SCHED_STACK_SIZE / 8; i++) g[i] = STACK_GUARD;
}

void scheduler_init(void) {
    k_memset(g_task, 0, sizeof(g_task));
    g_task[0].state = TASK_RUNNING;
    g_task[0].name  = "boot";
    g_task[0].stack = NULL;                     /* uses the bootstrap stack  */
    g_cur = 0; g_ntasks = 1; g_switches = 0; g_active = true;
}

/* When a task's entry function returns, land here rather than off the end of
   the stack: mark it done and yield forever. */
static void task_exit(void) {
    g_task[g_cur].state = TASK_DONE;
    for (;;) sched_yield();
}

int sched_spawn(const char *name, task_entry_t entry) {
    if (!g_active || !entry) return -1;
    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        if (g_task[i].state != TASK_FREE) continue;
        uint8_t *base = g_stack[i - 1];
        /* Stamp FIRST. Stamping after the frame is built writes the guard
           pattern over it, and ctx_switch then `ret`s into 0x5247584F53544B21
           — which is exactly what happened the first time this was written. */
        stamp_guard(base);
        /* The frame layout belongs to the architecture: what a context
           switch restores, and in what order, is not something a scheduler
           can know portably. */
        g_task[i].rsp      = arch_task_frame_init(
                                 (uint64_t)(base + SCHED_STACK_SIZE),
                                 (void (*)(void))entry, task_exit);
        g_task[i].state    = TASK_READY;
        g_task[i].name     = name;
        g_task[i].stack    = base;
        g_task[i].switches = 0;
        g_ntasks++;
        return i;
    }
    return -1;
}

/* Round-robin from the task after the current one. Returns -1 if nobody else
   is runnable (the caller then just keeps going — yielding must stay cheap). */
static int pick_next(void) {
    for (int n = 1; n <= SCHED_MAX_TASKS; n++) {
        int i = (g_cur + n) % SCHED_MAX_TASKS;
        if (g_task[i].state == TASK_READY) return i;
    }
    return -1;
}

void sched_yield(void) {
    if (!g_active) return;
    int next = pick_next();
    if (next < 0) return;                       /* nobody else can run       */

    int prev = g_cur;
    if (g_task[prev].state == TASK_RUNNING) g_task[prev].state = TASK_READY;
    g_task[next].state = TASK_RUNNING;
    g_cur = next;
    g_switches++;
    g_task[next].switches++;
    arch_context_switch(&g_task[prev].rsp, g_task[next].rsp);
    /* resumed: we are `prev` again */
}

void sched_block(void) {
    if (!g_active) return;
    g_task[g_cur].state = TASK_BLOCKED;
    /* If nothing else is runnable, un-block rather than wedge the machine:
       a single-task system has nobody to wake us. */
    if (pick_next() < 0) { g_task[g_cur].state = TASK_RUNNING; return; }
    sched_yield();
}

void sched_wake(int task_id) {
    if (task_id <= 0 || task_id >= SCHED_MAX_TASKS) return;
    if (g_task[task_id].state == TASK_BLOCKED) g_task[task_id].state = TASK_READY;
}

int sched_current(void) { return g_cur; }

bool sched_wait_until(bool (*ready)(void), int budget) {
    if (!ready) return false;
    for (int i = 0; i < budget; i++) {
        if (ready()) return true;
        sched_yield();               /* let the rest of the system run       */
    }
    return ready();
}

int scheduler_task_count(void) {
    int n = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++)
        if (g_task[i].state != TASK_FREE && g_task[i].state != TASK_DONE) n++;
    return n ? n : 1;
}

const char *scheduler_status(void) {
    return g_active ? "OK (cooperative, real ctx switch)"
                    : "STUB (single-threaded unikernel)";
}

uint64_t sched_switches(void) { return g_switches; }

const char *sched_task_name(int i) {
    if (i < 0 || i >= SCHED_MAX_TASKS || g_task[i].state == TASK_FREE) return NULL;
    return g_task[i].name;
}

int sched_task_state(int i) {
    if (i < 0 || i >= SCHED_MAX_TASKS) return TASK_FREE;
    return (int)g_task[i].state;
}

/* ------------------------------ self-test ---------------------------------- */
/* Proves the switch actually works: a spawned task must run, interleave with
   the boot task, block, be woken, and finish. If ctx_switch were wrong this
   faults or hangs rather than silently "passing". */
static volatile int g_st_stage;
static int          g_st_id;

static void st_task(void) {
    g_st_stage = 1;
    sched_yield();              /* back to boot, which sets stage 2          */
    if (g_st_stage != 2) { g_st_stage = -1; return; }
    g_st_stage = 3;
    sched_block();              /* boot wakes us                            */
    if (g_st_stage != 4) { g_st_stage = -1; return; }
    g_st_stage = 5;
}

bool scheduler_selftest(void) {
    if (!g_active) return false;
    g_st_stage = 0;
    g_st_id = sched_spawn("selftest", st_task);
    if (g_st_id < 0) return false;

    sched_yield();                          /* -> task runs to its 1st yield */
    if (g_st_stage != 1) return false;
    g_st_stage = 2;
    sched_yield();                          /* -> task checks, blocks        */
    if (g_st_stage != 3) return false;
    if (sched_task_state(g_st_id) != TASK_BLOCKED) return false;

    g_st_stage = 4;
    sched_wake(g_st_id);
    sched_yield();                          /* -> task finishes              */
    if (g_st_stage != 5) return false;
    if (sched_task_state(g_st_id) != TASK_DONE) return false;

    /* reclaim the slot so later spawns start from a clean table */
    g_task[g_st_id].state = TASK_FREE;
    g_ntasks--;
    return true;
}

/* --- guard inspection ------------------------------------------------------
   Cheap enough to call from `status`: the intactness check is eight
   comparisons per task. The high-water mark is found by walking up from the
   base to the first word still holding the stamp — everything above it has
   been written at some point, which bounds the deepest use. That is why
   sched_spawn stamps the whole stack and not only its low end. */
bool sched_stacks_intact(void) {
    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        if (g_task[i].state == TASK_FREE || !g_task[i].stack) continue;
        const uint64_t *g = (const uint64_t *)g_task[i].stack;
        for (int w = 0; w < GUARD_WORDS; w++)
            if (g[w] != STACK_GUARD) return false;
    }
    return true;
}

static int         g_worst_used;
static const char *g_worst_name = "-";

int sched_stack_worst_used(void) {
    g_worst_used = 0; g_worst_name = "-";
    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        if (g_task[i].state == TASK_FREE || !g_task[i].stack) continue;
        const uint64_t *g = (const uint64_t *)g_task[i].stack;
        int words = SCHED_STACK_SIZE / 8, untouched = 0;
        while (untouched < words && g[untouched] == STACK_GUARD) untouched++;
        int used = SCHED_STACK_SIZE - untouched * 8;
        if (used > g_worst_used) {
            g_worst_used = used;
            g_worst_name = g_task[i].name ? g_task[i].name : "?";
        }
    }
    return g_worst_used;
}

const char *sched_stack_worst_task(void) { return g_worst_name; }
