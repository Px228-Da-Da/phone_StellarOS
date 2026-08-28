/*
 * main.c — точка входа ядра на Си.
 *
 * Этап 1: доказать, что наш код реально исполняется на железе:
 * поднять вывод, показать состояние процессора и не упасть.
 */
#include "types.h"
#include "io.h"
#include "uart.h"
#include "print.h"
#include "fb.h"

#define OS_NAME     "VELO-OS"
#define OS_VERSION  "0.1"

/* Заголовок DTB: big-endian, магия 0xd00dfeed */
struct fdt_header {
    u32 magic;
    u32 totalsize;
    u32 off_dt_struct;
    u32 off_dt_strings;
    u32 off_mem_rsvmap;
    u32 version;
};

static u32 be32(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

/* Активное ожидание по системному счётчику: он тикает и без настроенного таймера */
static void delay_ms(u32 ms)
{
    u64 freq = read_cntfrq();
    u64 target = read_cntpct() + (freq / 1000) * ms;

    while (read_cntpct() < target)
        ;
}

static void banner(void)
{
    kprintf("\n");
    kprintf("================================\n");
    kprintf("  %s v%s\n", OS_NAME, OS_VERSION);
    kprintf("  AARCH64 BARE METAL KERNEL\n");
    kprintf("================================\n\n");
}

static void dump_cpu(void)
{
    u64 mpidr = read_mpidr();

    kprintf("EL       : %lu\n", read_currentel());
    kprintf("MPIDR    : %016lx (CORE %lu)\n", mpidr, mpidr & 0xFF);
    kprintf("CNTFRQ   : %lu HZ\n", read_cntfrq());
    kprintf("CNTPCT   : %lu\n", read_cntpct());
}

static void dump_dtb(u64 dtb_phys)
{
    struct fdt_header *h = (struct fdt_header *)(uintptr_t)dtb_phys;

    kprintf("DTB      : %p\n", (void *)(uintptr_t)dtb_phys);
    if (!dtb_phys) {
        kprintf("DTB      : NOT PASSED\n");
        return;
    }
    if (be32(h->magic) != 0xD00DFEEDu) {
        kprintf("DTB      : BAD MAGIC %08x\n", be32(h->magic));
        return;
    }
    kprintf("DTB SIZE : %u BYTES, VER %u\n", be32(h->totalsize), be32(h->version));
}

static void dump_fb(void)
{
    u64 base;
    u32 w, h, stride;

    if (!fb_available()) {
        kprintf("SCREEN   : NONE (UART ONLY)\n");
        return;
    }
    fb_info(&base, &w, &h, &stride);
    kprintf("SCREEN   : %ux%u STRIDE %u\n", w, h, stride);
    kprintf("FB ADDR  : %p\n", (void *)(uintptr_t)base);
}

/* Цветные полосы: видны, даже если шрифт вдруг рисуется неверно */
static void test_pattern(void)
{
    static const u32 colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW, COLOR_CYAN, COLOR_WHITE
    };
    u64 base; u32 w, h, stride;

    if (!fb_available())
        return;

    fb_info(&base, &w, &h, &stride);
    for (u32 i = 0; i < ARRAY_SIZE(colors); i++)
        fb_fill_rect(i * (w / ARRAY_SIZE(colors)), h - 120,
                     w / ARRAY_SIZE(colors), 120, colors[i]);
}

void kmain(u64 dtb_phys)
{
    uart_init();

    if (fb_init() == 0) {
        fb_set_colors(COLOR_GREEN, COLOR_BLACK);
        fb_clear(COLOR_BLACK);
    }

    banner();
    dump_cpu();
    dump_dtb(dtb_phys);
    dump_fb();
    test_pattern();

    kprintf("\nBOOT OK. HEARTBEAT:\n");

    /* Пульс: показывает, что ядро живо, а не замерло на красивой картинке */
    for (u32 tick = 0; ; tick++) {
        kprintf("TICK %u\n", tick);
        if (fb_available())
            fb_fill_rect(0, 0, 40, 40, (tick & 1) ? COLOR_GREEN : COLOR_BLACK);
        delay_ms(1000);
    }
}

/* --- Обработчики из vectors.S --- */

void exception_fatal(u64 type, u64 esr, u64 elr, u64 far)
{
    static const char *names[16] = {
        "SYNC SP0", "IRQ SP0", "FIQ SP0", "SERR SP0",
        "SYNC",     "IRQ",     "FIQ",     "SERROR",
        "SYNC L64", "IRQ L64", "FIQ L64", "SERR L64",
        "SYNC L32", "IRQ L32", "FIQ L32", "SERR L32",
    };

    fb_set_colors(COLOR_RED, COLOR_BLACK);
    kprintf("\n\n*** PANIC: %s ***\n", names[type & 15]);
    kprintf("ESR  = %016lx  (EC=%lu)\n", esr, (esr >> 26) & 0x3F);
    kprintf("ELR  = %016lx\n", elr);
    kprintf("FAR  = %016lx\n", far);
    kprintf("HALTED.\n");

    for (;;)
        wfi();
}

void irq_handler(void)
{
    /* Появится вместе с драйвером GIC (этап 3) */
}
