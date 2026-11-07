/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * PendSV_Handler: the context switch. It stops the task that runs now
 * and resumes another one, chosen by task_switch_context() in
 * kernel/task.c.
 *
 * Who asks for it? Nobody calls PendSV: the kernel makes it pending
 * (task_request_switch() writes ICSR.PENDSVSET) from SysTick when the
 * time slice is over, or from SVCall on yield, delay and exit. PendSV
 * has the lowest priority (0xE0, set in boot/startup.s), so it runs
 * only after every other exception has finished, just before the CPU
 * would go back to the task. So it always interrupts a task (thread
 * mode, process stack), never another handler.
 *
 * A task's registers are saved in two halves:
 *   1. on exception entry, the HARDWARE has already pushed r0-r3, r12,
 *      lr, pc and xPSR onto the task's own stack (PSP);
 *   2. r4-r11 are not saved by the hardware: this handler pushes them
 *      onto the same stack, just below the hardware frame.
 * Then the whole state of the task is on its stack, and the only thing
 * left to remember is where: the stack pointer, kept in the first field
 * of its TCB (tcb_current->sp, offset 0 of struct tcb).
 * Resuming another task is the same in reverse: load its saved sp, pop
 * r4-r11 by software, set PSP, and return from the exception: the
 * hardware pops the rest.
 *
 * C equivalent (pseudo-C, since C cannot name PSP or r4-r11):
 *     void PendSV_Handler(void) {
 *         uint32_t *sp = PSP;            // the old task's stack
 *         sp -= 8; save r4-r11 at sp;    // push r4-r11
 *         tcb_current->sp = sp;          // remember where
 *         task_switch_context();         // tcb_current = the next task
 *         sp = tcb_current->sp;          // the new task's stack
 *         load r4-r11 from sp; sp += 8;  // pop r4-r11
 *         PSP = sp;
 *         return_from_exception(0xFFFFFFFD);  // thread mode, PSP
 *     }
 *
 * Interrupts stay enabled. A UART interrupt (0xC0) may interrupt this
 * handler, but it never touches PSP or the TCBs. SysTick has the same
 * priority as PendSV, so it cannot, and SVCall cannot happen while a
 * handler runs.
 *
 * This definition is strong: it replaces the weak PendSV_Handler alias
 * in boot/startup.s.
 */

/* Same assembler settings as boot/startup.s. */
    .syntax unified             /* modern ARM assembly syntax */
    .cpu cortex-m3              /* only instructions this CPU really has */
    .thumb                      /* Thumb code (the only kind Cortex-M runs) */

    .section .text,"ax",%progbits   /* code goes into the normal code section */

    .global PendSV_Handler      /* visible to the linker: the vector table uses it */
    .type PendSV_Handler, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
PendSV_Handler:
    /* ---- 1. Save the old task ------------------------------------ */
    mrs r0, psp                 /* sp = PSP: the old task's stack, below its hardware frame */
    /* `stmdb` = "store multiple, decrement before": it stores r4..r11
     * in the 8 words below r0 (r4 at the lowest address), and the `!`
     * writes the new lowest address back into r0. Like a push, but on
     * PSP instead of the handler's own stack. */
    stmdb r0!, {r4-r11}         /* sp -= 8; sp[0..7] = r4..r11 */
    ldr r1, =tcb_current        /* r1 = &tcb_current */
    ldr r2, [r1]                /* r2 = tcb_current */
    str r0, [r2]                /* tcb_current->sp = sp  (sp is at offset 0) */

    /* ---- 2. Choose the next task ---------------------------------- */
    /* A C call may change r0-r3, r12 and lr. The old task's r4-r11 are
     * already safe on its stack, and lr (EXC_RETURN) is not needed: the
     * handler ends with a fixed value below. No push here, so the main
     * stack keeps the 8-byte alignment the hardware gave it on entry. */
    bl task_switch_context      /* task_switch_context(); changes tcb_current */

    /* ---- 3. Resume the new task ----------------------------------- */
    ldr r1, =tcb_current        /* r1 = &tcb_current */
    ldr r2, [r1]                /* r2 = tcb_current (the new task) */
    ldr r0, [r2]                /* sp = tcb_current->sp */
    /* `ldmia` = "load multiple, increment after": the reverse of stmdb.
     * It loads r4..r11 from the 8 words at r0 and moves r0 up past them. */
    ldmia r0!, {r4-r11}         /* r4..r11 = sp[0..7]; sp += 8 */
    msr psp, r0                 /* PSP = sp: now it points at the hardware frame */

    /* EXC_RETURN 0xFFFFFFFD: return to thread mode, on the process
     * stack. The hardware pops r0-r3, r12, lr, pc, xPSR from PSP, and
     * the new task goes on where it stopped (or, the first time, starts
     * at its entry). CONTROL.nPRIV was set once when the first task
     * started, and nothing clears it, so the task is unprivileged. */
    ldr r0, =0xFFFFFFFD         /* r0 = EXC_RETURN: thread mode, PSP */
    bx r0                       /* return_from_exception(0xFFFFFFFD) */

/* Literal pool for the `ldr rX, =VALUE` lines above (see boot/startup.s). */
    .ltorg

    .size PendSV_Handler, . - PendSV_Handler /* record its size in bytes */
