/*
 * sched.c — очередь готовых задач и переключение между ними.
 *
 * Самое тонкое место здесь — момент переключения. Наивная схема
 * «выбрал задачу, отпустил замок, переключился» ломается на многих ядрах:
 * пока мы ещё не сохранили контекст уходящей задачи, другое ядро уже видит
 * её в очереди готовых, забирает и начинает исполнять — на том же стеке.
 * Две копии одной задачи на одном стеке — это мгновенная порча памяти.
 *
 * Поэтому замок очереди удерживается ЧЕРЕЗ переключение и снимается уже
 * в новом контексте: тем, кого мы разбудили. Отсюда и sched_after_switch()
 * в двух местах — в schedule() после ctx_switch и в task_start для задач,
 * которым возвращаться ещё некуда.
 */
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "kmalloc.h"
#include "pmm.h"
#include "timer.h"
#include "mmu.h"
#include "print.h"
#include "io.h"

#define TASK_STACK_PAGES    2       /* 8 КБ на задачу */

struct task {
    u64 sp;                     /* сохранённый указатель стека           */
    u64 id;
    const char *name;
    u32 state;
    u32 prio;                   /* больше — важнее, см. sched.h          */
    u64 wake_ms;                /* когда будить, если спит               */
    u64 wait_chan;              /* чего ждёт, если ждёт события          */
    u32 slice_used;             /* сколько тиков задача уже отработала   */
    u64 slices;                 /* сколько раз получала процессор        */
    u64 last_cpu;
    u64 ttbr0;                  /* своё адресное пространство; 0 — ядра  */
    void *stack;
    struct task *next;          /* следующая в очереди готовых           */
    struct task *all_next;      /* список всех задач, для диагностики    */
};

/* Переключатель контекста, switch.S */
extern void ctx_switch(u64 *save_sp, u64 new_sp);
extern void task_start(void);

static struct spinlock rq_lock = SPINLOCK_INIT("runqueue");
static struct task *rq_head;
static struct task *rq_tail;
static struct task *all_head;
static struct task *zombie_head;    /* отработали, ждут разбора          */

/*
 * Журнал завершившихся.
 *
 * Ждущему нужен код завершения, а самой задачи к моменту вопроса может
 * уже не быть — её разобрал сборщик. Держать ради этого зомби до тех
 * пор, пока кто-нибудь не спросит, нельзя: спросить могут никогда, и
 * тогда память не вернётся. Кольцевой журнал решает обе задачи разом:
 * ответ переживает саму задачу, но не навсегда.
 */
#define EXIT_LOG_SIZE   32

static struct {
    u64 id;
    u64 code;
    int filled;
} exit_log[EXIT_LOG_SIZE];
static u32 exit_log_next;
static u64 next_id;
static u64 task_count;

/* Разбудить ждущих канала — определена ниже, а нужна уже в записи
 * кода завершения */
static void wake_chan_locked(u64 chan);

static void rq_push(struct task *t)
{
    t->next = NULL;
    if (rq_tail)
        rq_tail->next = t;
    else
        rq_head = t;
    rq_tail = t;
}

/*
 * Забрать самую важную из готовых.
 *
 * Раньше очередь была честно круговой: кто первый встал, тот и пошёл. Для
 * счётных задач это правильно, а для интерфейса — нет. Оболочка отдаёт
 * процессор сразу, как разобрала события, и уходит в конец очереди; пока
 * десяток счётных задач выберут по кванту, проходит полсекунды, и нажатие
 * срабатывает с заметной задержкой. Именно это и наблюдалось.
 *
 * Поэтому берём не первую, а самую приоритетную; среди равных —
 * по-прежнему первую, то есть круговой порядок внутри уровня сохраняется
 * и счётные задачи не голодают друг относительно друга.
 */
static struct task *rq_pop(void)
{
    struct task *best = NULL, *best_prev = NULL;
    struct task *prev = NULL, *t = rq_head;

    while (t) {
        if (!best || t->prio > best->prio) {
            best = t;
            best_prev = prev;
        }
        prev = t;
        t = t->next;
    }

    if (!best)
        return NULL;

    if (best_prev)
        best_prev->next = best->next;
    else
        rq_head = best->next;
    if (rq_tail == best)
        rq_tail = best_prev;
    best->next = NULL;
    return best;
}

/*
 * Разбудить тех, кому пора.
 *
 * Спящая задача не лежит в очереди готовых — иначе она получала бы
 * процессор только чтобы тут же его вернуть. Она ждёт в общем списке, и
 * сюда её возвращает время.
 *
 * Вызывается из schedule() под замком очереди. Отдельного таймера не
 * нужно: schedule() и так вызывается на каждом кванте и на каждой
 * уступке, а если работы нет вовсе — из холостого цикла по тику таймера.
 */
static void wake_sleepers(void)
{
    u64 now = timer_uptime_ms();

    for (struct task *t = all_head; t; t = t->all_next) {
        if (t->state != TASK_SLEEPING)
            continue;
        if (now < t->wake_ms)
            continue;
        t->state = TASK_READY;
        rq_push(t);
    }
}

void sched_init(void)
{
    rq_head = rq_tail = NULL;
    all_head = NULL;
    next_id = 0;
    task_count = 0;
}

/*
 * idle-задача ядра — это не отдельный поток, а обёртка над тем контекстом,
 * в котором ядро сейчас исполняется. При первом же переключении ctx_switch
 * сохранит в неё текущий стек, и вернуться в idle можно будет как в обычную
 * задачу. Своя память под стек ей не нужна: у неё есть загрузочный стек.
 */
void sched_init_cpu(void)
{
    struct cpu *c = this_cpu();
    struct task *idle = kzalloc(sizeof(*idle));

    if (!idle) {
        kprintf("SCHED    : НЕТ ПАМЯТИ ПОД IDLE ЯДРА %lu\n", c->id);
        return;
    }

    idle->name  = "idle";
    idle->state = TASK_RUNNING;
    idle->last_cpu = c->id;

    u64 flags = spin_lock_irq(&rq_lock);

    idle->id = next_id++;
    idle->all_next = all_head;
    all_head = idle;
    spin_unlock_irq(&rq_lock, flags);

    c->idle = idle;
    c->current = idle;
}

struct task *task_create(const char *name, task_fn fn, void *arg)
{
    return task_create_prio(name, fn, arg, TASK_PRIO_NORMAL);
}

struct task *task_create_prio(const char *name, task_fn fn, void *arg,
                              u32 prio)
{
    return task_create_space(name, fn, arg, prio, 0);
}

struct task *task_create_space(const char *name, task_fn fn, void *arg,
                               u32 prio, u64 ttbr0)
{
    struct task *t = kzalloc(sizeof(*t));
    u8 *stack;
    u64 *frame;

    if (!t)
        return NULL;

    stack = pmm_alloc_pages(TASK_STACK_PAGES);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    t->name  = name;
    t->prio  = prio;
    t->stack = stack;
    t->state = TASK_READY;
    t->ttbr0 = ttbr0;

    /*
     * Собираем кадр так, будто задача уже когда-то вызывала ctx_switch:
     * порядок слотов в точности повторяет stp из switch.S.
     * x19 — функция, x20 — аргумент, x30 — task_start, куда уйдёт ret.
     */
    frame = (u64 *)(stack + TASK_STACK_PAGES * PAGE_SIZE);
    frame -= 12;

    frame[0]  = (u64)(uintptr_t)fn;         /* x19 */
    frame[1]  = (u64)(uintptr_t)arg;        /* x20 */
    for (u32 i = 2; i < 11; i++)
        frame[i] = 0;                       /* x21..x29 */
    frame[11] = (u64)(uintptr_t)task_start; /* x30 */

    t->sp = (u64)(uintptr_t)frame;

    u64 flags = spin_lock_irq(&rq_lock);

    t->id = next_id++;
    t->all_next = all_head;
    all_head = t;
    task_count++;
    rq_push(t);
    spin_unlock_irq(&rq_lock, flags);

    return t;
}

/* Записать, чем кончилась задача. Зовётся с закрытыми прерываниями. */
static void remember_exit(u64 id, u64 code)
{
    u64 flags = spin_lock_irq(&rq_lock);

    exit_log[exit_log_next].id = id;
    exit_log[exit_log_next].code = code;
    exit_log[exit_log_next].filled = 1;
    exit_log_next = (exit_log_next + 1) % EXIT_LOG_SIZE;

    /* Под тем же замком, которым закрыта проверка у ждущих: иначе между
     * их проверкой и сном как раз и уместилось бы это пробуждение. */
    wake_chan_locked(id);
    spin_unlock_irq(&rq_lock, flags);
}

/* Обе проверки — под уже взятым замком: ими пользуется и ожидание */
static int find_exit_locked(u64 id, u64 *code)
{
    for (u32 i = 0; i < EXIT_LOG_SIZE; i++) {
        if (!exit_log[i].filled || exit_log[i].id != id)
            continue;
        if (code)
            *code = exit_log[i].code;
        return 1;
    }

    return 0;
}

static int alive_locked(u64 id)
{
    for (struct task *t = all_head; t; t = t->all_next)
        if (t->id == id && t->state != TASK_DONE)
            return 1;

    return 0;
}

int sched_exit_code(u64 id, u64 *code)
{
    u64 flags = spin_lock_irq(&rq_lock);
    int found = find_exit_locked(id, code);

    spin_unlock_irq(&rq_lock, flags);
    return found;
}

int sched_task_alive(u64 id)
{
    u64 flags = spin_lock_irq(&rq_lock);
    int alive = alive_locked(id);

    spin_unlock_irq(&rq_lock, flags);
    return alive;
}

u64 task_id_of(const struct task *t)
{
    return t ? t->id : 0;
}

/*
 * Принять завершившуюся задачу.
 *
 * Задача не может освободить себя сама: она стоит на своём стеке, и
 * отдать его аллокатору означало бы вырвать пол из-под ног — причём не
 * сразу, а когда эти страницы кто-нибудь займёт. Своё она отдать может
 * только чужими руками.
 *
 * Эстафета такая: уходящая задача помечается умершей и записывается в
 * поле ядра, а подобрать её обязана следующая — она уже исполняется на
 * своём стеке, значит чужой свободен наверняка. Делается это под тем же
 * замком очереди, который удерживается через переключение, поэтому
 * промежутка, в котором задачу видно и уже можно тронуть, не возникает.
 *
 * Освобождает не подобравший: он лишь перекладывает задачу в список к
 * сборщику. Освобождение — это работа с аллокаторами и таблицами
 * страниц, а мы здесь под замком и с закрытыми прерываниями.
 */
static void take_dying(void)
{
    struct cpu *c = this_cpu();
    struct task *d = c ? c->dying : NULL;

    if (!d)
        return;

    c->dying = NULL;

    /* Из общего списка убираем сразу: с этого момента задачи для системы
     * не существует — ни в отчётах, ни в пробуждении спящих. */
    for (struct task **pp = &all_head; *pp; pp = &(*pp)->all_next) {
        if (*pp == d) {
            *pp = d->all_next;
            break;
        }
    }

    d->all_next = zombie_head;
    zombie_head = d;
    if (task_count)
        task_count--;
}

/*
 * Снять замок очереди уже в новом контексте.
 *
 * Вызывается ТОЛЬКО из task_start — из точки входа задачи, которая
 * запускается впервые. Прерывания она разрешает себе сама: их маску
 * восстанавливать неоткуда и не из чего, задача начинается с чистого листа.
 */
void sched_after_switch(void)
{
    take_dying();
    spin_unlock(&rq_lock);
}

/*
 * Переключиться, когда замок очереди УЖЕ взят, а прерывания закрыты.
 *
 * Вынесено ради ожидания по событию. Ждущая задача обязана проверить
 * условие и уснуть, не отпуская замок: отпусти она его между проверкой и
 * сном — и разбудить её успели бы ровно в этот промежуток, а разбудить
 * ещё не спящего некого. Задача уснула бы навсегда, и ошибка эта редкая,
 * плавающая и почти неотлаживаемая.
 *
 * flags передаётся вызывающим: маска прерываний обязана лежать на стеке
 * той задачи, которая её сохранила, — см. хвост этой же функции.
 */
static void schedule_locked(u64 flags)
{
    struct cpu *c;
    struct task *prev, *next;

    c = this_cpu();
    prev = c->current;

    wake_sleepers();

    /* idle в очередь не возвращаем: она принадлежит ядру, а не очереди,
     * и попади она туда — другое ядро увело бы чужой загрузочный стек.
     * Спящую и ждущую тоже не возвращаем: первую вернёт время,
     * вторую — событие. */
    if (prev && prev != c->idle && prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        rq_push(prev);
    }

    next = rq_pop();
    if (!next)
        next = c->idle;

    /* Состояние и квант обновляем ДО проверки на «та же самая»: если
     * задача единственная, она сама себя и вытянула из очереди — и должна
     * снова считаться исполняющейся, иначе останется помеченной готовой,
     * хотя работает. Такое расхождение потом читается в дампе как загадка. */
    next->state = TASK_RUNNING;
    next->slice_used = 0;
    next->last_cpu = c->id;

    if (next == prev) {
        spin_unlock(&rq_lock);
        irq_restore(flags);
        return;
    }

    next->slices++;
    c->current = next;

    /* Уходящая насовсем — оставляем следующей, пусть подберёт: сами мы
     * стоим на том самом стеке, который надо отдать. */
    if (prev->state == TASK_DONE && prev != c->idle)
        c->dying = prev;

    /*
     * Смена адресного пространства.
     *
     * Делается ДО переключения контекста и только при разнице: у задач
     * ядра пространство одно на всех, и переписывать TTBR0 на каждом
     * переключении означало бы платить за то, чего не происходит.
     *
     * Сброса TLB здесь нет: пользовательские записи помечены номером
     * своего пространства (ASID), ядерные глобальны, и процессор сам
     * не возьмёт чужую.
     *
     * Порядок «сначала таблицы, потом стек» важен: после ctx_switch мы
     * уже исполняемся на стеке новой задачи, а он обязан быть отображён
     * в том пространстве, которое сейчас включено. Обязан он и в старом —
     * стеки задач ядерные, а ядерные отображения есть в каждом
     * пространстве, поэтому переход безопасен в любую сторону.
     */
    if (next->ttbr0 != prev->ttbr0)
        mmu_switch(next->ttbr0 ? next->ttbr0 : mmu_kernel_space());

    ctx_switch(&prev->sp, next->sp);

    /*
     * Сюда мы возвращаемся, когда нас выбрали снова — возможно, спустя
     * секунды и уже на другом ядре.
     *
     * Замок снимаем здесь, а flags — это НАША локальная переменная, лежащая
     * на НАШЕМ стеке: её сохранил тот вызов schedule(), из которого нас
     * когда-то вытеснили. Именно поэтому маску прерываний нельзя хранить
     * per-CPU: задача, уснувшая с разрешёнными прерываниями внутри
     * обработчика чужого таймера, проснулась бы с запрещёнными — и её ядро
     * больше никогда не получило бы ни одного прерывания.
     */
    take_dying();
    spin_unlock(&rq_lock);
    irq_restore(flags);
}

void schedule(void)
{
    u64 flags = irq_save();

    spin_lock(&rq_lock);
    schedule_locked(flags);
}

/*
 * Ожидание по событию.
 *
 * Канал — это просто число, о котором договорились ждущий и будящий; у
 * нас это номер задачи, конца которой ждут. Ничего больше от канала не
 * требуется: важно лишь, чтобы обе стороны назвали одно и то же.
 *
 * Проверка условия и засыпание идут под замком очереди, а пробуждение
 * берёт тот же замок — поэтому промежутка, в котором условие уже
 * выполнено, а задача ещё не спит, попросту нет.
 */
static void wait_on_locked(u64 chan)
{
    struct cpu *c = this_cpu();

    if (c->current && c->current != c->idle) {
        c->current->wait_chan = chan;
        c->current->state = TASK_WAITING;
    }
}

/* Разбудить всех, кто ждёт этого канала. Зовётся под замком очереди. */
static void wake_chan_locked(u64 chan)
{
    for (struct task *t = all_head; t; t = t->all_next) {
        if (t->state != TASK_WAITING || t->wait_chan != chan)
            continue;
        t->state = TASK_READY;
        t->wait_chan = 0;
        rq_push(t);
    }
}

int sched_wait_for(u64 id, u64 *code)
{
    for (;;) {
        u64 flags = irq_save();

        spin_lock(&rq_lock);

        if (find_exit_locked(id, code)) {
            spin_unlock(&rq_lock);
            irq_restore(flags);
            return 1;
        }

        if (!alive_locked(id)) {
            spin_unlock(&rq_lock);
            irq_restore(flags);
            return 0;               /* такой задачи нет и не было */
        }

        /* Засыпаем, не отпуская замок: разбудить нас можно только взяв
         * его, то есть не раньше, чем мы уснём по-настоящему. */
        wait_on_locked(id);
        schedule_locked(flags);

        /* Проснулись — и снова проверяем. Пробуждение означает
         * «посмотри», а не «готово»: разбудить могли и по другому
         * поводу. */
    }
}

void sched_tick(void)
{
    struct cpu *c = this_cpu();
    struct task *t = c->current;

    if (!t)
        return;

    /* idle уступает при первом же тике: если появилась работа, ждать нечего */
    if (t == c->idle) {
        c->resched = 1;
        return;
    }

    if (++t->slice_used >= TASK_SLICE_TICKS)
        c->resched = 1;
}

/*
 * Уснуть на заданное время.
 *
 * Задача, которой нечего делать до срока, обязана спать, а не уступать
 * процессор в пустом цикле. Уступка возвращает её в очередь готовых, и с
 * приоритетом выше среднего она бы забрала всё процессорное время себе,
 * ничего при этом не делая.
 */
void task_sleep_ms(u64 ms)
{
    struct cpu *c;
    u64 flags = irq_save();

    c = this_cpu();
    if (c->current && c->current != c->idle) {
        c->current->wake_ms = timer_uptime_ms() + ms;
        c->current->state = TASK_SLEEPING;
    }
    irq_restore(flags);
    schedule();
}

void task_exit(void)
{
    task_exit_code(0);
}

void task_exit_code(u64 code)
{
    struct cpu *c;
    u64 flags = irq_save();

    c = this_cpu();
    if (c->current) {
        c->current->state = TASK_DONE;
        remember_exit(c->current->id, code);
    }
    irq_restore(flags);

    /* Стек завершённой задачи пока не освобождаем: он у нас под ногами,
     * освободить его может только другая задача. Сборщик появится вместе
     * с ожиданием завершения (join). */
    schedule();

    for (;;)
        wfi();
}

void sched_idle_loop(void)
{
    for (;;) {
        schedule();     /* появилась работа — уйдём на неё */
        wfi();          /* нет — спим до ближайшего прерывания */
    }
}

/*
 * Сборщик: единственное место, где освобождается память задач.
 *
 * Работает обычной задачей, а не в планировщике, ровно по той причине,
 * по которой задача не может освободить себя сама: здесь нужны
 * аллокаторы и таблицы страниц, а планировщик держит замок очереди с
 * закрытыми прерываниями.
 *
 * Порядок внутри задачи важен: сначала адресное пространство (в нём
 * лежат страницы программы), потом стек ядра, потом сама структура.
 */
static void reap_one(struct task *t)
{
    if (t->ttbr0)
        mmu_free_user_space(t->ttbr0);
    if (t->stack)
        pmm_free_pages(t->stack, TASK_STACK_PAGES);
    kfree(t);
}

static void reaper_task(void *arg)
{
    (void)arg;

    for (;;) {
        struct task *dead;
        u64 flags = spin_lock_irq(&rq_lock);

        dead = zombie_head;
        zombie_head = NULL;
        spin_unlock_irq(&rq_lock, flags);

        while (dead) {
            struct task *next = dead->all_next;

            reap_one(dead);
            dead = next;
        }

        /* Спешить некуда: пока никто не завершился, разбирать нечего,
         * а просыпаться чаще, чем раз в четверть секунды, значит будить
         * ядро ради пустого списка. */
        task_sleep_ms(250);
    }
}

void sched_start_reaper(void)
{
    task_create("сборщик", reaper_task, NULL);
}

u32 sched_task_count(void)
{
    return (u32)task_count;
}

u64 task_id(void)
{
    struct cpu *c = this_cpu();

    return (c && c->current) ? c->current->id : 0;
}

const char *task_name(void)
{
    struct cpu *c = this_cpu();

    return (c && c->current && c->current->name) ? c->current->name : "?";
}

void sched_dump(void)
{
    static const char *state_name[] = {
        "ГОТОВА", "БЕЖИТ", "СПИТ", "ЖДЁТ", "КОНЕЦ"
    };
    u64 flags = spin_lock_irq(&rq_lock);

    /* idle-задачи пропускаем: их ровно по одной на ядро, они никогда
     * не попадают в очередь, и в списке из них получается стена строк,
     * за которой не видно настоящих задач. */
    for (struct task *t = all_head; t; t = t->all_next) {
        if (t->stack == NULL)
            continue;

        kprintf("  %s ID %lu %s ЯДРО %lu КВАНТОВ %lu\n",
                t->name, t->id,
                t->state < ARRAY_SIZE(state_name) ? state_name[t->state] : "?",
                t->last_cpu, t->slices);
    }

    spin_unlock_irq(&rq_lock, flags);
}
