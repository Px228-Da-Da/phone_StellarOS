#ifndef SPINLOCK_H
#define SPINLOCK_H
#include "types.h"

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
};

#define SPINLOCK_INIT(n) { 0, (n) }

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

static inline void spin_lock(struct spinlock *l)
{
    u32 tmp;

    /*
     * sevl + wfe — не оптимизация, а способ не жечь батарею: ядро,
     * не получившее блокировку, засыпает до события вместо того, чтобы
     * молотить шину запросами. Освобождающий делает sev и будит всех.
     * Первый wfe проходит насквозь благодаря sevl.
     */
    __asm__ volatile(
        "   sevl\n"
        "1: wfe\n"
        "2: ldaxr   %w0, [%1]\n"
        "   cbnz    %w0, 1b\n"
        "   stxr    %w0, %w2, [%1]\n"
        "   cbnz    %w0, 2b\n"
        : "=&r"(tmp)
        : "r"(&l->locked), "r"(1U)
        : "memory");
}

static inline void spin_unlock(struct spinlock *l)
{
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
