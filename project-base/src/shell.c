#include <stdint.h>
#include "lib/printf.h"
#include "lib/syscalls.h"
#include "kernel/task.h"

#define LINE_MAX 31                 // maximum number of characters stored per line

/* Symbols defined by the linker script (scripts/mps2_m3.ld).
 * They are plain addresses with no variable behind them: declared as
 * arrays, their name already gives the address. */
extern uint32_t _shell_part_start[], _shell_stack_top[], _shell_part_end[];
extern uint32_t _shell_data_start[], _shell_data_end[];

extern const uint32_t _shell_data_load[];   /* in FLASH: read-only */


/* Read one line from the UART into buf (at least LINE_MAX + 1 bytes).
 * Every stored character is echoed back unchanged; at most LINE_MAX
 * characters are stored, extra ones are dropped without echo.
 * A CR or an LF prints a single newline and ends the line.
 * On return buf is always NUL-terminated. */
static void read_line(char *buf)
{
    int n = 0;                      // characters stored

    for (;;)
    {
        // sys_read never waits: -1 means no byte has arrived yet.
        // Sleep for one tick (10 ms) so the idle task can run, then retry.
        int c = sys_read();
        if (c == -1)
        {
            task_delay(1);
            continue;
        }

        // End of line: print one newline, terminate the string.
        char ch = (char) c;
        if (ch == '\r' || ch == '\n')
        {
            printf("\n");
            buf[n] = '\0';
            return;
        }

        // Store and echo only while there is room: the last cell of buf
        // is kept for the terminating '\0'.
        if (n < LINE_MAX)
        {
            buf[n] = ch;
            n++;
            printf("%c", ch);
        }
        // Otherwise the character is silently dropped (no echo, no message).
    }
}


/* Check whether line starts with prefix.
 * If it does, return a pointer to the rest of the line, just after the
 * prefix; otherwise return 0. An exact match is a match whose rest is
 * the empty string, so one helper covers both "help" and "run <n>". */
static const char *skip_prefix(const char *line, const char *prefix)
{
    // Walk both strings together until the prefix ends or they differ.
    while (*prefix)
    {
        if (*line != *prefix)
        {
            return 0;
        }
        line++;
        prefix++;
    }
    return line;                    // the whole prefix matched
}


/* The "ls" command: list the programs stored in the image.
 * SVC 11 returns how many directory entries the image has, or -1 if the
 * image is not valid. SVC 12 then copies the name of each entry into a
 * local buffer, which lives on the shell stack, inside its partition. */
static void cmd_ls(void)
{
    char name[16];                  // names have at most 15 characters + '\0'
    int n = sys_image_count();      // SVC 11

    // Invalid image
    if (n < 0)
    {
        printf("error: no valid image\n");
        return;
    }

    // One name per line, in directory order. An entry whose name cannot
    // be copied is skipped.
    for (int i = 0; i < n; i++)
    {
        if (sys_image_name((unsigned) i, name, sizeof name) == 0)   // SVC 12
        {
            printf("%s\n", name);
        }
    }
}


/* The "run <n>" command: load a program and wait for it to finish.
 * SVC 13 loads the program into a free slot and starts it as a task,
 * returning its task id, or a negative error code. On success the shell
 * blocks in SVC 14, so no prompt is printed while the program runs. */
static void cmd_run(const char *name)
{
    int id = sys_program_run(name); // SVC 13

    // Translate the error codes into the exact messages of the spec:
    // -2 and -3 have their own message, any other negative value is generic.
    if (id == -2)
    {
        printf("error: program not found\n");
    }
    else if (id == -3)
    {
        printf("error: no free slot\n");
    }
    else if (id < 0)
    {
        printf("error: program cannot be loaded\n");
    }
    else
    {
        // The kernel prints the loader, exit and fault lines itself:
        // the shell only waits, and ignores the exit status.
        (void) sys_program_wait(id); // SVC 14
    }
}


/* Interpret one line and run the matching command.
 * Only "help", "ls" and "run <name>" are accepted, matched exactly:
 * no spaces are stripped, so "help " or " ls" are unknown commands.
 * Everything after "run " is the program name, extra spaces included. */
static void execute(const char *line)
{
    const char *rest;               // what follows the matched prefix

    // "help" with nothing after it.
    if ((rest = skip_prefix(line, "help")) && *rest == '\0')
    {
        printf("help - show commands\n");
        printf("ls - list programs\n");
        printf("run <name> - run a program\n");
    }
    // "ls" .
    else if ((rest = skip_prefix(line, "ls")) && *rest == '\0')
    {
        cmd_ls();
    }
    // "run " followed by a non-empty name: "run" and "run " are rejected.
    else if ((rest = skip_prefix(line, "run ")) && *rest != '\0')
    {
        cmd_run(rest);
    }
    // Empty line or anything else.
    else
    {
        printf("error: unknown command\n");
    }
}

/* Entry point of the shell task: runs unprivileged, forever.
 * Each turn prints the prompt, reads a line and executes it.
 * It must never return: the initial stack frame has lr = task_exit,
 * so returning would end the shell task for good. */
static void shell_main(void)
{
    char line[LINE_MAX + 1];        // on the shell stack, inside its partition

    for (;;)
    {
        printf("scratch> ");        // trailing space, no newline
        read_line(line);
        execute(line);
    }
}

/* Hook called once by task_start_first(), inside the kernel (SVC 5).
 * It overrides the weak empty version in kernel/task.c and registers
 * the shell as a task: name, entry point, stack, partition and data.
 * It does not run the shell: the scheduler starts it afterwards. */
void task_project_configure(void)
{
    task_configure_shell("shell",
                         shell_main,                     /* entry: address of the function*/
                         _shell_stack_top,               /* already uint32_t *: initial stack is built below it */
                         (uint32_t) _shell_part_start,   /* integer: used for the MPU region and pointer checks */
                         (uint32_t) _shell_part_end,     /* same */
                         _shell_data_start,              /* already uint32_t *: start of the shell variables */
                         _shell_data_end,                /* already uint32_t *: end of the shell variables */
                         _shell_data_load);              /* already const uint32_t *: initial values in FLASH */
}
