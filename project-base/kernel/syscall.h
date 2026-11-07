/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Kernel side of the system calls.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Run the system call asked for by `svc #n`. Called only by
 * SVC_Handler (kernel/svc_entry.s), with frame = the address of the
 * 8 registers the hardware saved on exception entry, and exc_return =
 * the EXC_RETURN value of this exception. A normal service writes its
 * result into frame[0], which becomes the caller's r0, and returns
 * exc_return unchanged (a service that gives the CPU away only makes
 * PendSV pending; the switch happens after this exception). Only
 * SYS_START_TASKS enters a task instead, and returns the EXC_RETURN
 * value for that.
 * SVC_Handler ends the exception with the value returned. */
uint32_t svc_dispatch( uint32_t *frame, uint32_t exc_return );

#endif /* SYSCALL_H */
