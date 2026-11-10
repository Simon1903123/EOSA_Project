#include "../lib/syscalls.h"
#include "../lib/printf.h"

int main(void) {
    printf("hello from a loaded program\n");
    return 0; /* Quando questo return viene eseguito, il controllo torna al file user_start.s */
}