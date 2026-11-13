struct load_plan {
    uint32_t entry;                  /* offset nello slot, bit 0 = 1 (Thumb) */
    uint32_t file_off, copy_size;    /* byte da copiare dal file allo slot */
    uint32_t bss_start, bss_end;     /* offset da azzerare */
    uint32_t code_end;               /* fine della parte RX = inizio della parte RW */
    uint32_t mem_end;                /* fine del programma, <= 3072 */
};

int  elf_check_layout(const uint8_t *f, uint32_t size, struct load_plan *p); /* 0 / -4 */

int  elf_check_relocs(const uint8_t *f, uint32_t size, const struct load_plan *p); /* 0 / -4 */

void elf_apply_relocs(const uint8_t *f, uint32_t slot_base);
