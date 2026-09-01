/*
 * fdt-test — прогон парсера device tree на обычном ПК.
 *
 * Зачем это нужно. Разбор дерева — единственный код ядра, чья ошибка
 * не даёт ни паники, ни чёрного экрана: он просто не заметит защищённую
 * область, а телефон перезагрузится позже и без объяснений. Проверять
 * такое прошивкой — самый дорогой способ.
 *
 * Здесь тот же самый kernel/src/fdt.c собирается обычным gcc и натравливается
 * на настоящий .dtb. Никакой копии парсера: если файл поменяется, поменяется
 * и проверка.
 *
 * Сборка и запуск:
 *     make -C tools/fdt-test
 *     tools/fdt-test/fdt-test дерево.dtb
 *
 * Для merlin ждём (см. device-info/dt-tree.txt):
 *     памяти около 4 ГБ одним куском с 0x40000000
 *     защищённых областей 21
 *     GIC v3: дистрибьютор 0x0c000000, редистрибьюторы рядом
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "types.h"
#include "fdt.h"

/* Ядро печатает через kprintf; на хосте подменяем его на printf.
 * Форматы у нас свои (%lu для u64), но на 64-битном Linux они совпадают. */
void kprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void kputs(const char *s)
{
    fputs(s, stdout);
}

static u64 load_dtb(const char *path, void **buf_out)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *buf;

    if (!f) {
        perror(path);
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "не прочитать %s\n", path);
        fclose(f);
        return 0;
    }
    fclose(f);

    *buf_out = buf;
    return (u64)(uintptr_t)buf;
}

static void print_mb(const char *label, u64 bytes)
{
    printf("%s%lu МБ (%lu байт)\n", label, bytes / (1024 * 1024), bytes);
}

int main(int argc, char **argv)
{
    void *buf = NULL;
    u64 dtb;
    struct fdt_region mem[8], res[64], gic;
    u32 nmem, nres;
    const char *model;
    u64 reserved_total = 0;

    if (argc < 2) {
        fprintf(stderr, "использование: %s дерево.dtb\n", argv[0]);
        return 2;
    }

    dtb = load_dtb(argv[1], &buf);
    if (!dtb)
        return 2;

    if (fdt_check(dtb) != 0) {
        printf("НЕ ДЕРЕВО: магия или версия не те\n");
        return 1;
    }

    printf("=== %s ===\n", argv[1]);
    printf("размер дерева : %lu байт\n", fdt_total_size(dtb));

    model = fdt_root_string(dtb, "model");
    printf("модель        : %s\n", model ? model : "(нет)");

    /* --- Память --- */
    nmem = fdt_memory(dtb, mem, ARRAY_SIZE(mem));
    printf("\nобластей памяти: %u\n", nmem);
    for (u32 i = 0; i < nmem; i++) {
        printf("  %2u: %016lx .. %016lx  ", i, mem[i].base, mem[i].base + mem[i].size);
        print_mb("", mem[i].size);
    }

    /* --- Защищённые области --- */
    nres = fdt_reserved(dtb, res, ARRAY_SIZE(res));
    printf("\nзащищённых областей: %u%s\n", nres,
           nres == ARRAY_SIZE(res) ? "  (УПЁРЛИСЬ В ПРЕДЕЛ, ЕСТЬ ЕЩЁ!)" : "");
    for (u32 i = 0; i < nres; i++) {
        printf("  %2u: %016lx  %8lu КБ\n", i, res[i].base, res[i].size / 1024);
        reserved_total += res[i].size;
    }
    print_mb("\nвсего защищено: ", reserved_total);

    /* --- Контроллер прерываний --- */
    printf("\n");
    if (fdt_compatible_reg(dtb, "arm,gic-v3", 0, &gic) == 0)
        printf("GICD          : %016lx (%lu КБ)\n", gic.base, gic.size / 1024);
    else
        printf("GICD          : НЕ НАЙДЕН — ядро возьмёт адрес из карты SoC\n");

    if (fdt_compatible_reg(dtb, "arm,gic-v3", 1, &gic) == 0)
        printf("GICR          : %016lx (%lu КБ)\n", gic.base, gic.size / 1024);
    else
        printf("GICR          : НЕ НАЙДЕН — ядро возьмёт адрес из карты SoC\n");

    /* --- Таймер: номер прерывания из дерева --- */
    {
        u32 intid;

        if (fdt_interrupt(dtb, "arm,armv8-timer", 2, &intid) == 0)
            printf("таймер (вирт): INTID %u  (ждём 27)\n", intid);
        else
            printf("таймер (вирт): не найден\n");
    }

    /*
     * Приговор. Ровно те условия, при которых ядру можно доверить память
     * этого устройства: память найдена, защищённые области найдены,
     * и в предел массива мы не упёрлись.
     */
    printf("\n");
    if (!nmem) {
        printf("ИТОГ: памяти в дереве нет — ядро останется на встроенной карте\n");
        return 1;
    }
    if (!nres) {
        printf("ИТОГ: защищённых областей нет. Для телефона это ЗАПРЕТ на прошивку\n");
        return 1;
    }
    if (nres == ARRAY_SIZE(res)) {
        printf("ИТОГ: областей больше, чем помещается — увеличить массив в main.c\n");
        return 1;
    }

    printf("ИТОГ: дерево разобрано полностью\n");
    free(buf);
    return 0;
}
