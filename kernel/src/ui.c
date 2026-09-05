/*
 * Оболочка: первый экран.
 *
 * Задача одна и живёт всё время работы ядра. Кадр она рисует не каждый
 * тик, а только когда что-то изменилось: фон и плитки ложатся один раз,
 * дальше меняются подпись выбранной плитки и строка состояния.
 *
 * Подсветка нажатия и точка под пальцем сделаны слоями оверлея, а не
 * рисованием. Разница принципиальная: слой перемещается записью одного
 * числа в регистр, и картинка под ним не портится — её не нужно потом
 * восстанавливать. Рисованием пришлось бы каждый раз затирать прошлое
 * место, а значит помнить, что там было.
 */
#include "ui.h"
#include "fb.h"
#include "ovl.h"
#include "input.h"
#include "sched.h"
#include "timer.h"
#include "pmm.h"
#include "mmu.h"
#include "gic.h"
#include "smp.h"
#include "print.h"
#include "io.h"

/* --- Цвета оболочки ---------------------------------------------
 * Тёмная тема не из моды, а по делу: экран OLED-подобной яркости в
 * тёмной комнате, и белый фон на весь дисплей слепит. */
#define UI_BG           0xFF0B0E14      /* почти чёрный с синевой   */
#define UI_TILE         0xFF161B26      /* плитка                    */
#define UI_TILE_EDGE    0xFF2A3345      /* её обводка                */
#define UI_TEXT         0xFFE6EAF2      /* основной текст            */
#define UI_DIM          0xFF7F8CA6      /* второстепенный            */
#define UI_ACCENT       0xFF4FC3F7      /* акцент: заголовок, цифры  */
#define UI_TRANSPARENT  0x00000000      /* фон текста не закрашивать */

/* --- Разметка ---------------------------------------------------- */
#define MARGIN      40
#define GAP         24
#define TILE_H      210
#define TILES_Y     560
#define COLS        2
#define ROWS        3
#define TILE_COUNT  (COLS * ROWS)

struct tile {
    const char *title;
    const char *detail;
};

/*
 * Плитки рассказывают о самой машине. Это честнее любых заглушек: всё,
 * что здесь написано, ядро действительно знает и проверило само.
 */
static const struct tile tiles[TILE_COUNT] = {
    { "ЭКРАН",   "1080x2340, 60 ГЦ, ЧЕТЫРЕ АППАРАТНЫХ СЛОЯ" },
    { "КАСАНИЯ", "NOVATEK NT36672A, ДО ДЕСЯТИ ПАЛЬЦЕВ" },
    { "ПАМЯТЬ",  "4096 МБ, СТРАНИЧНЫЙ УЧЁТ И КУЧА" },
    { "ЯДРА",    "CORTEX-A75 И A55, ВЫТЕСНЯЮЩИЙ ПЛАНИРОВЩИК" },
    { "ШИНЫ",    "SPI, I2C, PWRAP К КОНТРОЛЛЕРУ ПИТАНИЯ" },
    { "СВЯЗЬ",   "USB: КОНСОЛЬ ЯДРА НА КОМПЬЮТЕРЕ" },
};

static u32 scr_w, scr_h;
static u32 tile_w;
static int selected = -1;           /* какая плитка выбрана */
static int pressed  = -1;           /* какая нажата прямо сейчас */

/* --- Слои: подсветка и точка под пальцем -------------------------- */
#define DOT_SIZE    72
static u32 *layer_hl;               /* подсветка плитки */
static u32 *layer_dot;              /* точка под пальцем */
static int  layers_ready;

/* --- Мелочи ------------------------------------------------------- */

/* Число в строку. Своей библиотеки у нас нет, а kprintf пишет в консоль,
 * не в кадр. */
static void utoa(u64 v, char *buf)
{
    char tmp[24];
    int n = 0;

    if (!v) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

static void str_cat(char *dst, u32 cap, const char *src)
{
    u32 n = 0;

    while (dst[n] && n < cap - 1)
        n++;
    while (*src && n < cap - 1)
        dst[n++] = *src++;
    dst[n] = 0;
}

static void tile_rect(int i, u32 *x, u32 *y)
{
    *x = MARGIN + (u32)(i % COLS) * (tile_w + GAP);
    *y = TILES_Y + (u32)(i / COLS) * (TILE_H + GAP);
}

/* Какая плитка под точкой; -1 если мимо */
static int tile_at(u32 px, u32 py)
{
    for (int i = 0; i < TILE_COUNT; i++) {
        u32 x, y;

        tile_rect(i, &x, &y);
        if (px >= x && px < x + tile_w && py >= y && py < y + TILE_H)
            return i;
    }
    return -1;
}

/* --- Рисование ---------------------------------------------------- */

static void draw_frame_rect(u32 x, u32 y, u32 w, u32 h, u32 fill, u32 edge)
{
    fb_fill_rect(x, y, w, h, fill);
    fb_fill_rect(x, y, w, 2, edge);
    fb_fill_rect(x, y + h - 2, w, 2, edge);
    fb_fill_rect(x, y, 2, h, edge);
    fb_fill_rect(x + w - 2, y, 2, h, edge);
}

static void draw_tile(int i)
{
    u32 x, y;

    tile_rect(i, &x, &y);
    draw_frame_rect(x, y, tile_w, TILE_H, UI_TILE, UI_TILE_EDGE);
    fb_text(x + 28, y + 36, 4, UI_ACCENT, UI_TRANSPARENT, tiles[i].title);
}

static void draw_header(void)
{
    const char *title = "VELO-OS";
    const char *sub   = "СВОЯ ОС НА ГОЛОМ ЖЕЛЕЗЕ";

    fb_text(MARGIN, 150, 9, UI_TEXT, UI_TRANSPARENT, title);
    fb_text(MARGIN, 280, 3, UI_DIM, UI_TRANSPARENT, sub);
    fb_fill_rect(MARGIN, 360, scr_w - 2 * MARGIN, 2, UI_TILE_EDGE);
}

/* Полоса под плитками: подробность о выбранной плитке */
#define DETAIL_Y    (TILES_Y + ROWS * (TILE_H + GAP) + 30)
#define DETAIL_H    170

static void draw_detail(void)
{
    u32 w = scr_w - 2 * MARGIN;

    draw_frame_rect(MARGIN, DETAIL_Y, w, DETAIL_H, UI_BG, UI_TILE_EDGE);

    if (selected < 0) {
        fb_text(MARGIN + 24, DETAIL_Y + 60, 3, UI_DIM, UI_TRANSPARENT,
                "НАЖМИ НА ПЛИТКУ");
        return;
    }
    fb_text(MARGIN + 24, DETAIL_Y + 34, 3, UI_ACCENT, UI_TRANSPARENT,
            tiles[selected].title);
    fb_text(MARGIN + 24, DETAIL_Y + 90, 2, UI_TEXT, UI_TRANSPARENT,
            tiles[selected].detail);
}

/* Строка состояния внизу: живые числа, обновляется раз в секунду */
static void draw_status(u32 touches)
{
    char line[96];
    char num[24];
    u32 y = scr_h - 120;

    fb_fill_rect(MARGIN, y, scr_w - 2 * MARGIN, 40, UI_BG);

    line[0] = 0;
    str_cat(line, sizeof(line), "ЯДЕР ");
    utoa(cpu_online_count(), num);
    str_cat(line, sizeof(line), num);
    str_cat(line, sizeof(line), "  ЗАДАЧ ");
    utoa(sched_task_count(), num);
    str_cat(line, sizeof(line), num);
    str_cat(line, sizeof(line), "  КАСАНИЙ ");
    utoa(touches, num);
    str_cat(line, sizeof(line), num);
    str_cat(line, sizeof(line), "  СЕК ");
    utoa(timer_uptime_ms() / 1000, num);
    str_cat(line, sizeof(line), num);

    fb_text(MARGIN, y, 2, UI_DIM, UI_TRANSPARENT, line);
}

static void draw_all(void)
{
    fb_clear(UI_BG);
    draw_header();
    for (int i = 0; i < TILE_COUNT; i++)
        draw_tile(i);
    draw_detail();
    draw_status(0);
}

/* --- Слои --------------------------------------------------------- */

/*
 * Готовим два слоя: подсветку размером с плитку и точку под палец.
 *
 * Память берём той, что годится внешнему блоку: некэшируемой она
 * помечается блоками по два мегабайта, и соседей у буфера быть не должно.
 */
static void layers_init(void)
{
    u64 span = 2UL * 1024 * 1024;

    layer_hl  = pmm_alloc_dma((u64)tile_w * TILE_H * 4);
    layer_dot = pmm_alloc_dma(DOT_SIZE * DOT_SIZE * 4);
    if (!layer_hl || !layer_dot) {
        kprintf("UI: НЕТ ПАМЯТИ ПОД СЛОИ\n");
        return;
    }
    mmu_set_range_nc((u64)(uintptr_t)layer_hl, span);
    mmu_set_range_nc((u64)(uintptr_t)layer_dot, span);

    /* Подсветка: светлая рамка и лёгкая заливка. Прозрачность задаёт
     * сам слой, поэтому здесь пиксели непрозрачные. */
    for (u32 y = 0; y < TILE_H; y++)
        for (u32 x = 0; x < tile_w; x++) {
            int edge = (x < 4 || y < 4 || x >= tile_w - 4 || y >= TILE_H - 4);

            layer_hl[y * tile_w + x] = edge ? 0xFF4FC3F7 : 0xFF1E4A63;
        }

    /* Точка: круг. Считаем по расстоянию от центра, за границей — прозрачно.
     * Прозрачность здесь пиксельная: у слоя она одна на всех. */
    for (u32 y = 0; y < DOT_SIZE; y++)
        for (u32 x = 0; x < DOT_SIZE; x++) {
            int dx = (int)x - DOT_SIZE / 2;
            int dy = (int)y - DOT_SIZE / 2;
            int r2 = dx * dx + dy * dy;
            u32 lim = (DOT_SIZE / 2) * (DOT_SIZE / 2);

            layer_dot[y * DOT_SIZE + x] =
                (r2 <= (int)lim) ? 0xFF4FC3F7 : 0x00000000;
        }

    dsb();
    layers_ready = 1;
}

/*
 * Настройка слоя ждёт окна между кадрами — до тридцати трёх миллисекунд.
 * Делать это на каждое событие движения нельзя: события идут сто раз в
 * секунду, и оболочка встала бы колом. Поэтому слой настраивается один
 * раз, а дальше только перемещается — а перемещение это одна запись в
 * регистр, ради чего аппаратная композиция и нужна.
 */
/*
 * Состояние слоёв: чего мы хотим и что уже применено.
 *
 * Разделение не от лишней аккуратности. События приходят сто раз в
 * секунду, а кадров шестьдесят, и правка регистра посреди вывода даёт
 * ровно то, что было видно на записи: верх кадра успевает уйти со старым
 * смещением слоя, низ — уже с новым, и под пальцем оказывается не одна
 * точка, а две-три подряд. Поэтому сначала копим желаемое, а применяем
 * один раз за проход и только в окне между кадрами.
 */
static int hl_tile = -1;            /* подсвечено сейчас   */
static int hl_want = -1;            /* хотим подсветить    */
static int dot_on;                  /* точка показана      */
static int dot_want;                /* хотим показать      */
static u32 dot_x, dot_y;            /* где показана        */
static u32 dot_wx, dot_wy;          /* где хотим           */

static void highlight(int tile)
{
    hl_want = tile;
}

static void dot_at(u32 x, u32 y)
{
    u32 half = DOT_SIZE / 2;

    dot_wx = x > half ? x - half : 0;
    dot_wy = y > half ? y - half : 0;
    dot_want = 1;
}

static void dot_hide(void)
{
    dot_want = 0;
}

/*
 * Применить накопленное.
 *
 * Ждём окно между кадрами один раз и только если есть что менять. Дальше
 * правим регистры: перемещение слоя — одна запись, и все правки
 * укладываются в окно с огромным запасом, оно около восьмидесяти пяти
 * микросекунд.
 */
static void layers_commit(void)
{
    int hl_change  = (hl_want != hl_tile);
    int dot_change = (dot_want != dot_on) ||
                     (dot_want && (dot_wx != dot_x || dot_wy != dot_y));

    if (!layers_ready || (!hl_change && !dot_change))
        return;

    fb_wait_frame_gap();

    if (hl_change) {
        if (hl_want < 0) {
            ovl_layer_off(1);
        } else {
            u32 x, y;

            tile_rect(hl_want, &x, &y);
            if (hl_tile < 0)
                ovl_layer_set(1, layer_hl, x, y,
                              tile_w, TILE_H, tile_w * 4, 140);
            else
                ovl_layer_move(1, x, y);
        }
        hl_tile = hl_want;
    }

    if (dot_change) {
        if (dot_want && !dot_on) {
            ovl_layer_set(2, layer_dot, dot_wx, dot_wy,
                          DOT_SIZE, DOT_SIZE, DOT_SIZE * 4, 200);
            dot_on = 1;
        } else if (dot_want) {
            ovl_layer_move(2, dot_wx, dot_wy);
        } else {
            ovl_layer_off(2);
            dot_on = 0;
        }
        dot_x = dot_wx;
        dot_y = dot_wy;
    }
}

/* --- Задача оболочки ---------------------------------------------- */

static void ui_task(void *arg)
{
    u64 base;
    u32 stride;
    u32 touches = 0;
    u64 next_status = 0;

    (void)arg;

    fb_info(&base, &scr_w, &scr_h, &stride);
    tile_w = (scr_w - 2 * MARGIN - (COLS - 1) * GAP) / COLS;

    /* Экран теперь наш: отладочный вывод уходит только в консоль по USB.
     * Поверх бегущего текста интерфейс рисовать бессмысленно. */
    kprint_to_fb(0);

    layers_init();
    draw_all();
    kprintf("UI: ОБОЛОЧКА ЗАПУЩЕНА, ПЛИТОК %d, СЛОИ %s\n",
            TILE_COUNT, layers_ready ? "ЕСТЬ" : "НЕТ");

    for (;;) {
        struct input_event e;
        int redraw_detail = 0;

        while (input_pop(&e)) {
            if (e.action == TOUCH_UP) {
                /* Ведём себя как нормальный интерфейс: действие
                 * срабатывает на отпускании, и только если палец
                 * отпустили там же, где нажали. Иначе не отменить
                 * случайное нажатие, уведя палец в сторону. */
                if (pressed >= 0 && tile_at(e.x, e.y) == pressed) {
                    selected = pressed;
                    redraw_detail = 1;
                }
                pressed = -1;
                highlight(-1);
                dot_hide();
                continue;
            }

            dot_at(e.x, e.y);

            if (e.action == TOUCH_DOWN) {
                touches++;
                pressed = tile_at(e.x, e.y);
                highlight(pressed);
            } else if (e.action == TOUCH_MOVE) {
                /* Увели палец с плитки — подсветку снимаем, но нажатие
                 * не забываем: палец могут вернуть обратно, и тогда
                 * нажатие должно засчитаться. */
                highlight(tile_at(e.x, e.y) == pressed ? pressed : -1);
            }
        }

        /* Правки слоёв — один раз за проход и в окне между кадрами.
         * Внутри цикла разбора событий их делать нельзя: событий больше,
         * чем кадров, и слой успел бы переехать посреди вывода. */
        layers_commit();

        if (redraw_detail)
            draw_detail();

        if (timer_uptime_ms() >= next_status) {
            next_status = timer_uptime_ms() + 1000;
            draw_status(touches);
        }

        /* Спим до следующей проверки очереди событий. Пять миллисекунд —
         * половина периода опроса тачскрина: чаще смотреть незачем,
         * реже — половина событий ждала бы лишний круг. */
        task_sleep_ms(5);
    }
}

void ui_start(void)
{
    task_create_prio("оболочка", ui_task, NULL, TASK_PRIO_UI);
}
