/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * irq_save / irq_restore: mask interrupts around a short piece of code
 * and then put the mask back exactly as it was. Declared for C in
 * kernel/irq.h.
 *
 * Typical use from C:
 *     uint32_t old = irq_save();   // mask interrupts, remember the old state
 *     ... touch data that an interrupt handler also touches ...
 *     irq_restore(old);            // put the old state back
 *
 * Why "save and restore" instead of just cpsid / cpsie? If the caller
 * had already masked interrupts, a plain `cpsie i` at the end would
 * unmask them too early. Restoring the saved value leaves them masked
 * in that case, and unmasked otherwise.
 *
 * PRIMASK is a 1-bit CPU register: 1 = interrupts masked, 0 = allowed.
 * C cannot name it, so these two functions are written in assembly.
 *
 * How can C call an assembly function? Both sides follow the same
 * rules, the AAPCS (Procedure Call Standard for the Arm Architecture):
 *   - the first four arguments travel in r0, r1, r2, r3;
 *   - the return value comes back in r0;
 *   - `bl func` puts the return address in lr (the link register),
 *     and the function returns with `bx lr` ("jump to the address in lr");
 *   - r0-r3 and r12 may be destroyed by the function: the caller does
 *     not expect them to survive the call ("caller-saved");
 *   - r4-r11 must hold the same values when the function returns: a
 *     function that wants to use them must save them first and restore
 *     them before returning ("callee-saved").
 * The two functions below only use r0 and lr and never call anything,
 * so they have nothing to save: no push, no pop, no stack at all.
 */

/* Same assembler settings as boot/startup.s. */
    .syntax unified             /* modern ARM assembly syntax */
    .cpu cortex-m3              /* only instructions this CPU really has */
    .thumb                      /* Thumb code (the only kind Cortex-M runs) */

    .section .text,"ax",%progbits   /* code goes into the normal code section */

/* ------------------------------------------------------------------ */
/* uint32_t irq_save(void)                                             */
/* ------------------------------------------------------------------ */
/* No arguments. Returns the old PRIMASK in r0 (the AAPCS result
 * register), then masks interrupts.
 * C equivalent (pseudo-C, since C cannot name PRIMASK):
 *     uint32_t irq_save(void) {
 *         uint32_t old = PRIMASK;  // remember the current state
 *         PRIMASK = 1;             // mask interrupts
 *         return old;              // give the old state to the caller
 *     }
 */
    .global irq_save            /* visible to C files */
    .type irq_save, %function   /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
irq_save:
    mrs r0, primask             /* old = PRIMASK; r0 is also the return value */
    cpsid i                     /* PRIMASK = 1: interrupts masked */
    bx lr                       /* return old (already in r0) to the caller */
    .size irq_save, . - irq_save /* record its size in bytes */

/* ------------------------------------------------------------------ */
/* void irq_restore(uint32_t old)                                      */
/* ------------------------------------------------------------------ */
/* One argument, `old`, which arrives in r0 (the first AAPCS argument
 * register). Writes it back into PRIMASK. Returns nothing.
 * C equivalent (pseudo-C):
 *     void irq_restore(uint32_t old) {
 *         PRIMASK = old;           // masked again only if it was masked before
 *     }
 */
    .global irq_restore         /* visible to C files */
    .type irq_restore, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
irq_restore:
    msr primask, r0             /* PRIMASK = old (old arrived in r0) */
    bx lr                       /* return to the caller */
    .size irq_restore, . - irq_restore /* record its size in bytes */
