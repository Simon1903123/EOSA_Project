/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Register definitions for the peripherals of QEMU's mps2-an385 machine
 * (ARM Cortex-M3 core + CMSDK peripherals). Hand-written from the ARMv7-M
 * architecture reference and the MPS2 board's documented memory map.
 *
 * Each peripheral is a struct laid over its registers: the first field
 * sits at the base address, the next one 4 bytes later, and so on.
 * `volatile` tells the compiler the hardware can change a value at any
 * time, so it must really read or write it every time. Each struct has a
 * _Static_assert that checks the offsets at build time.
 */

/* Include guard: keep the definitions single even if included twice. */
#ifndef SCRATCH_OS_CMSDK_H
#define SCRATCH_OS_CMSDK_H

/* uint32_t and uint8_t, used for every register field below. */
#include <stdint.h>
/* offsetof(), used by the layout checks. Allowed in freestanding code. */
#include <stddef.h>

/* ==================================================================== */
/* IRQ numbers                                                          */
/* ==================================================================== */

/* Position of each device interrupt in the NVIC (0 = first device line).
 * The vector table entry is isr_vector[16 + N]. Numbers follow the AN385
 * board map and were checked on QEMU. */
#define UART0RX_IRQn    0   /* UART0 received a byte */
#define UART0TX_IRQn    1   /* UART0 finished sending a byte */
#define UART1RX_IRQn    2   /* UART1 received a byte */
#define UART1TX_IRQn    3   /* UART1 finished sending a byte */
#define UART2RX_IRQn    4   /* UART2 received a byte */
#define UART2TX_IRQn    5   /* UART2 finished sending a byte */
#define TIMER0_IRQn     8   /* APB timer 0 reached zero */
#define TIMER1_IRQn     9   /* APB timer 1 reached zero */
#define DUALTIMER_IRQn  10  /* dual timer */
#define UARTOVF_IRQn    12  /* combined UART overflow (documented; QEMU never raises it) */
/* The watchdog is not an IRQ line: its interrupt arrives as NMI. */

/* ==================================================================== */
/* SysTick (core timer, fixed address on every Cortex-M)                */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t CTRL;  /* 0x00 control and status */
    volatile uint32_t LOAD;  /* 0x04 reload value (24 bits) */
    volatile uint32_t VAL;   /* 0x08 current value; any write clears it */
    volatile uint32_t CALIB; /* 0x0C calibration value (not used here) */
} SysTick_TypeDef;

/* Cast the architectural base address to a pointer to the struct. */
#define SysTick                 ( ( SysTick_TypeDef * ) 0xE000E010UL )

#define SysTick_CTRL_ENABLE     ( 1UL << 0 )  /* start the counter */
#define SysTick_CTRL_TICKINT    ( 1UL << 1 )  /* raise the SysTick exception on wrap */
#define SysTick_CTRL_CLKSOURCE  ( 1UL << 2 )  /* 1 = core clock, 0 = reference clock */
#define SysTick_CTRL_COUNTFLAG  ( 1UL << 16 ) /* 1 if the counter wrapped since the last read */

_Static_assert( offsetof( SysTick_TypeDef, CTRL )  == 0x00, "SysTick CTRL offset" );
_Static_assert( offsetof( SysTick_TypeDef, LOAD )  == 0x04, "SysTick LOAD offset" );
_Static_assert( offsetof( SysTick_TypeDef, VAL )   == 0x08, "SysTick VAL offset" );
_Static_assert( offsetof( SysTick_TypeDef, CALIB ) == 0x0C, "SysTick CALIB offset" );

/* ==================================================================== */
/* NVIC (interrupt controller, core peripheral)                         */
/* ==================================================================== */

/* One bit per IRQ: 8 words of 32 bits cover 256 interrupt lines.
 * RESERVED fields are gaps in the address map, never touch them. */
typedef struct
{
    volatile uint32_t ISER[8];    /* 0x000 write 1 to ENABLE an IRQ */
    uint32_t RESERVED0[24];       /* 0x020 gap up to 0x080 */
    volatile uint32_t ICER[8];    /* 0x080 write 1 to DISABLE an IRQ */
    uint32_t RESERVED1[24];       /* 0x0A0 gap up to 0x100 */
    volatile uint32_t ISPR[8];    /* 0x100 write 1 to set an IRQ pending */
    uint32_t RESERVED2[24];       /* 0x120 gap up to 0x180 */
    volatile uint32_t ICPR[8];    /* 0x180 write 1 to clear a pending IRQ */
    uint32_t RESERVED3[24];       /* 0x1A0 gap up to 0x200 */
    volatile uint32_t IABR[8];    /* 0x200 read: 1 = IRQ is active now */
    uint32_t RESERVED4[56];       /* 0x220 gap up to 0x300 */
    volatile uint8_t  IPR[240];   /* 0x300 one priority byte per IRQ (0 = most urgent) */
} NVIC_TypeDef;

/* The NVIC starts at 0xE000E100. IPR therefore sits at 0xE000E400. */
#define NVIC                    ( ( NVIC_TypeDef * ) 0xE000E100UL )

_Static_assert( offsetof( NVIC_TypeDef, ISER ) == 0x000, "NVIC ISER offset" );
_Static_assert( offsetof( NVIC_TypeDef, ICER ) == 0x080, "NVIC ICER offset" );
_Static_assert( offsetof( NVIC_TypeDef, ISPR ) == 0x100, "NVIC ISPR offset" );
_Static_assert( offsetof( NVIC_TypeDef, ICPR ) == 0x180, "NVIC ICPR offset" );
_Static_assert( offsetof( NVIC_TypeDef, IABR ) == 0x200, "NVIC IABR offset" );
_Static_assert( offsetof( NVIC_TypeDef, IPR )  == 0x300, "NVIC IPR offset (0xE000E400)" );

/* ==================================================================== */
/* SCB (System Control Block, core peripheral)                          */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t CPUID;     /* 0x00 CPU identification */
    volatile uint32_t ICSR;      /* 0x04 interrupt control and state */
    volatile uint32_t VTOR;      /* 0x08 vector table offset: where the table lives */
    volatile uint32_t AIRCR;     /* 0x0C application interrupt and reset control */
    volatile uint32_t SCR;       /* 0x10 system control (sleep options) */
    volatile uint32_t CCR;       /* 0x14 configuration and control */
    volatile uint8_t  SHPR[12];  /* 0x18 priority byte of system handlers 4..15 */
    volatile uint32_t SHCSR;     /* 0x24 system handler control and state */
    volatile uint32_t CFSR;      /* 0x28 configurable fault status (Mem/Bus/Usage) */
    volatile uint32_t HFSR;      /* 0x2C HardFault status */
    volatile uint32_t DFSR;      /* 0x30 debug fault status */
    volatile uint32_t MMFAR;     /* 0x34 MemManage fault address: the address that was refused */
} SCB_TypeDef;

#define SCB                     ( ( SCB_TypeDef * ) 0xE000ED00UL )

/* ICSR bit that asks for a PendSV exception. Writing 1 here makes PendSV
 * pending; it runs as soon as no more urgent exception is active. */
#define SCB_ICSR_PENDSVSET      ( 1UL << 28 ) /* write 1: make PendSV pending */

/* SHPR3 is the 32-bit register at 0xE000ED20 = SHPR[8..11]: byte 2 is
 * the PendSV priority, byte 3 the SysTick priority. PendSV takes the
 * least urgent value (0xE0), so it runs only after every other
 * exception has finished. In SHPR3 the PendSV byte starts at bit 16. */
#define SCB_SHPR3_PENDSV_SHIFT  16            /* bit position of the PendSV byte in SHPR3 */
#define SCB_SHPR3_ADDR          0xE000ED20UL  /* address of SHPR3 */

/* SHPR index = exception number - 4. SVCall is exception 11, SysTick 15. */
#define SCB_SHPR_SVCALL         7   /* SHPR[7]  priority of SVCall */
#define SCB_SHPR_PENDSV         10  /* SHPR[10] priority of PendSV */
#define SCB_SHPR_SYSTICK        11  /* SHPR[11] priority of SysTick */

#define SCB_CCR_UNALIGN_TRP     ( 1UL << 3 )  /* fault on unaligned access */
#define SCB_CCR_DIV_0_TRP       ( 1UL << 4 )  /* fault on divide by zero */

#define SCB_SHCSR_MEMFAULTENA   ( 1UL << 16 ) /* enable the MemManage fault handler */
#define SCB_SHCSR_BUSFAULTENA   ( 1UL << 17 ) /* enable the BusFault handler */
#define SCB_SHCSR_USGFAULTENA   ( 1UL << 18 ) /* enable the UsageFault handler */

#define SCB_HFSR_VECTTBL        ( 1UL << 1 )  /* fault while reading the vector table */
#define SCB_HFSR_FORCED         ( 1UL << 30 ) /* a configurable fault escalated to HardFault */

#define SCB_CFSR_IACCVIOL       ( 1UL << 0 )  /* MemManage: instruction fetch not allowed */
#define SCB_CFSR_DACCVIOL       ( 1UL << 1 )  /* MemManage: data access not allowed */
#define SCB_CFSR_MUNSTKERR      ( 1UL << 3 )  /* MemManage: fault while unstacking on exception return */
#define SCB_CFSR_MSTKERR        ( 1UL << 4 )  /* MemManage: fault while stacking on exception entry */
#define SCB_CFSR_MMARVALID      ( 1UL << 7 )  /* MMFAR holds a valid address */
#define SCB_CFSR_IBUSERR        ( 1UL << 8 )  /* BusFault: instruction fetch error */
#define SCB_CFSR_PRECISERR      ( 1UL << 9 )  /* BusFault: precise data access error */
#define SCB_CFSR_UNDEFINSTR     ( 1UL << 16 ) /* UsageFault: undefined instruction */
#define SCB_CFSR_INVSTATE       ( 1UL << 17 ) /* UsageFault: bad EPSR state (e.g. ARM mode) */
#define SCB_CFSR_DIVBYZERO      ( 1UL << 25 ) /* UsageFault: divide by zero */

_Static_assert( offsetof( SCB_TypeDef, ICSR )  == 0x04, "SCB ICSR offset" );
_Static_assert( offsetof( SCB_TypeDef, VTOR )  == 0x08, "SCB VTOR offset" );
_Static_assert( offsetof( SCB_TypeDef, CCR )   == 0x14, "SCB CCR offset" );
_Static_assert( offsetof( SCB_TypeDef, SHPR )  == 0x18, "SCB SHPR offset" );
_Static_assert( offsetof( SCB_TypeDef, SHPR ) + 8 == 0x20, "SCB SHPR3 address (0xE000ED20)" );
_Static_assert( offsetof( SCB_TypeDef, SHCSR ) == 0x24, "SCB SHCSR offset" );
_Static_assert( offsetof( SCB_TypeDef, CFSR )  == 0x28, "SCB CFSR offset" );
_Static_assert( offsetof( SCB_TypeDef, HFSR )  == 0x2C, "SCB HFSR offset" );
_Static_assert( offsetof( SCB_TypeDef, MMFAR ) == 0x34, "SCB MMFAR offset (0xE000ED34)" );

/* ==================================================================== */
/* MPU (Memory Protection Unit, core peripheral)                        */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t TYPE;      /* 0x00 how many regions this MPU has (bits 15:8) */
    volatile uint32_t CTRL;      /* 0x04 control: turn the MPU on or off */
    volatile uint32_t RNR;       /* 0x08 region number: which region RBAR/RASR talk to */
    volatile uint32_t RBAR;      /* 0x0C region base address (and region number, if VALID) */
    volatile uint32_t RASR;      /* 0x10 region attributes and size */
} MPU_TypeDef;

#define MPU                     ( ( MPU_TypeDef * ) 0xE000ED90UL )

#define MPU_CTRL_ENABLE         ( 1UL << 0 )  /* the MPU checks every access */
#define MPU_CTRL_PRIVDEFENA     ( 1UL << 2 )  /* privileged code also gets the default memory map */

#define MPU_RBAR_REGION_MASK    ( 0xFUL )     /* bits 3:0: region number (used when VALID is set) */
#define MPU_RBAR_VALID          ( 1UL << 4 )  /* write RBAR with the region number inside, skip RNR */

#define MPU_RASR_ENABLE         ( 1UL << 0 )  /* this region is active */
#define MPU_RASR_SIZE( n )      ( ( (n) - 1UL ) << 1 ) /* region size = 2^n bytes (n = 5 for 32 B, 12 for 4 KB) */
#define MPU_RASR_B              ( 1UL << 16 ) /* bufferable */
#define MPU_RASR_C              ( 1UL << 17 ) /* cacheable */
#define MPU_RASR_S              ( 1UL << 18 ) /* shareable */
#define MPU_RASR_AP( v )        ( (uint32_t)(v) << 24 ) /* access permissions, 3 bits */
#define MPU_RASR_XN             ( 1UL << 28 ) /* execute never: no instruction fetch from here */
#define MPU_AP_PRIV_RW_USER_NONE 1UL          /* AP=001: privileged read/write, unprivileged no access */
#define MPU_AP_FULL_RW           3UL          /* AP=011: read/write for everyone */
#define MPU_AP_RO                6UL          /* AP=110: read only for everyone */

_Static_assert( offsetof( MPU_TypeDef, TYPE ) == 0x00, "MPU TYPE offset" );
_Static_assert( offsetof( MPU_TypeDef, CTRL ) == 0x04, "MPU CTRL offset" );
_Static_assert( offsetof( MPU_TypeDef, RNR )  == 0x08, "MPU RNR offset" );
_Static_assert( offsetof( MPU_TypeDef, RBAR ) == 0x0C, "MPU RBAR offset" );
_Static_assert( offsetof( MPU_TypeDef, RASR ) == 0x10, "MPU RASR offset" );

/* ==================================================================== */
/* CMSDK UART (three identical copies on the board)                     */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t DATA;      /* 0x00 read = received byte, write = byte to send */
    volatile uint32_t STATE;     /* 0x04 status flags */
    volatile uint32_t CTRL;      /* 0x08 control flags */
    volatile uint32_t INTSTATUS; /* 0x0C interrupt status; write 1 to clear a bit */
    volatile uint32_t BAUDDIV;   /* 0x10 baud-rate divider */
} CMSDK_UART_TypeDef;

#define CMSDK_UART0          ( ( CMSDK_UART_TypeDef * ) 0x40004000UL )
#define CMSDK_UART1          ( ( CMSDK_UART_TypeDef * ) 0x40005000UL )
#define CMSDK_UART2          ( ( CMSDK_UART_TypeDef * ) 0x40006000UL )

#define UART_STATE_TXBF      ( 1UL << 0 ) /* send buffer full: wait before writing DATA */
#define UART_STATE_RXBF      ( 1UL << 1 ) /* receive buffer full: a byte is waiting */
#define UART_STATE_TXOR      ( 1UL << 2 ) /* send overrun (write one to clear) */
#define UART_STATE_RXOR      ( 1UL << 3 ) /* receive overrun (write one to clear) */

#define UART_CTRL_TXEN       ( 1UL << 0 ) /* enable the transmitter */
#define UART_CTRL_RXEN       ( 1UL << 1 ) /* enable the receiver */
#define UART_CTRL_TXIRQEN    ( 1UL << 2 ) /* IRQ when a byte has been sent */
#define UART_CTRL_RXIRQEN    ( 1UL << 3 ) /* IRQ when a byte has arrived */
#define UART_CTRL_TXORIRQEN  ( 1UL << 4 ) /* IRQ on send overrun */
#define UART_CTRL_RXORIRQEN  ( 1UL << 5 ) /* IRQ on receive overrun */

#define UART_INT_TX          ( 1UL << 0 ) /* send interrupt pending; write 1 to clear */
#define UART_INT_RX          ( 1UL << 1 ) /* receive interrupt pending; write 1 to clear */
#define UART_INT_TXOR        ( 1UL << 2 ) /* send overrun interrupt pending */
#define UART_INT_RXOR        ( 1UL << 3 ) /* receive overrun interrupt pending */

_Static_assert( offsetof( CMSDK_UART_TypeDef, DATA )      == 0x00, "UART DATA offset" );
_Static_assert( offsetof( CMSDK_UART_TypeDef, STATE )     == 0x04, "UART STATE offset" );
_Static_assert( offsetof( CMSDK_UART_TypeDef, CTRL )      == 0x08, "UART CTRL offset" );
_Static_assert( offsetof( CMSDK_UART_TypeDef, INTSTATUS ) == 0x0C, "UART INTSTATUS offset" );
_Static_assert( offsetof( CMSDK_UART_TypeDef, BAUDDIV )   == 0x10, "UART BAUDDIV offset" );

/* ==================================================================== */
/* CMSDK APB timer (two copies: TIMER0, TIMER1)                         */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t CTRL;      /* 0x00 control flags */
    volatile uint32_t VALUE;     /* 0x04 current counter value (counts down) */
    volatile uint32_t RELOAD;    /* 0x08 value loaded when the counter reaches 0 */
    volatile uint32_t INTSTATUS; /* 0x0C read: IRQ pending; write 1: clear it (INTCLEAR) */
} CMSDK_TIMER_TypeDef;

#define CMSDK_TIMER0         ( ( CMSDK_TIMER_TypeDef * ) 0x40000000UL )
#define CMSDK_TIMER1         ( ( CMSDK_TIMER_TypeDef * ) 0x40001000UL )

#define TIMER_CTRL_EN        ( 1UL << 0 ) /* enable the timer */
#define TIMER_CTRL_EXTIN     ( 1UL << 1 ) /* use the external input as enable */
#define TIMER_CTRL_EXTCLK    ( 1UL << 2 ) /* use the external input as clock */
#define TIMER_CTRL_IRQEN     ( 1UL << 3 ) /* raise an IRQ when the counter reaches 0 */
#define TIMER_INT_PENDING    ( 1UL << 0 ) /* IRQ pending; write 1 to clear */

_Static_assert( offsetof( CMSDK_TIMER_TypeDef, CTRL )      == 0x00, "TIMER CTRL offset" );
_Static_assert( offsetof( CMSDK_TIMER_TypeDef, VALUE )     == 0x04, "TIMER VALUE offset" );
_Static_assert( offsetof( CMSDK_TIMER_TypeDef, RELOAD )    == 0x08, "TIMER RELOAD offset" );
_Static_assert( offsetof( CMSDK_TIMER_TypeDef, INTSTATUS ) == 0x0C, "TIMER INTSTATUS offset" );

/* ==================================================================== */
/* CMSDK watchdog                                                       */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t LOAD;      /* 0x000 start value of the down counter */
    volatile uint32_t VALUE;     /* 0x004 current counter value */
    volatile uint32_t CTRL;      /* 0x008 control flags */
    volatile uint32_t INTCLR;    /* 0x00C write any value to clear the interrupt and reload */
    volatile uint32_t RIS;       /* 0x010 raw interrupt status */
    volatile uint32_t MIS;       /* 0x014 masked interrupt status */
    uint32_t RESERVED[762];      /* 0x018 gap up to 0xC00 */
    volatile uint32_t LOCK;      /* 0xC00 write the key to unlock, anything else to lock */
} CMSDK_WDOG_TypeDef;

#define CMSDK_WDOG           ( ( CMSDK_WDOG_TypeDef * ) 0x40008000UL )

#define WDOG_UNLOCK_KEY      0x1ACCE551UL /* write it to LOCK to allow register writes */
#define WDOG_CTRL_INTEN      ( 1UL << 0 ) /* enable the interrupt (arrives as NMI) */
#define WDOG_CTRL_RESEN      ( 1UL << 1 ) /* reset the system on the second expiry */

_Static_assert( offsetof( CMSDK_WDOG_TypeDef, LOAD )   == 0x000, "WDOG LOAD offset" );
_Static_assert( offsetof( CMSDK_WDOG_TypeDef, VALUE )  == 0x004, "WDOG VALUE offset" );
_Static_assert( offsetof( CMSDK_WDOG_TypeDef, CTRL )   == 0x008, "WDOG CTRL offset" );
_Static_assert( offsetof( CMSDK_WDOG_TypeDef, INTCLR ) == 0x00C, "WDOG INTCLR offset" );
_Static_assert( offsetof( CMSDK_WDOG_TypeDef, RIS )    == 0x010, "WDOG RIS offset" );
_Static_assert( offsetof( CMSDK_WDOG_TypeDef, MIS )    == 0x014, "WDOG MIS offset" );
_Static_assert( offsetof( CMSDK_WDOG_TypeDef, LOCK )   == 0xC00, "WDOG LOCK offset" );

/* ==================================================================== */
/* CMSDK AHB GPIO (general-purpose pins)                                */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t DATA;         /* 0x000 read: current pin levels */
    volatile uint32_t DATAOUT;      /* 0x004 value driven on the output pins */
    uint32_t RESERVED[2];           /* 0x008 gap up to 0x010 */
    volatile uint32_t OUTENABLESET; /* 0x010 write 1 to make a pin an output */
    volatile uint32_t OUTENABLECLR; /* 0x014 write 1 to make a pin an input */
} CMSDK_GPIO_TypeDef;

#define CMSDK_GPIO0          ( ( CMSDK_GPIO_TypeDef * ) 0x40010000UL )

_Static_assert( offsetof( CMSDK_GPIO_TypeDef, DATA )         == 0x000, "GPIO DATA offset" );
_Static_assert( offsetof( CMSDK_GPIO_TypeDef, DATAOUT )      == 0x004, "GPIO DATAOUT offset" );
_Static_assert( offsetof( CMSDK_GPIO_TypeDef, OUTENABLESET ) == 0x010, "GPIO OUTENABLESET offset" );
_Static_assert( offsetof( CMSDK_GPIO_TypeDef, OUTENABLECLR ) == 0x014, "GPIO OUTENABLECLR offset" );

/* ==================================================================== */
/* FPGAIO (board LEDs and buttons)                                      */
/* ==================================================================== */

typedef struct
{
    volatile uint32_t LED;       /* 0x00 one bit per LED: 1 = on */
    uint32_t RESERVED;           /* 0x04 not used here */
    volatile uint32_t BUTTON;    /* 0x08 one bit per button/switch (read only) */
} FPGAIO_TypeDef;

#define FPGAIO               ( ( FPGAIO_TypeDef * ) 0x40028000UL )

_Static_assert( offsetof( FPGAIO_TypeDef, LED )    == 0x00, "FPGAIO LED offset" );
_Static_assert( offsetof( FPGAIO_TypeDef, BUTTON ) == 0x08, "FPGAIO BUTTON offset" );

#endif /* SCRATCH_OS_CMSDK_H */
