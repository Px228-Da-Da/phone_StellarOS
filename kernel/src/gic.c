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
#include "smp.h"
#include "spinlock.h"
#include "fdt.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"
#define GICD_DEFAULT   MT_GICD_BASE
#define GICR_DEFAULT   MT_GICR_BASE
#else
#include "soc/qemu_virt.h"
#define GICD_DEFAULT   QEMU_GICD_BASE
#define GICR_DEFAULT   QEMU_GICR_BASE
#endif

/* Значения из карты SoC — только отправная точка: если есть device tree,
 * адреса берутся оттуда (см. gic_bases_from_fdt). */
static u64 gicd_base = GICD_DEFAULT;
static u64 gicr_base = GICR_DEFAULT;

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
#define ICC_RPR_EL1         "S3_0_C12_C11_3"    /* текущий приоритет      */
#define ICC_HPPIR1_EL1      "S3_0_C12_C12_2"    /* что ждёт своей очереди */

#define INTID_SPURIOUS      1020        /* ответ GIC: прерываний больше нет */
#define INTID_MASK          0xFFFFFF

/* Редистрибьютор у каждого ядра свой, поэтому его база живёт
 * в per-CPU структуре, а не в общей переменной. */
#define my_gicr()   (this_cpu()->gicr)

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
    u64 base = gicr_base;

    /*
     * Конец списка помечает бит Last в GICR_TYPER — на него и полагаемся.
     * Свой предел на число кадров нужен только как страховка от мусора
     * в регистрах: если Last не встретится, цикл обязан кончиться сам.
     *
     * Он именно страховочный и потому большой. Сначала здесь стояло 16 —
     * «ядер же не больше», — и на конфигурации с ядрами второго кластера
     * поиск сдавался ровно перед нужным кадром: номер кадра считается по
     * порядковому номеру ядра в системе, а не по числу ядер, которые
     * используем мы.
     */
    for (u32 i = 0; i < 512; i++) {
        u64 typer = mmio_read64(base + GICR_TYPER);

        if ((u32)(typer >> 32) == want)
            return base;
        if (typer & (1UL << 4))         /* Last: кадров больше нет */
            break;
        base += GICR_STRIDE;
    }
    return 0;
}

/* Разбудить редистрибьютор: после сброса он считает ядро спящим
 * и не доставляет ему ничего. */
static int gicr_wake(u64 gicr)
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

/*
 * Настройка интерфейса прерываний ТЕКУЩЕГО ядра.
 *
 * Всё, что здесь делается, — процессорное: регистры ICC_* принадлежат
 * ядру, редистрибьютор у каждого ядра свой. Настройки CPU0 на разбуженные
 * ядра не распространяются никак, поэтому каждое повторяет это само.
 */
/*
 * Состояние контроллера прерываний у текущего ядра.
 *
 * Печатается, когда прерывания не пошли. Разбираться в таком отказе по
 * молчанию невозможно, а эти шесть чисел отвечают почти на всё:
 *
 *   RPR   текущий приоритет. Не 0xFF — значит какое-то прерывание
 *         считается обслуживаемым прямо сейчас, и всё, что не важнее,
 *         до нас не дойдёт. Так бывает, если загрузчик взял прерывание
 *         и не отпустил его перед передачей управления.
 *   HPPIR что ждёт очереди. 1023 — не ждёт ничего.
 *   PMR   порог: прерывания хуже него не проходят вовсе.
 *   IGRPEN разрешена ли наша группа.
 *   ISENABLER какие локальные прерывания включены: таймер — 27-й бит.
 *   WAKER  проснулся ли редистрибьютор.
 */
void gic_debug_dump(void)
{
    struct cpu *c = this_cpu();
    u64 gicr = c ? c->gicr : 0;

    kprintf("GIC      : RPR %02lx HPPIR %lu PMR %02lx GRP1EN %lu SRE %lu\n",
            SYSREG_READ(ICC_RPR_EL1),
            SYSREG_READ(ICC_HPPIR1_EL1) & INTID_MASK,
            SYSREG_READ(ICC_PMR_EL1),
            SYSREG_READ(ICC_IGRPEN1_EL1) & 1,
            SYSREG_READ(ICC_SRE_EL1) & 1);

    if (gicr)
        kprintf("GIC      : GICR %p ВКЛЮЧЕНО %08x ЖДЁТ %08x WAKER %08x\n",
                (void *)(uintptr_t)gicr,
                mmio_read32(gicr + GICR_ISENABLER0),
                mmio_read32(gicr + GICR_ICPENDR0),
                mmio_read32(gicr + GICR_WAKER));
}

/*
 * Отпустить всё, что осталось от загрузчика.
 *
 * Прерывание, которое кто-то взял и не отпустил, продолжает считаться
 * обслуживаемым — и блокирует всё, что не важнее его. Обработчиков у нас
 * ещё нет, поэтому просто забираем и отпускаем по кругу, пока контроллер
 * не ответит «больше нечего». Ограничение по числу оборотов обязательно:
 * если контроллер отвечает не так, как мы думаем, зациклиться здесь
 * означало бы потерять загрузку целиком.
 */
static void gic_drain(void)
{
    for (u32 i = 0; i < 64; i++) {
        u64 iar = SYSREG_READ(ICC_IAR1_EL1);
        u32 intid = (u32)(iar & INTID_MASK);

        if (intid >= INTID_SPURIOUS)
            return;
        SYSREG_WRITE(ICC_EOIR1_EL1, iar);
        isb();
    }
}

int gic_init_cpu(void)
{
    struct cpu *c = this_cpu();
    u64 gicr;

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

    /* 2. Свой редистрибьютор — тот, чьё аффинити совпадает с нашим MPIDR */
    gicr = gicr_find_self();
    if (!gicr) {
        kprintf("GIC      : РЕДИСТРИБЬЮТОР НЕ НАЙДЕН (ЯДРО %lu)\n", c->id);
        return -1;
    }
    if (gicr_wake(gicr) != 0) {
        kprintf("GIC      : РЕДИСТРИБЬЮТОР НЕ ПРОСНУЛСЯ (ЯДРО %lu)\n", c->id);
        return -1;
    }
    c->gicr = gicr;

    /* 3. SGI и PPI: гасим всё и снимаем зависшие флаги ожидания.
     *    Загрузчик мог оставить включённым что угодно, а обработчиков
     *    у нас ещё нет — первое же такое прерывание дало бы вечный цикл. */
    mmio_write32(gicr + GICR_ICENABLER0, 0xFFFFFFFF);
    mmio_write32(gicr + GICR_ICPENDR0,   0xFFFFFFFF);
    /* Все локальные прерывания — в группу 1 non-secure: это наша группа */
    mmio_write32(gicr + GICR_IGROUPR0,   0xFFFFFFFF);
    dsb();

    /* 4. CPU-интерфейс: пропускаем прерывания любого приоритета,
     *    без деления на подприоритеты, группа 1 включена. */
    SYSREG_WRITE(ICC_PMR_EL1, 0xFF);
    SYSREG_WRITE(ICC_BPR1_EL1, 0);
    SYSREG_WRITE(ICC_CTLR_EL1, 0);      /* EOI одним шагом: и приоритет, и деактивация */
    SYSREG_WRITE(ICC_IGRPEN1_EL1, 1);
    isb();

    /* 5. Забираем и отпускаем всё, что осталось незакрытым от загрузчика:
     *    иначе оно продолжает считаться обслуживаемым и блокирует наши. */
    gic_drain();

    return 0;
}

/*
 * Взять адреса контроллера из device tree.
 *
 * Раньше обе базы были константами: дистрибьютор снят с имени узла живого
 * устройства, а редистрибьютор — взят по раскладке GIC-500 из чужих ядер,
 * то есть оставался единственной непроверенной величиной в карте регистров.
 * Дерево знает оба адреса точно, и знает их про ЭТО устройство.
 *
 * Константы остаются запасным вариантом: дерева может и не быть.
 */
static void gic_bases_from_fdt(void)
{
    struct fdt_region r;
    u64 dtb = fdt_root();

    if (!dtb)
        return;

    if (fdt_compatible_reg(dtb, "arm,gic-v3", 0, &r) == 0 && r.base) {
        if (r.base != gicd_base)
            kprintf("GIC      : GICD ИЗ DTB %p (В КАРТЕ БЫЛО %p)\n",
                    (void *)(uintptr_t)r.base, (void *)(uintptr_t)gicd_base);
        gicd_base = r.base;
    }

    if (fdt_compatible_reg(dtb, "arm,gic-v3", 1, &r) == 0 && r.base) {
        if (r.base != gicr_base)
            kprintf("GIC      : GICR ИЗ DTB %p (В КАРТЕ БЫЛО %p)\n",
                    (void *)(uintptr_t)r.base, (void *)(uintptr_t)gicr_base);
        gicr_base = r.base;
    }
}

int gic_init(void)
{
    u32 ctlr;

    /* Адреса уточняем ДО первого обращения к железу */
    gic_bases_from_fdt();

    if (gic_init_cpu() != 0)
        return -1;

    /* Дистрибьютор один на систему, и настраивает его только CPU0.
     * Именно read-modify-write: на телефоне дистрибьютор уже настроен
     * ATF, и затирать его биты своими — верный способ потерять
     * прерывания, о которых мы пока ничего не знаем. */
    ctlr = mmio_read32(gicd_base + GICD_CTLR);
    mmio_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_ARE_NS | GICD_CTLR_ENGRP1NS);
    dsb();

    kprintf("GIC      : GICv3, GICD %p, GICR %p\n",
            (void *)(uintptr_t)gicd_base, (void *)(uintptr_t)my_gicr());
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
        /* SGI и PPI — хозяйство редистрибьютора нашего ядра.
         * Поэтому таймерный PPI включает каждое ядро себе само. */
        /* Приоритет - ровно один байт: массив IPRIORITYR байтовый */
        mmio_write8(my_gicr() + GICR_IPRIORITYR + intid, (u8)prio);
        mmio_write32(my_gicr() + GICR_ISENABLER0, bit);
    } else {
        /* SPI: сначала группа и маршрут, только потом включение —
         * иначе прерывание может прийти раньше, чем решено, куда его слать. */
        u32 g = mmio_read32(gicd_base + GICD_IGROUPR + reg * 4);

        mmio_write32(gicd_base + GICD_IGROUPR + reg * 4, g | bit);
        mmio_write8(gicd_base + GICD_IPRIORITYR + intid, (u8)prio);
        /* Маршрут: конкретному ядру, тому самому, где сейчас исполняемся */
        mmio_write64(gicd_base + GICD_IROUTER + intid * 8,
                     read_mpidr() & 0xFF00FFFFFFUL);
        mmio_write32(gicd_base + GICD_ISENABLER + reg * 4, bit);
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

        /* Счётчики общие для всех ядер, поэтому только атомарно:
         * обычный ++ на восьми ядрах теряет часть прибавлений. */
        if (intid < GIC_MAX_IRQS && handlers[intid]) {
            handlers[intid](intid);
            atomic_inc(&irq_count);
        } else {
            atomic_inc(&spurious_count);
        }

        /* EOI строго после обработчика: до него прерывание того же
         * приоритета повторно не придёт. */
        SYSREG_WRITE(ICC_EOIR1_EL1, iar);
    }
}

u64 gic_count(void)    { return atomic_read(&irq_count); }
u64 gic_spurious(void) { return atomic_read(&spurious_count); }
