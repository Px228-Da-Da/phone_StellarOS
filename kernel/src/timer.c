/*
 * timer.c — периодические прерывания от ARM Generic Timer.
 *
 * Устройство таймера предельно простое: CNTV_TVAL_EL0 — счётчик, который
 * сам уменьшается на каждый тик системного счётчика. Дошёл до нуля —
 * поднял прерывание и ушёл в минус. Чтобы получить следующее прерывание,
 * достаточно снова записать в TVAL интервал.
 *
 * Отсюда и первая настоящая единица времени в ядре: до этого «секунда»
 * была активным ожиданием в цикле, теперь процессор спит в wfi и просыпается
 * по прерыванию. На телефоне это разница между горячим корпусом и холодным.
 */
#include "timer.h"
#include "gic.h"
#include "io.h"
#include "print.h"
#include "smp.h"
#include "sched.h"

#define CNTV_CTL_ENABLE     (1UL << 0)
#define CNTV_CTL_IMASK      (1UL << 1)
#define CNTV_CTL_ISTATUS    (1UL << 2)

/* Интервал и частота общие: таймеры разных ядер идут от одного
 * системного счётчика. А вот счётчик тиков у каждого ядра свой —
 * он лежит в per-CPU структуре, потому что прерывание таймера
 * приходит каждому ядру отдельно (это PPI). */
static u64 interval;            /* тиков счётчика между прерываниями */
static u64 start_count;
static u32 timer_hz;

static void timer_write_tval(u64 v)
{
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(v) : "memory");
}

static void timer_write_ctl(u64 v)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(v) : "memory");
    isb();
}

/*
 * Обработчик. Перезаряжаем TVAL сразу: пока он в минусе, таймер
 * продолжает держать линию прерывания поднятой, и EOI не поможет —
 * вернёмся из исключения и тут же провалимся в него снова.
 */
static void timer_irq(u32 intid)
{
    (void)intid;

    timer_write_tval(interval);
    this_cpu()->ticks++;

    /* Отсюда и берётся вытеснение: планировщик считает кванты
     * по тем же тикам и решает, не пора ли сменить задачу. */
    sched_tick();
}

int timer_init(u32 hz)
{
    u64 freq = read_cntfrq();

    if (!hz || !freq) {
        kprintf("TIMER    : НЕТ ЧАСТОТЫ СЧЁТЧИКА\n");
        return -1;
    }

    timer_hz    = hz;
    interval    = freq / hz;
    start_count = read_cntvct();

    /* Сначала обработчик и разрешение в GIC, потом запуск самого таймера:
     * иначе первое же прерывание прилетит в пустоту и станет «ничьим». */
    gic_enable_irq(TIMER_IRQ_VIRT, GIC_PRIO_DEFAULT, timer_irq);

    timer_write_tval(interval);
    timer_write_ctl(CNTV_CTL_ENABLE);   /* IMASK=0: маску снимаем */

    kprintf("TIMER    : %u ГЦ, ИНТЕРВАЛ %lu ТИКОВ, PPI %u\n",
            hz, interval, TIMER_IRQ_VIRT);
    return 0;
}

/*
 * Запуск таймера на разбуженном ядре.
 *
 * Интервал уже посчитан на CPU0, но включить таймер и разрешить себе PPI
 * обязано каждое ядро само: регистры CNTV_* и редистрибьютор у него
 * собственные. Без этого вторичное ядро не получит ни одного вытеснения
 * и застрянет на первой же задаче навсегда.
 */
int timer_init_cpu(void)
{
    if (!interval)
        return -1;

    gic_enable_irq(TIMER_IRQ_VIRT, GIC_PRIO_DEFAULT, timer_irq);
    timer_write_tval(interval);
    timer_write_ctl(CNTV_CTL_ENABLE);
    return 0;
}

u64 timer_ticks(void)
{
    return this_cpu()->ticks;
}

u64 timer_uptime_ms(void)
{
    u64 freq = read_cntfrq();

    if (!freq)
        return 0;

    return ((read_cntvct() - start_count) * 1000UL) / freq;
}
