/*
 * pmm.c — учёт физических страниц по битовой карте.
 *
 * Раскладка после инициализации:
 *
 *   ram_base .............. начало DRAM (0x40000000 на обеих платах)
 *   ...                     сюда LK/ATF положили своё — не трогаем
 *   __image_start ......... наше ядро
 *   __image_end ........... конец образа
 *   [битовая карта] ....... кладём сразу за образом, размер знаем только тут
 *   ...                     свободная память, ей и распоряжаемся
 *   ram_base + ram_size ... конец DRAM
 *
 * Карта живёт не в .bss, а в самой памяти сразу за ядром. Причина простая:
 * её размер зависит от объёма RAM, а объём мы узнаём в рантайме. Держать
 * в .bss массив «на максимум» значит раздувать образ ради памяти, которой
 * на конкретном устройстве может и не быть.
 */
#include "pmm.h"
#include "string.h"
#include "print.h"
#include "io.h"

/* Границы образа расставляет линкер, см. linker.ld */
extern char __image_start[];
extern char __image_end[];

static u64 base_addr;           /* начало учитываемой области            */
static u64 total_pages;
static u64 used_pages;
static u8 *bitmap;
static u64 bitmap_bytes;
static u64 search_hint;         /* с какой страницы продолжать поиск     */

static u64 align_up(u64 v, u64 a)   { return (v + a - 1) & ~(a - 1); }
static u64 align_down(u64 v, u64 a) { return v & ~(a - 1); }

static int page_is_used(u64 i)
{
    return (bitmap[i / 8] >> (i % 8)) & 1;
}

static void page_mark_used(u64 i)
{
    if (page_is_used(i))
        return;
    bitmap[i / 8] |= (u8)(1U << (i % 8));
    used_pages++;
}

static void page_mark_free(u64 i)
{
    if (!page_is_used(i))
        return;
    bitmap[i / 8] &= (u8)~(1U << (i % 8));
    used_pages--;
}

/* Номер страницы по физическому адресу; вне области — (u64)-1 */
static u64 page_index(u64 addr)
{
    if (addr < base_addr)
        return (u64)-1;

    u64 i = (addr - base_addr) >> PAGE_SHIFT;

    return i < total_pages ? i : (u64)-1;
}

void pmm_reserve(u64 start, u64 size)
{
    u64 first = align_down(start, PAGE_SIZE);
    u64 last  = align_up(start + size, PAGE_SIZE);

    for (u64 a = first; a < last; a += PAGE_SIZE) {
        u64 i = page_index(a);

        if (i != (u64)-1)
            page_mark_used(i);
    }
}

int pmm_init(u64 ram_base, u64 ram_size, u64 dtb_phys)
{
    u64 image_end = align_up((u64)(uintptr_t)__image_end, PAGE_SIZE);

    base_addr   = ram_base;
    total_pages = ram_size / PAGE_SIZE;
    used_pages  = 0;

    /* Карта ложится сразу за образом ядра и занимает целое число страниц */
    bitmap       = (u8 *)(uintptr_t)image_end;
    bitmap_bytes = align_up((total_pages + 7) / 8, PAGE_SIZE);

    if (image_end + bitmap_bytes >= ram_base + ram_size) {
        kprintf("PMM      : НЕ ВЛЕЗАЕТ КАРТА ПАМЯТИ\n");
        return -1;
    }

    memset(bitmap, 0, bitmap_bytes);

    /* Всё от начала DRAM до конца карты — чужое или наше служебное.
     * Сюда попадает и то, что ниже ядра: там хозяйничают LK и ATF,
     * и выдать эту память под кучу означало бы затереть их данные. */
    pmm_reserve(ram_base, (image_end + bitmap_bytes) - ram_base);

    /* DTB нам ещё понадобится на этапе 5 — держим его нетронутым.
     * Реальный размер лежит в его заголовке, но пока хватит запаса. */
    if (dtb_phys)
        pmm_reserve(dtb_phys, 2 * 1024 * 1024);

    search_hint = 0;

    kprintf("PMM      : %lu МБ, СТРАНИЦ %lu, ЗАНЯТО %lu, КАРТА %p (%lu КБ)\n",
            ram_size / (1024 * 1024), total_pages, used_pages,
            (void *)bitmap, bitmap_bytes / 1024);
    return 0;
}

void *pmm_alloc_pages(u32 count)
{
    if (!count || !bitmap)
        return NULL;

    /* Ищем count свободных подряд. Начинаем с подсказки, но при неудаче
     * обязательно доходим до конца и заходим на второй круг — иначе после
     * череды освобождений аллокатор перестанет видеть освободившееся. */
    for (u64 pass = 0; pass < 2; pass++) {
        u64 i = pass ? 0 : search_hint;
        u64 limit = pass ? search_hint : total_pages;

        while (i < limit) {
            u64 run = 0;

            while (i + run < limit && run < count && !page_is_used(i + run))
                run++;

            if (run == count) {
                for (u64 k = 0; k < count; k++)
                    page_mark_used(i + k);

                search_hint = i + count;

                void *p = (void *)(uintptr_t)(base_addr + (i << PAGE_SHIFT));

                /* Обнуляем всегда: выданная страница не должна приносить
                 * следующему владельцу чужие данные. */
                memset(p, 0, count * PAGE_SIZE);
                return p;
            }

            /* Промах: страница i+run занята, продолжать проверку смысла нет */
            i += run + 1;
        }
    }

    return NULL;
}

void *pmm_alloc(void)
{
    return pmm_alloc_pages(1);
}

void pmm_free_pages(void *page, u32 count)
{
    u64 addr = (u64)(uintptr_t)page;
    u64 i = page_index(addr);

    if (!page || (addr & (PAGE_SIZE - 1)) || i == (u64)-1) {
        kprintf("PMM      : ПОПЫТКА ОСВОБОДИТЬ ЧУЖОЙ АДРЕС %p\n", page);
        return;
    }

    for (u64 k = 0; k < count && i + k < total_pages; k++)
        page_mark_free(i + k);

    if (i < search_hint)
        search_hint = i;
}

void pmm_free(void *page)
{
    pmm_free_pages(page, 1);
}

void pmm_stats(u64 *total, u64 *used)
{
    if (total)
        *total = total_pages;
    if (used)
        *used = used_pages;
}
