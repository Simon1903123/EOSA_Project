/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Minimal ARM Cortex-M3 boot code, written entirely in assembly:
 * the vector table, the reset handler, and a default exception handler.
 *
 * Nothing here is C: when the CPU comes out of reset there is no C
 * runtime yet (initialized variables are not in RAM, uninitialized ones
 * are not zero). This file builds that runtime by hand and then calls
 * main(). Every block below starts with the C code it is equivalent to,
 * so the assembly can be read side by side with something familiar.
 *
 * The file is organized in three parts:
 *   1. the vector table  - the list of addresses the CPU reads on reset
 *                          and whenever an exception or a device
 *                          interrupt happens;
 *   2. Reset_Handler     - the first code that runs after power-on;
 *   3. Default_Handler   - where every exception without its own
 *                          handler ends up.
 */

/* ------------------------------------------------------------------ */
/* Assembler settings                                                  */
/* ------------------------------------------------------------------ */

/* `.syntax unified`: use the modern ARM assembly syntax, the one the
 * ARM reference manuals use (shared by ARM and Thumb code). */
    .syntax unified
/* `.cpu cortex-m3`: only accept instructions this CPU really has, so a
 * typo or an unsupported instruction becomes a build error. */
    .cpu cortex-m3
/* `.thumb`: emit Thumb instructions. Cortex-M CPUs can only execute
 * Thumb code; they have no "ARM mode" at all. */
    .thumb

/* ------------------------------------------------------------------ */
/* Symbols defined elsewhere                                           */
/* ------------------------------------------------------------------ */

/* These come from scripts/mps2_m3.ld (the linker script). They are
 * addresses, not variables:
 *   _estack          top of the stack (end of RAM)
 *   _sidata          where the initial values of .data are stored in FLASH
 *   _sdata, _edata   start and end of .data in RAM
 *   _sbss, _ebss     start and end of .bss in RAM
 * `main` is defined in src/main.c.
 * The assembler treats any name it does not find in this file as
 * "defined somewhere else" and lets the linker fill in the address,
 * so they need no declaration here (unlike `extern` in C). */

/* ================================================================== */
/* 1. The vector table                                                 */
/* ================================================================== */

/* The vector table is an array of 32-bit words placed at the very
 * start of FLASH (the linker script puts section .isr_vector first,
 * at address 0x00000000).
 *   - word 0 is the initial value of the stack pointer (SP);
 *   - word 1 is the address of the first instruction to run (reset);
 *   - words 2..15 are the addresses of the handlers of the CPU's own
 *     exceptions (faults, system calls, system timer, ...);
 *   - words 16..47 are the addresses of the handlers of the board's
 *     device interrupts (UARTs, timers, ...), IRQ 0..31.
 * When exception number N happens, the CPU reads word N and jumps
 * there.
 *
 * C equivalent:
 *     void (* const isr_vector[])(void) = {
 *         (void (*)(void)) &_estack,   // 0
 *         Reset_Handler,               // 1
 *         NMI_Handler,                 // 2
 *         ...
 *         SysTick_Handler,             // 15
 *         UART0RX_Handler,             // 16 = IRQ 0
 *         ...
 *         Default_Handler,             // 47 = IRQ 31
 *     };
 */

/* `.section NAME,"a",%progbits`: put what follows into the section
 * called .isr_vector. "a" means "allocatable" (it takes space in the
 * final image), %progbits means "it contains real data, stored in the
 * file". The linker script uses the section name to place it. */
    .section .isr_vector,"a",%progbits
/* `.global NAME`: make the name visible to other files (and to the
 * debugger), like a non-static variable in C. */
    .global isr_vector
/* `.type NAME, %object`: tell the tools this name is data, not code. */
    .type isr_vector, %object

/* A label: `isr_vector` is the address of the first word below. */
isr_vector:
/* `.word X`: store the 32-bit value X here, like one array element. */
    .word _estack               /* 0: initial SP = top of RAM */
    .word Reset_Handler         /* 1: reset: first code after power-on */
    .word NMI_Handler           /* 2: NMI (non-maskable interrupt) */
    .word HardFault_Handler     /* 3: HardFault (any fault with nowhere else to go) */
    .word MemManage_Handler     /* 4: MemManage (memory protection violation) */
    .word BusFault_Handler      /* 5: BusFault (bad memory access on the bus) */
    .word UsageFault_Handler    /* 6: UsageFault (bad instruction, divide by 0, ...) */
    .word 0                     /* 7: reserved by the architecture, must be 0 */
    .word 0                     /* 8: reserved */
    .word 0                     /* 9: reserved */
    .word 0                     /* 10: reserved */
    .word SVC_Handler           /* 11: SVCall (the `svc` instruction) */
    .word DebugMon_Handler      /* 12: debug monitor */
    .word 0                     /* 13: reserved */
    .word PendSV_Handler        /* 14: PendSV (a software-requested exception) */
    .word SysTick_Handler       /* 15: SysTick (the CPU's built-in timer) */

/* Device interrupts (IRQs).
 * After the 16 CPU exceptions come the interrupts of the devices on
 * the board. The interrupt controller inside the CPU (the NVIC) numbers
 * them from 0: "IRQ n". IRQ n is exception number 16 + n, so its
 * handler address is word 16 + n of this table:
 *     IRQ 0  -> word 16      IRQ 8  -> word 24      IRQ 31 -> word 47
 * Which device is wired to which IRQ number is decided by the board,
 * not by the CPU. The numbers below are those of the MPS2-AN385 board
 * (the same numbers are the *_IRQn constants in include/cmsdk.h).
 * The CPU supports up to 32 IRQs on this board, so there are 32 slots,
 * and the whole table is now 16 + 32 = 48 words (192 bytes, 0xC0).
 * The devices this course uses get their own handler name (a weak
 * alias, see below); the others point straight to Default_Handler.
 *
 * C equivalent (continuing the array above):
 *         UART0RX_Handler,             // 16 = IRQ 0
 *         UART0TX_Handler,             // 17 = IRQ 1
 *         ...
 *         Default_Handler,             // 47 = IRQ 31
 */
    .word UART0RX_Handler       /* 16 = IRQ 0: UART0 received a byte */
    .word UART0TX_Handler       /* 17 = IRQ 1: UART0 finished sending a byte */
    .word UART1RX_Handler       /* 18 = IRQ 2: UART1 received a byte */
    .word UART1TX_Handler       /* 19 = IRQ 3: UART1 finished sending a byte */
    .word UART2RX_Handler       /* 20 = IRQ 4: UART2 received a byte */
    .word UART2TX_Handler       /* 21 = IRQ 5: UART2 finished sending a byte */
    .word Default_Handler       /* 22 = IRQ 6: GPIO0 combined (not used in this course) */
    .word Default_Handler       /* 23 = IRQ 7: GPIO1 combined (not used in this course) */
    .word TIMER0_Handler        /* 24 = IRQ 8: timer 0 reached zero */
    .word TIMER1_Handler        /* 25 = IRQ 9: timer 1 reached zero */
    .word DUALTIMER_Handler     /* 26 = IRQ 10: dual timer */
    .word Default_Handler       /* 27 = IRQ 11: SPI (not used in this course) */
    .word UARTOVF_Handler       /* 28 = IRQ 12: some UART lost a byte (overflow) */
    .word Default_Handler       /* 29 = IRQ 13: Ethernet (not used in this course) */
    .word Default_Handler       /* 30 = IRQ 14: audio I2S (not used in this course) */
    .word Default_Handler       /* 31 = IRQ 15: touchscreen (not used in this course) */
    .word Default_Handler       /* 32 = IRQ 16: other board interrupt (not used in this course) */
    .word Default_Handler       /* 33 = IRQ 17: other board interrupt (not used in this course) */
    .word Default_Handler       /* 34 = IRQ 18: other board interrupt (not used in this course) */
    .word Default_Handler       /* 35 = IRQ 19: other board interrupt (not used in this course) */
    .word Default_Handler       /* 36 = IRQ 20: other board interrupt (not used in this course) */
    .word Default_Handler       /* 37 = IRQ 21: other board interrupt (not used in this course) */
    .word Default_Handler       /* 38 = IRQ 22: other board interrupt (not used in this course) */
    .word Default_Handler       /* 39 = IRQ 23: other board interrupt (not used in this course) */
    .word Default_Handler       /* 40 = IRQ 24: other board interrupt (not used in this course) */
    .word Default_Handler       /* 41 = IRQ 25: other board interrupt (not used in this course) */
    .word Default_Handler       /* 42 = IRQ 26: other board interrupt (not used in this course) */
    .word Default_Handler       /* 43 = IRQ 27: other board interrupt (not used in this course) */
    .word Default_Handler       /* 44 = IRQ 28: other board interrupt (not used in this course) */
    .word Default_Handler       /* 45 = IRQ 29: other board interrupt (not used in this course) */
    .word Default_Handler       /* 46 = IRQ 30: other board interrupt (not used in this course) */
    .word Default_Handler       /* 47 = IRQ 31: other board interrupt (not used in this course) */

/* `.size NAME, EXPR`: record how many bytes NAME occupies (here: from
 * the label to the current position `.`). Used by nm, objdump and gdb. */
    .size isr_vector, . - isr_vector

/* ------------------------------------------------------------------ */
/* Default handler names                                               */
/* ------------------------------------------------------------------ */

/* Every handler in the table gets a default: Default_Handler.
 *
 * `.weak NAME`: NAME is a "weak" symbol, a fallback. If any other file
 * (for example src/main.c) defines a function with the same name, the
 * linker uses that one and silently drops this fallback. So to handle
 * SysTick in C you just write
 *     void SysTick_Handler(void) { ... }
 * and the table above points to your function, with no change here.
 *
 * `.thumb_set NAME, OTHER`: make NAME another name for OTHER (same
 * address), and mark it as Thumb code so the CPU stays in Thumb state
 * when it jumps there.
 *
 * C equivalent (GCC extension):
 *     void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
 */
    .weak      NMI_Handler                      /* NMI: may be replaced from C */
    .thumb_set NMI_Handler, Default_Handler     /* otherwise it is Default_Handler */

    .weak      HardFault_Handler                /* HardFault: may be replaced from C */
    .thumb_set HardFault_Handler, Default_Handler  /* otherwise it is Default_Handler */

    .weak      MemManage_Handler                /* MemManage: may be replaced from C */
    .thumb_set MemManage_Handler, Default_Handler  /* otherwise it is Default_Handler */

    .weak      BusFault_Handler                 /* BusFault: may be replaced from C */
    .thumb_set BusFault_Handler, Default_Handler   /* otherwise it is Default_Handler */

    .weak      UsageFault_Handler               /* UsageFault: may be replaced from C */
    .thumb_set UsageFault_Handler, Default_Handler /* otherwise it is Default_Handler */

    .weak      SVC_Handler                      /* SVCall: may be replaced (C or assembly) */
    .thumb_set SVC_Handler, Default_Handler     /* otherwise it is Default_Handler */

    .weak      DebugMon_Handler                 /* debug monitor: may be replaced from C */
    .thumb_set DebugMon_Handler, Default_Handler   /* otherwise it is Default_Handler */

    .weak      PendSV_Handler                   /* PendSV: may be replaced from C */
    .thumb_set PendSV_Handler, Default_Handler  /* otherwise it is Default_Handler */

    .weak      SysTick_Handler                  /* SysTick: may be replaced from C */
    .thumb_set SysTick_Handler, Default_Handler /* otherwise it is Default_Handler */

/* The same for the device interrupts that have a name in the table.
 * Writing, for example,
 *     void UART0TX_Handler(void) { ... }
 * in a C file is enough to handle IRQ 1. */
    .weak      UART0RX_Handler                  /* UART0 receive: may be replaced from C */
    .thumb_set UART0RX_Handler, Default_Handler /* otherwise it is Default_Handler */

    .weak      UART0TX_Handler                  /* UART0 transmit: may be replaced from C */
    .thumb_set UART0TX_Handler, Default_Handler /* otherwise it is Default_Handler */

    .weak      UART1RX_Handler                  /* UART1 receive: may be replaced from C */
    .thumb_set UART1RX_Handler, Default_Handler /* otherwise it is Default_Handler */

    .weak      UART1TX_Handler                  /* UART1 transmit: may be replaced from C */
    .thumb_set UART1TX_Handler, Default_Handler /* otherwise it is Default_Handler */

    .weak      UART2RX_Handler                  /* UART2 receive: may be replaced from C */
    .thumb_set UART2RX_Handler, Default_Handler /* otherwise it is Default_Handler */

    .weak      UART2TX_Handler                  /* UART2 transmit: may be replaced from C */
    .thumb_set UART2TX_Handler, Default_Handler /* otherwise it is Default_Handler */

    .weak      TIMER0_Handler                   /* timer 0: may be replaced from C */
    .thumb_set TIMER0_Handler, Default_Handler  /* otherwise it is Default_Handler */

    .weak      TIMER1_Handler                   /* timer 1: may be replaced from C */
    .thumb_set TIMER1_Handler, Default_Handler  /* otherwise it is Default_Handler */

    .weak      DUALTIMER_Handler                /* dual timer: may be replaced from C */
    .thumb_set DUALTIMER_Handler, Default_Handler  /* otherwise it is Default_Handler */

    .weak      UARTOVF_Handler                  /* UART overflow: may be replaced from C */
    .thumb_set UARTOVF_Handler, Default_Handler /* otherwise it is Default_Handler */

/* ================================================================== */
/* 2. Reset_Handler                                                    */
/* ================================================================== */

/* From here on it is code: put it in the normal code section. The flags
 * mean "allocatable" (a) and "executable" (x). */
    .section .text,"ax",%progbits
/* Make Reset_Handler visible to the linker (the linker script names it
 * as the ENTRY point, and the vector table above refers to it). */
    .global Reset_Handler
/* `.type NAME, %function`: tell the tools this name is code. */
    .type Reset_Handler, %function
/* `.thumb_func`: the next label is a Thumb function. The tools then set
 * bit 0 of its address wherever it is stored (e.g. in the vector
 * table); on Cortex-M that bit must be 1, meaning "Thumb code". */
    .thumb_func
Reset_Handler:

    /* -------------------------------------------------------------- */
    /* Step 1. Mask interrupts during setup.                           */
    /* -------------------------------------------------------------- */
    /* Nothing must interrupt us while RAM and the system registers are
     * still half set up: an interrupt handler written in C could read
     * a variable that has not been initialized yet.
     * (After reset PRIMASK is 0, i.e. interrupts are NOT masked; no
     * peripheral has been told to raise one yet, but masking them
     * explicitly makes the rule visible instead of relying on that.)
     * C equivalent:
     *     __disable_irq();        // PRIMASK = 1
     */
    cpsid i                     /* disable interrupts: from now on none can run */

    /* -------------------------------------------------------------- */
    /* Step 2. Set the stack pointer.                                  */
    /* -------------------------------------------------------------- */
    /* The hardware already loaded SP from word 0 of the vector table
     * before running this code, so this is redundant here. We do it
     * anyway because moving SP is something the software must be able
     * to do itself: the stack is just an address in a register, and
     * these two instructions are all it takes to choose a new one.
     * C equivalent (C cannot name SP directly, so this is pseudo-C):
     *     SP = (uint32_t)&_estack;
     */
    ldr r0, =_estack            /* r0 = address of the top of the stack */
    mov sp, r0                  /* SP = r0: the stack now starts there */

    /* -------------------------------------------------------------- */
    /* Step 3. Copy .data initial values from FLASH to RAM.            */
    /* -------------------------------------------------------------- */
    /* Variables such as `int x = 5;` live in RAM, but RAM is empty at
     * power-on. The linker stored their initial values in FLASH, at
     * _sidata; copy them to their place in RAM, one 32-bit word at a
     * time.
     * C equivalent:
     *     uint32_t *src = &_sidata;
     *     uint32_t *dst = &_sdata;
     *     while (dst < &_edata) {
     *         *dst++ = *src++;
     *     }
     */
    ldr r0, =_sidata            /* src = &_sidata (r0 is src) */
    ldr r1, =_sdata             /* dst = &_sdata  (r1 is dst) */
    ldr r2, =_edata             /* r2 = &_edata, the end of the loop */
copy_data:
    cmp r1, r2                  /* compare dst with &_edata ... */
    bhs copy_data_done          /* ... if dst >= &_edata, the loop is over */
    ldr r3, [r0], #4            /* r3 = *src; src++ */
    str r3, [r1], #4            /* *dst = r3; dst++ */
    b copy_data                 /* go back and test the while condition */
copy_data_done:

    /* -------------------------------------------------------------- */
    /* Step 4. Zero .bss.                                              */
    /* -------------------------------------------------------------- */
    /* C guarantees that variables without an initializer, such as
     * `static int counter;`, start at 0. They live in .bss, which RAM
     * does not clear by itself: write zeros there.
     * C equivalent:
     *     uint32_t *dst = &_sbss;
     *     while (dst < &_ebss) {
     *         *dst++ = 0;
     *     }
     */
    ldr r1, =_sbss              /* dst = &_sbss   (r1 is dst) */
    ldr r2, =_ebss              /* r2 = &_ebss, the end of the loop */
    movs r3, #0                 /* r3 = 0, the value to write */
zero_bss:
    cmp r1, r2                  /* compare dst with &_ebss ... */
    bhs zero_bss_done           /* ... if dst >= &_ebss, the loop is over */
    str r3, [r1], #4            /* *dst = 0; dst++ */
    b zero_bss                  /* go back and test the while condition */
zero_bss_done:

    /* -------------------------------------------------------------- */
    /* Step 5. Tell the CPU where the vector table is (VTOR).          */
    /* -------------------------------------------------------------- */
    /* VTOR (Vector Table Offset Register, address 0xE000ED08) holds the
     * address where the CPU looks for the vector table when an
     * exception happens. After reset it is 0, and our table is at 0,
     * so this write changes nothing here. What matters is the idea:
     * the table's location is just a value in a register, and writing
     * this register is how software chooses which table is in use.
     * C equivalent:
     *     *(volatile uint32_t *)0xE000ED08 = (uint32_t)isr_vector;
     */
    ldr r0, =0xE000ED08         /* r0 = address of VTOR */
    ldr r1, =isr_vector         /* r1 = (uint32_t)isr_vector */
    str r1, [r0]                /* VTOR = r1 */

    /* -------------------------------------------------------------- */
    /* Step 6. Give each fault its own handler (SHCSR).                */
    /* -------------------------------------------------------------- */
    /* By default MemManage, BusFault and UsageFault are disabled: if
     * one happens, the CPU reports it as a HardFault instead, and you
     * lose the information of which kind of fault it was. Setting
     * three bits in SHCSR (System Handler Control and State Register,
     * 0xE000ED24) enables them, so each fault goes to its own handler:
     *   bit 16 MEMFAULTENA  -> MemManage_Handler
     *   bit 17 BUSFAULTENA  -> BusFault_Handler
     *   bit 18 USGFAULTENA  -> UsageFault_Handler
     * Read-modify-write: read the register, set only our bits, write
     * it back, so every other bit keeps its value.
     * C equivalent:
     *     SCB_SHCSR |= (1u << 16) | (1u << 17) | (1u << 18);
     * where SCB_SHCSR is *(volatile uint32_t *)0xE000ED24.
     */
    ldr r0, =0xE000ED24         /* r0 = address of SHCSR */
    ldr r1, [r0]                /* r1 = SCB_SHCSR (read) */
    orr r1, r1, #0x00070000     /* r1 |= (1u<<16)|(1u<<17)|(1u<<18) (modify) */
    str r1, [r0]                /* SCB_SHCSR = r1 (write) */

    /* -------------------------------------------------------------- */
    /* Step 7. Make integer division by zero trap (CCR).               */
    /* -------------------------------------------------------------- */
    /* By default the Cortex-M3 `sdiv`/`udiv` instructions return 0 when
     * dividing by zero, silently hiding the bug. Setting bit 4
     * DIV_0_TRP in CCR (Configuration and Control Register,
     * 0xE000ED14) makes a division by zero raise a UsageFault instead.
     * Bit 3 UNALIGN_TRP would similarly trap every unaligned memory
     * access; it is left off because the C compiler and libraries
     * may legitimately produce unaligned accesses, which the M3
     * handles correctly in hardware.
     * C equivalent:
     *     SCB_CCR |= (1u << 4);
     * where SCB_CCR is *(volatile uint32_t *)0xE000ED14.
     */
    ldr r0, =0xE000ED14         /* r0 = address of CCR */
    ldr r1, [r0]                /* r1 = SCB_CCR (read) */
    orr r1, r1, #(1 << 4)       /* r1 |= (1u << 4), DIV_0_TRP (modify) */
    str r1, [r0]                /* SCB_CCR = r1 (write) */

    /* -------------------------------------------------------------- */
    /* Step 8. Set exception priorities (SHPR1, SHPR2, SHPR3).         */
    /* -------------------------------------------------------------- */
    /* Each configurable exception has an 8-bit priority. A LOWER number
     * means MORE urgent: an exception can interrupt a running handler
     * only if its priority number is lower. On this chip only the
     * upper bits of each byte are implemented (the lower ones always
     * read as 0), so use values that differ in the top bits, like the
     * ones below.
     * The priorities are stored one byte per exception in the SHPR
     * registers, so each can be written on its own with a byte store:
     *   0xE000ED18  MemManage    (SHPR1 byte 0)
     *   0xE000ED19  BusFault     (SHPR1 byte 1)
     *   0xE000ED1A  UsageFault   (SHPR1 byte 2)
     *   0xE000ED1F  SVCall       (SHPR2 byte 3)
     *   0xE000ED22  PendSV       (SHPR3 byte 2)
     *   0xE000ED23  SysTick      (SHPR3 byte 3)
     * The choice made here: faults most urgent (0x00), system calls in
     * the middle (0x80), PendSV and SysTick least urgent (0xE0).
     * Since week 5, PendSV does the context switch (kernel/pendsv.s),
     * so its 0xE0 matters: with the least urgent priority, PendSV never
     * interrupts another handler. It waits until every other exception
     * has finished, and switches just before the CPU goes back to a
     * task. SysTick shares 0xE0, so the two never interrupt each other:
     * when SysTick asks for a switch, PendSV runs right after it.
     * C equivalent:
     *     volatile uint8_t *prio = (volatile uint8_t *)0xE000ED18;
     *     prio[0] = 0x00;   // MemManage
     *     prio[1] = 0x00;   // BusFault
     *     prio[2] = 0x00;   // UsageFault
     *     prio[7] = 0x80;   // SVCall   (0xE000ED1F)
     *     prio[10] = 0xE0;  // PendSV   (0xE000ED22)
     *     prio[11] = 0xE0;  // SysTick  (0xE000ED23)
     */
    ldr r0, =0xE000ED18         /* prio = r0 = address of SHPR1 byte 0 */
    movs r1, #0x00              /* r1 = 0x00, most urgent */
    strb r1, [r0, #0]           /* prio[0] = 0x00: MemManage */
    strb r1, [r0, #1]           /* prio[1] = 0x00: BusFault */
    strb r1, [r0, #2]           /* prio[2] = 0x00: UsageFault */
    movs r1, #0x80              /* r1 = 0x80, middle priority */
    strb r1, [r0, #7]           /* prio[7] = 0x80: SVCall */
    movs r1, #0xE0              /* r1 = 0xE0, least urgent */
    strb r1, [r0, #10]          /* prio[10] = 0xE0: PendSV */
    strb r1, [r0, #11]          /* prio[11] = 0xE0: SysTick */

    /* -------------------------------------------------------------- */
    /* Step 9. Setup is complete: unmask interrupts.                   */
    /* -------------------------------------------------------------- */
    /* RAM and the system registers are ready, so interrupts can now
     * run safely.
     * C equivalent:
     *     __enable_irq();         // PRIMASK = 0
     */
    cpsie i                     /* enable interrupts again */

    /* -------------------------------------------------------------- */
    /* Step 10. Call main, and stop if it ever returns.                */
    /* -------------------------------------------------------------- */
    /* C equivalent:
     *     main();
     *     for (;;) { }
     */
    bl main                     /* call main(); "branch with link" saves the return address in LR */
hang:
    b hang                      /* for (;;) {}: main should never return; if it does, stay here */

/* Literal pool. `ldr rX, =VALUE` cannot fit a full 32-bit VALUE inside
 * a 16/32-bit instruction, so the assembler stores VALUE in a nearby
 * table of constants (the "literal pool") and loads it from there.
 * `.ltorg` says "put that table here", after the last instruction,
 * where the CPU will never try to execute it as code. */
    .ltorg

/* Record Reset_Handler's size in bytes (label to here). */
    .size Reset_Handler, . - Reset_Handler

/* ================================================================== */
/* 3. Default_Handler                                                  */
/* ================================================================== */

/* Every exception without its own handler lands here (see the `.weak`
 * / `.thumb_set` lines above). An unexpected exception is a bug, so
 * spin forever: the CPU freezes at a known place, and a debugger
 * stopped here shows immediately that "something went wrong" instead
 * of the CPU running off into random memory.
 * Unlike the handler names above, Default_Handler itself is a normal
 * (strong) global symbol: it is always this loop.
 * C equivalent:
 *     void Default_Handler(void) {
 *         for (;;) { }
 *     }
 */
    .global Default_Handler     /* visible to the linker and the debugger */
    .type Default_Handler, %function /* it is code */
    .thumb_func                 /* it is Thumb code (bit 0 set in its address) */
Default_Handler:
    b Default_Handler           /* for (;;) {}: jump to itself forever */
    .size Default_Handler, . - Default_Handler /* record its size in bytes */
