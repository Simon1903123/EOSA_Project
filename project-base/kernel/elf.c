/*
 * kernel/elf.c - Layer 2 of SVC 13, first half: ELF header and PT_LOAD
 * segments (Gianluigi). The second half, the sections, is elf_sections.c
 * (Athena).
 *
 * WHAT THIS FILE ANSWERS
 *   "Can this ELF file be loaded into a slot, and how?"
 *   Antonio (layer 1) gives us only f (the file, in FLASH) and size (its
 *   length). We give back 0 and a filled struct load_plan, or -4.
 *
 * THE FOUR RULES OF THIS FILE
 *   1. Never read a byte outside [0, size). Every offset and length comes
 *      from a file that may be damaged on purpose, so each one is checked
 *      with elf_fits() BEFORE it is used (assignment 6.3, big version).
 *   2. The only results are 0 and -4. A bad file must never fault the
 *      kernel.
 *   3. Read only: we never write into the slot (we are above the "point of
 *      no return" of the pipeline).
 *   4. No global variables: data travels through pointers. Internal
 *      helpers are static.
 *
 * NAMES
 *   The ELF standard names fields with a prefix: e_ (file header),
 *   p_ (one program-header row = one segment), sh_ (one section row).
 *   In our code the prefix goes away: e_phnum is read into v->phnum,
 *   p_vaddr into vaddr. Same number, two names.
 *
 */
#include "kernel/elf_internal.h"

/*
 * Code + data + BSS of a program must fit in the lower 3072 bytes of its
 * 4096-byte slot; the upper 1024 bytes are the stack, which starts at the
 * end of the slot and grows down (assignment 6.4).
 */
#define SLOT_PROG_MAX 3072u

/* ======================================================================
 * STEP 1 - The three common helpers.
 * They are also used by Athena (elf_sections.c) and Simone (relocations),
 * so their names and signatures are fixed by elf_internal.h.
 * ====================================================================== */

/*
 * elf_read16 - read a 16-bit number stored at p in the file.
 *
 * The file is little-endian: the LOW byte comes first, the HIGH byte second.
 *   p[0] = 0x19, p[1] = 0x00  ->  0x0019 = 25   (e_shnum of thumb_ptr)
 *   p[0] = 0x88, p[1] = 0x48  ->  0x4888        (e_shoff of thumb_ptr)
 * The caller must already know that p[0] and p[1] are inside the file.
 */
uint32_t elf_read16( const uint8_t *p )
{
    /* Each byte is widened to uint32_t before the shift; p[1] is moved
     * 8 bits up (times 256) and the two halves are joined with | (they
     * share no bits, so | works like a sum). */
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 );
}

/*
 * elf_read32 - read a 32-bit number stored at p in the file.
 *
 * Same idea with 4 bytes: p[0] is the lowest byte, p[3] the highest.
 *   88 48 00 00  ->  0x00004888   (e_shoff of thumb_ptr)
 * Reading byte by byte, instead of *(uint32_t *) p, avoids an unaligned
 * access fault: p comes from the file and may be any address.
 */
uint32_t elf_read32( const uint8_t *p )
{
    /* Without the (uint32_t) cast, p[3] << 24 would be computed on a
     * signed int and overflow when p[3] >= 0x80. */
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 )
           | ( (uint32_t) p[3] << 24 );
}

/*
 * elf_fits - is the range [off, off + len) completely inside [0, size)?
 *
 *   off   where the range starts, counted from the beginning of the
 *         container (a file offset, or a slot offset).
 *         Example: e_shoff = 0x4888, the start of the section table.
 *   len   how many bytes the range is long.
 *         Example: e_shnum * 40 = 1000, the whole section table.
 *   size  how big the container is: the file length (size of the ELF)
 *         or SLOT_PROG_MAX (3072) for the program part of a slot.
 *
 * Returns 1 if the range fits, 0 otherwise. All three values may come
 * from a damaged file, so off + len is never computed: with
 * off = 0xFFFFFFF0 and len = 0x20 the sum would wrap around to 0x10 and
 * look "inside".
 *
 * Examples: elf_fits( 0x4888, 1000, 19568 ) = 1   (thumb_ptr section table)
 *           elf_fits( 0, 3073, 3072 )       = 0   (too_large)
 */
int elf_fits( uint32_t off, uint32_t len, uint32_t size )
{
    /* First the start must be inside, then the length must fit in what is
     * left after the start. size - off cannot go below zero here, because
     * the first test already passed (&& stops at the first false). */
    return off <= size && len <= size - off;
}

/* ======================================================================
 * STEPS 2-4 - check_header(): pipeline step 2.1.
 * Reads the 52-byte file header, checks it, and fills the header fields of
 * v. After it returns 0, every field of the header and both tables are
 * known to be inside the file.
 * ====================================================================== */
static int check_header( const uint8_t *f, uint32_t size, struct elf_view *v )
{
    /*
     * STEP 2a - The whole 52-byte ELF header (Ehdr, ELF_EHDR_SIZE) must be
     * inside the file: every field read below lives in bytes 0..51.
     * Test: short_51.
     */
    if ( !elf_fits( 0, ELF_EHDR_SIZE, size ) )
        return -4;

    /*
     * STEP 2b - Magic, bytes 0..3: 0x7f 'E' 'L' 'F'. Fixed by the ELF
     * standard; assignment section 2: a file with another magic is refused
     * at once. Test: bad_magic.
     */
    if ( f[0] != 0x7f || f[1] != 'E' || f[2] != 'L' || f[3] != 'F' )
        return -4;

    /*
     * STEP 2c - The identification bytes right after the magic (e_ident).
     * EH_CLASS, EH_DATA and EH_VERSION are positions, not values: single
     * bytes, read directly as f[EH_CLASS].
     *   EH_CLASS   (byte 4) word size of the file: 1 = 32-bit, 2 = 64-bit.
     *                       The Cortex-M3 is a 32-bit CPU.
     *   EH_DATA    (byte 5) byte order of every number in the file:
     *                       1 = little-endian (lowest byte first),
     *                       2 = big-endian. elf_read16/32 read little-endian.
     *   EH_VERSION (byte 6) version of the ELF format: the standard defines
     *                       only version 1.
     * Assignment 6.3: "32-bit little-endian, version 1".
     * Test: bad_header sets byte 4 to 2.
     */
    if ( f[EH_CLASS] != 1        /* must be 32-bit        */
         || f[EH_DATA] != 1      /* must be little-endian */
         || f[EH_VERSION] != 1 ) /* must be version 1     */
        return -4;

    /*
     * STEP 3a - Three questions about the identity of the file. Each field
     * is at a fixed place in the header (the EH_* macro is that place).
     *
     *   e_type    byte 16, 2 bytes -> elf_read16
     *             WHAT KIND of ELF file is this? We want 2 = ET_EXEC, a
     *             complete program ready to run. The compiler also makes
     *             other kinds: a .o (1) is only a piece of a program, still
     *             with "holes" (calls to functions it cannot find yet); a
     *             shared library (3) needs dynamic linking, forbidden here.
     *   e_machine byte 18, 2 bytes -> elf_read16
     *             For WHICH CPU was it compiled? We want 40 = EM_ARM, the
     *             number the ELF standard gives to ARM. A PC program (x86-64)
     *             would have 62 and instructions the Cortex-M3 cannot run.
     *   e_version byte 20, 4 bytes -> elf_read32
     *             Which VERSION of the ELF format? We want 1. It is the same
     *             information as byte 6, stored a second time: both are checked.
     *
     * thumb_ptr: 02 00 -> 2, 28 00 -> 40, 01 00 00 00 -> 1.
     * All three values are required by assignment 6.3.
     */
    if ( elf_read16( f + EH_TYPE ) != ET_EXEC        /* must be an executable */
         || elf_read16( f + EH_MACHINE ) != EM_ARM   /* must be ARM code      */
         || elf_read32( f + EH_VERSION2 ) != 1 )     /* must be version 1     */
        return -4;

    /*
     * STEP 3b - How long one row of each table is.
     *
     * An ELF file has two TABLES, each made of rows of the same length:
     *   - the program-header table: one row per SEGMENT;
     *   - the section-header table: one row per SECTION.
     * To find row i the code computes
     *     start of row i = start of the table + i * length of one row
     * e.g. thumb_ptr sections (table at 0x4888, rows of 40 bytes):
     *     row 0 -> 0x4888, row 1 (.text) -> 0x48b0, row 2 -> 0x48d8.
     *
     * The header says how long a row is ("entry" = one row of a table):
     *   e_ehsize    byte 40  ELF Header size: length of the header itself (52)
     *   e_phentsize byte 42  Program Header ENTry size: one segment row (32)
     *   e_shentsize byte 46  Section Header ENTry size: one section row (40)
     * Do not confuse them with e_phnum / e_shnum, which say HOW MANY rows
     * there are: ...entsize = how long one row is, ...num = how many rows.
     *
     * Why they are checked: our code always uses the fixed lengths 32 and 40
     * (i * ELF_PHDR_SIZE, i * ELF_SHDR_SIZE) and reads fields at fixed places
     * inside a row (p_filesz at ph + 16). If a file said "my rows are 56
     * bytes long", row 1 would really be at +56, we would look at +32 and
     * read pieces of different rows mixed together. So we refuse the file
     * instead. Assignment 6.3: "52-byte file header, 32-byte program
     * headers, 40-byte section headers".
     */
    if ( elf_read16( f + EH_EHSIZE ) != ELF_EHDR_SIZE          /* 52 */
         || elf_read16( f + EH_PHENTSIZE ) != ELF_PHDR_SIZE    /* 32 */
         || elf_read16( f + EH_SHENTSIZE ) != ELF_SHDR_SIZE )  /* 40 */
        return -4;

    /*
     * STEP 4a - Copy what the next steps need into the notebook v.
     *
     * v is the struct elf_view of layer 2: the numbers read from the header
     * are written here once, so check_segments and Athena find them ready
     * and never read the header again. Same numbers as in the file, names
     * without the e_ prefix. v is a POINTER here (the real struct lives in
     * elf_check_layout), hence v->field and not v.field.
     *
     *   f, size  the two parameters: where the file is and how long it is
     *   entry    e_entry (byte 24, 4 bytes): offset of the FIRST INSTRUCTION
     *            of the program, plus 1 (bit 0 = 1 means Thumb code).
     *            Only copied here: to know if it points inside the code we
     *            need code_end, which Athena computes, so she checks it.
     *   phoff    e_phoff (byte 28, 4 bytes): WHERE the program-header table
     *            (segments) STARTS in the file
     *   phnum    e_phnum (byte 44, 2 bytes): HOW MANY rows (segments) it has
     *   shoff    e_shoff (byte 32, 4 bytes): where the section table starts
     *   shnum    e_shnum (byte 48, 2 bytes): how many rows (sections) it has
     *
     * thumb_ptr: entry 0x1, phoff 52, phnum 1, shoff 0x4888, shnum 25.
     */
    v->f = f;
    v->size = size;
    v->entry = elf_read32( f + EH_ENTRY );
    v->phoff = elf_read32( f + EH_PHOFF );
    v->phnum = elf_read16( f + EH_PHNUM );
    v->shoff = elf_read32( f + EH_SHOFF );
    v->shnum = elf_read16( f + EH_SHNUM );

    /*
     * STEP 4b - Are both tables really inside the file?
     *
     * phoff/phnum and shoff/shnum come from the file, and a damaged file
     * can hold any number. Before reading a single row we make sure the
     * WHOLE table is inside [0, size):
     *   segment table: from phoff to phoff + phnum * 32
     *                  thumb_ptr: 52 .. 52 + 1 * 32 = 84            <= 19568
     *   section table: from shoff to shoff + shnum * 40
     *                  thumb_ptr: 0x4888 .. 0x4888 + 25 * 40 = 19568 <= 19568
     * The section table ends exactly at the end of the file: it is the last
     * thing in it. This is why truncated_elf, which cuts the end of the
     * file, is stopped here.
     *
     * Each table also needs at least one row: no segments = nothing to load
     * (assignment 6.3: "at least one PT_LOAD"); no sections = nothing for
     * Athena and Simone to check.
     *
     * The products cannot overflow: phnum and shnum are 2-byte fields, so
     * at most 65535, and 65535 * 40 = 2621400 fits easily in 32 bits.
     *
     * After these two checks check_segments, Athena and Simone can read any
     * row of the two tables without checking again.
     */
    if ( v->phnum == 0 || !elf_fits( v->phoff, v->phnum * ELF_PHDR_SIZE, size ) )
        return -4;
    if ( v->shnum == 0 || !elf_fits( v->shoff, v->shnum * ELF_SHDR_SIZE, size ) )
        return -4;

    return 0;
}

/* ======================================================================
 * TODO STEPS 5-6 - check_segments(): pipeline step 2.2.
 * Walks the program-header table once, checks every PT_LOAD segment, and
 * writes v->file_off, v->file_size and v->load_end.
 * Our load_plan has ONE file_off and ONE copy_size, so the loader copies
 * one block. That is why we require (team rule, stricter than the
 * assignment) the same p_offset - p_vaddr for every segment with file
 * bytes, and BSS only after the copied bytes.
 * ====================================================================== */
static int check_segments( struct elf_view *v )
{
    uint32_t i = 0;
    /*
     * The state of the walk. Initial values chosen so that the first
     * segment always updates them.
     *   delta      p_offset - p_vaddr: "the byte that goes to slot offset X
     *              is at file offset X + delta". Becomes v->file_off.
     *   have_delta 0 until a PT_LOAD with file bytes is seen, then 1.
     *   min_addr   lowest p_vaddr; starts at the maximum, must end at 0
     *              (programs are linked at address 0, assignment 6.3).
     *   file_end   end, in memory, of the bytes that come from the file.
     *              Becomes v->file_size, then copy_size.
     *   load_end   end, in memory, of all PT_LOAD segments (BSS included).
     *              Becomes v->load_end, then bss_end and mem_end.
     */
    uint32_t delta = 0;
    int have_delta = 0;
    uint32_t min_addr = SLOT_PROG_MAX;
    uint32_t file_end = 0;
    uint32_t load_end = 0;

    for ( i = 0; i < v->phnum; i++ ) {
        /*
         * Row i of the program-header table. check_header already proved
         * that the whole table is inside the file, so reading it is safe.
         */
        const uint8_t *ph = v->f + v->phoff + i * ELF_PHDR_SIZE;

        /*
         * STEP 5a - read the five fields we use from the row:
         *   type   = p_type   at ph + 0
         *   offset = p_offset at ph + 4   (where the bytes are in the FILE)
         *   vaddr  = p_vaddr  at ph + 8   (where they go in the SLOT)
         *   filesz = p_filesz at ph + 16  (bytes taken from the file)
         *   memsz  = p_memsz  at ph + 20  (bytes in memory; the extra is BSS)
         * (p_paddr +12, p_flags +24 and p_align +28 are not used: W^X comes
         * from code_end, not from p_flags.)
         */
         
         uint32_t type         = elf_read32( ph + 0 );
         uint32_t offset     = elf_read32( ph + 4 );
         uint32_t vaddr     = elf_read32( ph + 8 );
         uint32_t filesz    = elf_read32( ph + 16 );
         uint32_t memsz     = elf_read32( ph + 20 );
         
        /*
         * STEP 5b - the type:
         *   PT_DYNAMIC -> return -4 (dynamic linking is forbidden, 6.3).
         *   not PT_LOAD, or memsz == 0 -> continue (skip the row).
         */
         if( type == PT_DYNAMIC){
             return -4;
         }

         if( type != PT_LOAD || memsz == 0 ){
             continue;
         }
         
        /*
         * STEP 5c - the segment fits:
         *   filesz <= memsz                          (ELF rule)
         *   [vaddr, vaddr + memsz) inside SLOT_PROG_MAX  (6.4; too_large)
         *   [offset, offset + filesz) inside v->size     (bytes exist)
         *   Use elf_fits for the last two.
         */
         if (filesz > memsz 
         || !elf_fits(vaddr, memsz , SLOT_PROG_MAX) 
         || !elf_fits(offset, filesz, v->size)){
             return -4;
         }
         
        /*STEP 6a - one copy only (only if filesz != 0):
         *   offset >= vaddr, otherwise delta would be "negative": -4.
         *   first time: delta = offset - vaddr, have_delta = 1;
         *   next times: offset - vaddr must equal delta, otherwise -4.
         *   file_end = max( file_end, vaddr + filesz ).
         */
        if (filesz != 0){

            if( offset < vaddr ){
                return -4;
            }

            if( !have_delta ){
                delta = offset - vaddr;
                have_delta = 1;
            }else if( offset - vaddr != delta){
                return -4;  
            }
            
            if( vaddr + filesz >= file_end ){
                file_end = vaddr + filesz;
            }
        }
        /*
         * STEP 6b - Where the whole program starts and ends in the slot,
         * over ALL PT_LOAD segments (also BSS-only ones, filesz == 0: they
         * copy nothing but still take memory).
         *   min_addr: lowest start seen so far (started at 3072, the
         *             maximum, so the first segment always lowers it).
         *             Must end at 0, checked after the loop.
         *   load_end: highest end seen so far (started at 0). Becomes
         *             v->load_end, then bss_end and mem_end.
         * vaddr + memsz cannot overflow: step 5c already proved it <= 3072.
         * thumb_ptr: one segment 0 .. 0x310 -> min_addr 0, load_end 0x310.
         */
        if( vaddr < min_addr ){
            min_addr = vaddr;
        }

        if( vaddr + memsz > load_end ){
            load_end = vaddr + memsz;
        }
    } //fine ciclo

    /*
     * STEP 6c - after the walk:
     *   have_delta must be 1 (there is something to load),
     *   min_addr must be 0 (the program starts at the start of the slot),
     *   [delta, delta + file_end) must be inside the file (the one copy).
     *
     */
    if( have_delta != 1 || min_addr != 0  || !elf_fits( delta, file_end, v -> size )){
        return -4;
    }
    
    /*
    * STEP 6d - second walk, BSS after the copied bytes:
    *   for every PT_LOAD with memsz > filesz, vaddr + filesz must be
    *   >= file_end; otherwise the single copy would put file bytes where
    *   zeros are expected.
    */
    for ( i = 0; i < v->phnum; i++ ) {
       const uint8_t *ph = v->f + v->phoff + i * ELF_PHDR_SIZE;
       uint32_t vaddr = elf_read32( ph + 8 );
       uint32_t filesz = elf_read32( ph + 16 );
       uint32_t memsz = elf_read32( ph + 20 );

       if ( elf_read32( ph + 0 ) == PT_LOAD && memsz > filesz && vaddr + filesz < file_end )
           return -4;
    }
    
    /*
     * STEP 6e - write the result:
     *   v->file_off = delta, v->file_size = file_end, v->load_end = load_end
     */
    v -> file_off = delta;
    v -> file_size = file_end;
    v -> load_end = load_end;
    
    return 0;
}

/* ======================================================================
 * TODO STEP 7 - elf_check_layout(): the only function of layer 2 that the other
 * layers see (declared in loader.h).
 * ====================================================================== */
int elf_check_layout( const uint8_t *f, uint32_t size, struct load_plan *p )
{
    /*
     * The "notebook" of layer 2: a local variable on the stack, so no
     * globals. Zeroed so that unfinished steps never leave garbage.
     */
    struct elf_view v = { 0 };

    /*
     * Steps 2.1 and 2.2. With || check_segments() does not run if
     * check_header() failed: it would read a table nobody has checked.
     */
    if ( check_header( f, size, &v ) != 0 || check_segments( &v ) != 0 )
        return -4;

    /* Steps 2.3, 2.4, 2.5 (Athena): sections, p->code_end, entry. */
    if ( elf_check_sections( &v, p ) != 0 )
        return -4;

    /*
     * STEP 7 - step 2.6, fill the load plan (p is a pointer: ->).
     * Every field is an offset in the SLOT, except file_off (in the FILE).
     *   p->entry     = v.entry      first instruction, bit 0 = 1 (Thumb)
     *   p->file_off  = v.file_off   Antonio copies from f + file_off
     *   p->copy_size = v.file_size  ... this many bytes, to slot offset 0
     *   p->bss_start = v.file_size  BSS starts right after the copied bytes
     *   p->bss_end   = v.load_end   ... and ends with the program
     *   p->mem_end   = v.load_end   end of the program, <= 3072
     * p->code_end is written by Athena in elf_check_sections(). 
     */

    p -> entry         = v.entry;
    p -> file_off     = v.file_off;
    p -> copy_size     = v.file_size;
    p -> bss_start     = v.file_size;
    p -> bss_end     = v.load_end;
    p -> mem_end     = v.load_end;
         
    return 0;
}
