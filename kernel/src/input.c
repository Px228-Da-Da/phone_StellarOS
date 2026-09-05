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
#include "sched.h"
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

/*
 * Очереди программ.
 *
 * Маленькие: касание — событие быстрое, и программа, которая не
 * забирает его сотню штук подряд, всё равно уже безнадёжно отстала.
 * Переполнение теряет новое событие, как и в общей очереди.
 */
#define SUB_MAX     4
#define SUB_RING    16

static struct {
    u64 owner;                          /* 0 — слот свободен */
    u32 head, tail;
    struct input_event ring[SUB_RING];
} subs[SUB_MAX];

/*
 * Разложить событие по очередям программ. Под общим замком ввода.
 *
 * Ведение пальцем складывается ОСОБЫМ образом: если в очереди последним
 * лежит ещё не забранное «ведут», оно заменяется новым, а не копится.
 *
 * Причина в том, что ведение — это состояние, а не история. Программе
 * нужно, где палец сейчас, а не где он побывал: показать окно она всё
 * равно может не чаще кадра, а касания приходят сто раз в секунду. Без
 * замены очередь набивается, и окно едет по адресам, которые палец
 * прошёл секунду назад — то самое «идёт с задержкой».
 *
 * Нажатие и отпускание не заменяются никогда: их пропуск меняет смысл
 * происходящего, а не точность.
 */
static void subs_push_locked(const struct input_event *e)
{
    for (u32 i = 0; i < SUB_MAX; i++) {
        u32 next;

        if (!subs[i].owner)
            continue;

        if (e->action == TOUCH_MOVE && subs[i].head != subs[i].tail) {
            u32 last = (subs[i].head - 1) & (SUB_RING - 1);

            if (subs[i].ring[last].action == TOUCH_MOVE &&
                subs[i].ring[last].id == e->id) {
                subs[i].ring[last] = *e;
                continue;
            }
        }

        next = (subs[i].head + 1) & (SUB_RING - 1);
        if (next == subs[i].tail) {
            input_dropped++;
            continue;
        }
        subs[i].ring[subs[i].head] = *e;
        subs[i].head = next;
    }
}

int input_pop_task(u64 task, struct input_event *e)
{
    u64 flags = spin_lock_irq(&input_lock);
    int got = 0;

    for (u32 i = 0; i < SUB_MAX; i++) {
        if (subs[i].owner != task)
            continue;
        if (subs[i].tail != subs[i].head) {
            *e = subs[i].ring[subs[i].tail];
            subs[i].tail = (subs[i].tail + 1) & (SUB_RING - 1);
            got = 1;
        }
        spin_unlock_irq(&input_lock, flags);
        return got;
    }

    /* Первое обращение: заводим очередь и уходим ни с чем — то, что
     * случилось до подписки, было без нас. */
    for (u32 i = 0; i < SUB_MAX; i++) {
        if (subs[i].owner)
            continue;
        subs[i].owner = task;
        subs[i].head = subs[i].tail = 0;
        break;
    }

    spin_unlock_irq(&input_lock, flags);
    return 0;
}

void input_unsubscribe(u64 task)
{
    u64 flags = spin_lock_irq(&input_lock);

    for (u32 i = 0; i < SUB_MAX; i++)
        if (subs[i].owner == task)
            subs[i].owner = 0;

    spin_unlock_irq(&input_lock, flags);
}

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

    struct input_event ev;

    ev.id = id;
    ev.action = action;
    ev.x = x;
    ev.y = y;

    ring[ring_head] = ev;
    ring_head = next;

    /* И каждому подписчику свою копию: они забирают из своих очередей,
     * а не наперегонки из этой. */
    subs_push_locked(&ev);
    spin_unlock_irq(&input_lock, flags);

    /* Разбудить тех, кто спит в ожидании касания. Замок очереди событий
     * к этому моменту отпущен: будить, держа его, значило бы взять замок
     * планировщика поверх нашего — а этот порядок в другом месте может
     * оказаться обратным, и получилась бы взаимная блокировка. */
    sched_wake(INPUT_CHAN);
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
