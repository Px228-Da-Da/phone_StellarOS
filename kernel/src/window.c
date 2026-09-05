/*
 * window.c — окно программы на экране.
 *
 * Три вещи, которые здесь важнее остального.
 *
 * Память окна некэшируемая. Читает её не процессор, а контроллер
 * дисплея, и о кэшах он не знает ничего: оставь буфер кэшируемым — и
 * пиксели осядут в кэше, а на экране останется прошлое. Пометка
 * односторонняя, вернуть страницы в общий котёл после этого нельзя,
 * поэтому буферы заведены раз и навсегда и переиспользуются.
 *
 * Регистры слоёв правятся только в промежутке между кадрами. Правило
 * не наше: контроллер читает их прямо во время вывода, и запись
 * посреди кадра видна на экране как разрыв или как удвоенный слой.
 * Окно между кадрами короткое, поэтому мало дождаться его — надо ещё
 * закрыть прерывания и переспросить, не начался ли уже следующий кадр.
 *
 * Слой у окна один и тот же — третий. Нулевой занят основным кадром,
 * первый и второй — оболочкой. Это значит, что окон на экране может быть
 * ровно одно; второму придётся ждать своей очереди или обойтись
 * копированием в кадр.
 */
#include "window.h"
#include "mmu.h"
#include "pmm.h"
#include "ovl.h"
#include "fb.h"
#include "sched.h"
#include "print.h"
#include "string.h"
#include "spinlock.h"
#include "io.h"

/* Адрес окна в пространстве программы: у всех один и тот же, потому что
 * пространства раздельные. Отступ от кода и стека — с запасом. */
#define WINDOW_UVA      0x0000001000200000UL

/* Слой оверлея под окно. Нулевой — кадр, первый и второй — оболочка. */
#define WINDOW_LAYER    3

struct window {
    u64  owner;             /* номер задачи; 0 — слот свободен      */
    void *buf;              /* буфер, он же физический адрес        */
    u32  w, h;
    u32  x, y;
    int  on_layer;          /* показано слоем оверлея, а не копией  */
    int  told_copy;         /* уже сказали, что идём копией         */
    u32  moves;             /* сколько раз переезжало по экрану     */
};

static struct spinlock win_lock = SPINLOCK_INIT("window");
static struct window windows[WINDOW_MAX];

/*
 * Кто сейчас занимает слой оверлея.
 *
 * Слой один, а окон может быть несколько. Первому пришедшему достаётся
 * аппаратная композиция, остальным — копирование в кадр: медленнее, но
 * работает. Без этого учёта второе окно просто перенастроило бы слой на
 * свой буфер, и первое исчезло бы с экрана без всякого объяснения.
 */
static u64 layer_owner;

/*
 * Взять буфер под окно.
 *
 * Буферы живут в своих слотах и не возвращаются постраничному
 * аллокатору: они некэшируемые, а пометка эта односторонняя. Первый
 * запрос слот и заводит, дальше он просто переиспользуется.
 */
static void *slot_buffer(struct window *win)
{
    if (win->buf)
        return win->buf;

    win->buf = pmm_alloc_dma(WINDOW_BYTES);
    if (!win->buf)
        return NULL;

    /* Читать эту память будет контроллер дисплея, мимо кэшей */
    mmu_set_range_nc((u64)(uintptr_t)win->buf, WINDOW_BYTES);
    return win->buf;
}

static struct window *window_of(u64 task)
{
    for (u32 i = 0; i < WINDOW_MAX; i++)
        if (windows[i].owner == task)
            return &windows[i];

    return NULL;
}

u64 window_open(u32 w, u32 h)
{
    struct window *win = NULL;
    u64 task = task_id();
    u64 flags;
    void *buf;

    if (!w || !h || (u64)w * h * 4 > WINDOW_BYTES)
        return 0;

    flags = spin_lock_irq(&win_lock);

    if (window_of(task)) {              /* второе окно одной задаче */
        spin_unlock_irq(&win_lock, flags);
        return 0;
    }

    for (u32 i = 0; i < WINDOW_MAX; i++) {
        if (!windows[i].owner) {
            win = &windows[i];
            break;
        }
    }

    if (!win) {
        spin_unlock_irq(&win_lock, flags);
        kprintf("ОКНО     : %s ПРОСИТ ОКНО, А СВОБОДНЫХ НЕТ\n", task_name());
        return 0;
    }

    win->owner = task;                  /* слот занят, дальше можно не спеша */
    spin_unlock_irq(&win_lock, flags);

    buf = slot_buffer(win);
    if (!buf) {
        win->owner = 0;
        return 0;
    }

    memset(buf, 0, (u64)w * h * 4);
    win->w = w;
    win->h = h;
    win->x = 0;
    win->y = 0;
    win->on_layer = 0;
    win->told_copy = 0;
    win->moves = 0;

    /*
     * Отображаем буфер программе некэшируемым — тем же, чем он помечен у
     * ядра. Разные пометки на одну память в двух отображениях
     * архитектура считает ошибкой программиста, а не поводом что-то
     * согласовать: писала бы программа через кэш, а контроллер читал
     * мимо него.
     */
    if (mmu_map_user(task_space(), WINDOW_UVA, (u64)(uintptr_t)buf,
                     WINDOW_BYTES, MMU_USER_FB) != 0) {
        win->owner = 0;
        return 0;
    }

    kprintf("ОКНО     : %s ОТКРЫЛА %ux%u ПО АДРЕСУ %p\n",
            task_name(), w, h, (void *)WINDOW_UVA);
    return WINDOW_UVA;
}

/*
 * Показать окно слоем оверлея.
 *
 * Три попытки поймать промежуток между кадрами; не сложилось — пишем как
 * есть. Увиденный раз в жизни шов лучше, чем программа, которая перестала
 * отвечать, — то же решение и по той же причине, что в оболочке.
 */
/*
 * Кому достанется слой.
 *
 * Слой один, а окон несколько, и раздавать его по очереди прихода — не
 * то же самое, что раздавать по надобности. Неподвижному окну слой не
 * нужен: оно рисуется раз и остаётся на месте, копия в кадр обходится
 * ему в одну отрисовку. А окно, которое ездит за пальцем, копией
 * оставляет за собой след из своих прежних положений — там кадр никто
 * не восстанавливает.
 *
 * Поэтому правило простое: движущееся забирает слой у неподвижного.
 * Обратно — нет: иначе два подвижных окна отбирали бы слой друг у друга
 * на каждом кадре.
 */
static struct window *layer_holder(void)
{
    for (u32 i = 0; i < WINDOW_MAX; i++)
        if (windows[i].owner && windows[i].owner == layer_owner)
            return &windows[i];

    return NULL;
}

static int may_take_layer(struct window *win)
{
    struct window *held;

    if (!layer_owner || layer_owner == win->owner)
        return 1;

    held = layer_holder();
    if (!held || held->moves || !win->moves)
        return 0;               /* держит подвижное или мы сами стоим */

    return 1;
}

/*
 * Записываем владельца ТОЛЬКО после удачной настройки слоя.
 *
 * Иначе владельцем становится тот, у кого ничего не получилось: там, где
 * слоёв нет вовсе, первое же окно объявляло бы слой занятым, а
 * остальные получали бы отказ и жалобу в лог на пустом месте.
 */
static void claim_layer(struct window *win)
{
    struct window *held = layer_holder();

    if (held && held != win) {
        held->on_layer = 0;     /* прежний владелец уходит на копию */
        held->told_copy = 0;
    }

    layer_owner = win->owner;
}

static int show_by_layer(struct window *win, u32 x, u32 y)
{
    int first;

    if (!may_take_layer(win))
        return -1;              /* слой у того, кому он нужнее */

    first = !win->on_layer;

    for (int attempt = 0; attempt < 3; attempt++) {
        u64 flags;

        fb_wait_frame_gap();
        flags = irq_save();
        if (!fb_frame_idle()) {
            irq_restore(flags);
            continue;
        }

        if (first) {
            if (ovl_layer_set(WINDOW_LAYER, win->buf, x, y,
                              win->w, win->h, win->w * 4, 255) != 0) {
                irq_restore(flags);
                return -1;
            }
            win->on_layer = 1;
            claim_layer(win);
        } else {
            ovl_layer_move(WINDOW_LAYER, x, y);
        }

        irq_restore(flags);
        return 0;
    }

    if (first) {
        if (ovl_layer_set(WINDOW_LAYER, win->buf, x, y,
                          win->w, win->h, win->w * 4, 255) != 0)
            return -1;
        win->on_layer = 1;
        claim_layer(win);
    } else {
        ovl_layer_move(WINDOW_LAYER, x, y);
    }

    return 0;
}

/*
 * Показать окно копированием в кадр.
 *
 * Запасной путь для эмулятора, где слоёв нет вовсе. Процессор копирует
 * каждый пиксель — на телефоне так делать не стоит, но иметь один и тот
 * же вызов рабочим везде дороже сэкономленных тактов: иначе программу с
 * окном нельзя было бы даже запустить без телефона.
 */
static int show_by_copy(struct window *win, u32 x, u32 y)
{
    u64 base;
    u32 sw, sh, stride;
    const u32 *src = win->buf;
    u32 *dst;

    fb_info(&base, &sw, &sh, &stride);
    if (!base || !sw)
        return -1;

    dst = (u32 *)(uintptr_t)base;

    for (u32 row = 0; row < win->h; row++) {
        u32 sy = y + row;

        if (sy >= sh)
            break;
        for (u32 col = 0; col < win->w; col++) {
            u32 sx = x + col;

            if (sx >= sw)
                break;
            dst[sy * stride + sx] = src[row * win->w + col];
        }
    }

    return 0;
}

int window_present(u32 x, u32 y)
{
    struct window *win = window_of(task_id());
    int rc;

    if (!win || !win->buf)
        return -1;

    if (win->x != x || win->y != y)
        win->moves++;

    rc = show_by_layer(win, x, y);
    if (rc != 0) {
        /* Сказать один раз: слой один на всех, и кто именно его занял —
         * это то, чего иначе не видно ни в логе, ни на экране. */
        if (!win->told_copy) {
            /* Причины две, и они разные: слой может быть занят соседом,
             * а может не существовать вовсе — как в эмуляторе. Путать
             * их в отчёте значит искать потом несуществующего соседа. */
            kprintf("ОКНО     : %s ПОКАЗЫВАЕТСЯ КОПИЕЙ В КАДР (%s)\n",
                    task_name(),
                    layer_owner ? "СЛОЙ ЗАНЯТ" : "СЛОЁВ НЕТ");
            win->told_copy = 1;
        }
        rc = show_by_copy(win, x, y);
    }

    if (rc == 0) {
        win->x = x;
        win->y = y;
    }

    return rc;
}

int window_text(u32 x, u32 y, u32 scale, u32 fg, const char *s)
{
    struct window *win = window_of(task_id());

    if (!win || !win->buf || !scale || scale > 8)
        return -1;

    /* Фон прозрачный: программа сама решает, чем заливать окно, и
     * затирать её работу прямоугольником под каждой строкой мы не
     * вправе. */
    fb_text_to(win->buf, win->w, win->w, win->h, x, y, scale, fg, 0, s);
    return 0;
}

/*
 * Общая часть закрытия: снять со слоя и освободить слот.
 *
 * Отдельной функцией, потому что закрывают окно двое — сама программа и
 * сборщик за завершившейся, — и делать это они обязаны одинаково.
 * Отображение снимает вызывающий: у программы оно своё и текущее, а у
 * сборщика — чужое, взятое из задачи.
 */
static void window_release(struct window *win, u64 task)
{
    if (win->on_layer)
        ovl_layer_off(WINDOW_LAYER);
    if (layer_owner == task)
        layer_owner = 0;        /* слой освободился для соседей */

    win->on_layer = 0;
    win->told_copy = 0;
    win->moves = 0;
    win->owner = 0;
}

int window_close(void)
{
    u64 task = task_id();
    u64 space = task_space();
    struct window *win;
    u64 flags = spin_lock_irq(&win_lock);

    win = window_of(task);
    if (!win) {
        spin_unlock_irq(&win_lock, flags);
        return -1;
    }

    window_release(win, task);
    spin_unlock_irq(&win_lock, flags);

    /* Отображение снимаем последним: пока оно есть, программа ещё может
     * писать в буфер, который уже никому не показывается, — это лучше,
     * чем наоборот. */
    if (space)
        mmu_unmap_user(space, WINDOW_UVA, WINDOW_BYTES);

    return 0;
}

void window_task_gone(u64 task, u64 ttbr0)
{
    struct window *win;
    u64 flags = spin_lock_irq(&win_lock);

    win = window_of(task);
    if (!win) {
        spin_unlock_irq(&win_lock, flags);
        return;
    }

    window_release(win, task);
    spin_unlock_irq(&win_lock, flags);

    /*
     * Снять отображение обязательно, и именно здесь. Сборщик сейчас
     * разберёт пространство программы и вернёт постраничному аллокатору
     * всё, что в нём отображено, — вместе с этим буфером, которому туда
     * нельзя: он некэшируемый навсегда.
     */
    if (ttbr0)
        mmu_unmap_user(ttbr0, WINDOW_UVA, WINDOW_BYTES);
}
