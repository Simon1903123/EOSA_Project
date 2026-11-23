#include <stdint.h>
#include "lib/printf.h"

int main( void )
{
    printf( "kernel_store: writing 0x20000000\n" );
    *(volatile uint32_t *) 0x20000000u = 0x12345678u;
    return 0;
}
