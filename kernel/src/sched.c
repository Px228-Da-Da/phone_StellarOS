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
#include "print.h"
#include "io.h"

#define TASK_STACK_PAGES    2       /* 8 КБ на задачу */

struct task {
    u64 sp;                     /* сохранённый указатель стека           */
    u64 id;
    const char *name;
    u32 state;
    u32 slice_used;             /* сколько тиков задача уже отработала   */
    u64 slices;                 /* сколько раз получала процессор        */
    u64 last_cpu;
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
static u64 next_id;
static u64 task_count;

static void rq_push(struct task *t)
{
    t->next = NULL;
    if (rq_tail)
        rq_tail->next = t;
    else
        rq_head = t;
    rq_tail = t;
}

static struct task *rq_pop(void)
{
    struct task *t = rq_head;

    if (!t)
        return NULL;

    rq_head = t->next;
    if (!rq_head)
        rq_tail = NULL;
    t->next = NULL;
    return t;
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
    t->stack = stack;
    t->state = TASK_READY;

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

/*
 * Снять замок очереди уже в новом контексте.
 *
 * Вызывается ТОЛЬКО из task_start — из точки входа задачи, которая
 * запускается впервые. Прерывания она разрешает себе сама: их маску
 * восстанавливать неоткуда и не из чего, задача начинается с чистого листа.
 */
void sched_after_switch(void)
{
    spin_unlock(&rq_lock);
}

void schedule(void)
{
    struct cpu *c;
    struct task *prev, *next;
    u64 flags = irq_save();

    spin_lock(&rq_lock);

    c = this_cpu();
    prev = c->current;

    /* idle в очередь не возвращаем: она принадлежит ядру, а не очереди,
     * и попади она туда — другое ядро увело бы чужой загрузочный стек. */
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
    spin_unlock(&rq_lock);
    irq_restore(flags);
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

void task_exit(void)
{
    struct cpu *c;
    u64 flags = irq_save();

    c = this_cpu();
    if (c->current)
        c->current->state = TASK_DONE;
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

u32 sched_task_count(void)
{
    return (u32)task_count;
}

void sched_dump(void)
{
    static const char *state_name[] = { "ГОТОВА", "БЕЖИТ", "КОНЕЦ" };
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
