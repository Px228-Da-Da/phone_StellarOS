/*
 * gic.c — драйвер GICv3 (ARM Generic Interrupt Controller).
 *
 * Три блока, с которыми имеем дело:
 *
 *   Distributor (GICD)      один на систему, раздаёт SPI (прерывания устройств)
 *   Redistributor (GICR)    свой на каждое ядро, отвечает за SGI и PPI
 *   CPU interface (ICC_*)   системные регистры процессора: ack, EOI, маска
 *
 * Таймер — это PPI, поэтому для него хватает редистрибьютора и ICC_*;
 * дистрибьютор трогаем только ради SPI, которые появятся на этапе 5.
 */
#include "gic.h"
#include "io.h"
#include "print.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"
#define GICD_BASE   MT_GICD_BASE
#define GICR_BASE   MT_GICR_BASE
#else
#include "soc/qemu_virt.h"
#define GICD_BASE   QEMU_GICD_BASE
#define GICR_BASE   QEMU_GICR_BASE
#endif

/* --- Distributor --- */
#define GICD_CTLR           0x0000
#define GICD_TYPER          0x0004
#define GICD_IGROUPR        0x0080      /* 32 прерывания на регистр  */
#define GICD_ISENABLER      0x0100
#define GICD_ICENABLER      0x0180
#define GICD_IPRIORITYR     0x0400      /* по байту на прерывание    */
#define GICD_ICFGR          0x0C00
#define GICD_IROUTER        0x6000      /* по 64 бита, с INTID 32    */

#define GICD_CTLR_ENGRP1NS  (1U << 1)   /* раздавать группу 1 non-secure    */
#define GICD_CTLR_ARE_NS    (1U << 4)   /* Affinity Routing: у v3 обязателен */

/* --- Redistributor ---
 * Кадр каждого ядра: 64 КБ RD_base + 64 КБ SGI_base, итого шаг 128 КБ. */
#define GICR_STRIDE         0x20000
#define GICR_SGI_OFFSET     0x10000

#define GICR_CTLR           0x0000
#define GICR_TYPER          0x0008
#define GICR_WAKER          0x0014
#define GICR_WAKER_SLEEP    (1U << 1)   /* ProcessorSleep  */
#define GICR_WAKER_CHILDREN (1U << 2)   /* ChildrenAsleep  */

#define GICR_IGROUPR0       (GICR_SGI_OFFSET + 0x0080)
#define GICR_ISENABLER0     (GICR_SGI_OFFSET + 0x0100)
#define GICR_ICENABLER0     (GICR_SGI_OFFSET + 0x0180)
#define GICR_ICPENDR0       (GICR_SGI_OFFSET + 0x0280)
#define GICR_IPRIORITYR     (GICR_SGI_OFFSET + 0x0400)

/* --- CPU interface: системные регистры ---
 * Имена ICC_* старый ассемблер может не знать, поэтому кодировки прямо тут. */
#define ICC_PMR_EL1         "S3_0_C4_C6_0"      /* маска по приоритету    */
#define ICC_IAR1_EL1        "S3_0_C12_C12_0"    /* взять прерывание       */
#define ICC_EOIR1_EL1       "S3_0_C12_C12_1"    /* отпустить прерывание   */
#define ICC_BPR1_EL1        "S3_0_C12_C12_3"
#define ICC_CTLR_EL1        "S3_0_C12_C12_4"
#define ICC_SRE_EL1         "S3_0_C12_C12_5"    /* System Register Enable */
#define ICC_IGRPEN1_EL1     "S3_0_C12_C12_7"

#define INTID_SPURIOUS      1020        /* ответ GIC: прерываний больше нет */
#define INTID_MASK          0xFFFFFF

static u64 gicr;                        /* кадр редистрибьютора нашего ядра */
static irq_handler_fn handlers[GIC_MAX_IRQS];
static u64 irq_count;
static u64 spurious_count;

/*
 * Найти свой редистрибьютор. Кадры лежат подряд, в каждом GICR_TYPER
 * хранит аффинити своего ядра — сравниваем с MPIDR и так узнаём свой.
 * Бит Last помечает последний кадр: дальше искать нечего.
 */
static u64 gicr_find_self(void)
{
    u64 mpidr = read_mpidr();
    /* Aff3.Aff2.Aff1.Aff0 в том же виде, в каком лежит в GICR_TYPER[63:32] */
    u32 want = (u32)((mpidr & 0x00FFFFFFUL) | ((mpidr >> 8) & 0xFF000000UL));
    u64 base = GICR_BASE;

    for (u32 i = 0; i < 16; i++) {
        u64 typer = mmio_read64(base + GICR_TYPER);

        if ((u32)(typer >> 32) == want)
            return base;
        if (typer & (1UL << 4))         /* Last */
            break;
        base += GICR_STRIDE;
    }
    return 0;
}

/* Разбудить редистрибьютор: после сброса он считает ядро спящим
 * и не доставляет ему ничего. */
static int gicr_wake(void)
{
    u32 w = mmio_read32(gicr + GICR_WAKER);

    mmio_write32(gicr + GICR_WAKER, w & ~GICR_WAKER_SLEEP);

    for (u32 spin = 0; spin < 1000000; spin++)
        if (!(mmio_read32(gicr + GICR_WAKER) & GICR_WAKER_CHILDREN))
            return 0;

    return -1;
}

/*
 * Есть ли у процессора системный интерфейс GIC.
 *
 * Проверка не формальная: на машине с GICv2 регистров ICC_* просто нет,
 * и первое же обращение к ним — undefined instruction, то есть паника
 * ещё до единой строчки диагностики. Процессор сам сообщает о поддержке
 * в поле GIC регистра ID_AA64PFR0_EL1 — спрашиваем у него, а не гадаем.
 */
static int gic_sysreg_supported(void)
{
    u64 pfr0;

    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));

    return ((pfr0 >> 24) & 0xF) != 0;
}

int gic_init(void)
{
    u32 ctlr;

    if (!gic_sysreg_supported()) {
        kprintf("GIC      : ПРОЦЕССОР НЕ ВИДИТ GICv3 (СТОИТ v2?)\n");
        return -1;
    }

    /* 1. Разрешаем себе системные регистры ICC_*. Бит может быть RAO/WI —
     *    поэтому пишем и тут же перечитываем, а не верим на слово. */
    SYSREG_WRITE(ICC_SRE_EL1, SYSREG_READ(ICC_SRE_EL1) | 1);
    isb();
    if (!(SYSREG_READ(ICC_SRE_EL1) & 1)) {
        kprintf("GIC      : НЕТ ДОСТУПА К ICC_* (SRE=0)\n");
        return -1;
    }

    /* 2. Свой редистрибьютор */
    gicr = gicr_find_self();
    if (!gicr) {
        kprintf("GIC      : РЕДИСТРИБЬЮТОР НЕ НАЙДЕН\n");
        return -1;
    }
    if (gicr_wake() != 0) {
        kprintf("GIC      : РЕДИСТРИБЬЮТОР НЕ ПРОСНУЛСЯ\n");
        return -1;
    }

    /* 3. SGI и PPI: гасим всё и снимаем зависшие флаги ожидания.
     *    Загрузчик мог оставить включённым что угодно, а обработчиков
     *    у нас ещё нет — первое же такое прерывание дало бы вечный цикл. */
    mmio_write32(gicr + GICR_ICENABLER0, 0xFFFFFFFF);
    mmio_write32(gicr + GICR_ICPENDR0,   0xFFFFFFFF);
    /* Все локальные прерывания — в группу 1 non-secure: это наша группа */
    mmio_write32(gicr + GICR_IGROUPR0,   0xFFFFFFFF);
    dsb();

    /* 4. Дистрибьютор: включаем маршрутизацию по аффинити и группу 1.
     *    Именно read-modify-write: на телефоне дистрибьютор уже настроен
     *    ATF, и затирать его биты своими — верный способ потерять
     *    прерывания, о которых мы пока ничего не знаем. */
    ctlr = mmio_read32(GICD_BASE + GICD_CTLR);
    mmio_write32(GICD_BASE + GICD_CTLR, ctlr | GICD_CTLR_ARE_NS | GICD_CTLR_ENGRP1NS);
    dsb();

    /* 5. CPU-интерфейс: пропускаем прерывания любого приоритета,
     *    без деления на подприоритеты, группа 1 включена. */
    SYSREG_WRITE(ICC_PMR_EL1, 0xFF);
    SYSREG_WRITE(ICC_BPR1_EL1, 0);
    SYSREG_WRITE(ICC_CTLR_EL1, 0);      /* EOI одним шагом: и приоритет, и деактивация */
    SYSREG_WRITE(ICC_IGRPEN1_EL1, 1);
    isb();

    kprintf("GIC      : GICv3, GICD %p, GICR %p\n",
            (void *)(uintptr_t)GICD_BASE, (void *)(uintptr_t)gicr);
    return 0;
}

void gic_enable_irq(u32 intid, u32 prio, irq_handler_fn fn)
{
    u32 reg = intid / 32;
    u32 bit = 1U << (intid % 32);

    if (intid >= GIC_MAX_IRQS)
        return;

    handlers[intid] = fn;

    if (intid < GIC_SPI_BASE) {
        /* SGI и PPI — хозяйство редистрибьютора нашего ядра */
        mmio_write32(gicr + GICR_IPRIORITYR + intid, prio);
        mmio_write32(gicr + GICR_ISENABLER0, bit);
    } else {
        /* SPI: сначала группа и маршрут, только потом включение —
         * иначе прерывание может прийти раньше, чем решено, куда его слать. */
        u32 g = mmio_read32(GICD_BASE + GICD_IGROUPR + reg * 4);

        mmio_write32(GICD_BASE + GICD_IGROUPR + reg * 4, g | bit);
        mmio_write32(GICD_BASE + GICD_IPRIORITYR + intid, prio);
        /* Маршрут: конкретному ядру, тому самому, где сейчас исполняемся */
        mmio_write64(GICD_BASE + GICD_IROUTER + intid * 8,
                     read_mpidr() & 0xFF00FFFFFFUL);
        mmio_write32(GICD_BASE + GICD_ISENABLER + reg * 4, bit);
    }
    dsb();
}

/*
 * Разбор прерываний. Крутимся в цикле, пока GIC не ответит «больше нет»:
 * за одно исключение могло накопиться несколько запросов, и выйти раньше
 * времени значит оставить их висеть до следующего раза.
 */
void gic_dispatch(void)
{
    for (;;) {
        u64 iar = SYSREG_READ(ICC_IAR1_EL1);
        u32 intid = (u32)(iar & INTID_MASK);

        if (intid >= INTID_SPURIOUS)
            return;

        if (intid < GIC_MAX_IRQS && handlers[intid]) {
            handlers[intid](intid);
            irq_count++;
        } else {
            spurious_count++;
        }

        /* EOI строго после обработчика: до него прерывание того же
         * приоритета повторно не придёт. */
        SYSREG_WRITE(ICC_EOIR1_EL1, iar);
    }
}

u64 gic_count(void)    { return irq_count; }
u64 gic_spurious(void) { return spurious_count; }
