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
#define FLICK_MIN   90          /* тише этого — просто отпустили, px/с      */
#define FRAME_MS    16

/*
 * Затухание броска: тысячных долей скорости остаётся за МИЛЛИСЕКУНДУ.
 *
 * 0,998 в миллисекунду — то самое «долгое» затухание, к которому приучил
 * айфон: за секунду от скорости остаётся примерно седьмая часть. Первое,
 * что здесь стояло, отнимало по семь процентов за кадр, и список
 * останавливался вдвое быстрее — движение выглядело не свободным
 * полётом, а торможением в песке.
 *
 * Считаем на миллисекунду, а не на кадр, потому что кадр у нас не
 * постоянный: полная перерисовка экрана 1080x2340 стоит два десятка
 * миллисекунд, и на фиксированном шаге список полз бы медленнее
 * задуманного ровно во столько раз, во сколько кадр длиннее.
 */
#define DECAY       998

/*
 * Резиновые края.
 *
 * За границей список идёт втрое медленнее пальца, а отпустишь — сам
 * возвращается. Это не украшение: без него непонятно, кончился список
 * или система перестала отвечать. Палец тянет, ничего не двигается —
 * и человек честно думает, что всё зависло.
 */
#define RUBBER      3           /* во сколько раз слабее тянется за краем */
#define RUBBER_MAX  180         /* дальше этого не пускаем вовсе, px      */
#define SPRING      28          /* процентов оставшегося пути за кадр     */

/* Сколько последних точек помним для скорости броска */
#define VSAMPLES    4

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
#define COL_OFF      0xFF15203A      /* выключение: спокойный синий  */
#define COL_OFF_ARM  0xFF2E4A86
#define COL_OFF_EDGE 0xFF32476E
#define COL_OFF_TEXT 0xFF9FC4FF

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
static int scroll_want;             /* куда просится палец       */
static int scroll_max;
static int fling;                   /* скорость по инерции, px/с */

/* Разбор жеста */
static int touching;
static int dragging;
static u32 down_x, down_y;
static int down_scroll;
static u64 down_ms;
static u32 vy[VSAMPLES];            /* последние точки пальца    */
static u64 vt[VSAMPLES];
static u32 vn;                      /* сколько их набралось      */
static int stopped_fling;           /* этим касанием остановили полёт */
static u64 step_ms;                 /* когда двигали список в прошлый раз */
static int frac;                    /* недоехавшие тысячные пикселя       */

/* Замер кадра: без чисел «плавно» и «дёргано» остаются спором о вкусах */
static u64 frame_sum;
static u64 draw_sum;
static u32 frame_count;
static u64 frame_told;

/*
 * Кнопки питания.
 *
 * Их две, и обе опасные, поэтому взведена может быть только одна:
 * первое нажатие взводит, второе выполняет. Нажатие мимо или четыре
 * секунды тишины взвод снимают — иначе случайное касание через час
 * выключило бы телефон.
 */
#define BTN_REBOOT  0
#define BTN_OFF     1
#define BTN_COUNT   2

static int armed;                   /* какая кнопка взведена, -1 — ни одна */
static int off_rc;                  /* чем кончилась попытка выключить    */
static u64 armed_at;
static u64 last_seen_ms;     /* когда последний раз слышали палец */
static u64 app_task;         /* запущенное приложение; 0 — не запускали */

/*
 * Первая плитка — не рассказ, а действие: она запускает приложение.
 *
 * Оболочка перестала быть витриной. Система загружается в рабочий стол,
 * приложение открывается отсюда и занимает весь экран, а полоска «домой»
 * переключает между ними — как и положено телефону.
 */
#define TILE_APP    0

static const char *const tile_name[TILE_COUNT] = {
    "ПЛИТКИ", "ЭКРАН", "КАСАНИЯ", "ПАМЯТЬ", "ЯДРА",
    "ОКНА", "О СИСТЕМЕ", "ПРОГРАММЫ", "ЯЗЫК", "ШРИФТ",
};

static const char *const tile_note[TILE_COUNT] = {
    "ПРИЛОЖЕНИЕ НА HITTIS — НАЖМИ, ЧТОБЫ ЗАПУСТИТЬ",
    "СЛОИ ОВЕРЛЕЯ, КАДР ЧИТАЕТ КОНТРОЛЛЕР",
    "NOVATEK NT36672A, ОЧЕРЕДЬ У КАЖДОЙ ПРОГРАММЫ",
    "СВОИ ТАБЛИЦЫ У КАЖДОЙ ПРОГРАММЫ",
    "ВОСЕМЬ ЯДЕР, ОЧЕРЕДЬ ЗАДАЧ ОДНА",
    "БУФЕР В СВОЕЙ ПАМЯТИ, ПОКАЗ СЛОЕМ",
    "ОБОЛОЧКА РАБОТАЕТ В EL0, КАК ОБЫЧНАЯ ПРОГРАММА",
    "СВОЁ ПРОСТРАНСТВО, СВОЙ ASID, СНЯТИЕ ПРИ НАРУШЕНИИ",
    "HITTIS: .HT СОБИРАЕТСЯ В .SLT, МАШИНА В EL0",
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

/* Две кнопки делят ширину плитки поровну, с зазором посередине */
#define BTN_GAP     16

static void btn_rect(int which, u32 *x, u32 *y, u32 *w, u32 *h)
{
    u32 bw = (tile_w > BTN_GAP) ? (tile_w - BTN_GAP) / 2 : tile_w;

    *h = BTN_H;
    *y = (sh > BTN_H + MARGIN) ? sh - BTN_H - MARGIN : HEADER_H;
    *w = bw;
    *x = MARGIN + (which == BTN_OFF ? bw + BTN_GAP : 0);
}

/* Какая кнопка под точкой; -1 — ни одной */
static int btn_at(u32 px, u32 py)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        u32 x, y, w, h;

        btn_rect(i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h)
            return i;
    }
    return -1;
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
    if (y + 100 >= 0 && y + 100 < (int)sh - 16) {
        const char *note = tile_note[i];

        if (i == TILE_APP && app_task)
            note = "РАБОТАЕТ — НАЖМИ, ЧТОБЫ ВЕРНУТЬ НА ЭКРАН";
        text((u32)x + 24, (u32)(y + 100), 2, COL_DIM, body, note);
    }
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
    int at = scroll;
    u32 h, y;

    if (scroll_max <= 0)
        return;

    /*
     * Положение считаем по ЗАЖАТОЙ прокрутке.
     *
     * С резиновыми краями scroll выходит за границы — в том числе в
     * минус, — а здесь он превращался в беззнаковое число: минус
     * тридцать становился четырьмя миллиардами, ползунок уезжал за край
     * буфера, и ядро снимало оболочку за запись в чужую память. Урок
     * простой: если величина стала знаковой, надо пройти глазами все
     * места, где она превращается в беззнаковую.
     */
    if (at < 0)
        at = 0;
    if (at > scroll_max)
        at = scroll_max;

    h = (u32)((u64)view * view / content_h);
    if (h < 40)
        h = 40;
    if (h > view)
        h = view;

    y = list_top + (u32)((u64)(view - h) * (u32)at / (u32)scroll_max);
    if (y + h > sh)
        h = (y < sh) ? sh - y : 0;
    if (!h)
        return;

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
    for (int i = 0; i < BTN_COUNT; i++) {
        u32 x, y, w, h;
        int hot = (armed == i);
        u32 body, edge, fg;
        const char *note;

        btn_rect(i, &x, &y, &w, &h);

        if (i == BTN_REBOOT) {
            body = hot ? COL_BTN_ARM : COL_BTN;
            edge = COL_BTN_EDGE;
            fg   = hot ? COL_WHITE : COL_BTN_TEXT;
        } else {
            body = hot ? COL_OFF_ARM : COL_OFF;
            edge = COL_OFF_EDGE;
            fg   = hot ? COL_WHITE : COL_OFF_TEXT;
        }

        /*
         * Нижняя строка говорит ровно то, что происходит сейчас: взведена
         * ли кнопка, а у выключения — не отказала ли прошивка. Молчать о
         * таком отказе нельзя: человек нажал, экран не погас, и без
         * надписи он вправе решить, что сломалась кнопка.
         */
        if (hot)
            note = "НАЖМИ ЕЩЁ РАЗ";
        else if (i == BTN_OFF && off_rc == -3)
            note = "СНАЧАЛА ОТКЛЮЧИ КАБЕЛЬ";
        else if (i == BTN_OFF && off_rc)
            note = "НЕ ВЫКЛЮЧИЛОСЬ";
        else
            note = "НАЖАТЬ ДВАЖДЫ";

        urect(back, sw, x, y, w, h, body);
        uframe(back, sw, x, y, w, h, 2, edge);
        text(x + 20, y + 22, 3, fg, body,
             i == BTN_REBOOT ? "ПЕРЕЗАГРУЗКА" : "ВЫКЛЮЧИТЬ");
        text(x + 20, y + 66, 2, hot ? COL_WHITE : COL_DIM, body, note);
    }
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

/*
 * Кадр целиком.
 *
 * Заливки всего окна здесь нет намеренно. Раньше кадр начинался с
 * ufill на 1080x2340 — десять мегабайт записи в некэшируемую память, —
 * а потом плитки закрашивали ту же площадь второй раз. Восемнадцать
 * мегабайт на кадр не укладываются в шестнадцать миллисекунд ни при
 * каком старании, и прокрутка получалась рваной.
 *
 * Теперь каждый пиксель пишется ровно один раз: плитки закрашивают
 * себя, промежутки между ними — отдельные полосы, поля по краям —
 * два столбца. Шапка и нижняя полоса рисуются последними и заодно
 * служат отсечением для уехавших плиток.
 */
static void draw_all(void)
{
    int y = (int)list_top - scroll;
    u32 step = TILE_H + GAP;

    /* Поля слева и справа от списка */
    vrect(0, (int)list_top, MARGIN, (int)(list_bottom - list_top), COL_BG);
    vrect((int)(MARGIN + tile_w), (int)list_top,
          (int)(sw - MARGIN - tile_w), (int)(list_bottom - list_top), COL_BG);

    /* Промежуток над первой плиткой — он появляется на резиновом крае */
    if (y > (int)list_top)
        vrect(MARGIN, (int)list_top, (int)tile_w, y - (int)list_top, COL_BG);

    for (u32 i = 0; i < TILE_COUNT; i++) {
        draw_tile(i);
        /* Промежуток под плиткой */
        vrect(MARGIN, y + TILE_H, (int)tile_w, GAP, COL_BG);
        y += (int)step;
    }

    /* Остаток снизу, если список кончился раньше видимой части */
    if (y < (int)list_bottom)
        vrect(MARGIN, y, (int)tile_w, (int)list_bottom - y, COL_BG);

    draw_scrollbar();

    /* Шапка и полоса — последними, поверх списка: это и есть отсечение */
    draw_header();
    draw_bar();
}

/* --- Прокрутка ------------------------------------------------------ */

/*
 * Куда поставить список.
 *
 * За краями не обрезаем, а пускаем с сопротивлением: палец уводит список
 * втрое слабее, чем внутри, и не дальше ладони. Возвращает его пружина —
 * см. fling_step.
 */
static void scroll_to(int v)
{
    if (v < 0) {
        v /= RUBBER;
        if (v < -RUBBER_MAX)
            v = -RUBBER_MAX;
    } else if (v > scroll_max) {
        v = scroll_max + (v - scroll_max) / RUBBER;
        if (v > scroll_max + RUBBER_MAX)
            v = scroll_max + RUBBER_MAX;
    }
    scroll_want = v;
}

/*
 * Подтянуться к пальцу.
 *
 * Список идёт не прямо в точку последнего замера, а долей пути к ней за
 * кадр. Причина не в красоте: панель опрашивается двести раз в секунду,
 * рисуем мы шестьдесят кадров, и между кадрами набегает то три замера,
 * то четыре. Шаг выходит неровным при идеально ровных кадрах — и именно
 * это глаз читает как дрожь. Доля пути эту неровность съедает, а
 * отставание получается меньше кадра и незаметно.
 *
 * Возвращает 1, если картинка изменилась.
 */
static int follow_finger(void)
{
    int d = scroll_want - scroll;

    if (!d)
        return 0;

    if (d > -2 && d < 2)
        scroll = scroll_want;
    else
        scroll += d * 2 / 3;

    return 1;
}

/* Внутри границ или уже за ними */
static int out_of_bounds(void)
{
    return scroll < 0 || scroll > scroll_max;
}

/* Список сам по себе в движении: летит или возвращается пружиной */
static int scroll_busy(void)
{
    return fling != 0 || out_of_bounds();
}

/*
 * Шаг движения: сначала пружина, потом полёт.
 *
 * Скорость гасим долей за кадр, а не вычитанием: тогда быстрый бросок
 * летит долго, а медленный останавливается сразу — так ведёт себя
 * трение. Возвращает 1, пока картинка меняется.
 */
static int fling_step(void)
{
    u64 now = uptime_ms();
    int dt, edge, d;

    if (!scroll_busy()) {
        step_ms = now;
        frac = 0;
        return 0;
    }

    /*
     * Пока палец на экране — не двигаем ничего сами.
     *
     * У края список уходит в резину, а пружина честно тянула его
     * обратно прямо под пальцем: держишь, а он ползёт. Список слушается
     * либо пальца, либо себя, но не обоих сразу.
     */
    if (touching) {
        step_ms = now;
        frac = 0;
        return 0;
    }

    dt = step_ms ? (int)(now - step_ms) : FRAME_MS;
    step_ms = now;
    if (dt < 1)
        dt = 1;
    if (dt > 64)                /* проспали дольше — не прыгаем через пол-экрана */
        dt = 64;

    /*
     * Пружина сильнее полёта: за краем список только возвращается.
     *
     * Цель пальца обновляем ПОСЛЕ каждого шага, а не до. Сначала было до
     * — и список после первого же отскока начинал дёргаться на месте
     * навсегда: пружина тянула его к краю, а подтягивание к пальцу
     * возвращало на прежнее место, потому что цель осталась старой.
     * Снаружи это выглядело как «прокрутка перестала работать».
     */
    if (out_of_bounds()) {
        edge = (scroll < 0) ? 0 : scroll_max;
        d = edge - scroll;
        fling = 0;

        if (d > -3 && d < 3) {
            scroll = edge;
            scroll_want = scroll;
            return 1;
        }

        /* Шаг — доля оставшегося пути: у самого края движение
         * замедляется само, а не обрывается на полпути */
        d = d * SPRING * dt / (100 * FRAME_MS);
        scroll += d ? d : (edge > scroll ? 1 : -1);
        scroll_want = scroll;
        return 1;
    }

    /*
     * Двигаем с накоплением остатка.
     *
     * Скорость в пикселях в секунду, время в миллисекундах, а координата
     * целая: при 200 px/с за кадр набегает три с лишним пикселя, и
     * дробная часть каждый раз пропадала. На глаз это ступеньки — список
     * то стоит, то прыгает. Остаток теперь переносится в следующий кадр.
     */
    {
        int move = fling * dt + frac;

        scroll -= move / 1000;
        frac = move % 1000;
        scroll_want = scroll;
    }

    /* Затухание по миллисекундам: сколько прошло, столько раз и гасим */
    for (int i = 0; i < dt; i++)
        fling = fling * DECAY / 1000;

    /*
     * Долетели до края на скорости — пусть выскочит за него и вернётся
     * пружиной. Резко обрывать движение нельзя: именно по отскоку глаз
     * понимает, что список кончился, а не застрял.
     */
    if (scroll < -RUBBER_MAX) {
        scroll = -RUBBER_MAX;
        fling = 0;
    } else if (scroll > scroll_max + RUBBER_MAX) {
        scroll = scroll_max + RUBBER_MAX;
        fling = 0;
    }

    if (fling < FLICK_MIN && fling > -FLICK_MIN)
        fling = 0;

    return 1;
}

/* --- Разбор касаний ------------------------------------------------- */

static void vsample(u32 y, u64 ms)
{
    if (vn < VSAMPLES) {
        vy[vn] = y;
        vt[vn] = ms;
        vn++;
        return;
    }
    for (u32 i = 1; i < VSAMPLES; i++) {
        vy[i - 1] = vy[i];
        vt[i - 1] = vt[i];
    }
    vy[VSAMPLES - 1] = y;
    vt[VSAMPLES - 1] = ms;
}

/*
 * Скорость броска — по последним точкам, а не по всему жесту.
 *
 * Важно, как палец двигался в конце: медленно провёл и резко дёрнул
 * напоследок — список должен полететь. По двум соседним событиям
 * получается дёрганно (между ними бывает три миллисекунды и один
 * пиксель), поэтому берём окно из нескольких точек.
 */
static int gesture_velocity(void)
{
    u64 dt;
    int v;

    if (vn < 2)
        return 0;

    dt = vt[vn - 1] - vt[0];
    if (!dt)
        return 0;

    v = ((int)vy[vn - 1] - (int)vy[0]) * 1000 / (int)dt;
    if (v > FLICK_MAX)
        v = FLICK_MAX;
    if (v < -FLICK_MAX)
        v = -FLICK_MAX;

    return v;
}

/*
 * Запустить приложение.
 *
 * Второй раз запускать не даём: окно у программы одно, слоёв под окна
 * два, и десяток копий одного приложения не нужен никому. Если оно уже
 * работает — значит его просто спрятали полоской «домой», и вернуть его
 * той же полоской и надо.
 */
static void launch_app(void)
{
    s64 id;

    /*
     * Уже запущенное — вернуть на экран, а не молчать. Нажимая на
     * приложение, человек хочет его увидеть; узнавать от системы, что
     * оно «и так работает», ему незачем.
     */
    if (app_task) {
        if (window_raise(app_task) == 0) {
            write("EL0      : ОБОЛОЧКА: ВЕРНУЛА ПРИЛОЖЕНИЕ НА ЭКРАН\n");
            return;
        }
        app_task = 0;           /* окна нет — значит программы больше нет */
    }

    id = spawn(IMG_HITTIS);
    if (id > 0) {
        app_task = (u64)id;
        write("EL0      : ОБОЛОЧКА: ЗАПУСТИЛА ПРИЛОЖЕНИЕ\n");
    } else {
        write("EL0      : ОБОЛОЧКА: ПРИЛОЖЕНИЕ НЕ ЗАПУСТИЛОСЬ\n");
    }
}

static void on_down(const struct touch *t)
{
    touches++;

    /*
     * Касание во время полёта только останавливает список и ничего не
     * выбирает. Так ведёт себя всякий приличный список: палец ловит
     * убегающий текст, а не открывает то, на что случайно попал.
     */
    stopped_fling = scroll_busy();
    fling = 0;

    {
        int b = btn_at(t->x, t->y);

        if (b >= 0) {
            if (armed == b && b == BTN_REBOOT) {
                write("EL0      : ОБОЛОЧКА: ПЕРЕЗАГРУЗКА ПО КНОПКЕ\n");
                reboot();
            }
            if (armed == b && b == BTN_OFF) {
                write("EL0      : ОБОЛОЧКА: ВЫКЛЮЧЕНИЕ ПО КНОПКЕ\n");
                /*
                 * Возвращается только при неудаче, и код говорит, какой
                 * именно: -3 значит «мешает кабель». Показать причину
                 * важнее, чем показать сам факт отказа: с кабелем
                 * человек может что-то сделать, с общим «не вышло» —
                 * ничего.
                 */
                off_rc = (int)power_off();
                if (!off_rc)
                    off_rc = -1;
                armed = -1;
                return;
            }
            armed = b;
            off_rc = 0;
            armed_at = uptime_ms();
            return;
        }
    }

    if (armed >= 0)             /* нажали мимо — снимаем взвод */
        armed = -1;

    touching = 1;
    dragging = 0;
    down_x = t->x;
    down_y = t->y;
    down_scroll = scroll;
    scroll_want = scroll;
    down_ms = uptime_ms();
    vn = 0;
    vsample(t->y, down_ms);
    pressed = stopped_fling ? -1 : tile_at(t->x, t->y);
}

static void on_move(const struct touch *t)
{
    int dy;

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

        /*
         * Отсчёт начинаем ЗАНОВО, от этой точки.
         *
         * Иначе к списку сразу приезжает весь путь, пройденный до
         * порога, — четырнадцать пикселей одним прыжком. Особенно заметно,
         * когда докручиваешь: палец опустился, чуть сдвинулся, и список
         * дёрнулся, вместо того чтобы плавно пойти следом. Порог нужен,
         * чтобы отличить нажатие от ведения, а не чтобы копить смещение.
         */
        down_y = t->y;
        down_scroll = scroll;
        dy = 0;
        vn = 0;
        vsample(t->y, uptime_ms());
    }

    if (!dragging)
        return;

    scroll_to(down_scroll - dy);
    vsample(t->y, uptime_ms());
}

static void on_up(const struct touch *t)
{
    int was = pressed;

    if (!touching) {
        pressed = -1;
        return;
    }
    touching = 0;
    pressed = -1;

    if (dragging) {
        vsample(t->y, uptime_ms());
        fling = gesture_velocity();
        if (fling < FLICK_MIN && fling > -FLICK_MIN)
            fling = 0;          /* просто отпустили, а не бросили */
        dragging = 0;
        return;
    }

    /* Касание, которым поймали летящий список, ничего не выбирает */
    if (!stopped_fling && was >= 0 && tile_at(t->x, t->y) == was) {
        chosen = was;
        if (was == TILE_APP)
            launch_app();
    }
}

/* --- Точка входа ---------------------------------------------------- */

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    u64 shown_sec;

    pressed = -1;
    chosen = -1;
    armed = -1;         /* у программы в EL0 не бывает начальных значений */

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
        int moving = scroll_busy() || scroll != scroll_want;
        int redraw = 0;

        /*
         * Ждём пальца, а когда список движется — почти не ждём.
         *
         * Здесь была потеря половины кадров. Стояло ожидание в целый
         * кадр: пока список летит, событий нет, цикл честно высыпал
         * шестнадцать миллисекунд и только потом принимался рисовать —
         * а рисование с показом стоят ещё шестнадцать. Тридцать две
         * миллисекунды на кадр, тридцать кадров в секунду вместо
         * шестидесяти, и именно это выглядело дёрганым.
         *
         * Ждать незачем: показ и так привязан к развёртке панели и сам
         * держит нас ровно на шестидесяти кадрах. Миллисекунда нужна
         * только чтобы забрать событие, если оно уже пришло.
         */
        got = moving ? input_wait(&t, 1) : input(&t);

        /*
         * Палец, о котором давно ничего не слышно, считаем отпущенным.
         *
         * Событие «отпустили» может не дойти — потеряться в очереди,
         * уйти системе, не случиться вовсе. Программа, которая на него
         * рассчитывает безоговорочно, залипает навсегда: считает, что
         * палец на экране, перерисовывается без остановки и не выбирает
         * ничего. Так и было, и лечить это только в ядре мало —
         * приличная программа должна переживать потерю события.
         */
        if (touching && uptime_ms() - last_seen_ms > 500) {
            struct touch fake;

            fake.x = (u16)down_x;
            fake.y = (u16)down_y;
            fake.id = 0;
            fake.action = TOUCH_UP;
            on_up(&fake);
            redraw = 1;
        }

        if (got == 1) {
            last_seen_ms = uptime_ms();
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
        if (follow_finger())
            redraw = 1;

        /* Взведённая кнопка гаснет сама: иначе случайное касание через
         * час перезагрузило бы телефон. */
        if (armed >= 0 && uptime_ms() - armed_at > BTN_ARM_MS) {
            armed = -1;
            redraw = 1;
        }

        if (uptime_ms() / 1000 != shown_sec) {
            shown_sec = uptime_ms() / 1000;
            redraw = 1;
        }

        if (redraw) {
            u64 t0 = uptime_ms();
            u64 t1;

            draw_all();
            t1 = uptime_ms();
            show();
            draw_sum += t1 - t0;

            /*
             * Раз в секунду говорим, во сколько обходится кадр. Спорить
             * о плавности словами бессмысленно: либо кадр укладывается в
             * шестнадцать миллисекунд, либо нет, и это видно числом.
             */
            frame_sum += uptime_ms() - t0;
            frame_count++;
            if (frame_count >= 30 && uptime_ms() - frame_told > 1000) {
                frame_told = uptime_ms();
                ulabel(line, "EL0      : ОБОЛОЧКА: КАДР ",
                       frame_sum / frame_count);
                {
                    const char *mid = ", ИЗ НИХ РИСОВАНИЕ ";
                    u32 n = ustrlen(line);

                    for (u32 k = 0; mid[k]; k++)
                        line[n++] = mid[k];
                    unum(line + n, draw_sum / frame_count);
                }
                line[ustrlen(line)] = 10;
                line[ustrlen(line) + 1] = 0;
                write(line);
                frame_sum = 0;
                draw_sum = 0;
                frame_count = 0;
            }
        }
    }
}
