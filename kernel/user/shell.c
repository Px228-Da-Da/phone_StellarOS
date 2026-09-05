/*
 * Оболочка VELO-OS — теперь программа, а не часть ядра.
 *
 * Раньше плитки, подсветку и разбор касаний делало ядро: интерфейс жил
 * в EL1 с полными правами, мог испортить что угодно и падал бы вместе со
 * всем остальным. Теперь это обычная программа в EL0. Ей доступны
 * ровно три вещи: буфер своего окна, шрифт ядра и очередь касаний.
 * Ни фреймбуфера, ни регистров контроллера дисплея она не видит.
 *
 * Что от этого меняется по существу: ошибка в интерфейсе перестала быть
 * ошибкой в ядре. Если оболочка залезет не туда, её снимут, а система
 * продолжит работать — как это уже делает нарушитель.
 *
 * Рисуем прямо в буфер окна, а показывает его контроллер дисплея сам,
 * отдельным слоем. Просить ядро о показе нужно один раз: дальше он
 * читает ту же память постоянно, и перерисовка плитки видна без единого
 * системного вызова.
 */
#include "ulib.h"

#define COLS        2
#define TILE_COUNT  6
#define MARGIN      24
#define GAP         16
#define TITLE_H     150
#define TILE_H      170
#define STATUS_H    60

#define COL_BG      0xFF0B1020      /* фон                       */
#define COL_TILE    0xFF16203A      /* плитка                    */
#define COL_TILE_HL 0xFF2E63C8      /* нажатая плитка            */
#define COL_EDGE    0xFF2A3A5E      /* рамка плитки              */
#define COL_TEXT    0xFF7FD4FF      /* подписи                   */
#define COL_DIM     0xFF5A6A88      /* второстепенное            */
#define COL_WHITE   0xFFFFFFFF

static u32 *win;
static u32 sw, sh;                  /* размеры экрана и окна     */
static u32 tile_w;
/* Без начальных значений: у программ для EL0 их не бывает вовсе — образ
 * содержит только код, а изменяемые данные ядро выдаёт чистыми. Поэтому
 * «ничего не нажато» назначается при запуске, а не при сборке. */
static int pressed;                 /* какая плитка нажата       */
static int chosen;                  /* какая выбрана             */
static u64 touches;
static char line[96];

static const char *const tile_name[TILE_COUNT] = {
    "ЭКРАН", "КАСАНИЯ", "ПАМЯТЬ", "ЯДРА", "ОКНА", "О СИСТЕМЕ",
};

static const char *const tile_note[TILE_COUNT] = {
    "СЛОИ ОВЕРЛЕЯ, КАДР ЧИТАЕТ КОНТРОЛЛЕР",
    "NOVATEK NT36672A, ОЧЕРЕДЬ У КАЖДОЙ ПРОГРАММЫ",
    "СВОИ ТАБЛИЦЫ У КАЖДОЙ ПРОГРАММЫ",
    "ВОСЕМЬ ЯДЕР, ОЧЕРЕДЬ ЗАДАЧ ОДНА",
    "БУФЕР В СВОЕЙ ПАМЯТИ, ПОКАЗ СЛОЕМ",
    "ОБОЛОЧКА РАБОТАЕТ В EL0, КАК ОБЫЧНАЯ ПРОГРАММА",
};

/* Где лежит плитка: по номеру — прямоугольник */
static void tile_rect(u32 i, u32 *x, u32 *y)
{
    *x = MARGIN + (i % COLS) * (tile_w + GAP);
    *y = TITLE_H + (i / COLS) * (TILE_H + GAP);
}

/* Какая плитка под точкой; -1 — мимо всех */
static int tile_at(u32 px, u32 py)
{
    for (u32 i = 0; i < TILE_COUNT; i++) {
        u32 x, y;

        tile_rect(i, &x, &y);
        if (px >= x && px < x + tile_w && py >= y && py < y + TILE_H)
            return (int)i;
    }

    return -1;
}

static void draw_tile(u32 i)
{
    u32 x, y;
    u32 body = ((int)i == pressed) ? COL_TILE_HL : COL_TILE;

    tile_rect(i, &x, &y);
    urect(win, sw, x, y, tile_w, TILE_H, body);
    uframe(win, sw, x, y, tile_w, TILE_H, 2, COL_EDGE);
    text(x + 20, y + 22, 3, ((int)i == chosen) ? COL_WHITE : COL_TEXT,
         tile_name[i]);
}

/* Строка внизу: что выбрано и сколько было касаний */
static void draw_status(void)
{
    u32 y = TITLE_H + 3 * (TILE_H + GAP) + 20;

    urect(win, sw, MARGIN, y, sw - 2 * MARGIN, STATUS_H * 2, COL_BG);

    if (chosen >= 0)
        text(MARGIN, y, 2, COL_TEXT, tile_note[chosen]);
    else
        text(MARGIN, y, 2, COL_DIM, "НАЖМИ НА ПЛИТКУ");

    ulabel(line, "КАСАНИЙ ", touches);
    text(MARGIN, y + 34, 2, COL_DIM, line);

    ulabel(line, "СЕКУНД ", uptime_ms() / 1000);
    text(MARGIN + 260, y + 34, 2, COL_DIM, line);
}

static void draw_all(void)
{
    ufill(win, sw * sh, COL_BG);

    text(MARGIN, 28, 6, COL_WHITE, "VELO-OS");
    text(MARGIN, 92, 2, COL_DIM, "ОБОЛОЧКА В ПОЛЬЗОВАТЕЛЬСКОМ РЕЖИМЕ");

    /*
     * Образцы известных цветов.
     *
     * На телефоне окно выглядело синим и просвечивающим, а по числам в
     * буфере этого не видно: там ровно те цвета, что мы написали.
     * Значит расходится не значение, а его толкование железом — какой
     * байт считается красным, какой прозрачностью. Четыре образца с
     * подписями отвечают на это одной фотографией.
     *
     * Порядок подписей и есть проверка: если «КРАСНЫЙ» окажется синим,
     * байты разложены не так, как мы думаем.
     */
    {
        u32 y = 120;
        u32 w = (sw - 2 * MARGIN) / 4;

        urect(win, sw, MARGIN + 0 * w, y, w - 4, 26, 0xFFFF0000);
        urect(win, sw, MARGIN + 1 * w, y, w - 4, 26, 0xFF00FF00);
        urect(win, sw, MARGIN + 2 * w, y, w - 4, 26, 0xFF0000FF);
        urect(win, sw, MARGIN + 3 * w, y, w - 4, 26, 0xFFFFFFFF);

        text(MARGIN + 0 * w, y + 30, 1, COL_WHITE, "КРАСНЫЙ");
        text(MARGIN + 1 * w, y + 30, 1, COL_WHITE, "ЗЕЛЁНЫЙ");
        text(MARGIN + 2 * w, y + 30, 1, COL_WHITE, "СИНИЙ");
        text(MARGIN + 3 * w, y + 30, 1, COL_WHITE, "БЕЛЫЙ");
    }

    for (u32 i = 0; i < TILE_COUNT; i++)
        draw_tile(i);
    draw_status();
}

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    struct touch t;

    pressed = -1;
    chosen = -1;

    screen_size(&sw, &sh);
    if (!sw || !sh) {
        write("EL0      : ОБОЛОЧКА: ЭКРАНА НЕТ\n");
        exit(1);
    }

    win = window(sw, sh);
    if (!win) {
        write("EL0      : ОБОЛОЧКА: ОКНА НЕ ДАЛИ\n");
        exit(2);
    }

    tile_w = (sw - 2 * MARGIN - (COLS - 1) * GAP) / COLS;

    /*
     * Первый кадр меряем.
     *
     * Буфер окна некэшируемый — иначе контроллер дисплея читал бы
     * старое, — а это значит, что каждая запись идёт прямо в память.
     * Десять мегабайт таких записей могут стоить заметного времени, и
     * знать эту цену лучше числом, чем на глаз.
     */
    {
        u64 t0 = uptime_ms();

        draw_all();
        ulabel(line, "EL0      : ОБОЛОЧКА: ПЕРВЫЙ КАДР, МС ", uptime_ms() - t0);
        line[ustrlen(line)] = 10;
        line[ustrlen(line)] = 0;
        write(line);
    }

    /*
     * Просим показать один раз. Дальше контроллер дисплея читает этот
     * буфер сам, и перерисовка плитки видна без обращения к ядру.
     */
    if (present(0, 0) != 0) {
        write("EL0      : ОБОЛОЧКА: ПОКАЗАТЬ ОКНО НЕ ВЫШЛО\n");
        exit(3);
    }

    write("EL0      : ОБОЛОЧКА В EL0 ЗАПУЩЕНА\n");

    for (;;) {
        u64 last_sec = uptime_ms() / 1000;

        if (input(&t) != 1)
            continue;

        if (t.action == TOUCH_DOWN) {
            touches++;
            pressed = tile_at(t.x, t.y);
            if (pressed >= 0)
                draw_tile((u32)pressed);
        } else if (t.action == TOUCH_MOVE) {
            /*
             * Увели палец с плитки — подсветку снимаем, но нажатие не
             * забываем: палец могут вернуть, и тогда нажатие
             * засчитывается. Так ведёт себя всякий приличный интерфейс.
             */
            int now = tile_at(t.x, t.y);

            if (pressed >= 0 && now != pressed) {
                int was = pressed;

                pressed = -1;
                draw_tile((u32)was);
                pressed = was;      /* помним, но не показываем */
            }
        } else {                    /* отпустили */
            int was = pressed;

            if (was >= 0 && tile_at(t.x, t.y) == was)
                chosen = was;
            pressed = -1;
            if (was >= 0)
                draw_tile((u32)was);
            if (chosen >= 0)
                draw_tile((u32)chosen);
        }

        draw_status();
        (void)last_sec;
    }
}
