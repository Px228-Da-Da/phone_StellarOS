/*
 * smp.c — запуск и учёт ядер.
 *
 * Порядок оживления вторичного ядра:
 *
 *   1. CPU0 просит прошивку: psci_cpu_on(mpidr, _secondary_entry, номер)
 *   2. Прошивка включает питание ядра и передаёт ему управление
 *   3. Ядро приходит в _secondary_entry с ВЫКЛЮЧЕННЫМ MMU и своим стеком
 *   4. secondary_main: своя запись в TPIDR_EL1, MMU, GIC, таймер
 *   5. online = 1 — CPU0 видит, что ядро поднялось
 *
 * Пункт 4 нельзя переставлять. Пока MMU выключен, память для этого ядра
 * некэшируемая, и данные, записанные CPU0 в кэш, ему не видны: он прочитает
 * из DRAM то, что было там до этого. Поэтому до включения MMU вторичное
 * ядро не трогает НИЧЕГО общего — только свой стек и свои системные регистры.
 */
#include "smp.h"
#include "psci.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
#include "print.h"
#include "io.h"
#include "spinlock.h"

/* Вход для вторичных ядер, boot.S */
extern void _secondary_entry(void);

static struct cpu cpus[MAX_CPUS];
static u64 online_count;

void percpu_init(u32 id, u64 mpidr)
{
    struct cpu *c = &cpus[id];

    c->id = id;
    c->mpidr = mpidr;

    /* TPIDR_EL1 — «свой карман» ядра ОС. Дальше this_cpu() стоит
     * одну инструкцию и работает одинаково на любом ядре. */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(c) : "memory");
}

struct cpu *this_cpu(void)
{
    struct cpu *c;

    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

struct cpu *cpu_of(u32 id)
{
    return id < MAX_CPUS ? &cpus[id] : NULL;
}

u32 cpu_id(void)
{
    struct cpu *c = this_cpu();

    return c ? (u32)c->id : 0;
}

u32 cpu_online_count(void)
{
    return (u32)atomic_read(&online_count);
}

/*
 * Точка входа вторичного ядра на Си. Вызывается из boot.S,
 * MMU ещё выключен, из общего состояния трогать нельзя ничего.
 */
void secondary_main(u64 id)
{
    percpu_init((u32)id, read_mpidr());

    /* С этого момента память кэшируемая и общая с остальными ядрами */
    mmu_enable_cpu();

    /* Свой интерфейс контроллера прерываний и свой таймер: и то, и другое
     * у каждого ядра собственное, настройки CPU0 сюда не распространяются.
     *
     * Если хоть что-то из этого не поднялось — ядро НЕ идёт в планировщик
     * и не отмечается работающим. Без своего таймера его никто не вытеснит:
     * взятая задача осталась бы на нём навсегда, а в отчёте ядро выглядело
     * бы живым и работающим. Лучше честно потерять ядро, чем задачу. */
    if (gic_init_cpu() != 0 || timer_init_cpu() != 0) {
        kprintf("SMP      : ЯДРО %lu НЕ ГОТОВО, ОСТАНАВЛИВАЮ ЕГО\n", id);
        for (;;)
            wfi();
    }

    sched_init_cpu();

    this_cpu()->online = 1;
    atomic_inc(&online_count);

    irq_enable();
    sched_idle_loop();          /* не возвращается */
}

/*
 * Разбудить остальные ядра.
 *
 * Какие MPIDR существуют, нам неоткуда узнать до разбора DTB, поэтому
 * перебираем правдоподобные: восемь ядер первого кластера и четыре
 * второго. У Helio G85 два кластера (2xA75 + 6xA55), у QEMU один.
 * Несуществующее ядро прошивка отвергнет с INVALID_PARAMETERS — это
 * штатный ответ, а не ошибка.
 */
u32 smp_start_secondaries(void)
{
    static const u64 candidates[] = {
        0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007,
        0x100, 0x101, 0x102, 0x103,
    };
    u64 self = read_mpidr() & 0x00FFFFFFUL;
    s64 ver = psci_version();
    u32 next_id = 1;

    /* Загрузочное ядро тоже входит в счёт: оно работает с самого начала,
     * просто отмечать себя ему было негде до этого момента. */
    if (!cpus[0].online) {
        cpus[0].online = 1;
        atomic_inc(&online_count);
    }

    if (ver < 0) {
        kprintf("SMP      : PSCI НЕДОСТУПЕН, ОСТАЁМСЯ НА ОДНОМ ЯДРЕ\n");
        return 1;
    }

    kprintf("SMP      : PSCI %lu.%lu\n",
            ((u64)ver >> 16) & 0xFFFF, (u64)ver & 0xFFFF);

    for (u32 i = 0; i < ARRAY_SIZE(candidates) && next_id < MAX_CPUS; i++) {
        u64 mpidr = candidates[i];
        s64 rc;

        if (mpidr == self)
            continue;

        rc = psci_cpu_on(mpidr, (u64)(uintptr_t)_secondary_entry, next_id);
        if (rc != PSCI_SUCCESS)
            continue;

        /* Ждём, пока ядро само отчитается. Ограничение по времени
         * обязательно: ядро может не подняться, и висеть здесь вечно
         * означало бы потерять и то ядро, которое работает. */
        u64 deadline = read_cntvct() + read_cntfrq();   /* секунда */

        while (!cpus[next_id].online && read_cntvct() < deadline)
            ;

        if (!cpus[next_id].online) {
            kprintf("SMP      : ЯДРО MPIDR %lx НЕ ОТВЕТИЛО\n", mpidr);
            continue;
        }

        next_id++;
    }

    return cpu_online_count();
}

void smp_dump(void)
{
    kprintf("ЯДЕР     : %u\n", cpu_online_count());

    for (u32 i = 0; i < MAX_CPUS; i++)
        if (cpus[i].online)
            kprintf("  CPU%u   : MPIDR %lx, ТИКОВ %lu\n",
                    i, cpus[i].mpidr, cpus[i].ticks);
}
