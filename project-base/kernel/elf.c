/* kernel/elf.c - ELF header and segments (Gianluigi). */
#include "kernel/elf_internal.h"

uint32_t elf_read16( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 );
}

uint32_t elf_read32( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 )
           | ( (uint32_t) p[3] << 24 );
}

int elf_fits( uint32_t off, uint32_t len, uint32_t size )
{
    return off <= size && len <= size - off;
}

int elf_check_layout( const uint8_t *f, uint32_t size, struct load_plan *p )
{
    struct elf_view v;

    /* TODO: check_header( f, size, &v ), check_segments( &v ) */
    v.f = f;
    v.size = size;
    v.entry = 0;
    v.phoff = 0;
    v.phnum = 0;
    v.shoff = 0;
    v.shnum = 0;
    v.file_off = 0;
    v.file_size = 0;
    v.load_end = 0;

    if ( elf_check_sections( &v, p ) != 0 )
        return -4;

    /* TODO: fill entry, file_off, copy_size, bss_start, bss_end, mem_end */
    return 0;
}
