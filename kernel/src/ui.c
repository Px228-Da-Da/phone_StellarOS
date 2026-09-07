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
#include "battery.h"
#include "charger.h"
#include "sched.h"
#include "spinlock.h"
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
/*
 * Те же системные цвета тёмной темы iOS, что и в оболочке.
 *
 * Держать их согласованными обязательно: полосу состояния рисует ядро, а
 * всё под ней — программа, и разойдись они хоть на оттенок, стык было бы
 * видно поперёк экрана. Оттенка синевы в фоне здесь больше нет: там, где
 * у Apple чёрный, он именно чёрный.
 */
#define UI_BG           0xFF000000      /* фон полос                 */
#define UI_TILE         0xFF1C1C1E      /* вторичный фон             */
#define UI_TILE_EDGE    0xFF38383A      /* разделитель               */
#define UI_TEXT         0xFFFFFFFF      /* основной текст            */
#define UI_DIM          0xFF8E8E93      /* второстепенный, systemGray */
#define UI_ACCENT       0xFF0A84FF      /* акцент, systemBlue         */
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
/*
 * Точка под пальцем.
 *
 * Своего слоя у неё больше нет: слоёв четыре, нулевой занят кадром,
 * первый — подсветкой плитки, а два оставшихся отданы окнам программ.
 * Ровно эту работу — показать, где палец, — теперь делает программа в
 * EL0, и делает её там, где ей и место.
 *
 * Код точки оставлен целиком: он понадобится обратно в тот день, когда
 * оболочка сама переедет в EL0 и станет такой же программой со своим
 * окном.
 */
#define UI_DOT_LAYER    0           /* 0 — точку рисует не оболочка */
#define UI_KERNEL_SHELL 0           /* 0 — интерфейс живёт в EL0     */

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

/*
 * Строка фиксированной ширины.
 *
 * Раньше меняющийся текст рисовался в два приёма: стереть прямоугольник,
 * потом нарисовать буквы. Между этими действиями контроллер дисплея
 * успевает вывести кадр — и в нём область пустая. Отсюда мигание. А если
 * новая строка короче старой, её хвост остаётся на экране: отсюда буквы
 * поверх букв.
 *
 * Лечится тем, что каждый пиксель пишется ровно один раз: строка
 * дополняется пробелами до нужной ширины и рисуется с непрозрачным фоном.
 * Промежуточного состояния, в котором пусто, просто не возникает.
 */
static void draw_line(u32 x, u32 y, u32 scale, u32 fg,
                      const char *text, u32 width_px)
{
    char buf[128];
    u32 n = 0;

    while (text[n] && n < sizeof(buf) - 2)
        buf[n] = text[n], n++;
    buf[n] = 0;

    while (n < sizeof(buf) - 2 && fb_text_width(scale, buf) < width_px) {
        buf[n++] = ' ';
        buf[n] = 0;
    }
    fb_text(x, y, scale, fg, UI_BG, buf);
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
    const char *title = "StellarOS";
    const char *sub   = "СВОЯ ОС НА ГОЛОМ ЖЕЛЕЗЕ";

    fb_text(MARGIN, 150, 9, UI_TEXT, UI_TRANSPARENT, title);
    fb_text(MARGIN, 280, 3, UI_DIM, UI_TRANSPARENT, sub);
    fb_fill_rect(MARGIN, 360, scr_w - 2 * MARGIN, 2, UI_TILE_EDGE);
}

/*
 * Батарея — отдельной строкой над плитками.
 *
 * Показываем и напряжение, и процент. Процент считается по кривой разряда
 * этой батареи, но по напряжению без учёта нагрузки, поэтому под нагрузкой
 * он занижен; напряжение же измерено точно, и по нему видно, растёт заряд
 * или падает, даже когда процент стоит на месте.
 */
#define BATT_Y      (TILES_Y - 120)

/*
 * Куда идёт заряд.
 *
 * Признак «кабель подключён» отвечает не на тот вопрос. Кабель может быть
 * воткнут, а телефон при этом всё равно разряжаться — если потребление
 * больше, чем даёт зарядка. У нас именно такой случай возможен:
 * контроллер заряда мы не настраиваем, а расход немаленький.
 *
 * Поэтому сравниваем напряжение с тем, что было полминуты назад. Порог в
 * пять милливольт — чтобы шум измерения не выдавался за движение.
 */
static u32 batt_ref_mv;
static u64 batt_ref_ms;
static const char *batt_trend = "";

static void update_trend(u32 mv)
{
    u64 now = timer_uptime_ms();

    if (!batt_ref_ms) {
        batt_ref_mv = mv;
        batt_ref_ms = now;
        return;
    }
    if (now - batt_ref_ms < 30000)
        return;

    if (mv > batt_ref_mv + 5)
        batt_trend = " РАСТЁТ";
    else if (mv + 5 < batt_ref_mv)
        batt_trend = " ПАДАЕТ";
    else
        batt_trend = " СТОИТ";

    batt_ref_mv = mv;
    batt_ref_ms = now;
}

/*
 * Опрос питания — в своей задаче, а не в оболочке.
 *
 * Причина не в стройности, а в том, что железо отвечает не мгновенно:
 * измерение батареи занимает полторы миллисекунды, а опрос контроллера
 * заряда — шесть посылок по I2C, и любая из них может не дойти. Пока это
 * делалось прямо в оболочке, неудачная посылка замораживала интерфейс.
 *
 * Теперь оболочка читает только готовые числа и не ждёт никого.
 */
static struct battery_state power_batt;
static const char *power_stage = "";
static const char *power_port = "";
static u32 power_input_ma;

static void power_task(void *arg)
{
    (void)arg;
    for (;;) {
        struct battery_state st;

        battery_read(&st);

        /*
         * Первый удачный замер печатаем целиком: по этим числам видно,
         * заработал ли счётчик заряда и насколько поправка на ток
         * отличается от напряжения на выводах.
         */
        {
            static int told;

            if (st.valid && !told) {
                told = 1;
                kprintf("БАТАРЕЯ  : %u мВ, БЕЗ НАГРУЗКИ %u мВ, ТОК %d мА, ЗАРЯД %u%%%s\n",
                        st.mv, st.ocv_mv, st.current_ma, st.percent,
                        battery_current_valid() ? "" : " (СЧЁТЧИК МОЛЧИТ)");
            }
        }

        if (st.valid) {
            update_trend(st.mv);
            if (st.charging) {
                /* Порядок важен: stage опрашивает контроллер и заодно
                 * обновляет то, что читают две другие функции. */
                const char *stage = charger_stage_text();

                power_port = charger_port_text();
                power_input_ma = charger_input_ma();
                power_stage = stage;
            } else {
                power_stage = "";
                power_port = "";
                power_input_ma = 0;
            }
        }
        power_batt = st;

        /* Раз в две секунды: чаще незачем, а каждое обращение к железу
         * стоит времени и немного тока. */
        task_sleep_ms(2000);
    }
}

/*
 * Последний замер батареи.
 *
 * Читатель у PMIC ровно один — задача питания. Полоса состояния и пульс
 * берут готовое: канал к PMIC последовательный и небыстрый, а два
 * читателя в разных задачах — это две посылки внахлёст на одной шине.
 * Пока обходилось, но обходиться такое перестаёт в самый неудобный
 * момент.
 */
void battery_last(struct battery_state *out)
{
    *out = power_batt;
}

static void draw_battery(void)
{
    struct battery_state st = power_batt;
    char line[80];
    char num[24];
    u32 color;

    if (!st.valid) {
        draw_line(MARGIN, BATT_Y + 12, 3, UI_DIM,
                  "БАТАРЕЯ: ЖДУ ЗАМЕРА", scr_w - 2 * MARGIN);
        return;
    }

    line[0] = 0;
    str_cat(line, sizeof(line), st.charging ? "ЗАРЯД " : "БАТАРЕЯ ");
    utoa(st.percent, num);
    str_cat(line, sizeof(line), num);
    str_cat(line, sizeof(line), "%  ");
    utoa(st.mv, num);
    str_cat(line, sizeof(line), num);
    str_cat(line, sizeof(line), " МВ");
    if (st.charging)
        str_cat(line, sizeof(line), "  КАБЕЛЬ");
    str_cat(line, sizeof(line), batt_trend);

    /* Цветом отмечаем только то, что требует внимания: мало заряда. */
    color = st.charging ? UI_ACCENT : (st.percent <= 15 ? 0xFFFF453A : UI_TEXT);
    draw_line(MARGIN, BATT_Y + 12, 3, color, line, scr_w - 2 * MARGIN);

    /*
     * Вторая строка — про сам зарядник.
     *
     * Нужна не для красоты: настоящая проверка заряда делается в розетке,
     * а там USB-консоли нет и смотреть на числа можно только на экране.
     * Тип источника контроллер распознаёт сам, и по нему сразу видно,
     * во что упирается ток — в порт компьютера или в наше потребление.
     */
    line[0] = 0;
    if (st.charging && power_stage[0]) {
        str_cat(line, sizeof(line), power_port);
        str_cat(line, sizeof(line), ", ");
        str_cat(line, sizeof(line), power_stage);
        if (power_input_ma) {
            str_cat(line, sizeof(line), ", ВХОД ");
            utoa(power_input_ma, num);
            str_cat(line, sizeof(line), num);
            str_cat(line, sizeof(line), " МА");
        }
    }
    draw_line(MARGIN, BATT_Y + 42, 2, UI_DIM, line, scr_w - 2 * MARGIN);

    /* В консоль — сырой код АЦП. Спорить о том, верен ли делитель, можно
     * только имея на руках то, что вернуло железо, а не наш пересчёт. */
    {
        static u64 next_log;

        if (timer_uptime_ms() >= next_log) {
            next_log = timer_uptime_ms() + 10000;
            kprintf("БАТАРЕЯ: КОД %u -> %u МВ, %u%%, КАБЕЛЬ %d%s\n",
                    st.raw, st.mv, st.percent, st.charging, batt_trend);
        }
    }
}

/* Полоса под плитками: подробность о выбранной плитке */
#define DETAIL_Y    (TILES_Y + ROWS * (TILE_H + GAP) + 30)
#define DETAIL_H    170

static void draw_detail(void)
{
    u32 w = scr_w - 2 * MARGIN;
    u32 inner = w - 48;

    /* Рамку рисуем один раз при запуске, дальше меняется только текст —
     * и он сам закрывает прошлый, потому что идёт с непрозрачным фоном. */
    if (selected < 0) {
        draw_line(MARGIN + 24, DETAIL_Y + 34, 3, UI_DIM, "", inner);
        draw_line(MARGIN + 24, DETAIL_Y + 90, 2, UI_DIM,
                  "НАЖМИ НА ПЛИТКУ", inner);
        return;
    }
    draw_line(MARGIN + 24, DETAIL_Y + 34, 3, UI_ACCENT,
              tiles[selected].title, inner);
    draw_line(MARGIN + 24, DETAIL_Y + 90, 2, UI_TEXT,
              tiles[selected].detail, inner);
}

/* Строка состояния внизу: живые числа, обновляется раз в секунду */
static void draw_status(u32 touches)
{
    char line[96];
    char num[24];
    u32 y = scr_h - 120;

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

    draw_line(MARGIN, y, 2, UI_DIM, line, scr_w - 2 * MARGIN);
}

static void draw_all(void)
{
    fb_clear(UI_BG);
    draw_header();
    draw_battery();
    for (int i = 0; i < TILE_COUNT; i++)
        draw_tile(i);
    draw_frame_rect(MARGIN, DETAIL_Y, scr_w - 2 * MARGIN, DETAIL_H,
                    UI_BG, UI_TILE_EDGE);
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
    layer_dot = UI_DOT_LAYER ? pmm_alloc_dma(DOT_SIZE * DOT_SIZE * 4) : NULL;
    if (!layer_hl || (UI_DOT_LAYER && !layer_dot)) {
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

            layer_hl[y * tile_w + x] = edge ? 0xFF0A84FF : 0xFF1C1C1E;
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
                (r2 <= (int)lim) ? 0xFF0A84FF : 0x00000000;
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

/* Собственно правка регистров. Вызывается уже внутри окна между кадрами
 * и с закрытыми прерываниями — сама ничего не ждёт. */
static void layers_apply(int hl_change, int dot_change)
{
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

/*
 * Применить накопленное — в окне между кадрами.
 *
 * Дождаться окна мало. Оно длится около восьмидесяти пяти микросекунд, а
 * между «окно наступило» и «регистр записан» задачу успевает вытеснить
 * прерывание таймера — оно приходит сто раз в секунду и заодно гоняет
 * консоль по USB. Запись при этом ложится уже посреди вывода, и слой
 * снова двоится. Именно так и оставалось после первой правки: реже, но
 * оставалось.
 *
 * Поэтому три шага: дождаться окна с разрешёнными прерываниями, закрыть
 * их, и уже с закрытыми переспросить оверлей — не начал ли он читать
 * следующий кадр. Не успели — ждём следующего окна.
 *
 * Попыток три. Не сложилось за три кадра — пишем как есть: увиденный раз
 * в жизни шов лучше, чем интерфейс, который перестал отвечать.
 */
static void layers_commit(void)
{
    int hl_change  = (hl_want != hl_tile);
    int dot_change = (dot_want != dot_on) ||
                     (dot_want && (dot_wx != dot_x || dot_wy != dot_y));

    if (!layers_ready || (!hl_change && !dot_change))
        return;

    for (int attempt = 0; attempt < 3; attempt++) {
        u64 flags;

        fb_wait_frame_gap();
        flags = irq_save();
        if (fb_frame_idle()) {
            layers_apply(hl_change, dot_change);
            irq_restore(flags);
            return;
        }
        irq_restore(flags);
    }

    layers_apply(hl_change, dot_change);
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
                if (UI_DOT_LAYER)
                    dot_hide();
                continue;
            }

            if (UI_DOT_LAYER)
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
            /* Батарею опрашиваем тем же тактом. Измерение занимает около
             * полутора миллисекунд — это дорого для каждого прохода, но
             * раз в секунду незаметно. */
            draw_battery();
        }

        /* Спим до следующей проверки очереди событий. Пять миллисекунд —
         * половина периода опроса тачскрина: чаще смотреть незачем,
         * реже — половина событий ждала бы лишний круг. */
        task_sleep_ms(5);
    }
}

void ui_start(void)
{
    /* Обычный приоритет: питание не срочно, а вот мешать вводу оно не
     * должно — обращения к железу тут медленные. */
    task_create("питание", power_task, NULL);

    /*
     * Ядерную оболочку больше не запускаем: её работу делает программа
     * в EL0 (user/shell.c). Код оставлен целиком и намеренно.
     *
     * Во-первых, он всё ещё единственный, кто умеет показывать заряд и
     * состояние контроллера питания: программе такие сведения пока
     * получить неоткуда, и когда для них появится системный вызов,
     * отсюда его и возьмём.
     *
     * Во-вторых, это работающий образец того, как выглядит интерфейс,
     * написанный в ядре, — рядом с тем же интерфейсом, написанным
     * программой. Разница между ними и есть содержание этого шага.
     */
    if (UI_KERNEL_SHELL)
        task_create_prio("оболочка", ui_task, NULL, TASK_PRIO_UI);
}
