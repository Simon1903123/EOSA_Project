ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text._start) *(.text*) *(.rodata*) . = ALIGN(4); }
  .data : { *(.data*) . = ALIGN(4); }
  .bss  : { *(.bss*) *(COMMON) . = ALIGN(4); }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
