    .syntax unified
    .cpu cortex-m3
    .thumb

    .section .text,"ax",%progbits /* Direziona le istruzioni successive nella zona .text, poichè solo codice. %a indica che occupa spazio fisico in memoria e x executable, mentre progbits indica che la sezione contiene dati reali da salvare fisicamente nell'elf*/
    .global _start /* _start indica l'entry point del programma e global rende visibile il suo nome visibile all'esterno (Linker e Debugger) */
    .type _start, %function /* Identifica _start come codice eseguibile e non dati grezzi */
    .thumb_func  /* Avvisa l'assembler che l'etichetta immediatamente successiva (_start) contiene codice Thumb. L'assembler modificherà l'indirizzo fisico di _start impostando il bit meno significativo (il bit 0) a 1. Questo passaggio è un vincolo hardware insuperabile */

_start: /* Questa è l'etichetta fisica, indica l'indirizzo di partenza. */ 
    bl main       /* 1. Salta al codice C del programma utente */
    bl sys_exit   /* 2. Se il main fa 'return', chiama la system call 4 per spegnere il task */
hang:
    b hang        /* Fallback di sicurezza */
    /* Se per colpa di un grave bug architetturale sys_exit dovesse fallire e "ritornare" al programma chiamante, la CPU eseguirebbe b hang. Questa istruzione cattura il processore in un ciclo infinito innocuo */