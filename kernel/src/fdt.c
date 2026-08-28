/*
 * fdt.c — чтение device tree.
 *
 * Дерево лежит в памяти одним куском: заголовок, блок memreserve,
 * поток тегов и куча строк. Поток тегов устроен так:
 *
 *   BEGIN_NODE "имя\0" ...выравнивание до 4...
 *       PROP  длина  смещение_имени  данные ...выравнивание...
 *       PROP  ...
 *       BEGIN_NODE "вложенный" ... END_NODE
 *   END_NODE
 *   END
 *
 * Всё в big-endian. Читаем побайтово: во-первых, так не зависим от
 * выравнивания (у нас -mstrict-align, и невыровненный доступ — исключение,
 * а не замедление), во-вторых, порядок байт виден прямо в коде.
 */
#include "fdt.h"
#include "string.h"
#include "print.h"

#define FDT_MAGIC           0xD00DFEEDu
#define FDT_VERSION_MIN     16

#define FDT_BEGIN_NODE      1
#define FDT_END_NODE        2
#define FDT_PROP            3
#define FDT_NOP             4
#define FDT_END             9

#define FDT_MAX_DEPTH       16

struct fdt_hdr {
    u32 magic;
    u32 totalsize;
    u32 off_dt_struct;
    u32 off_dt_strings;
    u32 off_mem_rsvmap;
    u32 version;
    u32 last_comp_version;
    u32 boot_cpuid_phys;
    u32 size_dt_strings;
    u32 size_dt_struct;
};

/* --- Чтение big-endian без требований к выравниванию --- */

static u32 be32p(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static u64 be64p(const u8 *p)
{
    return ((u64)be32p(p) << 32) | be32p(p + 4);
}

static u32 hdr_field(u64 dtb, u32 word_index)
{
    return be32p((const u8 *)(uintptr_t)dtb + word_index * 4);
}

static u64 align4(u64 v)
{
    return (v + 3) & ~3UL;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Имя узла без адресной части: "gic500@0c000000" -> сравниваем с "gic500" */
static int node_name_eq(const char *node, const char *want)
{
    while (*want) {
        if (*node != *want)
            return 0;
        node++;
        want++;
    }
    return *node == '\0' || *node == '@';
}

/* --- Обход дерева --- */

enum {
    FDT_EV_NODE,        /* вошли в узел      */
    FDT_EV_PROP,        /* свойство узла     */
    FDT_EV_END_NODE,    /* вышли из узла     */
    FDT_EV_DONE,
};

struct fdt_iter {
    const u8 *p;
    const u8 *end;
    const char *strings;
    u32 depth;
    const char *names[FDT_MAX_DEPTH];
};

struct fdt_event {
    int kind;
    const char *name;   /* имя узла или свойства */
    const u8 *data;     /* только для свойств    */
    u32 len;
    u32 depth;          /* корень = 0            */
};

static int fdt_iter_init(u64 dtb, struct fdt_iter *it)
{
    const u8 *base = (const u8 *)(uintptr_t)dtb;

    if (fdt_check(dtb) != 0)
        return -1;

    it->p       = base + hdr_field(dtb, 2);                 /* off_dt_struct  */
    it->end     = it->p + hdr_field(dtb, 9);                /* size_dt_struct */
    it->strings = (const char *)base + hdr_field(dtb, 3);   /* off_dt_strings */
    it->depth   = 0;
    return 0;
}

/*
 * Следующее событие. Возвращает 0, пока дерево не кончилось.
 *
 * Глубина считается так: корень — 0, его дети — 1. Событие входа в узел
 * сообщает уже новую глубину, событие выхода — ту же, что и вход.
 */
static int fdt_next(struct fdt_iter *it, struct fdt_event *ev)
{
    while (it->p + 4 <= it->end) {
        u32 tag = be32p(it->p);

        it->p += 4;

        switch (tag) {
        case FDT_NOP:
            continue;

        case FDT_BEGIN_NODE: {
            const char *name = (const char *)it->p;
            u64 skip = 0;

            while (it->p + skip < it->end && it->p[skip])
                skip++;

            it->p += align4(skip + 1);

            ev->kind  = FDT_EV_NODE;
            ev->name  = name;
            ev->data  = NULL;
            ev->len   = 0;
            ev->depth = it->depth;

            if (it->depth < FDT_MAX_DEPTH)
                it->names[it->depth] = name;
            it->depth++;
            return 0;
        }

        case FDT_END_NODE:
            if (it->depth)
                it->depth--;

            ev->kind  = FDT_EV_END_NODE;
            ev->name  = it->depth < FDT_MAX_DEPTH ? it->names[it->depth] : "";
            ev->data  = NULL;
            ev->len   = 0;
            ev->depth = it->depth;
            return 0;

        case FDT_PROP: {
            u32 len, nameoff;

            if (it->p + 8 > it->end)
                return -1;

            len     = be32p(it->p);
            nameoff = be32p(it->p + 4);
            it->p  += 8;

            ev->kind  = FDT_EV_PROP;
            ev->name  = it->strings + nameoff;
            ev->data  = it->p;
            ev->len   = len;
            ev->depth = it->depth - 1;      /* свойство принадлежит узлу */

            it->p += align4(len);
            return 0;
        }

        case FDT_END:
        default:
            ev->kind = FDT_EV_DONE;
            return -1;
        }
    }

    ev->kind = FDT_EV_DONE;
    return -1;
}

/* Прочитать адрес или размер шириной в cells ячеек по 32 бита */
static u64 read_cells(const u8 *p, u32 cells)
{
    u64 v = 0;

    for (u32 i = 0; i < cells; i++)
        v = (v << 32) | be32p(p + i * 4);

    return v;
}

/* Разобрать свойство reg в пары (адрес, размер) */
static u32 parse_reg(const u8 *data, u32 len, u32 ac, u32 sc,
                     struct fdt_region *out, u32 max, u32 count)
{
    u32 stride = (ac + sc) * 4;

    if (!stride)
        return count;

    for (u32 off = 0; off + stride <= len && count < max; off += stride) {
        out[count].base = read_cells(data + off, ac);
        out[count].size = read_cells(data + off + ac * 4, sc);
        count++;
    }

    return count;
}

/* --- Публичный интерфейс --- */

int fdt_check(u64 dtb_phys)
{
    u32 magic, version;

    if (!dtb_phys || (dtb_phys & 3))
        return -1;

    magic = hdr_field(dtb_phys, 0);
    if (magic != FDT_MAGIC)
        return -1;

    version = hdr_field(dtb_phys, 5);
    if (version < FDT_VERSION_MIN)
        return -1;

    return 0;
}

u64 fdt_total_size(u64 dtb_phys)
{
    return fdt_check(dtb_phys) == 0 ? hdr_field(dtb_phys, 1) : 0;
}

/*
 * Узлы /memory. Опознаём их по свойству device_type = "memory", а не по
 * имени: на merlin рядом лежит memory-ssmr-features, который памятью
 * не является, и разбор по префиксу имени принял бы его за неё.
 *
 * Свойства узла могут идти в любом порядке, поэтому reg запоминаем,
 * а решение принимаем на выходе из узла, когда известно и device_type.
 */
u32 fdt_memory(u64 dtb_phys, struct fdt_region *out, u32 max)
{
    struct fdt_iter it;
    struct fdt_event ev;
    u32 root_ac = 2, root_sc = 1;       /* значения по умолчанию из спецификации */
    u32 count = 0;
    int is_memory = 0;
    const u8 *reg = NULL;
    u32 reg_len = 0;

    if (fdt_iter_init(dtb_phys, &it) != 0)
        return 0;

    while (fdt_next(&it, &ev) == 0 && count < max) {
        if (ev.kind == FDT_EV_PROP && ev.depth == 0) {
            if (str_eq(ev.name, "#address-cells") && ev.len >= 4)
                root_ac = be32p(ev.data);
            else if (str_eq(ev.name, "#size-cells") && ev.len >= 4)
                root_sc = be32p(ev.data);
        }

        if (ev.kind == FDT_EV_NODE && ev.depth == 1) {
            is_memory = 0;
            reg = NULL;
            reg_len = 0;
        }

        if (ev.kind == FDT_EV_PROP && ev.depth == 1) {
            if (str_eq(ev.name, "device_type") && ev.len &&
                str_eq((const char *)ev.data, "memory"))
                is_memory = 1;
            else if (str_eq(ev.name, "reg")) {
                reg = ev.data;
                reg_len = ev.len;
            }
        }

        if (ev.kind == FDT_EV_END_NODE && ev.depth == 1 && is_memory && reg)
            count = parse_reg(reg, reg_len, root_ac, root_sc, out, max, count);
    }

    return count;
}

/*
 * Всё, что трогать нельзя: устаревший блок memreserve из заголовка плюс
 * узлы /reserved-memory. У последних свои #address-cells, поэтому берём
 * их у самого узла reserved-memory, а не у корня.
 *
 * Дети без reg пропускаем сознательно: у них вместо адреса стоит size с
 * alloc-ranges, то есть область выделяет операционная система по своему
 * усмотрению. Раз её не разместил загрузчик — она не занята.
 */
u32 fdt_reserved(u64 dtb_phys, struct fdt_region *out, u32 max)
{
    struct fdt_iter it;
    struct fdt_event ev;
    const u8 *base = (const u8 *)(uintptr_t)dtb_phys;
    u32 count = 0;
    u32 rm_ac = 2, rm_sc = 2;
    int in_rm = 0;

    if (fdt_check(dtb_phys) != 0)
        return 0;

    /* Блок memreserve: пары 64-битных чисел, конец — пара нулей */
    for (const u8 *p = base + hdr_field(dtb_phys, 4); count < max; p += 16) {
        u64 addr = be64p(p);
        u64 size = be64p(p + 8);

        if (!addr && !size)
            break;

        out[count].base = addr;
        out[count].size = size;
        count++;
    }

    if (fdt_iter_init(dtb_phys, &it) != 0)
        return count;

    while (fdt_next(&it, &ev) == 0 && count < max) {
        if (ev.kind == FDT_EV_NODE && ev.depth == 1)
            in_rm = node_name_eq(ev.name, "reserved-memory");

        if (ev.kind == FDT_EV_END_NODE && ev.depth == 1)
            in_rm = 0;

        if (!in_rm)
            continue;

        if (ev.kind == FDT_EV_PROP && ev.depth == 1) {
            if (str_eq(ev.name, "#address-cells") && ev.len >= 4)
                rm_ac = be32p(ev.data);
            else if (str_eq(ev.name, "#size-cells") && ev.len >= 4)
                rm_sc = be32p(ev.data);
        }

        /* Сами защищённые области — свойства reg у детей */
        if (ev.kind == FDT_EV_PROP && ev.depth == 2 && str_eq(ev.name, "reg"))
            count = parse_reg(ev.data, ev.len, rm_ac, rm_sc, out, max, count);
    }

    return count;
}

int fdt_node_reg(u64 dtb_phys, const char *node_name, u32 index,
                 struct fdt_region *out)
{
    struct fdt_iter it;
    struct fdt_event ev;
    u32 ac = 2, sc = 2;
    int found = 0;
    u32 node_depth = 0;

    if (fdt_iter_init(dtb_phys, &it) != 0)
        return -1;

    while (fdt_next(&it, &ev) == 0) {
        if (ev.kind == FDT_EV_PROP && ev.depth == 0) {
            if (str_eq(ev.name, "#address-cells") && ev.len >= 4)
                ac = be32p(ev.data);
            else if (str_eq(ev.name, "#size-cells") && ev.len >= 4)
                sc = be32p(ev.data);
        }

        if (ev.kind == FDT_EV_NODE && node_name_eq(ev.name, node_name)) {
            found = 1;
            node_depth = ev.depth;
            continue;
        }

        if (found && ev.kind == FDT_EV_END_NODE && ev.depth == node_depth)
            break;                      /* узел кончился, reg так и не встретился */

        if (found && ev.kind == FDT_EV_PROP && ev.depth == node_depth &&
            str_eq(ev.name, "reg")) {
            struct fdt_region regs[8];
            u32 n = parse_reg(ev.data, ev.len, ac, sc, regs, ARRAY_SIZE(regs), 0);

            if (index >= n)
                return -1;

            *out = regs[index];
            return 0;
        }
    }

    return -1;
}

/* Есть ли в списке compatible нужная строка. Список — строки через \0. */
static int compat_has(const u8 *data, u32 len, const char *want)
{
    u32 off = 0;

    while (off < len) {
        const char *s = (const char *)data + off;

        if (str_eq(s, want))
            return 1;

        while (off < len && data[off])
            off++;
        off++;                          /* пропускаем сам \0 */
    }

    return 0;
}

int fdt_compatible_reg(u64 dtb_phys, const char *compat, u32 index,
                       struct fdt_region *out)
{
    struct fdt_iter it;
    struct fdt_event ev;
    u32 ac = 2, sc = 2;
    u32 node_depth = 0;
    int matched = 0;
    const u8 *reg = NULL;
    u32 reg_len = 0;

    if (fdt_iter_init(dtb_phys, &it) != 0)
        return -1;

    while (fdt_next(&it, &ev) == 0) {
        if (ev.kind == FDT_EV_PROP && ev.depth == 0) {
            if (str_eq(ev.name, "#address-cells") && ev.len >= 4)
                ac = be32p(ev.data);
            else if (str_eq(ev.name, "#size-cells") && ev.len >= 4)
                sc = be32p(ev.data);
        }

        /*
         * Новый узел — начинаем накапливать заново: compatible и reg идут
         * в произвольном порядке, решение принимаем на выходе из узла.
         *
         * Но только если мы НЕ внутри уже найденного узла. У контроллера
         * прерываний бывают вложенные узлы (в QEMU это ITS), и вход в такой
         * ребёнок сбрасывал совпадение родителя — поиск возвращал «не нашёл»,
         * а ядро молча оставалось на зашитом адресе.
         */
        if (ev.kind == FDT_EV_NODE && (!matched || ev.depth <= node_depth)) {
            matched = 0;
            reg = NULL;
            reg_len = 0;
            node_depth = ev.depth;
        }

        if (ev.kind == FDT_EV_PROP && ev.depth == node_depth) {
            if (str_eq(ev.name, "compatible") && compat_has(ev.data, ev.len, compat))
                matched = 1;
            else if (str_eq(ev.name, "reg")) {
                reg = ev.data;
                reg_len = ev.len;
            }
        }

        /* Выход именно ИЗ НАШЕГО узла, а не из его ребёнка */
        if (ev.kind == FDT_EV_END_NODE && matched && ev.depth == node_depth) {
            struct fdt_region regs[8];
            u32 n;

            if (!reg)
                return -1;

            n = parse_reg(reg, reg_len, ac, sc, regs, ARRAY_SIZE(regs), 0);
            if (index >= n)
                return -1;

            *out = regs[index];
            return 0;
        }
    }

    return -1;
}

/* Адрес дерева, переданный загрузчиком. Держим его здесь, чтобы драйверам
 * не приходилось получать его через полдесятка параметров. */
static u64 root_dtb;

void fdt_set_root(u64 dtb_phys)
{
    root_dtb = fdt_check(dtb_phys) == 0 ? dtb_phys : 0;
}

u64 fdt_root(void)
{
    return root_dtb;
}

const char *fdt_root_string(u64 dtb_phys, const char *prop)
{
    struct fdt_iter it;
    struct fdt_event ev;

    if (fdt_iter_init(dtb_phys, &it) != 0)
        return NULL;

    while (fdt_next(&it, &ev) == 0) {
        if (ev.kind == FDT_EV_NODE && ev.depth == 1)
            break;                      /* корневые свойства кончились */

        if (ev.kind == FDT_EV_PROP && ev.depth == 0 && str_eq(ev.name, prop))
            return (const char *)ev.data;
    }

    return NULL;
}
