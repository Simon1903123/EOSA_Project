#include <stdint.h>
#include "lib/printf.h"
#include "lib/syscalls.h"
#include "kernel/task.h"

#define LINE_MAX 31                 

// Simboli del linker script
extern uint32_t _shell_part_start[], _shell_stack_top[], _shell_part_end[];
extern uint32_t _shell_data_start[], _shell_data_end[];
extern const uint32_t _shell_data_load[];   /* in FLASH: sola lettura */


// Aspetta solo UN carattere dalla UART 
static int read_char(void)
{
    for (;;)
    {
        int c = sys_read();
        if (c != -1)
        {
            return c;              
        }
        task_delay(1);              /* lascia CPU per 10 ms */
    }
}


static int read_line(char *buf)
{
    int n = 0;                     

    for (;;)
    {
        char ch = (char) read_char();

        if (ch == '\r' || ch == '\n')
        {
            printf("\n");           
            buf[n] = '\0';          
            return n;
        }

        if (n < LINE_MAX)           // al massimo 31: la 32a cella è per '\0' 
        {
            buf[n] = ch;
            n++;
            printf("%c", ch);       
        }
        // altrimenti: carattere scartato
    }
}



static void shell_main(void)
{
    char line[LINE_MAX + 1];        /* sullo stack della shell, nella sua partizione */

    for (;;)
    {
        printf("scratch> ");        /* spazio finale, niente a capo */
        read_line(line);

        /* Tappa 3 */
    }
}

/* Hook chiamato una volta da task_start_first, dentro il kernel:
 * registra la shell come task. Non la esegue. */
void task_project_configure(void)
{
    task_configure_shell("shell",
                         shell_main,                     /* entry: indirizzo della funzione */
                         _shell_stack_top,               /* già uint32_t *  */
                         (uint32_t) _shell_part_start,   /* il prototipo vuole un intero */
                         (uint32_t) _shell_part_end,     /* idem */
                         _shell_data_start,              /* già uint32_t *  */
                         _shell_data_end,                /* già uint32_t *  */
                         _shell_data_load);              /* già const uint32_t * */
}