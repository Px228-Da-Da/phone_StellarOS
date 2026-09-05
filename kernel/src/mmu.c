/*
 * Таблицы трансляции AArch64 и включение MMU.
 *
 * Раскладка адресного пространства простая — единичное отображение
 * (виртуальный адрес = физический). Пока нет пользовательских процессов,
 * усложнять незачем.
 *
 *   0x00000000 .. 0x40000000   периферия -> Device-nGnRnE, блоки по 1 ГБ
 *   0x40000000 .. +DRAM_SIZE   память    -> Normal WB, блоки по 2 МБ
 *
 * Граница в 1 ГБ подходит обеим платам: и у QEMU virt, и у MT6768
 * вся периферия лежит ниже 0x40000000, а DRAM начинается ровно с него.
 *
 * Блоки по 2 МБ для DRAM выбраны не случайно: они позволяют потом
 * перекрасить в некэшируемые именно те страницы, где лежит фреймбуфер,
 * не трогая остальную память.
 */
#include "mmu.h"
#include "io.h"
#include "print.h"

#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"
#define DRAM_BASE   MT_RAM_BASE
#define DRAM_SIZE   MT_RAM_SIZE
#else
#include "soc/qemu_virt.h"
#define DRAM_BASE   QEMU_RAM_BASE
#define DRAM_SIZE   QEMU_RAM_SIZE
#endif

#define GB          (1024UL * 1024 * 1024)
#define MB2         (2UL * 1024 * 1024)
#define ENTRIES     512

/* Индексы в MAIR_EL1 */
#define ATTR_DEVICE 0
#define ATTR_NORMAL 1
#define ATTR_NC     2

/* Поля дескриптора трансляции */
#define D_VALID     (1UL << 0)
#define D_TABLE     (3UL << 0)      /* валидный + таблица  */
#define D_BLOCK     (1UL << 0)      /* валидный + блок     */
#define D_ATTRIDX(i) ((u64)(i) << 2)
#define D_AF        (1UL << 10)     /* access flag: иначе первый же доступ = ошибка */
#define D_SH_INNER  (3UL << 8)
#define D_PXN       (1UL << 53)
#define D_UXN       (1UL << 54)

#define ATTR_DEV_BLOCK  (D_BLOCK | D_ATTRIDX(ATTR_DEVICE) | D_AF | D_PXN | D_UXN)
#define ATTR_MEM_BLOCK  (D_BLOCK | D_ATTRIDX(ATTR_NORMAL) | D_AF | D_SH_INNER)
#define ATTR_NC_BLOCK   (D_BLOCK | D_ATTRIDX(ATTR_NC)     | D_AF | D_SH_INNER)

#define DRAM_GB_COUNT   (DRAM_SIZE / GB)

/* Уровень 1: 512 записей по 1 ГБ => покрываем 512 ГБ адресного пространства */
static u64 l1_table[ENTRIES]                  __attribute__((aligned(4096)));
/* Уровень 2: по таблице на каждый гигабайт DRAM, записи по 2 МБ */
static u64 l2_tables[DRAM_GB_COUNT][ENTRIES]  __attribute__((aligned(4096)));

static void build_tables(void)
{
    /* Периферия: первый гигабайт как Device. Исполнение там запрещено. */
    l1_table[0] = 0x00000000UL | ATTR_DEV_BLOCK;

    /* DRAM: каждый гигабайт разбиваем на блоки по 2 МБ */
    for (u64 g = 0; g < DRAM_GB_COUNT; g++) {
        u64 gb_base = DRAM_BASE + g * GB;
        u64 l1_idx  = gb_base / GB;

        for (u64 i = 0; i < ENTRIES; i++)
            l2_tables[g][i] = (gb_base + i * MB2) | ATTR_MEM_BLOCK;

        l1_table[l1_idx] = (u64)(uintptr_t)&l2_tables[g][0] | D_TABLE;
    }

    /* Остальное адресное пространство остаётся невалидным: обращение
     * туда даст понятную ошибку трансляции вместо тихого чтения мусора. */
}

int mmu_enable(void)
{
    build_tables();
    return mmu_enable_cpu();
}

/*
 * Включение MMU на ТЕКУЩЕМ ядре.
 *
 * Вынесено отдельно, потому что MAIR, TCR, TTBR0 и SCTLR — регистры
 * процессора, а не системы: каждое проснувшееся ядро обязано настроить
 * их себе само. Таблицы при этом общие, их строит только CPU0.
 */
int mmu_enable_cpu(void)
{
    u64 mair, tcr, sctlr, ips;

    /* Attr0 Device-nGnRnE, Attr1 Normal write-back, Attr2 Normal non-cacheable */
    mair = (0x00UL << (8 * ATTR_DEVICE)) |
           (0xFFUL << (8 * ATTR_NORMAL)) |
           (0x44UL << (8 * ATTR_NC));

    /* Разрядность физических адресов берём у самого процессора */
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(ips));
    ips &= 0xF;

    tcr = (25UL)            /* T0SZ=25 => 39-битный VA, начальный уровень L1 */
        | (1UL  << 8)       /* IRGN0: внутренний кэш write-back              */
        | (1UL  << 10)      /* ORGN0: внешний кэш write-back                 */
        | (3UL  << 12)      /* SH0: inner shareable                          */
        | (0UL  << 14)      /* TG0: гранула 4 КБ                             */
        | (1UL  << 23)      /* EPD1: TTBR1 не используем                     */
        | (ips  << 32);     /* IPS                                           */

    __asm__ volatile(
        "msr mair_el1,  %0\n"
        "msr tcr_el1,   %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((u64)(uintptr_t)l1_table) : "memory");

    /* Сбрасываем всё, что осталось от работы без трансляции */
    __asm__ volatile(
        "tlbi vmalle1\n"
        "ic iallu\n"
        "dsb nsh\n"
        "isb\n"
        ::: "memory");

    /* Собственно включение: M=трансляция, C=кэш данных, I=кэш инструкций */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        :: "r"(sctlr) : "memory");

    return 0;
}

u64 mmu_ram_limit(void)
{
    return DRAM_BASE + DRAM_SIZE;
}

/*
 * Вычистить кэш на диапазоне.
 *
 * Длину строки берём из CTR_EL0, а не считаем равной шестидесяти четырём:
 * у big.LITTLE ядра разные, и промахнуться здесь значит пройти мимо части
 * строк, оставив в кэше грязные данные.
 */
static void dcache_clean_inval(u64 start, u64 end)
{
    u64 ctr;
    u64 line;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = 4UL << ((ctr >> 16) & 0xF);          /* DminLine, в словах */

    for (u64 a = start & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    dsb();
}

void mmu_set_range_nc(u64 pa, u64 size)
{
    u64 start = pa & ~(MB2 - 1);
    u64 end   = (pa + size + MB2 - 1) & ~(MB2 - 1);

    /*
     * Сначала кэш, потом дескрипторы.
     *
     * Смена типа памяти сама по себе кэш не трогает. Всё, что писалось в
     * эту область раньше — например, обнуление страниц аллокатором, — так
     * и остаётся грязными строками. Дальше мы пишем сюда уже мимо кэша,
     * прямо в память, а потом вытеснение старой строки приходит и затирает
     * записанное. Наружу это выглядит как буфер, который сам собой
     * обнуляется без единой записи в него.
     */
    dcache_clean_inval(start, end);

    for (u64 a = start; a < end; a += MB2) {
        u64 g = (a - DRAM_BASE) / GB;
        u64 i = (a % GB) / MB2;

        if (a < DRAM_BASE || g >= DRAM_GB_COUNT)
            continue;
        l2_tables[g][i] = a | ATTR_NC_BLOCK;
    }

    /* Порядок важен: сначала запись дескрипторов должна дойти до памяти,
     * только потом сбрасываем TLB, иначе процессор подхватит старую запись. */
    dsb();
    for (u64 a = start; a < end; a += MB2)
        __asm__ volatile("tlbi vaae1, %0" :: "r"(a >> 12) : "memory");
    dsb();
    isb();

    /* И ещё раз после смены типа: за время правки дескрипторов процессор
     * мог подтянуть строки наперёд, по старому ещё описанию. */
    dcache_clean_inval(start, end);
}
