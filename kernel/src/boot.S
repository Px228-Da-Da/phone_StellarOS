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

/*
 * Спуск с EL2 на EL1. Оформлен макросом: этим кодом пользуются и загрузочное
 * ядро, и каждое разбуженное — а куда именно прошивка отдаёт управление,
 * EL2 или EL1, зависит от платформы, и полагаться на это нельзя.
 * Метка 9 локальная: в макросе она разворачивается заново на каждом вызове.
 */
.macro LOWER_TO_EL1
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    9f

    /* EL1 будет AArch64 */
    mrs     x0, hcr_el2
    orr     x0, x0, #(1 << 31)          /* HCR_EL2.RW = 1             */
    msr     hcr_el2, x0

    /* Разрешаем EL1 читать системный таймер напрямую */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3                  /* EL1PCTEN | EL1PCEN         */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr            /* виртуальное время = физическому */

    /* GICv3: без этого EL1 не сможет трогать регистры ICC_*.
     * SRE=1 включает системные регистры вместо MMIO, Enable=1 разрешает
     * их нижнему уровню. Кодировка сырая: S3_4_C12_C9_5 = ICC_SRE_EL2. */
    mrs     x0, S3_4_C12_C9_5
    orr     x0, x0, #(1 << 0)           /* SRE                        */
    orr     x0, x0, #(1 << 3)           /* Enable для EL1             */
    msr     S3_4_C12_C9_5, x0
    isb

    /* Сбрасываем SCTLR_EL1 в известное состояние: MMU/кэши off */
    mov     x0, #0x0800
    movk    x0, #0x30D0, lsl #16
    msr     sctlr_el1, x0

    /* Готовим возврат в EL1h с замаскированными прерываниями */
    mov     x0, #0x3C5                  /* DAIF=1111, M=EL1h          */
    msr     spsr_el2, x0
    adr     x0, 9f
    msr     elr_el2, x0
    eret
9:
.endm

/*
 * Установить стек текущему ядру. Стеки лежат подряд, растут вниз,
 * поэтому вершина ядра N — это база + (N + 1) * STACK_SIZE.
 * \id — регистр с логическим номером ядра.
 */
.macro SET_STACK id
    adrp    x0, __stacks_base
    add     x0, x0, :lo12:__stacks_base
    add     x1, \id, #1
    mov     x2, #STACK_SIZE
    madd    x0, x1, x2, x0
    mov     sp, x0
.endm

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
     * Проверяем Aff0..Aff2 целиком, а не только Aff0: у Helio G85 два
     * кластера, и ядро 0 второго кластера имеет Aff0=0, Aff1=1 — по одному
     * лишь Aff0 оно сошло бы за загрузочное. Остальные ядра мы разбудим
     * сами и осознанно, через PSCI, и придут они в _secondary_entry. */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFFFFFF
    cbz     x1, .Lprimary
.Lpark:
    wfe                         /* спим до sev от primary            */
    b       .Lpark

.Lprimary:
    LOWER_TO_EL1

.Lin_el1:
    /* --- Стек ---
     * У каждого ядра свой; загрузочное всегда номер 0 */
    mov     x20, #0
    SET_STACK x20

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

/*
 * Вход для разбуженных ядер.
 *
 * Сюда прошивка передаёт управление после psci_cpu_on. Состояние —
 * как после сброса: MMU выключен, кэши выключены, прерывания замаскированы.
 * В x0 лежит context_id, который мы передали в вызове, — логический номер
 * ядра. Ничего общего с другими ядрами трогать нельзя до включения MMU:
 * пока он выключен, память некэшируемая, и записанного другими ядрами
 * в кэш мы попросту не увидим.
 */
.global _secondary_entry
_secondary_entry:
    mov     x19, x0                 /* логический номер ядра        */

    LOWER_TO_EL1

    SET_STACK x19                   /* свой стек, чужой трогать нельзя */

    /* .bss уже обнулено загрузочным ядром — второй раз нельзя,
     * иначе затрём всё, что оно успело туда положить. */

    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb

    mov     x0, x19
    bl      secondary_main          /* не возвращается */

.Lsec_halt:
    wfe
    b       .Lsec_halt

/*
 * Стеки: свой на каждое ядро, подряд, выровнены на 16 байт (требование ABI).
 * Лежат в .bss, поэтому обнуляются загрузочным ядром заодно со всем остальным.
 */
.section ".bss", "aw", @nobits
.align 4
.global __stacks_base
__stacks_base:
    .space  STACK_SIZE * 8          /* MAX_CPUS */
__stacks_top:
