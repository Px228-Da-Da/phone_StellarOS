#ifndef SPINLOCK_H
#define SPINLOCK_H
#include "types.h"
#include "io.h"

/*
 * Спинлоки и атомарные операции.
 *
 * До этого момента ядро исполнялось на одном ядре, и «одновременно» было
 * невозможно по построению. С восемью ядрами обычное `counter++` перестаёт
 * работать: это три инструкции (прочитать, прибавить, записать), и два ядра
 * успевают прочитать одно и то же значение.
 *
 * Основа — пара LDXR/STXR: ldxr читает и ставит на адрес «монитор
 * исключительности», stxr записывает ТОЛЬКО если за это время никто другой
 * по этому адресу не писал, и сообщает об успехе. Не получилось — повторяем.
 * Никаких запретов прерываний и никакой остановки других ядер.
 *
 * Важно: монитор работает только для памяти, помеченной как Normal
 * cacheable. Пока MMU выключен, вся память Device — и spin_lock зависнет
 * навсегда. Поэтому ядра захватывают блокировки только после mmu_enable.
 */

struct spinlock {
    volatile u32 locked;
    const char *name;
    volatile u64 holder;        /* MPIDR держателя, для разбора зависаний */
};

#define SPINLOCK_INIT(n) { 0, (n), 0 }

/*
 * Сторож захвата.
 *
 * Ядро, вставшее на замке навсегда, снаружи выглядит как «всё работает,
 * только вот это не работает»: остальные ядра продолжают крутить задачи
 * и печатать, а один поток исчезает без единого слова. Разбирать такое
 * по молчанию невозможно.
 *
 * Поэтому ждём не молча: не дождавшись за две секунды, называем замок и
 * того, кто его держит. Две секунды — это заведомо больше любого
 * честного удержания: под нашими замками не делается ничего длиннее
 * обхода списка задач.
 *
 * Реализация печати вынесена в print.c, потому что здесь нельзя знать о
 * kprintf: заголовок спинлока включают и те файлы, что подключаются
 * раньше вывода.
 */
void spin_stuck(struct spinlock *l);

/* --- Маска прерываний ---
 * Возвращаем прежнее состояние, чтобы вложенные захваты не разрешили
 * прерывания раньше времени. */
static inline u64 irq_save(void)
{
    u64 flags;

    __asm__ volatile("mrs %0, daif\n"
                     "msr daifset, #2\n"
                     : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(u64 flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

/* Одна попытка захвата: 1 — получилось */
static inline int spin_trylock(struct spinlock *l)
{
    u32 seen, failed;

    __asm__ volatile(
        "   ldaxr   %w0, [%2]\n"
        "   cbnz    %w0, 1f\n"
        "   stxr    %w1, %w3, [%2]\n"
        "   b       2f\n"
        "1: mov     %w1, #1\n"
        "2:\n"
        : "=&r"(seen), "=&r"(failed)
        : "r"(&l->locked), "r"(1U)
        : "memory");

    return failed == 0;
}

static inline void spin_lock(struct spinlock *l)
{
    u64 deadline = 0;

    /*
     * wfe — не оптимизация, а способ не жечь батарею: ядро, не получившее
     * замок, засыпает до события вместо того, чтобы молотить шину
     * запросами. Освобождающий делает sev и будит всех. Прерывание,
     * даже запрещённое, тоже считается событием — поэтому спящий здесь
     * просыпается не реже тика таймера и успевает посмотреть на часы.
     */
    while (!spin_trylock(l)) {
        __asm__ volatile("wfe" ::: "memory");

        if (!deadline) {
            deadline = read_cntpct() + read_cntfrq() * 2;
        } else if (read_cntpct() > deadline) {
            spin_stuck(l);
            deadline = read_cntpct() + read_cntfrq() * 10;
        }
    }

    l->holder = read_mpidr() & 0x00FFFFFFUL;
}

static inline void spin_unlock(struct spinlock *l)
{
    l->holder = 0;
    /* stlr — release-запись: все наши изменения под блокировкой станут
     * видны другим ядрам ДО того, как они увидят снятие замка. */
    __asm__ volatile("stlr wzr, [%0]" :: "r"(&l->locked) : "memory");
    __asm__ volatile("sev" ::: "memory");
}

/*
 * Захват с запретом прерываний.
 *
 * Обязателен для всего, к чему обращается обработчик прерывания: если
 * прерывание придёт на том же ядре, пока блокировка удерживается, оно
 * попробует захватить её снова и будет ждать сам себя — вечно.
 */
static inline u64 spin_lock_irq(struct spinlock *l)
{
    u64 flags = irq_save();

    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irq(struct spinlock *l, u64 flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

/* --- Атомарные счётчики --- */

static inline u64 atomic_add(volatile u64 *p, u64 v)
{
    u64 val, ok;

    __asm__ volatile(
        "1: ldaxr   %0, [%2]\n"
        "   add     %0, %0, %3\n"
        "   stlxr   %w1, %0, [%2]\n"
        "   cbnz    %w1, 1b\n"
        : "=&r"(val), "=&r"(ok)
        : "r"(p), "r"(v)
        : "memory");

    return val;
}

static inline u64 atomic_inc(volatile u64 *p)
{
    return atomic_add(p, 1);
}

static inline u64 atomic_read(volatile u64 *p)
{
    u64 v;

    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

#endif
