/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * The MPU (Memory Protection Unit): the hardware that decides which
 * addresses unprivileged code may touch. The kernel uses two regions:
 *   region 0: all of FLASH, read-only for tasks (code and constants);
 *   region 1: the partition of the task that runs now, read-write.
 * Everything else is off limits to tasks. The kernel (privileged) keeps
 * the normal full memory map.
 */
#ifndef MPU_H
#define MPU_H

#include <stdint.h>

/* Set up region 0 (FLASH) and switch the MPU on. Called once by main(). */
void mpu_init( void );

/* Point region 1 at the partition [base, base + size).
 * size must be a power of two (at least 32), and base a multiple of
 * size. Called by the kernel just before it starts a task. */
void mpu_set_task_region( uint32_t base, uint32_t size, int executable );

#endif /* MPU_H */
