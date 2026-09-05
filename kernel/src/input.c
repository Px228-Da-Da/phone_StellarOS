/*
 * Ввод: опрос тачскрина в задаче и очередь событий.
 *
 * Опрашиваем сто раз в секунду. Реже — движение пальца становится рваным,
 * чаще — бессмысленно: контроллер обновляет данные с частотой кадра, и на
 * лишние обращения отвечает единицами. Проверено: без выдержки цикл
 * успевал под сто тысяч опросов в секунду, и между редкими удачными
 * чтениями шла сплошная единица.
 *
 * По-хорошему опрашивать надо не по времени, а по линии прерывания — она
 * для того и заведена. Но GPIO1 у нас читается нулём постоянно, поэтому
 * пока по времени; когда разберёмся с блоком внешних прерываний, менять
 * придётся только эту задачу.
 */
#include "input.h"
#include "nvt.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"
#include "io.h"
#include "print.h"

#define EVENT_RING  64      /* степень двойки: маска вместо деления */
#define MAX_FINGERS 10

static struct spinlock input_lock = SPINLOCK_INIT("input");
static struct input_event ring[EVENT_RING];
static u32 ring_head, ring_tail;

u32 input_dropped;

static void event_push(u8 id, u8 action, u16 x, u16 y)
{
    u32 next = (ring_head + 1) & (EVENT_RING - 1);
    u64 flags = spin_lock_irq(&input_lock);

    /*
     * Переполнение: теряем НОВОЕ событие, а не старое.
     *
     * У вывода в консоль правило обратное — там ценнее свежее. Здесь
     * наоборот: события связаны между собой, и если выбросить «нажали»,
     * оставив «отпустили», потребитель получит бессмыслицу.
     */
    if (next == ring_tail) {
        input_dropped++;
        spin_unlock_irq(&input_lock, flags);
        return;
    }

    ring[ring_head].id = id;
    ring[ring_head].action = action;
    ring[ring_head].x = x;
    ring[ring_head].y = y;
    ring_head = next;
    spin_unlock_irq(&input_lock, flags);
}

int input_pop(struct input_event *e)
{
    u64 flags = spin_lock_irq(&input_lock);
    int got = 0;

    if (ring_tail != ring_head) {
        *e = ring[ring_tail];
        ring_tail = (ring_tail + 1) & (EVENT_RING - 1);
        got = 1;
    }
    spin_unlock_irq(&input_lock, flags);
    return got;
}

/*
 * Состояние прошлого опроса.
 *
 * Контроллер отдаёт срез: какие пальцы сейчас на стекле. Переходы
 * считаем сами — палец, которого не было и он появился, даёт «нажали»;
 * был и пропал — «отпустили».
 */
static u8  prev_down[MAX_FINGERS + 1];
static u16 prev_x[MAX_FINGERS + 1];
static u16 prev_y[MAX_FINGERS + 1];

static void input_scan(void)
{
    struct nvt_touch t[MAX_FINGERS];
    u8 now_down[MAX_FINGERS + 1] = { 0 };
    int n = nvt_get_touches(t, MAX_FINGERS);

    if (n < 0)
        return;                 /* сбой чтения: молчим, следующий опрос через 10 мс */

    for (int i = 0; i < n; i++) {
        u8 id = t[i].id;

        if (id == 0 || id > MAX_FINGERS)
            continue;
        now_down[id] = 1;

        if (!prev_down[id])
            event_push(id, TOUCH_DOWN, t[i].x, t[i].y);
        else if (t[i].x != prev_x[id] || t[i].y != prev_y[id])
            event_push(id, TOUCH_MOVE, t[i].x, t[i].y);

        prev_x[id] = t[i].x;
        prev_y[id] = t[i].y;
    }

    for (u8 id = 1; id <= MAX_FINGERS; id++) {
        if (prev_down[id] && !now_down[id])
            event_push(id, TOUCH_UP, prev_x[id], prev_y[id]);
        prev_down[id] = now_down[id];
    }
}

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        input_scan();
        /* Спим до следующего опроса, а не уступаем в пустом цикле:
         * уступка вернула бы задачу в очередь готовых, и с высоким
         * приоритетом она забрала бы всё время себе, ничего не делая. */
        task_sleep_ms(10);      /* сто опросов в секунду */
    }
}

void input_start(void)
{
    /* Приоритет выше обычного: между касанием и откликом не должно
     * вклиниваться ничего, кроме собственно опроса. */
    task_create_prio("ввод", input_task, NULL, TASK_PRIO_UI);
}
