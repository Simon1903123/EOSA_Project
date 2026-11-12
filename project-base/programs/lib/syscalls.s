/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * User side of the system calls: the functions declared in
 * lib/syscalls.h. Each one is only two instructions: `svc #n`, then
 * `bx lr` (sys_exit and task_exit never return, so they end with a
 * loop instead).
 *
 * Why so short? Because of the AAPCS (the calling rules every C
 * function follows):
 *   - The caller has already put the arguments in r0, r1, r2 before
 *     jumping here. `svc` makes the hardware push r0-r3, r12, lr, pc
 *     and xPSR onto the stack, so the kernel finds the arguments in
 *     that saved frame: frame[0] = r0, frame[1] = r1, frame[2] = r2.
 *     Nothing needs to be moved.
 *   - The kernel writes its result into frame[0], the saved copy of r0.
 *     When the exception ends, the hardware pops the frame back into
 *     the registers, so r0 now holds the result. r0 is also where the
 *     AAPCS puts a function's return value, so `bx lr` returns it to
 *     the C caller as it is.
 *
 * The number after `svc` must match lib/syscalls.h (SYS_WRITE = 1,
 * SYS_READ = 2, SYS_UPTIME = 3, SYS_EXIT = 4, SYS_YIELD = 6,
 * SYS_DELAY = 7, SYS_DELAY_UNTIL = 8, SYS_TRACE_DUMP = 9,
 * SYS_TICK_COUNT = 10, SYS_IMAGE_COUNT = 11, SYS_IMAGE_NAME = 12,
 * SYS_PROGRAM_RUN = 13, SYS_PROGRAM_WAIT = 14). It is
 * written here as a plain number
 * because it is part of the instruction: the assembler cannot read the
 * C names from the header.
 */

/* Same assembler settings as boot/startup.s. */
    .syntax unified             /* modern ARM assembly syntax */
    .cpu cortex-m3              /* only instructions this CPU really has */
    .thumb                      /* Thumb code (the only kind Cortex-M runs) */

    .section .text,"ax",%progbits   /* code goes into the normal code section */

/* ------------------------------------------------------------------ */
/* int sys_write(const char *buf, unsigned len)                        */
/* ------------------------------------------------------------------ */
/* buf arrives in r0, len in r1. The accepted count comes back in r0.
 * C equivalent (pseudo-C, since C cannot write `svc`):
 *     int sys_write(const char *buf, unsigned len) {
 *         return svc(SYS_WRITE, buf, len);   // the kernel does the work
 *     }
 */
    .global sys_write           /* visible to C files */
    .type sys_write, %function  /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
sys_write:
    svc #1                      /* svc(SYS_WRITE, ...): buf, len already in r0, r1 */
    bx lr                       /* return the result, which the kernel put in r0 */
    .size sys_write, . - sys_write /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* int sys_read(void)                                                  */
/* ------------------------------------------------------------------ */
/* No arguments. The byte (or -1) comes back in r0.
 * C equivalent (pseudo-C):
 *     int sys_read(void) {
 *         return svc(SYS_READ);              // the kernel does the work
 *     }
 */
    .global sys_read            /* visible to C files */
    .type sys_read, %function   /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
sys_read:
    svc #2                      /* svc(SYS_READ) */
    bx lr                       /* return the result, which the kernel put in r0 */
    .size sys_read, . - sys_read /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* uint32_t sys_uptime(void)                                           */
/* ------------------------------------------------------------------ */
/* No arguments. The milliseconds come back in r0.
 * C equivalent (pseudo-C):
 *     uint32_t sys_uptime(void) {
 *         return svc(SYS_UPTIME);            // the kernel does the work
 *     }
 */
    .global sys_uptime          /* visible to C files */
    .type sys_uptime, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
sys_uptime:
    svc #3                      /* svc(SYS_UPTIME) */
    bx lr                       /* return the result, which the kernel put in r0 */
    .size sys_uptime, . - sys_uptime /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* void sys_exit(void)                                                 */
/* ------------------------------------------------------------------ */
/* No arguments, no result: the kernel never comes back here. It marks
 * the task finished and switches to another task (PendSV).
 * C equivalent (pseudo-C):
 *     void sys_exit(void) {
 *         svc(SYS_EXIT);                     // the kernel ends this task
 *         for (;;) { }                       // never reached
 *     }
 */
    .global sys_exit            /* visible to C files */
    .type sys_exit, %function   /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
sys_exit:
    svc #4                      /* svc(SYS_EXIT) */
    b sys_exit                  /* never reached; if it were, ask again */
    .size sys_exit, . - sys_exit /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* void task_yield(void)                                               */
/* ------------------------------------------------------------------ */
/* No arguments, no result. The kernel asks for a switch (PendSV), which
 * runs right after the `svc`; the task continues here at its next turn.
 * C equivalent (pseudo-C):
 *     void task_yield(void) {
 *         svc(SYS_YIELD);                    // let another task run
 *     }
 */
    .global task_yield          /* visible to C files */
    .type task_yield, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
task_yield:
    svc #6                      /* svc(SYS_YIELD) */
    bx lr                       /* back to the caller, at the task's next turn */
    .size task_yield, . - task_yield /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* void task_delay(uint32_t ticks)                                     */
/* ------------------------------------------------------------------ */
/* ticks arrives in r0. The task is blocked; it continues here once the
 * ticks have passed and its turn has come.
 * C equivalent (pseudo-C):
 *     void task_delay(uint32_t ticks) {
 *         svc(SYS_DELAY, ticks);             // block for `ticks` ticks
 *     }
 */
    .global task_delay          /* visible to C files */
    .type task_delay, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
task_delay:
    svc #7                      /* svc(SYS_DELAY, ...): ticks already in r0 */
    bx lr                       /* back to the caller, after the delay */
    .size task_delay, . - task_delay /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* int task_delay_until(uint32_t *last_wake, uint32_t period)          */
/* ------------------------------------------------------------------ */
/* last_wake arrives in r0, period in r1. 0 or -1 comes back in r0.
 * C equivalent (pseudo-C):
 *     int task_delay_until(uint32_t *last_wake, uint32_t period) {
 *         return svc(SYS_DELAY_UNTIL, last_wake, period);
 *     }
 */
    .global task_delay_until    /* visible to C files */
    .type task_delay_until, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
task_delay_until:
    svc #8                      /* svc(SYS_DELAY_UNTIL, ...): last_wake, period already in r0, r1 */
    bx lr                       /* return the result, which the kernel put in r0 */
    .size task_delay_until, . - task_delay_until /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* void trace_dump(void)                                               */
/* ------------------------------------------------------------------ */
/* No arguments, no result. The kernel prints the trace on the UART.
 * C equivalent (pseudo-C):
 *     void trace_dump(void) {
 *         svc(SYS_TRACE_DUMP);               // the kernel prints
 *     }
 */
    .global trace_dump          /* visible to C files */
    .type trace_dump, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
trace_dump:
    svc #9                      /* svc(SYS_TRACE_DUMP) */
    bx lr                       /* back to the caller */
    .size trace_dump, . - trace_dump /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* uint32_t task_tick_count(void)                                      */
/* ------------------------------------------------------------------ */
/* No arguments. The tick count comes back in r0.
 * C equivalent (pseudo-C):
 *     uint32_t task_tick_count(void) {
 *         return svc(SYS_TICK_COUNT);        // the kernel reads it
 *     }
 */
    .global task_tick_count     /* visible to C files */
    .type task_tick_count, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
task_tick_count:
    svc #10                     /* svc(SYS_TICK_COUNT) */
    bx lr                       /* return the result, which the kernel put in r0 */
    .size task_tick_count, . - task_tick_count /* record its size in bytes */

/* Project loader stubs. The kernel intentionally has no handlers yet. */
    .global sys_image_count
    .type sys_image_count, %function
    .thumb_func
sys_image_count:
    svc #11
    bx lr
    .size sys_image_count, . - sys_image_count

    .global sys_image_name
    .type sys_image_name, %function
    .thumb_func
sys_image_name:
    svc #12
    bx lr
    .size sys_image_name, . - sys_image_name

    .global sys_program_run
    .type sys_program_run, %function
    .thumb_func
sys_program_run:
    svc #13
    bx lr
    .size sys_program_run, . - sys_program_run

    .global sys_program_wait
    .type sys_program_wait, %function
    .thumb_func
sys_program_wait:
    svc #14
    bx lr
    .size sys_program_wait, . - sys_program_wait

/* ------------------------------------------------------------------ */
/* void task_exit(void)                                                */
/* ------------------------------------------------------------------ */
/* Where a task goes when its function returns. The kernel puts the
 * address of task_exit in the lr of the frame it builds to start a
 * task (kernel/task.c), so the task's final `bx lr` jumps here.
 * C equivalent:
 *     void task_exit(void) {
 *         sys_exit();                        // the task is finished
 *     }
 */
    .global task_exit           /* visible to C files (the kernel takes its address) */
    .type task_exit, %function  /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
task_exit:
    bl sys_exit                 /* sys_exit(); it does not come back */
    b task_exit                 /* never reached; if it were, try again */
    .size task_exit, . - task_exit /* record its size in bytes */
