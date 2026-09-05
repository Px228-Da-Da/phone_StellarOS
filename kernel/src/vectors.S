/*
 * vectors.S — таблица векторов исключений EL1.
 *
 * Без неё любая ошибка в ядре = молчаливое зависание, что на телефоне
 * без отладчика означает «чёрный экран и гадай». С ней мы печатаем
 * ESR/ELR/FAR и сразу видим причину.
 *
 * Формат: 16 входов по 0x80 байт, вся таблица выровнена на 0x800.
 *
 * В КАЖДОМ входе стоит ровно одна команда — переход на тело обработчика,
 * а тела лежат ниже таблицы. Раньше тела разворачивались прямо во входах,
 * и это было тихо сломано: сохранение регистров с возвратом занимает 168
 * байт, то есть больше отведённых входу 128. Ассемблер не ругается — он
 * просто выравнивает следующий вход на следующую границу 128 байт, и
 * вся таблица ниже переехавшего входа съезжает на слот. Обработчик IRQ
 * стоял пятым, за ним шли FIQ и SError, которыми мы не пользуемся, —
 * поэтому и не замечали. Входы для EL0 идут сразу за ними.
 */

/* Сохранить состояние в кадр на стеке (34 регистра * 8 = 272 байта) */
.macro SAVE_REGS
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x30, x21, [sp, #16 * 15]
    str     x22,      [sp, #16 * 16]
.endm

.macro RESTORE_REGS
    ldr     x22,      [sp, #16 * 16]
    ldp     x30, x21, [sp, #16 * 15]
    msr     elr_el1,  x21
    msr     spsr_el1, x22
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #272
.endm

/*
 * Вход из EL0 отличается от входа из EL1 одним, но принципиальным:
 * у программы свой стек, и он лежит в SP_EL0 — отдельном регистре,
 * которого переключение контекста не касается. Пока пользовательская
 * задача одна, это незаметно; как только вытесненная в EL0 задача
 * уступает место другой такой же, SP_EL0 достаётся ей от предшественницы,
 * и программа продолжается на чужом стеке. Поэтому SP_EL0 сохраняем в тот
 * же кадр и восстанавливаем перед возвратом.
 *
 * Кадр: 34 слота по 8 байт = 272. Заняты 0..263 (x0..x30, ELR, SPSR),
 * слот 264 свободен — туда и кладём.
 */
#define FRAME_SP_EL0    (16 * 16 + 8)

.macro EL0_SAVE
    SAVE_REGS
    mrs     x21, sp_el0
    str     x21, [sp, #FRAME_SP_EL0]
.endm

.macro EL0_RESTORE
    ldr     x21, [sp, #FRAME_SP_EL0]
    msr     sp_el0, x21
    RESTORE_REGS
.endm

.section ".text", "ax"

/*
 * Сама таблица. Одна команда во входе — и никаких сюрпризов с размером.
 */
.align 11
.global vector_table
vector_table:

/* --- Current EL, SP_EL0 (мы такой стек не используем) --- */
.align 7
    b       fatal_0
.align 7
    b       fatal_1
.align 7
    b       fatal_2
.align 7
    b       fatal_3

/* --- Current EL, SP_ELx — обычный режим работы ядра --- */
.align 7
    b       el1_sync            /* ошибка доступа, undef, svc из ядра */
.align 7
    b       el1_irq             /* таймер, тач, устройства            */
.align 7
    b       fatal_6             /* FIQ                                */
.align 7
    b       fatal_7             /* SError                             */

/* --- Lower EL, AArch64 — здесь живёт userspace --- */
.align 7
    b       el0_sync_entry      /* SVC и ошибки пользовательской программы */
.align 7
    b       el0_irq_entry       /* вытеснение программы по таймеру         */
.align 7
    b       fatal_10            /* FIQ                                     */
.align 7
    b       fatal_11            /* SError                                  */

/* --- Lower EL, AArch32 (не планируется) --- */
.align 7
    b       fatal_12
.align 7
    b       fatal_13
.align 7
    b       fatal_14
.align 7
    b       fatal_15

/*
 * Тела обработчиков.
 */

/* То, чего мы пока не умеем обрабатывать: печатаем и останавливаемся */
.macro FATAL_BODY, name, type
\name:
    SAVE_REGS
    mov     x0, #\type
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      exception_fatal
    b       .
.endm

    FATAL_BODY fatal_0,  0
    FATAL_BODY fatal_1,  1
    FATAL_BODY fatal_2,  2
    FATAL_BODY fatal_3,  3
    FATAL_BODY el1_sync, 4
    FATAL_BODY fatal_6,  6
    FATAL_BODY fatal_7,  7
    FATAL_BODY fatal_10, 10
    FATAL_BODY fatal_11, 11
    FATAL_BODY fatal_12, 12
    FATAL_BODY fatal_13, 13
    FATAL_BODY fatal_14, 14
    FATAL_BODY fatal_15, 15

/* Прерывание, заставшее ядро */
el1_irq:
    SAVE_REGS
    bl      irq_handler
    RESTORE_REGS
    eret

/*
 * Синхронное исключение из EL0 — системный вызов (SVC) или ошибка в
 * программе. Разбирается в Си: там читается ESR и решается, обслужить
 * или снять задачу. Указатель на кадр передаём первым аргументом: вызов
 * обязан не только прочитать аргументы из регистров, но и положить в кадр
 * результат — обратно программа уедет именно из него.
 */
el0_sync_entry:
    EL0_SAVE
    mov     x0, sp
    bl      el0_sync
    EL0_RESTORE
    eret

/* Прерывание, заставшее программу в EL0: обработчик тот же, что у ядра,
 * но кадр пользовательский, со своим SP_EL0. */
el0_irq_entry:
    EL0_SAVE
    bl      irq_from_el0
    EL0_RESTORE
    eret
