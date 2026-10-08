/* Cooperative context switch, AAPCS64.
 *
 * Saves the callee-saved registers the ABI says a function must preserve
 * (x19-x28, x29, x30) plus the stack pointer, then loads the other task's.
 * Caller-saved registers are, by definition, the caller's problem.
 */
.section .text
.global arch_context_switch
arch_context_switch:                    /* (uint64_t *save_sp, uint64_t load_sp) */
    stp     x29, x30, [sp, #-96]!
    stp     x27, x28, [sp, #16]
    stp     x25, x26, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x21, x22, [sp, #64]
    stp     x19, x20, [sp, #80]
    mov     x2, sp
    str     x2, [x0]                    /* *save_sp = sp                    */
    mov     sp, x1                      /* sp = load_sp                     */
    ldp     x19, x20, [sp, #80]
    ldp     x21, x22, [sp, #64]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #32]
    ldp     x27, x28, [sp, #16]
    ldp     x29, x30, [sp], #96
    ret

/* First entry into a spawned task.
 *
 * A task cannot simply be resumed with x30 = entry: `ret` does not pop
 * anything, so x30 still holds `entry` while the entry function runs, and the
 * function's own `ret` re-enters it from the top. (That is what made the
 * scheduler self-test report a task back at stage 1 after its wake.) So the
 * frame parks the entry in x19 and the exit hook in x20 and returns here;
 * `blr` then leaves a real return address behind.
 */
.global arch_task_trampoline
arch_task_trampoline:
    blr     x19                     /* entry(); returns here if it returns   */
    blr     x20                     /* task_exit(); never returns            */
1:  b       1b                      /* belt and braces                       */
