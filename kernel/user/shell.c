/*
 * Оболочка StellarOS — программа, а не часть ядра.
 *
 * Плитки, подсветку и разбор касаний делает обычная программа в EL0. Ей
 * доступны ровно три вещи: буфер своего окна, шрифт ядра и очередь
 * касаний. Ни фреймбуфера, ни регистров контроллера дисплея она не
 * видит, и ошибка в интерфейсе перестала быть ошибкой в ядре.
 *
 * Экран разделён на три части, и это не украшение, а способ обойтись без
 * отсечения: шапка и нижняя полоса рисуются ПОСЛЕ списка, поверх него.
 * Плитка, уехавшая под шапку, закрашивается ею и наружу не торчит.
 *
 *   шапка   — название, неподвижна
 *   список  — плитки, прокручивается пальцем
 *   полоса  — состояние и перезагрузка, неподвижна
 */
#include "ulib.h"

#define COLS        1
#define TILE_COUNT  10
#define MARGIN      24
#define GAP         16
#define HEADER_H    150
#define TILE_H      200
#define BTN_H       110
#define BAR_H       (BTN_H + 96)    /* кнопка и две строки состояния */
#define BTN_ARM_MS  4000

/* --- Жесты ---------------------------------------------------------- */

/*
 * Порог, за которым нажатие превращается в прокрутку.
 *
 * Палец никогда не стоит на месте: за время касания он смещается на
 * несколько пикселей, и без порога каждое нажатие оказывалось бы
 * прокруткой. Четырнадцать пикселей — примерно десятая доля миллиметра
 * промаха на этой панели, меньше брать нельзя, больше — начинает
 * казаться, что список «прилипает».
 */
#define SLOP        14

#define FLICK_MAX   9000        /* быстрее пальца всё равно не бывает, px/с */
#define FLICK_MIN   120         /* тише этого — просто отпустили, px/с      */
#define FRICTION    93          /* сколько процентов скорости остаётся за кадр */
#define FRAME_MS    16

#define COL_BG      0xFF0B1020
#define COL_TILE    0xFF16203A
#define COL_TILE_HL 0xFF2E63C8
#define COL_EDGE    0xFF2A3A5E
#define COL_TEXT    0xFF7FD4FF
#define COL_DIM     0xFF5A6A88
#define COL_WHITE   0xFFFFFFFF
#define COL_BAR     0xFF0E1526      /* нижняя полоса                */
#define COL_SCROLL  0xFF31456E      /* указатель прокрутки          */
#define COL_BTN     0xFF2A1119
#define COL_BTN_ARM 0xFF7A1F2E
#define COL_BTN_EDGE 0xFF5E2130
#define COL_BTN_TEXT 0xFFFF9AA8

static u32 *win;                    /* первая половина буфера    */
static u32 *back;                   /* та, в которую рисуем      */
static u32 half;                    /* какую сейчас показываем   */
static u32 sw, sh;                  /* размеры экрана и окна     */
static u32 tile_w;
static u32 list_top, list_bottom, content_h;

/* Без начальных значений: у программ для EL0 их не бывает вовсе — образ
 * содержит только код, а изменяемые данные ядро выдаёт чистыми. */
static int pressed;                 /* какая плитка нажата       */
static int chosen;                  /* какая выбрана             */
static u64 touches;
static char line[96];

/* Прокрутка */
static int scroll;                  /* на сколько уехал список   */
static int scroll_max;
static int fling;                   /* скорость по инерции, px/с */

/* Разбор жеста */
static int touching;
static int dragging;
static u32 down_x, down_y;
static int down_scroll;
static u64 down_ms;
static u32 last_y;
static u64 last_ms;

/* Кнопка перезагрузки */
static int reboot_armed;
static u64 armed_at;

static const char *const tile_name[TILE_COUNT] = {
    "ЭКРАН", "КАСАНИЯ", "ПАМЯТЬ", "ЯДРА", "ОКНА",
    "О СИСТЕМЕ", "ПРОГРАММЫ", "ЯЗЫК", "ФЛЕШКА", "ШРИФТ",
};

static const char *const tile_note[TILE_COUNT] = {
    "СЛОИ ОВЕРЛЕЯ, КАДР ЧИТАЕТ КОНТРОЛЛЕР",
    "NOVATEK NT36672A, ОЧЕРЕДЬ У КАЖДОЙ ПРОГРАММЫ",
    "СВОИ ТАБЛИЦЫ У КАЖДОЙ ПРОГРАММЫ",
    "ВОСЕМЬ ЯДЕР, ОЧЕРЕДЬ ЗАДАЧ ОДНА",
    "БУФЕР В СВОЕЙ ПАМЯТИ, ПОКАЗ СЛОЕМ",
    "ОБОЛОЧКА РАБОТАЕТ В EL0, КАК ОБЫЧНАЯ ПРОГРАММА",
    "СВОЁ ПРОСТРАНСТВО, СВОЙ ASID, СНЯТИЕ ПРИ НАРУШЕНИИ",
    "HITTIS: .HT СОБИРАЕТСЯ В .SLT, МАШИНА В EL0",
    "EMMC: РАЗДЕЛЫ ЧИТАЮТСЯ, ЗАПИСЕЙ НЕТ",
    "MANROPE, СВОЙ РАСТЕРИЗАТОР TRUETYPE",
};

/* --- Раскладка ------------------------------------------------------ */

/* Где лежит плитка. y — уже с учётом прокрутки */
static void tile_rect(u32 i, int *x, int *y)
{
    *x = (int)(MARGIN + (i % COLS) * (tile_w + GAP));
    *y = (int)list_top + (int)((i / COLS) * (TILE_H + GAP)) - scroll;
}

/* Какая плитка под точкой; -1 — мимо всех */
static int tile_at(u32 px, u32 py)
{
    if (py < list_top || py >= list_bottom)
        return -1;

    for (u32 i = 0; i < TILE_COUNT; i++) {
        int x, y;

        tile_rect(i, &x, &y);
        if ((int)px >= x && (int)px < x + (int)tile_w &&
            (int)py >= y && (int)py < y + TILE_H)
            return (int)i;
    }

    return -1;
}

static void btn_rect(u32 *x, u32 *y, u32 *w, u32 *h)
{
    *x = MARGIN;
    *h = BTN_H;
    *y = (sh > BTN_H + MARGIN) ? sh - BTN_H - MARGIN : HEADER_H;
    *w = tile_w;
}

static int in_button(u32 px, u32 py)
{
    u32 x, y, w, h;

    btn_rect(&x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* --- Рисование ------------------------------------------------------ */

/*
 * Прямоугольник, обрезанный по экрану.
 *
 * При прокрутке плитка наполовину уходит под шапку, и рисовать её
 * целиком нельзя — вылезем за буфер окна. Пропускать такую плитку тоже
 * нельзя: она исчезала бы целиком вместо того, чтобы уезжать. Значит
 * рисуем ровно её видимую часть.
 */
static void vrect(int x, int y, int w, int h, u32 color)
{
    int x1 = x + w, y1 = y + h;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int)sw)
        x1 = (int)sw;
    if (y1 > (int)sh)
        y1 = (int)sh;
    if (x1 <= x || y1 <= y)
        return;

    urect(back, sw, (u32)x, (u32)y, (u32)(x1 - x), (u32)(y1 - y), color);
}

static void draw_tile(u32 i)
{
    int x, y;
    u32 body = ((int)i == pressed) ? COL_TILE_HL : COL_TILE;
    u32 edge = ((int)i == chosen) ? COL_TILE_HL : COL_EDGE;

    tile_rect(i, &x, &y);

    /* Уехавшие целиком за края не рисуем вовсе: при прокрутке это
     * половина списка, и каждый пропущенный — сэкономленный проход по
     * десяткам тысяч пикселей некэшируемой памяти. */
    if (y + TILE_H <= (int)list_top || y >= (int)list_bottom)
        return;

    vrect(x, y, (int)tile_w, TILE_H, body);
    vrect(x, y, (int)tile_w, 2, edge);
    vrect(x, y + TILE_H - 2, (int)tile_w, 2, edge);
    vrect(x, y, 2, TILE_H, edge);
    vrect(x + (int)tile_w - 2, y, 2, TILE_H, edge);

    /*
     * Подписи рисует ядро, а оно про наш список ничего не знает и
     * обрезает только по окну. Поэтому строку, уехавшую за край, просто
     * не просим: то, что залезет под шапку, всё равно закрасится ею,
     * а вот отрицательная координата приехала бы в ядро огромным
     * беззнаковым числом.
     */
    if (y + 30 >= 0 && y + 30 < (int)sh - 24)
        text((u32)x + 24, (u32)(y + 30), 3,
             ((int)i == chosen) ? COL_WHITE : COL_TEXT, body, tile_name[i]);
    if (y + 100 >= 0 && y + 100 < (int)sh - 16)
        text((u32)x + 24, (u32)(y + 100), 2, COL_DIM, body, tile_note[i]);
}

/*
 * Указатель прокрутки справа.
 *
 * Без него непонятно, много ли осталось: список уезжает, а сколько его
 * ещё — не видно. Показываем только когда есть что прокручивать.
 */
static void draw_scrollbar(void)
{
    u32 view = list_bottom - list_top;
    u32 h, y;

    if (scroll_max <= 0)
        return;

    h = view * view / content_h;
    if (h < 40)
        h = 40;
    y = list_top + (u32)((u64)(view - h) * (u32)scroll / (u32)scroll_max);

    urect(back, sw, sw - 10, y, 4, h, COL_SCROLL);
}

static void copy_str(char *dst, const char *src, u32 max)
{
    u32 i = 0;

    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void draw_button(void)
{
    u32 x, y, w, h;
    u32 body = reboot_armed ? COL_BTN_ARM : COL_BTN;

    btn_rect(&x, &y, &w, &h);
    urect(back, sw, x, y, w, h, body);
    uframe(back, sw, x, y, w, h, 2, COL_BTN_EDGE);
    text(x + 20, y + 22, 3, reboot_armed ? COL_WHITE : COL_BTN_TEXT, body,
         "ПЕРЕЗАГРУЗКА");
    text(x + 20, y + 66, 2, reboot_armed ? COL_WHITE : COL_DIM, body,
         reboot_armed ? "НАЖМИ ЕЩЁ РАЗ" : "НАЖАТЬ ДВАЖДЫ");
}

/*
 * Нижняя полоса: состояние и кнопка. Рисуется поверх списка, поэтому
 * уехавшая плитка из-под неё не выглядывает.
 *
 * Каждая строка пишется с непрозрачным фоном и дополняется пробелами до
 * постоянной ширины: каждый пиксель меняется ровно один раз, пустого
 * промежуточного состояния не возникает, а хвост прежней надписи
 * затирается пробелами. Так лечится мигание текста.
 */
static void draw_bar(void)
{
    u32 y = sh - BAR_H;

    urect(back, sw, 0, y, sw, BAR_H, COL_BAR);

    copy_str(line, chosen >= 0 ? tile_name[chosen] : "ВЕДИ ПАЛЬЦЕМ — СПИСОК ПРОКРУТИТСЯ",
             sizeof(line));
    upad(line, 46);
    text(MARGIN, y + 12, 2, chosen >= 0 ? COL_TEXT : COL_DIM, COL_BAR, line);

    ulabel(line, "КАСАНИЙ ", touches);
    upad(line, 14);
    text(MARGIN, y + 46, 2, COL_DIM, COL_BAR, line);

    ulabel(line, "СЕКУНД ", uptime_ms() / 1000);
    upad(line, 14);
    text(MARGIN + 260, y + 46, 2, COL_DIM, COL_BAR, line);

    draw_button();
}

static void draw_header(void)
{
    urect(back, sw, 0, 0, sw, HEADER_H, COL_BG);
    text(MARGIN, 28, 6, COL_WHITE, COL_BG, "StellarOS");
    text(MARGIN, 92, 2, COL_DIM, COL_BG, "ОБОЛОЧКА В ПОЛЬЗОВАТЕЛЬСКОМ РЕЖИМЕ");
}

/*
 * Показать нарисованное.
 *
 * Рисуем всегда в ту половину, которая сейчас не видна, и переключаем
 * показ целиком. Промежуточных состояний на экране не бывает вовсе —
 * это и есть лечение мигания: контроллер дисплея не может застать нас
 * за работой, потому что работаем мы не в той памяти, которую он читает.
 */
static void show(void)
{
    flip(half);
    half ^= 1;
    back = win + (u64)half * sw * sh;
}

static void draw_all(void)
{
    ufill(back, sw * sh, COL_BG);

    for (u32 i = 0; i < TILE_COUNT; i++)
        draw_tile(i);
    draw_scrollbar();

    /* Шапка и полоса — последними, поверх списка: это и есть отсечение */
    draw_header();
    draw_bar();
}

/* --- Прокрутка ------------------------------------------------------ */

static void scroll_to(int v)
{
    if (v < 0)
        v = 0;
    if (v > scroll_max)
        v = scroll_max;
    scroll = v;
}

/*
 * Шаг инерции.
 *
 * Скорость гасим долей за кадр, а не вычитанием: тогда быстрый бросок
 * летит долго, а медленный останавливается сразу — так ведёт себя
 * трение, и так это выглядит естественно. Возвращает 1, пока движение
 * продолжается.
 */
static int fling_step(void)
{
    int was = scroll;

    if (!fling)
        return 0;

    scroll_to(scroll - fling * FRAME_MS / 1000);
    fling = fling * FRICTION / 100;

    /* Упёрлись в край или почти встали — движение кончилось */
    if (scroll == was || (fling < FLICK_MIN && fling > -FLICK_MIN)) {
        fling = 0;
        return 0;
    }

    return 1;
}

/* --- Разбор касаний ------------------------------------------------- */

static void on_down(const struct touch *t)
{
    touches++;
    fling = 0;                  /* палец на экране останавливает разгон */

    if (in_button(t->x, t->y)) {
        if (reboot_armed) {
            write("EL0      : ОБОЛОЧКА: ПЕРЕЗАГРУЗКА ПО КНОПКЕ\n");
            reboot();
        }
        reboot_armed = 1;
        armed_at = uptime_ms();
        return;
    }

    if (reboot_armed)           /* нажали мимо — отменяем */
        reboot_armed = 0;

    touching = 1;
    dragging = 0;
    down_x = t->x;
    down_y = t->y;
    down_scroll = scroll;
    down_ms = uptime_ms();
    last_y = t->y;
    last_ms = down_ms;
    pressed = tile_at(t->x, t->y);
}

static void on_move(const struct touch *t)
{
    int dy;
    u64 now;

    if (!touching)
        return;

    dy = (int)t->y - (int)down_y;

    /*
     * Пока палец не ушёл дальше порога, это ещё нажатие. Как только
     * ушёл — нажатие отменяется совсем: человек передумал и начал
     * прокрутку, и загоревшаяся под пальцем плитка была бы враньём.
     */
    if (!dragging && (dy > SLOP || dy < -SLOP)) {
        dragging = 1;
        pressed = -1;
    }

    if (!dragging)
        return;

    scroll_to(down_scroll - dy);

    /*
     * Скорость считаем по последним двум событиям, а не по всему жесту:
     * важно, как палец двигался в конце. Медленно провёл и резко дёрнул
     * напоследок — список должен полететь.
     */
    now = uptime_ms();
    if (now > last_ms) {
        int v = ((int)t->y - (int)last_y) * 1000 / (int)(now - last_ms);

        if (v > FLICK_MAX)
            v = FLICK_MAX;
        if (v < -FLICK_MAX)
            v = -FLICK_MAX;
        fling = v;
        last_y = t->y;
        last_ms = now;
    }
}

static void on_up(const struct touch *t)
{
    int was = pressed;

    if (!touching) {
        pressed = -1;
        return;
    }
    touching = 0;

    if (dragging) {
        /* Бросок: медленное отпускание не должно ничего запускать */
        if (fling < FLICK_MIN && fling > -FLICK_MIN)
            fling = 0;
        dragging = 0;
        pressed = -1;
        return;
    }

    if (was >= 0 && tile_at(t->x, t->y) == was)
        chosen = was;
    pressed = -1;
}

/* --- Точка входа ---------------------------------------------------- */

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    u64 shown_sec;

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
    list_top = HEADER_H;
    list_bottom = sh - BAR_H;
    content_h = ((TILE_COUNT + COLS - 1) / COLS) * (TILE_H + GAP) + GAP;
    scroll_max = (int)content_h - (int)(list_bottom - list_top);
    if (scroll_max < 0)
        scroll_max = 0;

    /* Показывается первая половина, рисуем во вторую */
    half = 1;
    back = win + (u64)sw * sh;

    draw_all();
    show();

    if (present(0, 0) != 0) {
        write("EL0      : ОБОЛОЧКА: ПОКАЗАТЬ ОКНО НЕ ВЫШЛО\n");
        exit(3);
    }

    write("EL0      : ОБОЛОЧКА В EL0 ЗАПУЩЕНА, ЖЕСТЫ И ПРОКРУТКА\n");
    shown_sec = uptime_ms() / 1000;

    for (;;) {
        struct touch t;
        s64 got;
        int moving = (fling != 0);
        int redraw = 0;

        /*
         * Ждём пальца, а когда список летит по инерции — ждём не дольше
         * кадра. Спать в ожидании события, которого не будет, значило бы
         * остановить движение сразу после отпускания.
         */
        got = moving ? input_wait(&t, FRAME_MS) : input(&t);

        if (got == 1) {
            if (t.action == TOUCH_DOWN)
                on_down(&t);
            else if (t.action == TOUCH_MOVE)
                on_move(&t);
            else
                on_up(&t);
            redraw = 1;
        }

        if (fling_step())
            redraw = 1;

        /* Взведённая кнопка гаснет сама: иначе случайное касание через
         * час перезагрузило бы телефон. */
        if (reboot_armed && uptime_ms() - armed_at > BTN_ARM_MS) {
            reboot_armed = 0;
            redraw = 1;
        }

        if (uptime_ms() / 1000 != shown_sec) {
            shown_sec = uptime_ms() / 1000;
            redraw = 1;
        }

        if (redraw) {
            draw_all();
            show();
        }
    }
}
