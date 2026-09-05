/*
 * uspace.c — как программа попадает в пользовательский режим.
 *
 * Порядок такой:
 *   1. взять страницы под код и стек у постраничного аллокатора;
 *   2. скопировать в них программу и согласовать кэши — иначе процессор
 *      выберет из памяти то, что лежало там до копирования;
 *   3. отобразить эти страницы по пользовательским адресам с правами
 *      для EL0: код — читать и исполнять, стек — читать и писать;
 *   4. завести обычную задачу ядра, которая первым же делом уйдёт в EL0
 *      и больше оттуда не вернётся.
 *
 * Четвёртый шаг объясняет, почему пользовательский процесс здесь — это
 * задача планировщика, а не что-то отдельное. У программы в EL0 всё
 * равно должен быть стек в EL1: на него процессор складывает кадр при
 * каждом системном вызове и каждом прерывании. Этот стек — и есть стек
 * задачи, а планировщику при переключении неважно, где задача находилась,
 * в ядре или в программе: он видит только сохранённый указатель стека.
 */
#include "uspace.h"
#include "mmu.h"
#include "pmm.h"
#include "sched.h"
#include "kmalloc.h"
#include "string.h"
#include "print.h"
#include "spinlock.h"

/* Спуск в EL0, switch.S */
extern void enter_el0(u64 pc, u64 sp);

/*
 * Где живут программы.
 *
 * Ядро отображено единично: виртуальный адрес равен физическому, а это
 * значит, что вся нижняя часть адресного пространства занята — сначала
 * гигабайт периферии, потом вся DRAM. Пользовательские адреса должны
 * лежать там, где ничего этого нет, поэтому берём заведомо пустой кусок
 * повыше: 64 ГБ. Верхняя граница у нас 512 ГБ (39-битные адреса), так
 * что места хватает.
 *
 * Когда у каждой программы появится свой корень таблиц, ядро уедет в
 * верхнюю половину адресного пространства, а программы переедут вниз,
 * к нулю, как и положено. Пока — так.
 */
#define USER_BASE       0x0000001000000000UL
#define USER_SLOT       0x0000000001000000UL    /* 16 МБ на программу   */
#define USER_STACK_OFF  0x0000000000100000UL    /* стек в мегабайте от кода */
#define USER_STACK_PAGES 2

struct uproc {
    u64 entry;      /* с какого адреса начинать         */
    u64 sp;         /* вершина пользовательского стека  */
};

static struct spinlock slot_lock = SPINLOCK_INIT("uspace");
static u32 next_slot;

/*
 * Точка входа задачи. Всё, что здесь происходит в EL1, — это одна
 * строка отчёта; дальше начинается пользовательский режим.
 */
static void user_trampoline(void *arg)
{
    struct uproc *p = arg;

    kprintf("EL0      : %s ПОШЛА, ВХОД %p, СТЕК %p\n",
            task_name(), (void *)(uintptr_t)p->entry,
            (void *)(uintptr_t)p->sp);

    enter_el0(p->entry, p->sp);
}

int uspace_spawn(const char *name, const void *image, u64 size)
{
    struct uproc *p;
    void *code, *stack;
    u64 base, code_bytes;
    u32 code_pages;

    if (!size)
        return -1;

    code_pages = (u32)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    code_bytes = (u64)code_pages * PAGE_SIZE;

    p = kzalloc(sizeof(*p));
    if (!p)
        return -1;

    code  = pmm_alloc_pages(code_pages);
    stack = pmm_alloc_pages(USER_STACK_PAGES);
    if (!code || !stack) {
        kprintf("EL0      : %s НЕ ЗАПУЩЕНА — НЕТ ПАМЯТИ\n", name);
        return -1;
    }

    memcpy(code, image, size);

    /*
     * Программу мы только что записали как данные, а исполняться она
     * будет через кэш инструкций. Согласовать их обязательно, и именно
     * по ядерному адресу: по пользовательскому трансляции ещё нет.
     */
    mmu_sync_icache((u64)(uintptr_t)code, code_bytes);

    u64 flags = spin_lock_irq(&slot_lock);
    base = USER_BASE + (u64)next_slot++ * USER_SLOT;
    spin_unlock_irq(&slot_lock, flags);

    if (mmu_map_user(base, (u64)(uintptr_t)code, code_bytes,
                     MMU_USER_RX) != 0 ||
        mmu_map_user(base + USER_STACK_OFF, (u64)(uintptr_t)stack,
                     USER_STACK_PAGES * PAGE_SIZE, MMU_USER_RW) != 0) {
        kprintf("EL0      : %s НЕ ЗАПУЩЕНА — НЕ ВЫШЛО ОТОБРАЗИТЬ\n", name);
        return -1;
    }

    p->entry = base;
    /* Стек растёт вниз, поэтому начинаем с верхней границы его страниц */
    p->sp = base + USER_STACK_OFF + USER_STACK_PAGES * PAGE_SIZE;

    if (!task_create(name, user_trampoline, p)) {
        kprintf("EL0      : %s НЕ ЗАПУЩЕНА — НЕТ МЕСТА ПОД ЗАДАЧУ\n", name);
        return -1;
    }

    return 0;
}
