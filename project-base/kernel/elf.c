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
 *   p[0] = 0x34, p[1] = 0x00  ->  0x0034 = 52   (e_phoff, in every program)
 *   p[0] = 0x18, p[1] = 0x00  ->  0x0018 = 24   (e_shnum of global_state)
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
 *   3c 36 01 00  ->  0x0001363c   (e_shoff of global_state)
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
 *         Example: e_shoff = 0x1363c, the start of the section table.
 *   len   how many bytes the range is long.
 *         Example: e_shnum * 40 = 24 * 40 = 960, the whole section table.
 *   size  how big the container is: the file length (size of the ELF)
 *         or SLOT_PROG_MAX (3072) for the program part of a slot.
 *
 * Returns 1 if the range fits, 0 otherwise. All three values may come
 * from a damaged file, so off + len is never computed: with
 * off = 0xFFFFFFF0 and len = 0x20 the sum would wrap around to 0x10 and
 * look "inside".
 *
 * Examples: elf_fits( 0, 3072, 3072 )          = 1   (ends exactly at the end)
 *           elf_fits( 0, 3073, 3072 )          = 0   (too_large: one byte too many)
 *           elf_fits( 0xFFFFFFF0, 0x20, 3072 ) = 0   (overflow avoided)
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
     * Every program of the test kit: 02 00 -> 2, 28 00 -> 40, 01 00 00 00 -> 1.
     * All three values are required by assignment 6.3.
     */
    if ( elf_read16( f + EH_TYPE ) != ET_EXEC      /* must be an executable */
         || elf_read16( f + EH_MACHINE ) != EM_ARM /* must be ARM code      */
         || elf_read32( f + EH_VERSION2 ) != 1 )   /* must be version 1     */
        return -4;

    /*
     * STEP 3b - How long one row of each table is.
     *
     * An ELF file has two TABLES, each made of rows of the same length:
     *   - the program-header table: one row per SEGMENT;
     *   - the section-header table: one row per SECTION.
     * To find row i the code computes
     *     start of row i = start of the table + i * length of one row
     * e.g. global_state sections (table at 0x1363c, rows of 40 bytes):
     *     row 0 -> 0x1363c, row 1 (.text) -> 0x13664, row 2 -> 0x1368c.
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
    if ( elf_read16( f + EH_EHSIZE ) != ELF_EHDR_SIZE         /* 52 */
         || elf_read16( f + EH_PHENTSIZE ) != ELF_PHDR_SIZE   /* 32 */
         || elf_read16( f + EH_SHENTSIZE ) != ELF_SHDR_SIZE ) /* 40 */
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
     * global_state: entry 0x1, phoff 52, phnum 1, shoff 0x1363c, shnum 24.
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
     *                  global_state: 52 .. 52 + 1 * 32 = 84
     *   section table: from shoff to shoff + shnum * 40
     *                  global_state: 0x1363c .. 0x1363c + 24 * 40 = 0x139fc
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
 * STEPS 5-6 - check_segments(): pipeline step 2.2.
 * Walks the program-header table once, checks every PT_LOAD segment, and
 * writes v->file_off, v->file_size and v->load_end.
 *
 * ONE COPY ONLY. struct load_plan has ONE file_off and ONE copy_size, so
 * Antonio copies a single block from the file to the slot. That is why we
 * require (team rule, stricter than the assignment):
 *   - the same p_offset - p_vaddr for every segment with file bytes;
 *   - BSS only after the copied bytes.
 * All 9 test programs have a single PT_LOAD, so they always pass these two
 * rules. If prog.ld changes and makes more segments, check with readelf -l.
 * ====================================================================== */
static int check_segments( struct elf_view *v )
{
    uint32_t i = 0;

    /*
     * The state of the walk. The initial values are chosen "backwards" (the
     * minimum starts at the maximum, the maxima start at 0) so that the
     * first segment always updates them.
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

    for ( i = 0; i < v->phnum; i++ )
    {
        /*
         * Row i of the program-header table: table start + i * 32 bytes.
         * check_header already proved that the whole table is inside the
         * file, so reading any field of this row is safe.
         */
        const uint8_t *ph = v->f + v->phoff + i * ELF_PHDR_SIZE;

        /*
         * STEP 5a - The five fields we use from the row. The numbers are the
         * position of each field inside the 32-byte row.
         * Not used: p_paddr (+12, equal to p_vaddr in our files), p_flags
         * (+24: W^X comes from code_end, not from here), p_align (+28).
         * global_state: type 1, offset 0x10000, vaddr 0, filesz = memsz = 0x314.
         */
        uint32_t type = elf_read32( ph + 0 );    /* p_type: kind of segment         */
        uint32_t offset = elf_read32( ph + 4 );  /* p_offset: where in the FILE     */
        uint32_t vaddr = elf_read32( ph + 8 );   /* p_vaddr: where in the SLOT      */
        uint32_t filesz = elf_read32( ph + 16 ); /* p_filesz: bytes from the file   */
        uint32_t memsz = elf_read32( ph + 20 );  /* p_memsz: bytes in memory (+BSS) */

        /*
         * STEP 5b - The type of the segment.
         *   PT_DYNAMIC (2) means dynamic linking (libraries loaded at run
         *   time): forbidden by assignment 6.3, so the file is refused.
         *   Any other type that is not PT_LOAD (1), or a PT_LOAD with
         *   memsz == 0, has nothing to load: continue skips to the next row.
         *   (A return here would end the whole function and the next rows
         *   would never be checked.)
         */
        if ( type == PT_DYNAMIC )
            return -4;
        if ( type != PT_LOAD || memsz == 0 )
            continue;

        /*
         * STEP 5c - The segment fits. Two different containers:
         *   [vaddr, vaddr + memsz) against the SLOT: code + data + BSS must
         *   stay in the lower 3072 bytes (assignment 6.4). Test too_large
         *   sets the end to 3073.
         *   [offset, offset + filesz) against the FILE: the bytes to copy
         *   must exist. Test truncated_elf would also fail here (but it is
         *   already stopped by check_header).
         *   filesz > memsz would take from the file more bytes than the
         *   segment occupies in memory (ELF rule).
         * elf_fits is used instead of writing vaddr + memsz, which could
         * overflow 32 bits with a damaged vaddr.
         */
        if ( filesz > memsz                              /* more bytes than room  */
             || !elf_fits( vaddr, memsz, SLOT_PROG_MAX ) /* past 3072: too_large  */
             || !elf_fits( offset, filesz, v->size ) )   /* bytes outside file    */
            return -4;

        /*
         * STEP 6a - One copy only, for segments with bytes in the file
         * (a BSS-only segment, filesz == 0, copies nothing).
         *   offset < vaddr would make delta "negative": with unsigned numbers
         *   it would wrap around to a huge value, so the file is refused.
         *   The first such segment fixes delta; every other one must have the
         *   same delta, otherwise Antonio would need a second copy.
         *   file_end keeps the highest vaddr + filesz: how many bytes to copy.
         * global_state: delta = 0x10000 - 0 = 0x10000, file_end = 0x314.
         */
        if ( filesz != 0 )
        {
            if ( offset < vaddr )
                return -4;

            if ( !have_delta )
            {
                delta = offset - vaddr;
                have_delta = 1;
            }
            else if ( offset - vaddr != delta )
            {
                return -4;
            }

            if ( vaddr + filesz >= file_end )
                file_end = vaddr + filesz;
        }

        /*
         * STEP 6b - Where the whole program starts and ends in the slot,
         * over ALL PT_LOAD segments (also BSS-only ones, filesz == 0: they
         * copy nothing but still take memory). This is why it is outside the
         * if ( filesz != 0 ) above.
         *   min_addr: lowest start seen so far. Must end at 0 (step 6c).
         *   load_end: highest end seen so far. Becomes v->load_end.
         * vaddr + memsz cannot overflow: step 5c already proved it <= 3072.
         * global_state: one segment 0 .. 0x314 -> min_addr 0, load_end 0x314.
         */
        if ( vaddr < min_addr )
            min_addr = vaddr;
        if ( vaddr + memsz > load_end )
            load_end = vaddr + memsz;
    }

    /*
     * STEP 6c - After the walk, three checks on the whole program:
     *   have_delta == 1  at least one PT_LOAD with bytes in the file,
     *                    otherwise there is nothing to load (assignment 6.3:
     *                    "at least one PT_LOAD"). Test no_pt_load.
     *   min_addr == 0    the program must start at the start of the slot,
     *                    where Antonio copies it (it is linked at 0).
     *                    Test vaddr_4.
     *   [delta, delta + file_end) inside the file: the one copy Antonio makes
     *                    (from f + file_off, file_end bytes) stays in the file.
     */
    if ( have_delta != 1 || min_addr != 0 || !elf_fits( delta, file_end, v->size ) )
        return -4;

    /*
     * STEP 6d - Second walk: BSS only after the copied bytes.
     * Antonio copies file_end bytes in one block, then fills the BSS with
     * zeros. If a segment had its BSS part (memsz > filesz) starting before
     * file_end, the copy would put file bytes where zeros are expected.
     * A second walk is needed because file_end is known only after the first.
     * Our programs have one segment and no BSS, so this never fails for them;
     * it protects against unusual or damaged files.
     */
    for ( i = 0; i < v->phnum; i++ )
    {
        const uint8_t *ph = v->f + v->phoff + i * ELF_PHDR_SIZE;
        uint32_t vaddr = elf_read32( ph + 8 );
        uint32_t filesz = elf_read32( ph + 16 );
        uint32_t memsz = elf_read32( ph + 20 );

        if ( elf_read32( ph + 0 ) == PT_LOAD && memsz > filesz && vaddr + filesz < file_end )
            return -4;
    }

    /*
     * STEP 6e - The result, into the notebook v.
     * global_state: file_off 0x10000, file_size 0x314, load_end 0x314.
     */
    v->file_off = delta;
    v->file_size = file_end;
    v->load_end = load_end;

    return 0;
}

/* ======================================================================
 * STEP 7 - elf_check_layout(): the only function of layer 2 that the other
 * layers see (declared in loader.h). Antonio calls it with the file found
 * in the image; it returns 0 and a filled load_plan, or -4.
 * ====================================================================== */
int elf_check_layout( const uint8_t *f, uint32_t size, struct load_plan *p )
{
    /*
     * The "notebook" of layer 2: a local variable on the stack, so no
     * globals. Not initialized with = { 0 }: the kernel is built with
     * -nostdlib, and gcc turns that into a call to memset, which does not
     * exist there (link error). It is not needed: check_header and
     * check_segments write every field before anyone reads it.
     */
    struct elf_view v;

    /*
     * Steps 2.1 and 2.2. With || check_segments() does not run if
     * check_header() failed: it would read a table nobody has checked.
     * &v gives them the address of v, so they can write into it.
     */
    if ( check_header( f, size, &v ) != 0 || check_segments( &v ) != 0 )
        return -4;

    /* Steps 2.3, 2.4, 2.5 (Athena): sections, p->code_end, entry. */
    if ( elf_check_sections( &v, p ) != 0 )
        return -4;

    /*
     * Step 2.6 - Fill the load plan. Here v is the struct itself (v.field)
     * and p is a pointer (p->field). Every field is an offset in the SLOT,
     * except file_off, which is an offset in the FILE. Real address in RAM =
     * slot_base + field.
     *   entry      first instruction, bit 0 = 1 (Thumb)   -> layer 6: pc
     *   file_off   Antonio copies from f + file_off...    -> layer 4
     *   copy_size  ...this many bytes, to slot offset 0   -> layer 4
     *   bss_start  BSS starts right after the copied bytes
     *   bss_end    ...and ends with the program           -> layer 4: zeros
     *   mem_end    end of the program, <= 3072 (the stack is above)
     * p->code_end is written by Athena in elf_check_sections().
     * If we returned -4 above, *p is not valid: the caller must not use it.
     * global_state: entry 0x1, file_off 0x10000, copy_size 0x314,
     *               bss 0x314..0x314 (empty), mem_end 0x314.
     */
    p->entry = v.entry;
    p->file_off = v.file_off;
    p->copy_size = v.file_size;
    p->bss_start = v.file_size;
    p->bss_end = v.load_end;
    p->mem_end = v.load_end;

    return 0;
}
