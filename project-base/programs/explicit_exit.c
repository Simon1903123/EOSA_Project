#include "lib/printf.h"
#include "lib/syscalls.h"

int main( void )
{
    printf( "explicit_exit: calling sys_exit\n" );
    sys_exit();
}
