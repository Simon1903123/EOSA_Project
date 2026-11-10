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

static int str_eq(const char *a, const char *b)     // restituisce 1 se le stringhe sono uguali, 0 altrimenti
{
    while(*a && *b)
    {
        if (*a != *b)
        {
            return 0;              
        }
        a++;
        b++;
    }
    return 1;
}

static int starts_with(const char *str, const char *prefix)  // restituisce 1 se str inizia con prefix, 0 altrimenti
{
    while (*prefix)
    {
        if (*str != *prefix)
        {
            return 0;              
        }
        str++;
        prefix++;
    }
    return 1;                      
}

static void cmd_help(void)  // mostra i comandi disponibili
{
    printf("help - show commands\n");
    printf("ls - list programs\n");
    printf("run <name> - run a program\n");
}


static void cmd_run(const char *name)
{
    
    /* int id = system_program_run(name);
    if (id == -2)      { printf("error: program not found\n"); return; }
    if (id == -3)      { printf("error: no free slot\n");      return; }
    if (id < 0)        { printf("error: program cannot be loaded\n"); return; }
    (void) system_program_wait(id); */   /* attende la fine del programma */
}

static void cmd_ls(void)  
{
    //char name[16];
    int n = sys_image_count();
    /*sys_image_count fa svc#11 richiamando in syscall.c task_image_count va in task.c per recuperare il n. di programmi caricati
      verificando la validità dell'immagine di programma. Se l'immagine non è valida, restituisce -1.
      Altrimenti, restituisce l'header dell'image contenendo i nomi programmi che successivamente dovrà essere utilizzato per ottenere i nomi.
    */
    if(n<0)
    {
        printf("error:no valid image\n");
        return;
    }
    else
    {
        printf("Available programs:\n");
        printf("%d programs found\n",n);
    }
    /*
    for(int i=0;i<n;i++)
    {
        if(system_image_name((unsigned) i, name, sizeof name) == 0)
        {
            printf("%s\n",name);
        }
    } */
}

static void execute(const char *line)  // esegue il comando digitato
{
    if (str_eq(line, "help"))
    {
        cmd_help();
    }
    else if (str_eq(line, "ls"))
    {
        cmd_ls();
    }
    else if (starts_with(line, "run "))
    {
        const char *name = line + 4;  
        cmd_run(name);
    }
    else
    {
        printf("error: unknown command\n");
    }
}

static void shell_main(void)
{
    char line[LINE_MAX + 1];        /* sullo stack della shell, nella sua partizione */

    for (;;)
    {
        printf("scratch> ");        /* spazio finale, niente a capo */
        read_line(line);
        execute(line);                   
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