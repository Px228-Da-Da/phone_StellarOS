/*
 * ramfb.c — договориться с QEMU об экране через fw_cfg.
 *
 * fw_cfg — это «окно выдачи» QEMU: гость выбирает файл по номеру и читает
 * или пишет его содержимое. Файлы бывают разные (таблицы ACPI, параметры
 * загрузки), нам нужен один — "etc/ramfb". Записав в него описание нашего
 * буфера, мы получаем работающий экран.
 *
 * Порядок такой:
 *   1. прочитать каталог файлов (файл с номером 0x19) и найти в нём ramfb
 *   2. записать в найденный файл структуру: адрес, формат, размеры, шаг
 *
 * Обмен идёт через DMA: в регистр кладётся физический адрес управляющей
 * структуры, QEMU её читает и выполняет. Все числа в fw_cfg — big-endian,
 * причём и в регистрах, и в самих структурах, поэтому каждое проходит
 * через swap. Это не прихоть протокола: fw_cfg родом из мира, где порядок
 * байт фиксирован сетевым, а не процессорным.
 */
#include "ramfb.h"

#if !defined(BOARD_MERLIN)

#include "io.h"
#include "print.h"
#include "string.h"
#include "soc/qemu_virt.h"

/* Регистры fw_cfg на машине virt */
#define FWCFG_SELECTOR      0x08        /* 16 бит, big-endian            */
#define FWCFG_DMA           0x10        /* 64 бита, big-endian           */

#define FWCFG_FILE_DIR      0x19        /* номер файла с каталогом файлов */

/* Биты поля control в управляющей структуре DMA */
#define DMA_ERROR           0x01
#define DMA_READ            0x02
#define DMA_SKIP            0x04
#define DMA_SELECT          0x08
#define DMA_WRITE           0x10

#define FOURCC_XRGB8888     0x34325258U /* 'XR24' — 32 бита, порядок BGRX */

static u32 swap32(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static u64 swap64(u64 v)
{
    return ((u64)swap32((u32)v) << 32) | swap32((u32)(v >> 32));
}

/* Управляющая структура DMA. Выравнена, потому что QEMU читает её целиком,
 * а невыровненный доступ на AArch64 к тому же запрещён нашими флагами. */
struct dma_cmd {
    u32 control;
    u32 length;
    u64 address;
} __attribute__((aligned(8)));

/* Запись каталога файлов fw_cfg */
struct fw_file {
    u32 size;
    u16 select;
    u16 reserved;
    char name[56];
};

/* То, что ждёт от нас ramfb: всё big-endian */
struct ramfb_cfg {
    u64 addr;
    u32 fourcc;
    u32 flags;
    u32 width;
    u32 height;
    u32 stride;
} __attribute__((packed));

static struct dma_cmd cmd;

/*
 * Выполнить одну операцию DMA и дождаться её конца.
 *
 * QEMU обнуляет поле control, когда закончил, поэтому ждём именно этого.
 * Ожидание ограничено: если устройства ramfb в командной строке нет,
 * регистр может не ответить вовсе, и вечный цикл здесь означал бы
 * зависание ядра на ровном месте.
 */
static int fwcfg_dma(u32 control, void *buf, u32 len)
{
    cmd.control = swap32(control);
    cmd.length  = swap32(len);
    cmd.address = swap64((u64)(uintptr_t)buf);
    dsb();

    mmio_write64(QEMU_FWCFG_BASE + FWCFG_DMA, swap64((u64)(uintptr_t)&cmd));
    dsb();

    for (u32 spin = 0; spin < 1000000; spin++) {
        u32 ctl = swap32(cmd.control);

        if (ctl & DMA_ERROR)
            return -1;
        if (ctl == 0)
            return 0;               /* QEMU отчитался: сделано */
    }

    return -1;
}

/* Найти в каталоге файл по имени и вернуть его номер */
static int fwcfg_find(const char *name, u16 *select_out)
{
    struct fw_file entry;
    u32 count;

    /* Выбрать каталог и прочитать из него число записей */
    if (fwcfg_dma((FWCFG_FILE_DIR << 16) | DMA_SELECT | DMA_READ,
                  &count, sizeof(count)) != 0)
        return -1;

    count = swap32(count);
    if (!count || count > 1024)
        return -1;

    /* Дальше записи читаются подряд, выбирать заново не нужно */
    for (u32 i = 0; i < count; i++) {
        if (fwcfg_dma(DMA_READ, &entry, sizeof(entry)) != 0)
            return -1;

        int same = 1;
        for (u32 k = 0; k < sizeof(entry.name); k++) {
            if (entry.name[k] != name[k]) {
                same = 0;
                break;
            }
            if (!name[k])
                break;
        }

        if (same) {
            *select_out = (u16)((entry.select >> 8) | (entry.select << 8));
            return 0;
        }
    }

    return -1;
}

int ramfb_setup(u64 addr, u32 width, u32 height, u32 stride_bytes)
{
    struct ramfb_cfg cfg;
    u16 select;

    if (fwcfg_find("etc/ramfb", &select) != 0)
        return -1;                  /* QEMU запущен без -device ramfb */

    cfg.addr   = swap64(addr);
    cfg.fourcc = swap32(FOURCC_XRGB8888);
    cfg.flags  = 0;
    cfg.width  = swap32(width);
    cfg.height = swap32(height);
    cfg.stride = swap32(stride_bytes);

    if (fwcfg_dma(((u32)select << 16) | DMA_SELECT | DMA_WRITE,
                  &cfg, sizeof(cfg)) != 0)
        return -1;

    return 0;
}

#endif /* !BOARD_MERLIN */
