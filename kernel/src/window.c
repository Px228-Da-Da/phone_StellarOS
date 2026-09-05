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

/*
 * Слои оверлея под окна.
 *
 * Их четыре. Нулевой занят основным кадром, первый — подсветкой плитки
 * в оболочке, а второй и третий отданы окнам программ: точку под
 * пальцем, ради которой оболочка держала второй, теперь рисует
 * программа в EL0, и держать его дальше значило бы делать одну работу
 * дважды.
 *
 * Два слоя — это два окна, которые видны без единого копирования
 * пикселей. Третьему и следующим достаётся копирование в кадр.
 */
#define WINDOW_LAYER_FIRST  2
#define WINDOW_LAYER_LAST   3
#define OVL_LAYERS          4

struct window {
    u64  owner;             /* номер задачи; 0 — слот свободен      */
    void *buf;              /* буфер, он же физический адрес        */
    u32  w, h;
    u32  x, y;
    int  layer;             /* номер занятого слоя или -1           */
    int  told_copy;         /* уже сказали, что идём копией         */
    u32  moves;             /* сколько раз переезжало по экрану     */
};

static struct spinlock win_lock = SPINLOCK_INIT("window");
static struct window windows[WINDOW_MAX];

/* Кто какой слой занимает: индекс — номер слоя, значение — задача */
static u64 layer_taken[OVL_LAYERS];

/*
 * Есть ли на этой плате слои вообще.
 *
 * Выясняется первой же попыткой: в эмуляторе их нет, и там окна
 * показываются копированием в кадр. Различать это важно не ради
 * отчёта — от ответа зависит, показывать ли копией ПОДВИЖНОЕ окно.
 */
static int layers_known, layers_exist;

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
    win->layer = -1;
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

    /*
     * Окно есть — значит программа показывает что-то человеку и ждёт
     * его касаний. Такой задаче процессор нужен раньше, чем счётным:
     * иначе окно едет за пальцем с задержкой в квант, а то и больше.
     */
    task_set_prio(TASK_PRIO_UI);

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
 * Слоёв два, окон может быть больше. Свободный берём сразу; если
 * свободных нет, движущееся окно забирает слой у неподвижного.
 * Неподвижному слой не нужен: оно рисуется раз и остаётся на месте,
 * копия в кадр обходится ему в одну отрисовку. А окно, которое ездит за
 * пальцем, копией оставляет за собой след из прежних положений — кадр
 * там никто не восстанавливает.
 *
 * Обратной замены нет: иначе два подвижных окна отбирали бы слой друг у
 * друга на каждом кадре.
 */
static struct window *holder_of(int layer)
{
    for (u32 i = 0; i < WINDOW_MAX; i++)
        if (windows[i].owner && windows[i].owner == layer_taken[layer])
            return &windows[i];

    return NULL;
}

static int pick_layer(struct window *win)
{
    if (win->layer >= 0)
        return win->layer;

    for (int l = WINDOW_LAYER_FIRST; l <= WINDOW_LAYER_LAST; l++)
        if (!layer_taken[l])
            return l;

    if (!win->moves)
        return -1;              /* сами стоим — обойдёмся копией */

    for (int l = WINDOW_LAYER_FIRST; l <= WINDOW_LAYER_LAST; l++) {
        struct window *held = holder_of(l);

        if (!held || held->moves)
            continue;
        return l;               /* отберём у неподвижного */
    }

    return -1;
}

/*
 * Записываем владельца ТОЛЬКО после удачной настройки слоя.
 *
 * Иначе владельцем становится тот, у кого ничего не получилось: там, где
 * слоёв нет вовсе, первое же окно объявляло бы слой занятым, а
 * остальные получали бы отказ и жалобу в лог на пустом месте.
 */
static void claim_layer(struct window *win, int layer)
{
    struct window *held = holder_of(layer);

    if (held && held != win)
        held->layer = -1;       /* прежний владелец уходит на копию */

    layer_taken[layer] = win->owner;
    win->layer = layer;
    layers_known = 1;
    layers_exist = 1;
}

static int show_by_layer(struct window *win, u32 x, u32 y)
{
    int layer = pick_layer(win);
    int first = (win->layer < 0);

    if (layer < 0)
        return -1;              /* слои у тех, кому они нужнее */

    for (int attempt = 0; attempt < 3; attempt++) {
        u64 flags;

        fb_wait_frame_gap();
        flags = irq_save();
        if (!fb_frame_idle()) {
            irq_restore(flags);
            continue;
        }

        if (first) {
            if (ovl_layer_set(layer, win->buf, x, y,
                              win->w, win->h, win->w * 4, 255) != 0) {
                irq_restore(flags);
                layers_known = 1;   /* слоёв тут нет */
                return -1;
            }
            claim_layer(win, layer);
        } else {
            ovl_layer_move(layer, x, y);
        }

        irq_restore(flags);
        return 0;
    }

    /* Три кадра подряд не поймали промежуток — пишем как есть. Увиденный
     * раз в жизни шов лучше, чем программа, переставшая отвечать. */
    if (first) {
        if (ovl_layer_set(layer, win->buf, x, y,
                          win->w, win->h, win->w * 4, 255) != 0) {
            layers_known = 1;
            return -1;
        }
        claim_layer(win, layer);
    } else {
        ovl_layer_move(layer, x, y);
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

    /*
     * Копией показываем не всегда.
     *
     * Там, где слоёв нет, копия — единственный способ хоть что-то
     * показать. Но там, где они есть и просто заняты, копией нельзя
     * показывать ПОДВИЖНОЕ окно: за ним останется след из прежних
     * положений, потому что кадр под ним никто не восстанавливает.
     * Лучше не показать ничего и сказать об этом программе, чем
     * измазать экран — тем более что слой освободится, как только
     * сосед закончит.
     */
    if (rc != 0) {
        if (layers_known && layers_exist && win->moves) {
            if (!win->told_copy) {
                kprintf("ОКНО     : %s ЖДЁТ СЛОЙ: ПОДВИЖНОЕ ОКНО КОПИЕЙ НЕ ПОКАЗЫВАЕМ\n",
                        task_name());
                win->told_copy = 1;
            }
            return -1;
        }

        if (!win->told_copy) {
            kprintf("ОКНО     : %s ПОКАЗЫВАЕТСЯ КОПИЕЙ В КАДР (%s)\n",
                    task_name(),
                    layers_exist ? "СЛОИ ЗАНЯТЫ" : "СЛОЁВ НЕТ");
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
    (void)task;

    if (win->layer >= 0) {
        ovl_layer_off(win->layer);
        layer_taken[win->layer] = 0;
        win->layer = -1;
    }

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
