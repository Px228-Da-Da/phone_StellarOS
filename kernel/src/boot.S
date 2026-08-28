/*
 * boot.S — точка входа ядра.
 *
 * Состояние на входе (гарантирует LK / QEMU по Linux arm64 boot protocol):
 *   x0  = физический адрес DTB
 *   x1..x3 = 0
 *   MMU выключен, D-cache выключен, IRQ/FIQ замаскированы
 *   исполняется одно ядро (CPU0), остальные спят в ATF
 */

#define STACK_SIZE  (16 * 1024)

.section ".text.boot", "ax"
.global _start

_start:
    /* --- 64-байтный заголовок arm64 Image ---
     * LK и QEMU читают его, чтобы понять размер и куда грузить образ.
     * Первая инструкция обязана быть исполняемой веткой. */
    b       _entry              /* code0: branch                       */
    .long   0                   /* code1: reserved                     */
    .quad   0x80000             /* text_offset: смещение от базы RAM   */
    .quad   _image_size         /* image_size (заполняет линкер)       */
    .quad   0xA                 /* flags: LE | 4K pages | anywhere     */
    .quad   0                   /* res2                                */
    .quad   0                   /* res3                                */
    .quad   0                   /* res4                                */
    .ascii  "ARM\x64"           /* magic                               */
    .long   0                   /* res5 / offset to PE header          */

_entry:
    /* DTB прячем в callee-saved регистр: x0 нам сейчас понадобится */
    mov     x19, x0

    /* --- Оставляем в живых только загрузочное ядро ---
     * Affinity0 в MPIDR_EL1 = номер ядра внутри кластера.
     * Остальные усыпляем: они и так спят, но LK может разбудить не одно. */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, .Lprimary
.Lpark:
    wfe                         /* спим до sev от primary            */
    b       .Lpark

.Lprimary:
    /* --- Если мы в EL2 — спускаемся в EL1 ---
     * Работать в EL1 удобнее: там «нормальные» sctlr/vbar/ttbr,
     * а EL2 нам пока нечем занять (гипервизор не пишем). */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    .Lin_el1

    /* EL1 будет AArch64 */
    mrs     x0, hcr_el2
    orr     x0, x0, #(1 << 31)          /* HCR_EL2.RW = 1             */
    msr     hcr_el2, x0

    /* Разрешаем EL1 читать системный таймер напрямую */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3                  /* EL1PCTEN | EL1PCEN         */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr            /* виртуальное время = физическому */

    /* Сбрасываем SCTLR_EL1 в известное состояние: MMU/кэши off */
    mov     x0, #0x0800
    movk    x0, #0x30D0, lsl #16
    msr     sctlr_el1, x0

    /* Готовим «возврат» в EL1h с замаскированными прерываниями */
    mov     x0, #0x3C5                  /* DAIF=1111, M=EL1h          */
    msr     spsr_el2, x0
    adr     x0, .Lin_el1
    msr     elr_el2, x0
    eret

.Lin_el1:
    /* --- Стек ---
     * Растёт вниз от вершины массива в .bss */
    adrp    x0, __stack_top
    add     x0, x0, :lo12:__stack_top
    mov     sp, x0

    /* --- Обнуляем .bss ---
     * Си-код рассчитывает, что глобалы без инициализатора = 0 */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
.Lbss_loop:
    cmp     x0, x1
    b.hs    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_loop
.Lbss_done:

    /* --- Таблица векторов исключений ---
     * Ставим до первого Си-кода, чтобы падение было видно, а не «зависон» */
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb

    /* --- В Си ---  kmain(dtb_phys) */
    mov     x0, x19
    bl      kmain

    /* kmain не должен возвращаться, но если вернулся — тихо стоим */
.Lhalt:
    wfe
    b       .Lhalt

/* Стек в .bss, выровнен на 16 байт (требование AArch64 ABI) */
.section ".bss", "aw", @nobits
.align 4
__stack_bottom:
    .space  STACK_SIZE
__stack_top:
