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
#include "pmm.h"
#include "spinlock.h"

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

/* ---------------------------------------------------------------------
 * Отображения для EL0
 *
 * Здесь впервые появляется третий уровень таблиц. Ядру хватало двух:
 * гигабайтные блоки под периферию и двухмегабайтные под память. Страница
 * в 4 КБ описывается записью третьего уровня, и путь к ней такой:
 *
 *   VA[38:30] -> запись L1 (гигабайт)   -> таблица L2
 *   VA[29:21] -> запись L2 (два мегабайта) -> таблица L3
 *   VA[20:12] -> запись L3 (страница)
 *
 * Таблицы берём у постраничного аллокатора, он же их и обнуляет: нулевая
 * запись — невалидная, то есть новая таблица по умолчанию не отображает
 * ничего. Это ровно то, что нужно: доступ к любому адресу, который мы не
 * отобразили явно, обязан кончаться ошибкой, а не случайной страницей.
 *
 * Права. Поле AP занимает биты 7:6 и означает не «что можно», а «кому»:
 *
 *   00  EL1 читает и пишет, EL0 не имеет доступа  — так отображено ядро
 *   01  EL1 читает и пишет, EL0 читает и пишет
 *   11  оба только читают
 *
 * PXN на пользовательских страницах ставим всегда: ядру незачем уметь
 * исполнять код, который писала программа. Если ошибка в ядре когда-нибудь
 * уведёт исполнение в пользовательскую страницу, процессор остановит это
 * сам, а не станет исполнять чужое с полными правами.
 */
#define D_PAGE      (3UL << 0)      /* валидная запись третьего уровня */
#define D_AP_USER   (1UL << 6)      /* AP[1]: доступ есть и у EL0      */
#define D_AP_RO     (2UL << 6)      /* AP[2]: только чтение            */
#define D_NG        (1UL << 11)     /* запись принадлежит одному ASID  */

/*
 * Спуститься на уровень ниже, создав таблицу, если её ещё нет.
 *
 * Возвращает NULL, только если кончилась память: сообщать об этом должен
 * вызывающий, у него есть имя программы, которую он не смог запустить.
 */
static u64 *next_level(u64 *table, u64 index)
{
    u64 entry = table[index];
    void *page;

    if (entry & D_VALID) {
        /*
         * Запись уже есть — но это может быть БЛОК, а не таблица. Блок на
         * пути пользовательского отображения означает, что мы пытаемся
         * положить программу поверх памяти ядра; молча разломать такой
         * блок на страницы было бы худшим из возможных ответов.
         */
        if ((entry & 3) != 3)
            return NULL;
        return (u64 *)(uintptr_t)(entry & 0x0000FFFFFFFFF000UL);
    }

    page = pmm_alloc();
    if (!page)
        return NULL;

    table[index] = (u64)(uintptr_t)page | D_TABLE;
    return (u64 *)page;
}

int mmu_map_user(u64 root, u64 va, u64 pa, u64 size, u32 kind)
{
    /*
     * nG обязателен. Без него запись считается глобальной, то есть
     * годной для любого адресного пространства, и процессор вправе
     * ответить соседней программе по её собственному адресу страницей
     * этой. Именно эта мелочь превращает раздельные пространства
     * обратно в общее — молча и не сразу.
     */
    u64 attr = D_PAGE | D_ATTRIDX(ATTR_NORMAL) | D_AF | D_SH_INNER |
               D_AP_USER | D_PXN | D_NG;
    u64 *l1 = (u64 *)(uintptr_t)(root & 0x0000FFFFFFFFF000UL);
    u64 end;

    if (!l1)
        return -1;

    if (kind != MMU_USER_RX)
        attr |= D_UXN;              /* исполнять можно только код     */
    if (kind == MMU_USER_RO)
        attr |= D_AP_RO;
    if (kind == MMU_USER_FB) {
        /* Память, которую читает не процессор: тип обязан совпадать с
         * тем, которым она помечена у ядра, иначе одна и та же память
         * оказывается кэшируемой с одной стороны и нет с другой. */
        attr &= ~D_ATTRIDX(ATTR_NORMAL);
        attr |= D_ATTRIDX(ATTR_NC);
    }

    /* Выравниваем на страницу в обе стороны: отобразить половину
     * страницы нельзя, а молча отбросить хвост — значит оставить
     * программе кусок памяти, которого у неё нет. */
    end = (va + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    va &= ~(PAGE_SIZE - 1);
    pa &= ~(PAGE_SIZE - 1);

    for (; va < end; va += PAGE_SIZE, pa += PAGE_SIZE) {
        u64 *l2 = next_level(l1, (va >> 30) & (ENTRIES - 1));
        u64 *l3;

        if (!l2)
            return -1;
        l3 = next_level(l2, (va >> 21) & (ENTRIES - 1));
        if (!l3)
            return -1;

        l3[(va >> 12) & (ENTRIES - 1)] = pa | attr;
    }

    /*
     * Порядок обязателен: сначала записи таблиц должны дойти до памяти,
     * и только потом сбрасываем TLB. Сброс — широковещательный (is),
     * потому что таблица одна на восемь ядер, и любое из них могло
     * заглянуть сюда раньше нас и запомнить, что трансляции нет.
     */
    dsb();
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    isb();
    return 0;
}

void mmu_sync_icache(u64 va, u64 size)
{
    u64 ctr;
    u64 dline, iline;
    u64 end = va + size;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    iline = 4UL << (ctr & 0xF);                 /* IminLine, в словах */
    dline = 4UL << ((ctr >> 16) & 0xF);         /* DminLine           */

    /* Выгнать написанное из кэша данных до точки, где его увидит выборка
     * инструкций (PoU), а потом выбросить из кэша инструкций всё старое,
     * что могло быть подтянуто по этим адресам. */
    for (u64 a = va & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");

    for (u64 a = va & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");

    /*
     * И ещё раз, целиком.
     *
     * Чистка по адресу должна была бы хватить: архитектура обещает, что
     * она выбрасывает все строки с этим физическим адресом, каким бы
     * виртуальным его ни назвали. На merlin этого оказалось мало.
     *
     * Видно это стало только когда страницы начали переиспользоваться
     * быстро: программа получала страницу, только что освобождённую
     * другой, писала туда свой код — и исполняла чужой, оставшийся в
     * кэше инструкций. Кончалось это отказом на неопределённой команде
     * далеко за концом собственного кода: там, где у прежнего жильца
     * что-то было, а у нового уже нули.
     *
     * Мы пишем код по одному адресу, а исполняется он по другому — и
     * это ровно тот случай, для которого существует чистка всего кэша.
     * Стоит она микросекунды и делается один раз на запуск программы,
     * так что спорить тут не о чем.
     */
    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    isb();
}

/*
 * Новое адресное пространство.
 *
 * Корень — одна страница на 512 записей верхнего уровня. Ядерные записи
 * копируем из исходной таблицы: это указатели на общие таблицы второго
 * уровня, поэтому копия не удваивает память и не расходится с оригиналом
 * при правках вроде перекраски фреймбуфера в некэшируемый.
 *
 * Номера пространств раздаём подряд. Их 255, и когда они кончатся, надо
 * будет освобождать номера завершившихся программ и вычищать за ними
 * TLB. Пока программ единицы, честнее отказать, чем выдать номер,
 * который уже кем-то занят: одинаковый ASID у двух разных таблиц — это
 * ровно та ошибка, ради предотвращения которой номера и заведены.
 */
/*
 * Номера пространств выдаём из битовой карты, а не подряд.
 *
 * Подряд — значит одноразово: программы приходят и уходят, а номера
 * кончаются, и на 256-й запуск система осталась бы без них. Номер
 * освободившейся программы можно выдать снова, но только после того, как
 * из TLB выгнаны её записи: иначе новая программа получит чужие
 * отображения, помеченные тем же номером. Поэтому чистка TLB и возврат
 * номера сделаны в одном месте и в этом порядке.
 *
 * Нулевой номер принадлежит ядру и не выдаётся никогда.
 */
#define ASID_COUNT  256

static struct spinlock asid_lock = SPINLOCK_INIT("asid");
static u64 asid_map[ASID_COUNT / 64] = { 1 };   /* нулевой сразу занят */

static u64 asid_take(void)
{
    u64 flags = spin_lock_irq(&asid_lock);

    for (u32 i = 0; i < ASID_COUNT; i++) {
        if (asid_map[i / 64] & (1UL << (i % 64)))
            continue;
        asid_map[i / 64] |= 1UL << (i % 64);
        spin_unlock_irq(&asid_lock, flags);
        return i;
    }

    spin_unlock_irq(&asid_lock, flags);
    return 0;                       /* свободных нет */
}

static void asid_give_back(u64 asid)
{
    u64 flags;

    if (!asid || asid >= ASID_COUNT)
        return;

    /* Выгнать из TLB ВСЁ, что помечено этим номером, и только потом
     * считать номер свободным. Широковещательно: программа успела
     * побывать на всех восьми ядрах. */
    __asm__ volatile("tlbi aside1is, %0" :: "r"(asid << 48) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    isb();

    flags = spin_lock_irq(&asid_lock);
    asid_map[asid / 64] &= ~(1UL << (asid % 64));
    spin_unlock_irq(&asid_lock, flags);
}

u64 mmu_kernel_space(void)
{
    return (u64)(uintptr_t)l1_table;    /* ASID 0 */
}

u64 mmu_new_user_space(void)
{
    u64 *root = pmm_alloc();
    u64 asid;

    if (!root)
        return 0;

    asid = asid_take();
    if (!asid) {
        kprintf("MMU      : НОМЕРА АДРЕСНЫХ ПРОСТРАНСТВ КОНЧИЛИСЬ\n");
        pmm_free(root);
        return 0;
    }

    for (u32 i = 0; i < ENTRIES; i++)
        root[i] = l1_table[i];

    dsb();
    return ((u64)(uintptr_t)root) | (asid << 48);
}

void mmu_switch(u64 ttbr0)
{
    /*
     * Сброса TLB здесь нет и не должно быть: пользовательские записи
     * помечены своим ASID, ядерные глобальны. Сброс на каждом
     * переключении задач стоил бы дороже самого переключения.
     */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0) : "memory");
    isb();
}

/*
 * Освободить адресное пространство целиком.
 *
 * Обходим корень и разбираем всё, чего нет у ядра: сравнение с исходной
 * таблицей — самый надёжный признак «это наше, не общее». Записи ядра
 * указывают на общие таблицы второго уровня, тронуть их значило бы
 * отобрать память у всех сразу.
 *
 * Вызывать можно только тогда, когда пространство точно не включено ни
 * на одном ядре. Сборщик задач это обеспечивает: он разбирает задачу
 * лишь после того, как на её ядре пошла следующая, а переключение задач
 * всегда переключает и TTBR0.
 */
static void free_l3(u64 *l3)
{
    for (u32 i = 0; i < ENTRIES; i++) {
        u64 e = l3[i];

        if ((e & 3) == 3)               /* валидная страница */
            pmm_free((void *)(uintptr_t)(e & 0x0000FFFFFFFFF000UL));
    }
    pmm_free(l3);
}

static void free_l2(u64 *l2)
{
    for (u32 i = 0; i < ENTRIES; i++) {
        u64 e = l2[i];

        /* Тройка в младших битах — таблица; единица была бы блоком на
         * два мегабайта, но в пользовательских отображениях их нет. */
        if ((e & 3) == 3)
            free_l3((u64 *)(uintptr_t)(e & 0x0000FFFFFFFFF000UL));
    }
    pmm_free(l2);
}

void mmu_unmap_user(u64 root, u64 va, u64 size)
{
    u64 *l1 = (u64 *)(uintptr_t)(root & 0x0000FFFFFFFFF000UL);
    u64 end = (va + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (!l1)
        return;

    for (va &= ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
        u64 e1 = l1[(va >> 30) & (ENTRIES - 1)];
        u64 *l2, *l3;
        u64 e2;

        if ((e1 & 3) != 3)
            continue;
        l2 = (u64 *)(uintptr_t)(e1 & 0x0000FFFFFFFFF000UL);
        e2 = l2[(va >> 21) & (ENTRIES - 1)];
        if ((e2 & 3) != 3)
            continue;
        l3 = (u64 *)(uintptr_t)(e2 & 0x0000FFFFFFFFF000UL);
        l3[(va >> 12) & (ENTRIES - 1)] = 0;
    }

    dsb();
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    isb();
}

void mmu_free_user_space(u64 ttbr0)
{
    u64 *root = (u64 *)(uintptr_t)(ttbr0 & 0x0000FFFFFFFFF000UL);
    u64 asid = (ttbr0 >> 48) & 0xFF;

    if (!root)
        return;

    /* Сначала TLB, потом память: страницу, на которую ещё может
     * сослаться процессор, отдавать другой программе нельзя. */
    asid_give_back(asid);

    for (u32 i = 0; i < ENTRIES; i++) {
        u64 e = root[i];

        if (!(e & D_VALID) || e == l1_table[i])
            continue;                   /* пусто или общее с ядром */
        if ((e & 3) == 3)
            free_l2((u64 *)(uintptr_t)(e & 0x0000FFFFFFFFF000UL));
    }

    pmm_free(root);
}
