/*
 * vectors.S — таблица векторов исключений EL1.
 *
 * Без неё любая ошибка в ядре = молчаливое зависание, что на телефоне
 * без отладчика означает «чёрный экран и гадай». С ней мы печатаем
 * ESR/ELR/FAR и сразу видим причину.
 *
 * Формат: 16 входов по 0x80 байт, вся таблица выровнена на 0x800.
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

/* Синхронные ошибки и прочее, что мы пока не умеем обрабатывать */
.macro FATAL_ENTRY, type
    SAVE_REGS
    mov     x0, #\type
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      exception_fatal
    b       .
.endm

/* IRQ — единственное, что мы обрабатываем и из чего возвращаемся */
.macro IRQ_ENTRY
    SAVE_REGS
    bl      irq_handler
    RESTORE_REGS
    eret
.endm

.section ".text", "ax"
.align 11
.global vector_table
vector_table:

/* --- Current EL, SP_EL0 (мы такой стек не используем) --- */
.align 7
    FATAL_ENTRY 0
.align 7
    FATAL_ENTRY 1
.align 7
    FATAL_ENTRY 2
.align 7
    FATAL_ENTRY 3

/* --- Current EL, SP_ELx — наш обычный режим работы --- */
.align 7
    FATAL_ENTRY 4               /* Synchronous: ошибка доступа, undef, svc */
.align 7
    IRQ_ENTRY                   /* IRQ: таймер, тач, устройства            */
.align 7
    FATAL_ENTRY 6               /* FIQ                                     */
.align 7
    FATAL_ENTRY 7               /* SError                                  */

/* --- Lower EL, AArch64 (появится, когда запустим userspace) --- */
.align 7
    FATAL_ENTRY 8
.align 7
    FATAL_ENTRY 9
.align 7
    FATAL_ENTRY 10
.align 7
    FATAL_ENTRY 11

/* --- Lower EL, AArch32 (не планируется) --- */
.align 7
    FATAL_ENTRY 12
.align 7
    FATAL_ENTRY 13
.align 7
    FATAL_ENTRY 14
.align 7
    FATAL_ENTRY 15
