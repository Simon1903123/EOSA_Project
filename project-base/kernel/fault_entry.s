    .syntax unified
    .cpu cortex-m3
    .thumb
    .section .text,"ax",%progbits

    /* Given exception-entry scaffold. C receives the fault kind and the
     * EXC_RETURN value, which says whether the fault came from a task. */
    .macro FAULT name, number
    .global \name
    .type \name, %function
    .thumb_func
\name:
    mov r1, lr
    movs r0, #\number
    b fault_dispatch
    .size \name, . - \name
    .endm

    FAULT MemManage_Handler, 1
    FAULT BusFault_Handler, 2
    FAULT UsageFault_Handler, 3
    FAULT HardFault_Handler, 4
