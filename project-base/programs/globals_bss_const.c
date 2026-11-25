#include <stdint.h>
#include "lib/printf.h"

static uint32_t initialized_global = 37;
static uint32_t zeroed_bss;
static const uint32_t constant_table[] = { 2, 4, 8, 16 };

int main( void )
{
    printf( "globals_bss_const: global=%u bss=%u table=%u sum=%u\n",
            initialized_global, zeroed_bss, constant_table[3],
            initialized_global + zeroed_bss + constant_table[3] );
    return 0;
}
