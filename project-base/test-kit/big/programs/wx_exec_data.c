#include <stdint.h>

#include "lib/printf.h"

static uint16_t data_code[] = { 0x4770u };

int main( void )
{
    void ( *function )( void ) = (void ( * )( void ))( (uintptr_t)data_code | 1u );

    printf( "wx_exec_data: jumping into data\n" );
    function();
    return 0;
}
