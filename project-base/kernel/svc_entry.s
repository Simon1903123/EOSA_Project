/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * SVC_Handler: the entry point of every system call (`svc #n`).
 * It finds the registers saved by the hardware, hands them to the C
 * function svc_dispatch() in kernel/syscall.c, and ends the exception
 * with the EXC_RETURN value svc_dispatch() chooses.
 *
 * What the hardware does when `svc` runs (exception entry):
 *   1. it pushes 8 registers onto the stack the caller was using, in
 *      this order of increasing address:
 *          frame[0] = r0     frame[4] = r12
 *          frame[1] = r1     frame[5] = lr   (the caller's lr)
 *          frame[2] = r2     frame[6] = pc   (where to resume afterwards)
 *          frame[3] = r3     frame[7] = xPSR (status flags)
 *      Since week 4 there are two stacks in use:
 *        - a task runs on its own stack, the process stack (PSP);
 *        - main() runs on the main stack (MSP). It calls `svc` once,
 *          to start the first task.
 *      So the frame may be on either stack.
 *   2. it puts a special value in lr, called EXC_RETURN, which says
 *      where the exception came from (and so where to go back):
 *          0xFFFFFFFD: thread mode, process stack (a task)
 *          0xFFFFFFF9: thread mode, main stack    (main)
 *      They differ only in bit 2: 1 means PSP, 0 means MSP.
 *   3. it jumps to SVC_Handler (word 11 of the vector table).
 *
 * C equivalent (pseudo-C, since C cannot name MSP, PSP or lr):
 *     void SVC_Handler(void) {
 *         uint32_t *frame = (lr & 4) ? PSP : MSP;   // which stack has the frame?
 *         lr = svc_dispatch(frame, lr);             // run it, choose how to return
 *         return_from_exception(lr);                // bx lr
 *     }
 *
 * Why does svc_dispatch() return an EXC_RETURN value? Most services
 * return to the caller, so they give back the same value. But the
 * start service, called once by main() on the main stack, enters the
 * first task instead (0xFFFFFFFD, on that task's PSP). The C code
 * decides; this file only uses its answer. (Services that switch to
 * another task, like yield or exit, do not need this: they only make
 * PendSV pending, and kernel/pendsv.s does the switch.)
 *
 * Stack alignment: on exception entry the hardware leaves SP aligned to
 * 8 bytes, which the AAPCS asks for at every call. Nothing is pushed
 * here, so SP is still aligned at `bl svc_dispatch`.
 *
 * This definition is strong: it replaces the weak SVC_Handler alias in
 * boot/startup.s.
 */

/* Same assembler settings as boot/startup.s. */
    .syntax unified             /* modern ARM assembly syntax */
    .cpu cortex-m3              /* only instructions this CPU really has */
    .thumb                      /* Thumb code (the only kind Cortex-M runs) */

    .section .text,"ax",%progbits   /* code goes into the normal code section */

    .global SVC_Handler         /* visible to the linker: the vector table uses it */
    .type SVC_Handler, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
SVC_Handler:
    /* Which stack holds the frame? Test bit 2 of EXC_RETURN. The result
     * of `tst` is only kept in the flags: Z = 1 when the bit is 0. */
    tst lr, #4                  /* flags = lr & 4: is bit 2 set? */
    /* `ite eq` (if-then-else): the next instruction runs only if Z = 1
     * (eq), the one after only if Z = 0 (ne). */
    ite eq                      /* if ((lr & 4) == 0) ... else ... */
    mrseq r0, msp               /*     frame = MSP;  // came from main() */
    mrsne r0, psp               /* else frame = PSP;  // came from a task */
    /* r0 is the first argument of a C function (AAPCS), r1 the second. */
    mov r1, lr                  /* exc_return = lr: second argument */
    /* `bl` overwrites lr with the return address, but EXC_RETURN is
     * safe: svc_dispatch has it in r1, and gives it (or another value)
     * back in r0. */
    bl svc_dispatch             /* r0 = svc_dispatch(frame, exc_return) */
    /* Jumping to an EXC_RETURN value ends the exception: the hardware
     * pops 8 registers from the stack that value names, and resumes. */
    bx r0                       /* return_from_exception(r0) */
    .size SVC_Handler, . - SVC_Handler /* record its size in bytes */
