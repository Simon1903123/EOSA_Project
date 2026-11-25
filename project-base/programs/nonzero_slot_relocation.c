#include "lib/printf.h"
#include "lib/syscalls.h"

int main( void )
{
    printf( "nonzero_slot: starting thumb_ptr\n" );
    int id = sys_program_run( "thumb_ptr" );
    if ( id < 0 )
    {
        printf( "nonzero_slot: start failed %d\n", id );
        return 0;
    }
    int status = sys_program_wait( id );
    printf( "nonzero_slot: child status=%d\n", status );
    return 0;
}
