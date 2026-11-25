#include <stdint.h>
#include "lib/printf.h"

static uint32_t add_seven( uint32_t value )
{
    return value + 7;
}

static uint32_t ( *operation )( uint32_t ) = add_seven;

int main( void )
{
    uintptr_t address = (uintptr_t) operation;
    printf( "thumb_function_pointer: thumb=%u result=%u\n",
            (uint32_t) ( address & 1u ), operation( 35 ) );
    return 0;
}
