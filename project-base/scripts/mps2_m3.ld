/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Linker script for QEMU's mps2-an385 machine (ARM Cortex-M3, MPS2
 * platform). Memory layout matches the board's actual FLASH/RAM
 * addresses and sizes.
 *
 * Deliberately minimal: one kernel stack, reserved static shell and idle
 * partitions, four reserved 4 KB program slots, and no heap
 * (nothing here allocates dynamically).
 */

/* Physical memory regions of the board, with their access attributes.
 * The linker uses these to place sections and to catch overflow. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M  /* code + read-only data, execute-in-place */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M  /* data, bss, stack */
}

/* The image's entry point - the symbol whose address the debugger and
 * QEMU use as the start of execution (also entry 1 of the vector table). */
ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* initial SP = top of RAM; the stack grows downward from here */
_Min_Stack_Size = 0x1000;              /* 4 KB reserved for the kernel stack - checked against .bss below */
PART_SIZE       = 0x1000;              /* size of one task partition: 4 KB (one MPU region needs a power of two) */
TASK_STACK_SIZE = 0x400;               /* 1 KB of each partition is the task's stack, at its bottom */
PROGRAM_SLOT_SIZE = 0x1000;            /* loaded code, data and stack share one 4 KB MPU region */
PROGRAM_SLOT_COUNT = 4;                /* fixed pool required by the published project contract */
__fs_image_start = 0x00200000;         /* QEMU loads build/fs.img here */

SECTIONS
{
    /* Vector table first, at address 0, so the CPU finds the initial SP
     * and reset vector where the architecture requires them. */
    .isr_vector :
    {
        . = ALIGN(4);            /* vector table must be word-aligned */
        KEEP(*(.isr_vector))     /* KEEP: never garbage-collect it, even though nothing "calls" it */
        . = ALIGN(4);            /* keep the location counter aligned for what follows */
    } > FLASH                    /* the vector table lives at the base of FLASH */

    /* Executable code and read-only data. */
    .text :
    {
        . = ALIGN(4);           /* word-align the start of this section */
        *(.text*)               /* all code from every input object */
        *(.rodata*)             /* string literals, const tables */
        . = ALIGN(4);           /* word-align the end so _etext is aligned */
        _etext = .;             /* end-of-text marker (unused here, handy when debugging) */
    } > FLASH                   /* place this section in the FLASH region */

    /* Each static task partition is 4 KB, aligned to 4 KB:
     *   [ stack (1 KB) | the task's .data and .bss | unused padding ]
     * The stack is at the bottom so an overflow leaves the partition
     * instead of silently damaging the task's own variables.
     * These statements come BEFORE the generic .data/.bss below: the
     * linker gives an input section to the first statement that matches it. */

    /* Reserve the shell partition even though the base supplies no shell
     * implementation. This freezes the program-slot addresses to the
     * published contract and gives student shell.o private data and stack. */
    .shell_stack ALIGN(ORIGIN(RAM), 4096) (NOLOAD) :
    {
        _shell_part_start = .;
        . = . + TASK_STACK_SIZE;
        _shell_stack_top = .;
    } > RAM

    .shell_data :
    {
        . = ALIGN(4);
        _shell_data_start = .;
        *shell.o(.data .data.* .bss .bss.* COMMON)
        . = ALIGN(4);
        _shell_data_end = .;
    } > RAM AT > FLASH
    _shell_data_load = LOADADDR(.shell_data);

    .shell_pad (NOLOAD) :
    {
        . = _shell_part_start + PART_SIZE;
        _shell_part_end = .;
    } > RAM

    /* Idle task. The shell partition above already selected RAM after
     * the preceding FLASH section. The idle
     * task runs when no other task can, so it gets a partition of its
     * own like any task. It has no variables of its own. */
    .idle_stack ALIGN(4096) (NOLOAD) :
    {
        _idle_part_start = .;              /* partition start = lowest stack address */
        . = . + TASK_STACK_SIZE;           /* reserve the stack */
        _idle_stack_top = .;               /* the stack grows down from here */
    } > RAM

    .idle_data :
    {
        . = ALIGN(4);                      /* word-align the start */
        _idle_data_start = .;              /* start in RAM */
        *idle.o(.data .data.* .bss .bss.* COMMON)   /* every variable of idle.o (none) */
        . = ALIGN(4);                      /* word-align the end */
        _idle_data_end = .;                /* end in RAM */
    } > RAM AT > FLASH                     /* run address in RAM, load address in FLASH */
    _idle_data_load = LOADADDR(.idle_data);   /* FLASH address of the initial image */

    .idle_pad (NOLOAD) :
    {
        . = _idle_part_start + PART_SIZE;   /* jump to the partition end */
        _idle_part_end = .;                /* partition end (exclusive) */
    } > RAM

    /* Program slots are address space only: the later loader owns their
     * contents. Named boundaries make the complete pool contract visible
     * to C code and to build/output.map. */
    .program_slots ALIGN(4096) (NOLOAD) :
    {
        __program_slots_start = .;
        __program_slot_0_start = .; . = . + PROGRAM_SLOT_SIZE; __program_slot_0_end = .;
        . = ALIGN(4096);
        __program_slot_1_start = .; . = . + PROGRAM_SLOT_SIZE; __program_slot_1_end = .;
        . = ALIGN(4096);
        __program_slot_2_start = .; . = . + PROGRAM_SLOT_SIZE; __program_slot_2_end = .;
        . = ALIGN(4096);
        __program_slot_3_start = .; . = . + PROGRAM_SLOT_SIZE; __program_slot_3_end = .;
        __program_slots_end = .;
    } > RAM

    __program_slot_size = PROGRAM_SLOT_SIZE;
    __program_slot_count = PROGRAM_SLOT_COUNT;

    /* Link-time guards: the static task and slot geometry must fit. */
    ASSERT(_shell_data_end <= _shell_part_start + PART_SIZE, "shell data does not fit in its partition")
    ASSERT(_idle_data_end <= _idle_part_start + PART_SIZE, "idle task data does not fit in its partition")
    ASSERT((__program_slots_start & (PROGRAM_SLOT_SIZE - 1)) == 0, "program slot pool is not 4 KiB aligned")
    ASSERT(__program_slots_end - __program_slots_start == PROGRAM_SLOT_COUNT * PROGRAM_SLOT_SIZE,
           "program slot pool has the wrong size")

    _sidata = LOADADDR(.data);  /* FLASH address where .data's initial image is stored */

    /* Initialized variables: they live (run) in RAM but their initial
     * contents are stored in FLASH and copied over by Reset_Handler. */
    .data :
    {
        . = ALIGN(4);           /* word-align the start of .data */
        _sdata = .;             /* start of .data in RAM - Reset_Handler copies from here */
        *(.data*)               /* every initialized-variable input section */
        . = ALIGN(4);           /* word-align the end */
        _edata = .;             /* end of .data in RAM - copy loop stops here */
    } > RAM AT > FLASH          /* run address in RAM, load address in FLASH */

    /* Zero-initialized variables: reserve RAM, no stored image. */
    .bss :
    {
        . = ALIGN(4);           /* word-align the start of .bss */
        _sbss = .;              /* start of .bss - Reset_Handler zeroes from here */
        *(.bss*)                /* every zero-initialized input section */
        *(COMMON)               /* legacy tentative-definition symbols */
        . = ALIGN(4);           /* word-align the end */
        _ebss = .;              /* end of .bss - zero loop stops here */
    } > RAM

    __StackLimit = _estack - _Min_Stack_Size;  /* lowest address the kernel stack may grow to */
    /* Link-time guard: fail the build if .bss and the reserved kernel
     * stack would overlap, rather than corrupting memory at runtime. */
    ASSERT(__StackLimit >= _ebss, "region RAM overflowed: .bss collides with the stack")
}
