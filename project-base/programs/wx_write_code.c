#include <stdint.h>

#include "lib/printf.h"

int main( void )
{
    volatile uint16_t *code = (volatile uint16_t *)( (uintptr_t)&main & ~(uintptr_t)1u );

    printf( "wx_write_code: storing into code\n" );
    *code = 0xbf00u;
    return 0;
}
