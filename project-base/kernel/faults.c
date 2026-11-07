/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Given fault classification and loaded-program recovery scaffold.
 * Loader code still owns MPU permissions and successful reclamation hooks.
 */

#include "cmsdk.h"
#include "kernel/kprintf.h"
#include "kernel/task.h"

#define EXC_RETURN_PSP_BIT 4u
#define BUS_STACK_ERRORS ( ( 1u << 11 ) | ( 1u << 12 ) )

/* Stop here for ever. */
static void halt( void )
{
    for ( ;; )
    {
    }
}

void fault_dispatch( uint32_t kind, uint32_t exc_return )
{
    uint32_t cfsr = SCB->CFSR;
    uint32_t unsafe = kind == 1 ?
        cfsr & ( SCB_CFSR_MSTKERR | SCB_CFSR_MUNSTKERR ) :
        kind == 2 ? cfsr & BUS_STACK_ERRORS :
        kind == 4 ? cfsr & ( SCB_CFSR_MSTKERR | SCB_CFSR_MUNSTKERR
                             | BUS_STACK_ERRORS ) : 0;

    if ( ( exc_return & EXC_RETURN_PSP_BIT ) != 0 && !unsafe
         && task_current_is_program() )
    {
        kprintf( "%s in loaded program: CFSR=0x%x",
                 kind == 1 ? "MemManage" : kind == 2 ? "BusFault" :
                 kind == 3 ? "UsageFault" : "HardFault", cfsr );
        if ( cfsr & SCB_CFSR_MMARVALID )
            kprintf( " address=0x%x", SCB->MMFAR );
        kprintf( "\n" );
        SCB->CFSR = cfsr;
        task_fault_current();
        return;
    }

    kprintf( "%s: unrecoverable CFSR=0x%x HFSR=0x%x\n",
             kind == 1 ? "MemManage" : kind == 2 ? "BusFault" :
             kind == 3 ? "UsageFault" : "HardFault", cfsr, SCB->HFSR );
    halt();
}
