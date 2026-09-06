/*
 * kmalloc.c — куча ядра.
 *
 * Каждый блок предваряется заголовком:
 *
 *   [ hdr ][ ....... полезные данные ....... ][ hdr ][ ... ]
 *
 * Все блоки — и занятые, и свободные — висят в одном списке, отсортированном
 * по адресу. Выделение: первый подходящий свободный (first fit), при нужде
 * блок делится надвое. Освобождение: пометить свободным и склеить с соседями.
 *
 * First fit, а не best fit, сознательно: best fit требует полного прохода
 * по списку ради экономии, которая на наших размерах тонет в накладных
 * расходах самого заголовка. Проще — надёжнее.
 */
#include "kmalloc.h"
#include "pmm.h"
#include "string.h"
#include "print.h"
#include "spinlock.h"

/* Один замок на всю кучу: список блоков общий, и любое изменение —
 * дробление, склейка, вставка нового куска — рвёт связи, которые в этот
 * же момент читает другое ядро. */
static struct spinlock heap_lock = SPINLOCK_INIT("heap");

#define HEAP_MAGIC      0x4F4C4556U         /* "STELLAR" — метка целостности */
#define HEAP_ALIGN      16UL                /* требование AArch64 ABI     */
#define HEAP_MIN_SPLIT  32UL                /* мельче дробить нет смысла  */

struct block {
    u64 size;                   /* размер полезной части, байт        */
    struct block *next;         /* следующий по адресу, NULL в конце  */
    u32 free;
    u32 magic;
    u64 pad;                    /* добивка до 32 байт, см. проверку ниже */
};

/*
 * Заголовок обязан быть кратен 16 байтам. Блоки нарезаются встык, поэтому
 * невыровненный заголовок сдвинет данные ВСЕХ последующих блоков, а на
 * AArch64 невыровненный доступ к u64 — это не «медленнее», это исключение.
 * Поля занимают 24 байта, остальное добирает pad.
 */
_Static_assert(sizeof(struct block) % HEAP_ALIGN == 0, "heap header must be 16-byte aligned");

static struct block *head;
static u64 heap_total;          /* сколько байт всего взято у pmm */

static u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

static void *block_data(struct block *b)
{
    return (void *)((u8 *)b + sizeof(struct block));
}

/* Конец блока — адрес сразу за его данными */
static u8 *block_end(struct block *b)
{
    return (u8 *)block_data(b) + b->size;
}

/* Вставить блок в список, сохранив порядок по адресу */
static void block_insert(struct block *nb)
{
    struct block **link = &head;

    while (*link && *link < nb)
        link = &(*link)->next;

    nb->next = *link;
    *link = nb;
}

/*
 * Попросить у pmm ещё памяти. Берём с запасом в целых страницах:
 * обращение к страничному аллокатору дороже, чем неиспользованный хвост,
 * а хвост всё равно останется в куче свободным блоком.
 */
static struct block *heap_grow(u64 need)
{
    u64 bytes = align_up(need + sizeof(struct block), PAGE_SIZE);
    u32 pages = (u32)(bytes / PAGE_SIZE);
    struct block *nb = (struct block *)pmm_alloc_pages(pages);

    if (!nb)
        return NULL;

    nb->size  = bytes - sizeof(struct block);
    nb->free  = 1;
    nb->magic = HEAP_MAGIC;
    block_insert(nb);

    heap_total += bytes;
    return nb;
}

/* Отделить от блока хвост, если после выделения остаётся заметный кусок */
static void block_split(struct block *b, u64 need)
{
    u64 rest;

    if (b->size < need + sizeof(struct block) + HEAP_MIN_SPLIT)
        return;

    struct block *tail = (struct block *)((u8 *)block_data(b) + need);

    rest = b->size - need - sizeof(struct block);

    tail->size  = rest;
    tail->free  = 1;
    tail->magic = HEAP_MAGIC;
    tail->next  = b->next;

    b->size = need;
    b->next = tail;
}

void *kmalloc(size_t size)
{
    u64 need, flags;
    struct block *b;
    void *result = NULL;

    if (!size)
        return NULL;

    need = align_up(size, HEAP_ALIGN);
    flags = spin_lock_irq(&heap_lock);

    for (b = head; b; b = b->next) {
        if (b->free && b->size >= need) {
            block_split(b, need);
            b->free = 0;
            result = block_data(b);
            break;
        }
    }

    if (!result) {
        b = heap_grow(need);
        if (b) {
            block_split(b, need);
            b->free = 0;
            result = block_data(b);
        }
    }

    spin_unlock_irq(&heap_lock, flags);
    return result;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);

    return p;
}

/*
 * Склейка. Идём по всему списку и объединяем каждую пару соседей,
 * которые оба свободны И лежат вплотную. Проверка на «вплотную»
 * обязательна: блоки из разных запросов к pmm могут оказаться рядом
 * в списке, но в памяти между ними будет чужое.
 */
static void heap_coalesce(void)
{
    for (struct block *b = head; b && b->next; ) {
        struct block *n = b->next;

        if (b->free && n->free && block_end(b) == (u8 *)n) {
            b->size += sizeof(struct block) + n->size;
            b->next = n->next;
            continue;               /* пробуем приклеить и следующий */
        }
        b = n;
    }
}

void kfree(void *ptr)
{
    struct block *b;
    u64 flags;

    if (!ptr)
        return;

    b = (struct block *)((u8 *)ptr - sizeof(struct block));
    flags = spin_lock_irq(&heap_lock);

    /* Метка ловит две классические ошибки: освобождение чужого указателя
     * и повреждение заголовка записью за границу соседнего блока.
     * Молча продолжать тут нельзя — дальше поедет весь список. */
    if (b->magic != HEAP_MAGIC) {
        spin_unlock_irq(&heap_lock, flags);
        kprintf("KMALLOC  : ИСПОРЧЕН ЗАГОЛОВОК БЛОКА %p\n", ptr);
        return;
    }
    if (b->free) {
        spin_unlock_irq(&heap_lock, flags);
        kprintf("KMALLOC  : ПОВТОРНОЕ ОСВОБОЖДЕНИЕ %p\n", ptr);
        return;
    }

    b->free = 1;
    heap_coalesce();
    spin_unlock_irq(&heap_lock, flags);
}

void heap_stats(u64 *total, u64 *used, u64 *blocks)
{
    u64 u = 0, n = 0;
    u64 flags = spin_lock_irq(&heap_lock);

    for (struct block *b = head; b; b = b->next) {
        n++;
        if (!b->free)
            u += b->size;
    }

    if (total)
        *total = heap_total;
    if (used)
        *used = u;
    if (blocks)
        *blocks = n;

    spin_unlock_irq(&heap_lock, flags);
}
