/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Kernel start. Reset_Handler (boot/startup.s) has already set up the
 * CPU and the memory, and calls main(). main() starts the devices,
 * then asks the kernel (with one `svc`) to run the tasks. Each task
 * runs unprivileged, on its own stack, in its own partition, and the
 * tasks take turns on the CPU (kernel/task.c). main() never runs again:
 * when every task has finished, the idle task runs for ever.
 */

#include "drivers/uart.h"
#include "drivers/systick.h"
#include "kernel/kprintf.h"
#include "kernel/mpu.h"

int main( void )
{
    /* 1. The console first, so everything after this can print. */
    uart_init();

    /* 2. Say we are alive. kprintf works without interrupts. */
    kprintf( "scratch-os: kernel started\n" );

    /* 3. Start the 10 ms clock tick (needed by sys_uptime, sleep_ms and
     * the scheduler's time slice). */
    systick_init();

    /* 4. Set up the MPU: region 0 = FLASH, then switch it on. */
    mpu_init();

    /* 5. Start the tasks. Only an exception return can enter a task
     * (it is the only way to switch to the process stack and drop
     * privilege in one step), so main() raises an exception: `svc`
     * with the kernel-only number SYS_START_TASKS (5, lib/syscalls.h).
     * The kernel starts the idle task. It never comes back here: there
     * is always a task to run. The project later adds the shell and
     * run-time program tasks.
     * C equivalent (pseudo-C):
     *     svc(SYS_START_TASKS);   // never returns
     */
    __asm volatile (
        "svc #5             \n" /* svc(SYS_START_TASKS) */
        : : : "memory" );

    /* 6. Never reached. If it ever were, stop here. */
    for ( ;; )
    {
    }
}
