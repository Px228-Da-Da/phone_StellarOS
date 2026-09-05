/*
 * syscall.c — обработка того, что приходит из EL0.
 *
 * Сюда попадает всё синхронное, что случилось с программой: и её просьбы
 * к системе (SVC), и её ошибки. Разница между этими двумя случаями —
 * единственное поле ESR_EL1, а вот последствия противоположны: просьбу
 * выполняем и возвращаемся, ошибку не прощаем.
 *
 * Главное правило всего файла: НИЧЕМУ из EL0 нельзя верить. Указатель,
 * пришедший от программы, — это не адрес, а пожелание; длина — не длина,
 * а число, которое кто-то положил в регистр. Ядро обязано проверить
 * каждое такое значение само, потому что ошибиться здесь означает не
 * «программа упадёт», а «упадёт система» — с полными правами.
 */
#include "syscall.h"
#include "sched.h"
#include "timer.h"
#include "print.h"
#include "string.h"
#include "spinlock.h"
#include "io.h"
#include "uspace.h"
#include "window.h"
#include "input.h"
#include "fb.h"

/* Общий обработчик прерываний, main.c */
void irq_handler(void);

/* Классы синхронных исключений (ESR_EL1.EC) */
#define EC_SVC64        0x15    /* SVC из AArch64 — системный вызов      */
#define EC_IABT_LOW     0x20    /* отказ при выборке инструкции из EL0   */
#define EC_DABT_LOW     0x24    /* отказ при доступе к данным из EL0     */

static u64 read_esr(void)
{
    u64 v;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(v));
    return v;
}

static u64 read_far(void)
{
    u64 v;
    __asm__ volatile("mrs %0, far_el1" : "=r"(v));
    return v;
}

/*
 * Может ли ЭТА программа прочитать этот адрес.
 *
 * Проверку делает не ядро, а сам процессор: команда AT прогоняет адрес
 * через таблицы трансляции ровно так, как это сделал бы доступ из EL0,
 * и складывает ответ в PAR_EL1. Бит 0 — признак отказа.
 *
 * Так честнее, чем сверять адрес с записанными где-то границами
 * программы: границы можно забыть обновить при новом отображении, а
 * таблицы трансляции — это и есть правда о том, что кому доступно.
 *
 * Прерывания на время проверки закрыты: PAR_EL1 один на ядро, и
 * вклинившееся между AT и чтением переключение задач затёрло бы ответ
 * чужим.
 */
static int user_can_read(u64 va)
{
    u64 par;
    u64 flags = irq_save();

    __asm__ volatile("at s1e0r, %0" :: "r"(va) : "memory");
    isb();
    __asm__ volatile("mrs %0, par_el1" : "=r"(par));
    irq_restore(flags);

    return (par & 1) == 0;
}

/*
 * Забрать строку у программы.
 *
 * Проверяем каждую страницу, а не только первую: программа вправе
 * попросить вывести кусок, который начинается в её памяти и уходит за
 * её границу — именно так и выглядит попытка вычитать чужое.
 */
static int copy_from_user(void *dst, u64 uva, u64 len)
{
    for (u64 page = uva & ~(u64)0xFFF; page < uva + len; page += 4096)
        if (!user_can_read(page))
            return -1;

    memcpy(dst, (const void *)(uintptr_t)uva, len);
    return 0;
}

static s64 sys_write(u64 uva, u64 len)
{
    char buf[SYS_WRITE_MAX + 1];

    if (len > SYS_WRITE_MAX)
        len = SYS_WRITE_MAX;
    if (!len)
        return 0;

    if (copy_from_user(buf, uva, len) != 0) {
        kprintf("EL0      : %s ПРОСИТ ВЫВЕСТИ ЧУЖОЕ, АДРЕС %p\n",
                task_name(), (void *)(uintptr_t)uva);
        return -1;
    }

    /* Строка из EL0 не обязана быть завершённой нулём — завершаем сами.
     * И печатаем её как строку, а не как формат: символы % в чужом тексте
     * иначе заставили бы kprintf читать несуществующие аргументы. */
    buf[len] = 0;
    kputs(buf);
    return (s64)len;
}

/*
 * Дождаться, пока задача кончится, и узнать чем.
 *
 * Ожидание сделано сном с переспросом, а не очередью ждущих. Очередь
 * правильнее — она будит ровно того, кого надо, и ровно тогда, когда
 * надо, — но ей нужен механизм пробуждения по событию, которого у
 * планировщика пока нет. Сон по десять миллисекунд стоит одного
 * переключения задач в эти же десять миллисекунд и на общей картине не
 * виден; когда появится ожидание по событию, замена будет ровно здесь.
 *
 * Ждать чужую задачу, а не только свою, никто не мешает: родство
 * задач ядро пока не отслеживает. Наружу это отдаёт только код
 * завершения — не тайну, но так и запишем, чтобы потом не выглядело
 * замыслом.
 */
static s64 sys_wait(u64 id)
{
    u64 code;

    /*
     * Ждать чужую задачу, а не только свою, никто не мешает: родство
     * задач ядро пока не отслеживает. Наружу это отдаёт только код
     * завершения — не тайну, но так и запишем, чтобы потом не выглядело
     * замыслом.
     */
    if (!sched_wait_for(id, &code))
        return -1;              /* такой задачи нет и не было */

    return (s64)code;
}

/*
 * Может ли программа писать по этому адресу.
 *
 * То же, что и проверка на чтение, но команда другая: право писать и
 * право читать — разные вещи, и страница кода, например, доступна
 * программе только на чтение. Спрашиваем ровно то, что собираемся
 * делать.
 */
static int user_can_write(u64 va)
{
    u64 par;
    u64 flags = irq_save();

    __asm__ volatile("at s1e0w, %0" :: "r"(va) : "memory");
    isb();
    __asm__ volatile("mrs %0, par_el1" : "=r"(par));
    irq_restore(flags);

    return (par & 1) == 0;
}

/*
 * Дождаться касания и отдать его программе.
 *
 * Ждём по событию, но со сроком. Условие живёт не в планировщике, а в
 * очереди событий ввода, и между «очередь пуста» и «уснул» остаётся
 * промежуток, в который пробуждение можно потерять. Срок превращает
 * потерю в задержку: событие не пропадёт, просто заберём мы его на
 * шестьдесят миллисекунд позже. Закрыть промежуток совсем можно было бы,
 * взяв оба замка сразу, — но это уже порядок взятия двух замков, то есть
 * разговор о взаимных блокировках ради выигрыша, которого не видно.
 */
static s64 sys_input(u64 uva)
{
    struct input_event e;

    if (!user_can_write(uva) || !user_can_write(uva + TOUCH_EVENT_BYTES - 1))
        return -1;

    for (;;) {
        if (input_pop_task(task_id(), &e)) {
            memcpy((void *)(uintptr_t)uva, &e, TOUCH_EVENT_BYTES);
            return 1;
        }

        sched_wait_timeout(INPUT_CHAN, 60);
    }
}

/*
 * Написать в своём окне.
 *
 * Строка приходит из EL0, поэтому проверяется и копируется так же, как в
 * выводе в консоль: длину ограничиваем, каждую страницу проверяем.
 * Разница только в том, что здесь она попадёт не в консоль, а в буфер
 * самой программы — но пройти по чужой памяти на пути к нему ядро может
 * ровно так же.
 */
static s64 sys_text(u64 x, u64 y, u64 scale, u64 color, u64 uva, u64 len)
{
    char buf[128];

    if (len > sizeof(buf) - 1)
        len = sizeof(buf) - 1;
    if (!len)
        return 0;

    if (copy_from_user(buf, uva, len) != 0) {
        kprintf("EL0      : %s ПРОСИТ НАПИСАТЬ ЧУЖОЕ, АДРЕС %p\n",
                task_name(), (void *)(uintptr_t)uva);
        return -1;
    }

    buf[len] = 0;
    return window_text((u32)x, (u32)y, (u32)scale, (u32)color, buf);
}

static void syscall(struct trapframe *f)
{
    u64 nr = f->x[8];

    switch (nr) {
    case SYS_EXIT:
        kprintf("EL0      : %s ЗАВЕРШИЛАСЬ, КОД %lu\n", task_name(), f->x[0]);
        task_exit_code(f->x[0]);
        return;                 /* сюда управление уже не возвращается */

    case SYS_WRITE:
        f->x[0] = (u64)sys_write(f->x[0], f->x[1]);
        return;

    case SYS_YIELD:
        schedule();
        f->x[0] = 0;
        return;

    case SYS_SLEEP_MS:
        task_sleep_ms(f->x[0]);
        f->x[0] = 0;
        return;

    case SYS_UPTIME_MS:
        f->x[0] = timer_uptime_ms();
        return;

    case SYS_GETPID:
        f->x[0] = task_id();
        return;

    case SYS_WINDOW:
        f->x[0] = window_open((u32)f->x[0], (u32)f->x[1]);
        return;

    case SYS_PRESENT:
        f->x[0] = (u64)(s64)window_present((u32)f->x[0], (u32)f->x[1]);
        return;

    case SYS_TEXT:
        f->x[0] = (u64)(s64)sys_text(f->x[0], f->x[1], f->x[2], f->x[3],
                                     f->x[4], f->x[5]);
        return;

    case SYS_CLOSE:
        f->x[0] = (u64)(s64)window_close();
        return;

    case SYS_SCREEN: {
        u64 base;
        u32 w, h, stride;

        fb_info(&base, &w, &h, &stride);
        f->x[0] = ((u64)w << 32) | h;
        return;
    }

    case SYS_INPUT:
        f->x[0] = (u64)sys_input(f->x[0]);
        return;

    case SYS_SPAWN:
        f->x[0] = (u64)uspace_spawn_image((u32)f->x[0]);
        return;

    case SYS_WAIT:
        f->x[0] = (u64)sys_wait(f->x[0]);
        return;

    default:
        kprintf("EL0      : %s ПРОСИТ ВЫЗОВ %lu, ТАКОГО НЕТ\n",
                task_name(), nr);
        f->x[0] = (u64)-1;
        return;
    }
}

/*
 * Ошибка в программе — не повод останавливать систему.
 *
 * До сих пор любое синхронное исключение вело в exception_fatal: печать
 * и вечный wfi. Для ошибки в ядре это правильно — доверять дальше нечему.
 * Для ошибки в пользовательской программе это ровно та беда, ради
 * избавления от которой пользовательский режим и придуман: снимаем одну
 * задачу, остальные продолжают работать.
 */
static void kill_task(struct trapframe *f, u64 esr)
{
    u32 ec = (u32)((esr >> 26) & 0x3F);
    const char *why = "НЕИЗВЕСТНО ЧТО";

    if (ec == EC_DABT_LOW)
        why = "ЗАЛЕЗЛА В ЧУЖУЮ ПАМЯТЬ";
    else if (ec == EC_IABT_LOW)
        why = "УШЛА ИСПОЛНЯТЬ ЧУЖОЕ";

    kprintf("EL0      : %s СНЯТА — %s\n", task_name(), why);
    kprintf("EL0      : АДРЕС %p, КОМАНДА %p, ESR %08lx (EC %u)\n",
            (void *)(uintptr_t)read_far(), (void *)(uintptr_t)f->elr,
            esr, ec);

    /*
     * Что лежит в памяти по адресу отказа.
     *
     * Отказ на неопределённой команде значит одно из двух: программа
     * действительно ушла не туда — или процессор исполнил не то, что
     * записано в памяти. Различить это можно только заглянув в саму
     * память: пространство отказавшей задачи ещё включено, читаем прямо
     * оттуда. Нули означают, что до этого места дошли не по своей воле.
     */
    if (ec == 0 && user_can_read(f->elr & ~3UL)) {
        const u32 *pc = (const u32 *)(uintptr_t)(f->elr & ~3UL);

        kprintf("EL0      : В ПАМЯТИ ТАМ %08x %08x, В НАЧАЛЕ %08x\n",
                pc[0], pc[1],
                user_can_read(f->elr & ~0xFFFUL)
                    ? *(const u32 *)(uintptr_t)(f->elr & ~0xFFFUL) : 0);
    }

    task_exit_code(EXIT_KILLED);
}

/*
 * Сколько раз прерывание застало программу в EL0.
 *
 * Не украшение отчёта, а доказательство. Программа, которая всё время
 * сидит в системных вызовах, вытесняется в EL1, и вход в таблице
 * векторов для «прерывание из младшего уровня» при этом ни разу не
 * используется — то есть может быть сломан и не замечен. Ненулевой
 * счётчик означает, что этот путь пройден по-настоящему.
 */
static volatile u64 el0_preempts;

void irq_from_el0(void)
{
    atomic_inc(&el0_preempts);
    irq_handler();
}

u64 el0_preempt_count(void)
{
    return atomic_read(&el0_preempts);
}

void el0_sync(struct trapframe *f)
{
    u64 esr = read_esr();

    if (((esr >> 26) & 0x3F) == EC_SVC64) {
        syscall(f);
        return;
    }

    kill_task(f, esr);
}
