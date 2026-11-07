/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Tasks and the scheduler, kernel side: the task control blocks, the
 * task table, and the code that decides which task runs next.
 *
 * A task always runs unprivileged, in thread mode, on its own stack
 * (the process stack, PSP), inside its own RAM partition. Since week 5
 * the tasks take turns on the CPU (preemptive round robin):
 *   - SysTick counts the ticks and, when the running task has used up
 *     its time slice (QUANTUM ticks), asks for a switch;
 *   - a task can also give the CPU away itself: yield, delay, exit;
 *   - the switch itself happens in PendSV (kernel/pendsv.s), which
 *     calls task_switch_context() to choose the next task.
 * When no application task can run, the idle task runs.
 */
#ifndef TASK_H
#define TASK_H

#include <stdint.h>

/* The length of a time slice, in ticks (one tick = 10 ms). A task that
 * keeps the CPU for QUANTUM ticks in a row is switched out. Change it
 * from the command line: `make QUANTUM=2 run`. QUANTUM = 0 means "never
 * switch on the tick": tasks run until they give the CPU away
 * themselves. */
#ifndef QUANTUM
#define QUANTUM 5
#endif

/* The states of a task. */
enum task_state {
    TASK_UNUSED,
    TASK_READY,         /* could run, waits for its turn */
    TASK_RUNNING,       /* owns the CPU now (exactly one task at a time) */
    TASK_BLOCKED,       /* waits for a tick (task_delay, task_delay_until) */
    TASK_WAITING,       /* waits for one loaded task id */
    TASK_FINISHED,      /* has called exit: never runs again */
};

/* The task control block (TCB): everything the kernel knows about one
 * task. The addresses come from the linker script (scripts/mps2_m3.ld). */
struct tcb {
    /* The task's saved stack pointer, while the task is not running.
     * It MUST stay the first field: PendSV_Handler (kernel/pendsv.s) is
     * assembly and reads and writes it at offset 0 of the TCB, with a
     * plain `ldr`/`str`, without knowing the rest of the struct. */
    uint32_t *sp;
    const char *name;           /* short name for messages, e.g. "A" */
    void ( *entry )( void );    /* the function the task runs */
    uint32_t *stack_top;        /* first address above the task's stack */
    uint32_t part_start;        /* lowest address of the partition */
    uint32_t part_end;          /* first address after the partition */
    uint32_t *data_start;       /* the task's variables in RAM: start... */
    uint32_t *data_end;         /* ...and end (exclusive) */
    const uint32_t *data_load;  /* their initial values, stored in FLASH */
    enum task_state state;      /* READY, RUNNING, BLOCKED or FINISHED */
    uint32_t wake_tick;         /* if BLOCKED: the tick at which it is READY again */
    uint32_t task_id;           /* generation-tagged id for a loaded program */
    uint32_t wait_id;           /* if WAITING: loaded task being joined */
    uint32_t *wait_result;      /* caller's saved r0, completed by task exit */
    uint8_t executable;         /* project MPU policy input */
};

#define PROGRAM_SLOT_COUNT 4
#define SHELL_TASK_INDEX 0
#define PROGRAM_TASK_FIRST 1
#define IDLE_TASK_INDEX ( PROGRAM_TASK_FIRST + PROGRAM_SLOT_COUNT )
#define NUM_TASKS ( IDLE_TASK_INDEX + 1 )

/* The task table (defined in kernel/task.c). The trace reads the names. */
extern struct tcb tasks[NUM_TASKS];

/* The TCB of the task that runs now, or 0 while the kernel runs alone
 * (before the first task starts). PendSV_Handler reads and writes it. */
extern struct tcb *tcb_current;

/* Ticks since the scheduler started (one tick = 10 ms). */
extern uint32_t tick_count;

/* Kernel-only service: prepare every task and start the first one.
 * Called from svc_dispatch when main() asks with `svc`. Returns the
 * EXC_RETURN value that enters the task (0xFFFFFFFD). */
uint32_t task_start_first( void );

/* Called by SysTick_Handler at every tick: count the tick, wake the
 * tasks whose delay is over, and ask for a switch when the time slice
 * of the running task is used up. */
void task_increment_tick( void );

/* Ask for a context switch: make PendSV pending. The switch happens as
 * soon as no more urgent exception is running. */
void task_request_switch( void );

/* Block the current task until tick_count reaches wake_tick, and ask
 * for a switch. Used by the delay system calls. */
void task_block_until( uint32_t wake_tick );

/* The current task has called exit: mark it finished, and ask for a
 * switch. It never runs again. */
void task_finish_current( void );

/* Given project scaffolding. Student shell code calls task_configure_shell()
 * from a strong task_project_configure() definition. The loader validates and
 * copies a program, then calls task_program_create() with its entry point.
 * Generation ids, waiters, completion records and fault reclamation are kept
 * here so teams do not have to reimplement scheduler bookkeeping. The loader
 * supplies the validated entry and contracted top-of-slot stack address. */
void task_project_configure( void );
void task_configure_shell( const char *name, void ( *entry )( void ),
                           uint32_t *stack_top, uint32_t part_start,
                           uint32_t part_end, uint32_t *data_start,
                           uint32_t *data_end, const uint32_t *data_load );
int task_program_create( unsigned slot, const char *name,
                         void ( *entry )( void ), uint32_t *stack_top,
                         int executable );
int task_program_slot_free( unsigned slot );
int task_wait_program( uint32_t id, uint32_t *saved_result );
void task_fault_current( void );
int task_current_is_program( void );

/* Called only by PendSV_Handler (kernel/pendsv.s), after it has saved
 * the old task's registers: choose the next task (round robin) and make
 * it the current one (tcb_current, states, MPU region 1). */
void task_switch_context( void );

#endif /* TASK_H */
