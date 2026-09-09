/*
 * Раскладка образа ядра.
 * LOAD_ADDR = 0x40080000 подходит и QEMU virt, и merlin:
 * у MTK база RAM 0x40000000, LK грузит ядро по base+0x80000.
 */
ENTRY(_start)

LOAD_ADDR = DEFINED(LOAD_ADDR) ? LOAD_ADDR : 0x40080000;

SECTIONS
{
    . = LOAD_ADDR;
    __image_start = .;

    .text : {
        KEEP(*(.text.boot))     /* заголовок Image обязан быть первым */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(COMMON) *(.bss .bss.*) }
    . = ALIGN(16);
    __bss_end = .;

    /*
     * Следы, переживающие перезагрузку.
     *
     * Лежат ЗА .bss нарочно: загрузчик обнуляет только .bss, и всё, что
     * попало сюда, при тёплой перезагрузке остаётся в памяти нетронутым.
     * Ради этого раздел и заведён — иначе после остановки ядра мы теряем
     * всё, что оно о себе знало, а именно тогда это и нужно.
     */
    . = ALIGN(64);
    __persist_start = .;
    .persist (NOLOAD) : { *(.persist) }
    . = ALIGN(64);
    __persist_end = .;

    __image_end = .;
    /* Значение для поля image_size в заголовке Image */
    _image_size = __image_end - __image_start;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
