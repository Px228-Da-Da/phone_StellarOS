/*
 * Следы, переживающие перезагрузку. Зачем — написано в trace.h.
 */
#include "trace.h"
#include "print.h"
#include "usb.h"

/*
 * Область лежит в разделе .persist, который загрузчик НЕ обнуляет: в
 * boot.S чистится только .bss, а этот раздел стоит за ним. Поэтому
 * содержимое переживает тёплую перезагрузку — ровно то, ради чего всё и
 * затевалось.
 */
#define TRACE_MAGIC 0x5354524CU     /* "STRL" */

__attribute__((section(".persist"))) struct trace_slot trace_slots[8];
__attribute__((section(".persist"))) static u32 trace_magic;
__attribute__((section(".persist"))) static u32 trace_boots;

static const char *const place[] = {
    "ничего", "спит", "таймер", "вызов из программы", "программа",
    "питание", "провод", "ждёт кадр", "обои", "текст", "полосы",
    "касания", "смена задач",
};

/*
 * Снимок прошлой жизни делается РАНО, а печатается ПОЗДНО.
 *
 * Разделено намеренно. Читать отметки надо до того, как их начнут
 * перезаписывать живые ядра, то есть в самом начале загрузки, — а там
 * ещё нет ни консоли, ни провода, и всё напечатанное уходит в никуда.
 * Первый вариант этим и кончился: отчёт исправно составлялся и
 * исправно пропадал.
 *
 * Поэтому сначала копируем в обычную память, потом печатаем.
 */
static struct trace_slot saved[8];
static u32 saved_boots;
static int saved_valid;

void trace_snapshot(void)
{
    saved_valid = (trace_magic == TRACE_MAGIC);
    saved_boots = trace_boots;
    for (u32 i = 0; i < 8; i++)
        saved[i] = trace_slots[i];
}

void trace_report(void)
{
    if (!saved_valid) {
        kprintf("СЛЕДЫ    : ПРОШЛОЙ ЖИЗНИ НЕТ (ХОЛОДНЫЙ ПУСК)\n");
        return;
    }

    kprintf("СЛЕДЫ    : ЭТО ЗАГРУЗКА %u ПОДРЯД. ГДЕ СТОЯЛИ ЯДРА:\n",
            saved_boots + 1);
    for (u32 i = 0; i < 8; i++) {
        u32 code = (u32)(saved[i].what >> 32);
        u32 detail = (u32)saved[i].what;

        /*
         * Тики — самое важное число здесь, и добавлены они не сразу.
         *
         * По отметкам видно только «где стояло», а по тикам — «когда
         * замолчало». Если у всех ядер тиков поровну, прерывания
         * пропали для всех разом и виновато что-то общее. Если у одного
         * заметно меньше — оно замолчало первым, и начинать надо с него.
         */
        kprintf("  ЯДРО %u  : %s (%u), ПОДР %u, ОТМЕТОК %lu, ТИКОВ %lu\n",
                i,
                code < sizeof(place) / sizeof(place[0]) ? place[code] : "?",
                code, detail, saved[i].seq, saved[i].ticks);
    }
    usb_flush();
}

void trace_begin(void)
{
    if (trace_magic == TRACE_MAGIC)
        trace_boots++;
    else
        trace_boots = 0;

    trace_magic = TRACE_MAGIC;
    for (u32 i = 0; i < 8; i++) {
        trace_slots[i].what = 0;
        trace_slots[i].seq = 0;
    }
}
