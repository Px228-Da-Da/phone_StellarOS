/*
 * Исполнитель байткода Hittis.
 *
 * Устройство самое простое из работающих: стек значений, стек кадров,
 * массив глобальных — и цикл разбора команд. Ни памяти по требованию,
 * ни библиотек: этот же файл собирается в программу для телефона, где
 * нет ни того, ни другого.
 *
 * Главное свойство — недоверие. Приложение приходит файлом, его мог
 * испортить кто угодно: и флешка, и наш собственный компилятор с
 * ошибкой. Поэтому проверяется всё: подпись файла, его целостность,
 * каждый переход, каждый номер ячейки, глубина стеков. Нарушение
 * останавливает приложение с внятной жалобой — а не портит соседнюю
 * память и не приводит к загадочному поведению через час.
 */
#include "vm.h"
#include "slt.h"

/* --- Мелочи, которых на телефоне взять негде -------------------------- */

static vm_u32 rd32(const vm_u8 *p)
{
    return (vm_u32)p[0] | ((vm_u32)p[1] << 8) |
           ((vm_u32)p[2] << 16) | ((vm_u32)p[3] << 24);
}

static vm_u16 rd16(const vm_u8 *p)
{
    return (vm_u16)((vm_u32)p[0] | ((vm_u32)p[1] << 8));
}

static vm_u32 vm_checksum(const vm_u8 *p, vm_u32 n)
{
    vm_u32 s = 0x5A17;
    vm_u32 i;

    for (i = 0; i < n; i++)
        s = (s * 31u) + p[i];
    return s;
}

/*
 * Разбор заголовка.
 *
 * Смещения полей считаем руками, а не накладываем структуру на байты:
 * так файл читается одинаково любым компилятором и на любой машине,
 * независимо от того, как он решит выравнивать поля.
 */
struct vm_image {
    const vm_u8 *funcs;     /* таблица функций          */
    const vm_u8 *classes;   /* таблица классов          */
    const vm_u8 *members;   /* поля и методы классов    */
    const vm_u8 *strs;      /* область строк            */
    const vm_u8 *code;      /* байткод                  */
    vm_u32 nfuncs, nglobals, str_bytes, code_bytes;
    vm_u32 nclasses, nmembers;
    vm_u16 win_w, win_h;
    const char *name;
};

#define HDR_MAGIC       0
#define HDR_VERSION     4
#define HDR_FLAGS       6
#define HDR_NAME        8
#define HDR_WIN_W       (HDR_NAME + SLT_NAME_MAX)
#define HDR_WIN_H       (HDR_WIN_W + 2)
#define HDR_FUNCS       (HDR_WIN_H + 2)
#define HDR_GLOBALS     (HDR_FUNCS + 4)
#define HDR_STRB        (HDR_GLOBALS + 4)
#define HDR_CODEB       (HDR_STRB + 4)
#define HDR_CLASSES     (HDR_CODEB + 4)
#define HDR_MEMBERS     (HDR_CLASSES + 4)
#define HDR_SUM         (HDR_MEMBERS + 4)
#define HDR_SIZE        (HDR_SUM + 4)

#define FUNC_ENTRY      0
#define FUNC_NARGS      4
#define FUNC_NLOCALS    6
#define FUNC_SIZE       8
#define CLASS_SIZE      12
#define MEMBER_SIZE     8

static int parse_image(const void *raw, vm_u32 len, struct vm_image *img,
                       const struct vm_host *host)
{
    const vm_u8 *p = raw;
    vm_u32 need, sum;

    if (len < HDR_SIZE) {
        host->print("HITTIS: файл короче заголовка\n");
        return -1;
    }
    if (p[0] != SLT_MAGIC0 || p[1] != SLT_MAGIC1 ||
        p[2] != SLT_MAGIC2 || p[3] != SLT_MAGIC3) {
        host->print("HITTIS: это не приложение StellarOS\n");
        return -1;
    }
    if (rd16(p + HDR_VERSION) != SLT_VERSION) {
        host->print("HITTIS: другая версия формата\n");
        return -1;
    }

    img->name      = (const char *)(p + HDR_NAME);
    img->win_w     = rd16(p + HDR_WIN_W);
    img->win_h     = rd16(p + HDR_WIN_H);
    img->nfuncs    = rd32(p + HDR_FUNCS);
    img->nglobals  = rd32(p + HDR_GLOBALS);
    img->str_bytes = rd32(p + HDR_STRB);
    img->code_bytes = rd32(p + HDR_CODEB);
    img->nclasses  = rd32(p + HDR_CLASSES);
    img->nmembers  = rd32(p + HDR_MEMBERS);

    need = HDR_SIZE + img->nfuncs * FUNC_SIZE +
           img->nclasses * CLASS_SIZE + img->nmembers * MEMBER_SIZE +
           img->str_bytes + img->code_bytes;
    if (need > len || img->nfuncs == 0 || img->nglobals > VM_GLOBALS) {
        host->print("HITTIS: заголовок не сходится с размером файла\n");
        return -1;
    }

    img->funcs   = p + HDR_SIZE;
    img->classes = img->funcs + img->nfuncs * FUNC_SIZE;
    img->members = img->classes + img->nclasses * CLASS_SIZE;
    img->strs    = img->members + img->nmembers * MEMBER_SIZE;
    img->code    = img->strs + img->str_bytes;

    sum = vm_checksum(img->funcs, need - HDR_SIZE);
    if (sum != rd32(p + HDR_SUM)) {
        host->print("HITTIS: файл испорчен, сумма не сходится\n");
        return -1;
    }

    return 0;
}

const char *vm_app_name(const void *image, vm_u32 len)
{
    const vm_u8 *p = image;

    if (len < HDR_SIZE)
        return 0;
    if (p[0] != SLT_MAGIC0 || p[1] != SLT_MAGIC1 ||
        p[2] != SLT_MAGIC2 || p[3] != SLT_MAGIC3)
        return 0;
    return (const char *)(p + HDR_NAME);
}

/* --- Сам исполнитель -------------------------------------------------- */

struct vm_frame {
    vm_u32 ret_pc;      /* куда вернуться        */
    vm_u32 base;        /* начало кадра в locals */
    vm_u32 nlocals;
};

struct vm {
    const struct vm_image *img;
    const struct vm_host *host;

    vm_i64 stack[VM_STACK];
    int    sp;

    vm_i64 locals[VM_LOCALS];
    int    lp;                      /* сколько ячеек занято */

    struct vm_frame frames[VM_FRAMES];
    int    fp;

    vm_i64 globals[VM_GLOBALS];

    /*
     * Куча объектов. Занимаем подряд и никогда не отпускаем — почему
     * именно так, сказано у VM_HEAP в vm.h. Нулевая ячейка не выдаётся
     * никому: ноль означает «ничего», и путать его с настоящим объектом
     * нельзя.
     */
    vm_i64 heap[VM_HEAP];
    vm_u32 hp;

    vm_u32 pc;
    int    stopped;
    vm_i64 result;
};

static void fail(struct vm *m, const char *why)
{
    m->host->print(why);
    m->stopped = -1;
}

static void push(struct vm *m, vm_i64 v)
{
    if (m->sp >= VM_STACK) {
        fail(m, "HITTIS: стек значений переполнен\n");
        return;
    }
    m->stack[m->sp++] = v;
}

static vm_i64 pop(struct vm *m)
{
    if (m->sp <= 0) {
        fail(m, "HITTIS: стек значений пуст\n");
        return 0;
    }
    return m->stack[--m->sp];
}

/* Строка по смещению; проверяем, что она лежит в своей области */
static const char *str_at(struct vm *m, vm_i64 off)
{
    if (off < 0 || (vm_u32)off >= m->img->str_bytes) {
        fail(m, "HITTIS: обращение к строке за границей\n");
        return "";
    }
    return (const char *)(m->img->strs + off);
}

static void do_native(struct vm *m, vm_u32 id)
{
    const struct vm_host *h = m->host;

    switch (id) {
    case NAT_PRINT: {
        const char *s = str_at(m, pop(m));

        h->print(s);
        push(m, 0);
        break;
    }
    case NAT_PRINTN:
        h->printn(pop(m));
        push(m, 0);
        break;
    case NAT_WINDOW: {
        vm_i64 hh = pop(m), ww = pop(m);

        push(m, h->window((int)ww, (int)hh));
        break;
    }
    case NAT_RECT: {
        vm_i64 c = pop(m), hh = pop(m), ww = pop(m), y = pop(m), x = pop(m);

        h->rect((int)x, (int)y, (int)ww, (int)hh, (vm_u32)c);
        push(m, 0);
        break;
    }
    case NAT_TEXT: {
        vm_i64 s = pop(m), c = pop(m), sc = pop(m), y = pop(m), x = pop(m);

        h->text((int)x, (int)y, (int)sc, (vm_u32)c, str_at(m, s));
        push(m, 0);
        break;
    }
    case NAT_TEXTN: {
        vm_i64 v = pop(m), c = pop(m), sc = pop(m), y = pop(m), x = pop(m);

        h->textn((int)x, (int)y, (int)sc, (vm_u32)c, v);
        push(m, 0);
        break;
    }
    case NAT_TEXTC: {
        vm_i64 v = pop(m), c = pop(m), sc = pop(m), y = pop(m), x = pop(m);

        push(m, h->textc((int)x, (int)y, (int)sc, (vm_u32)c, v));
        break;
    }
    case NAT_TEXTW: {
        vm_i64 s = pop(m), sc = pop(m);

        push(m, h->textw((int)sc, str_at(m, s)));
        break;
    }
    case NAT_SHOW:
        h->show();
        push(m, 0);
        break;
    case NAT_TOUCH:
        push(m, h->touch());
        break;
    case NAT_SLEEP:
        h->sleep_ms(pop(m));
        push(m, 0);
        break;
    case NAT_TIME:
        push(m, h->time_ms());
        break;
    case NAT_WIDTH:
        push(m, h->width());
        break;
    case NAT_HEIGHT:
        push(m, h->height());
        break;
    case NAT_EXIT:
        m->result = pop(m);
        m->stopped = 1;
        break;
    default:
        fail(m, "HITTIS: неизвестный вызов к системе\n");
        break;
    }
}

/* Прочитать операнд команды и сдвинуть счётчик */
static vm_u32 operand(struct vm *m)
{
    vm_u32 v;

    if (m->pc + 4 > m->img->code_bytes) {
        fail(m, "HITTIS: код оборван на середине команды\n");
        return 0;
    }
    v = rd32(m->img->code + m->pc);
    m->pc += 4;
    return v;
}

/* --- Объекты -------------------------------------------------------- */

/*
 * Найти член класса по имени.
 *
 * Имя приходит смещением в области строк, и сравниваем мы именно
 * смещения, а не сами строки: одинаковые имена компилятор кладёт в
 * область строк один раз, поэтому равенство смещений и есть равенство
 * имён. Сравнение вышло в одно действие вместо посимвольного обхода.
 */
static int member_of(struct vm *m, vm_u32 cls, vm_u32 name,
                     int want_method, vm_u32 *value)
{
    const vm_u8 *c;
    vm_u32 first, nf, nm, i, from, to;

    if (cls >= m->img->nclasses)
        return 0;

    c = m->img->classes + cls * CLASS_SIZE;
    nf    = rd16(c + 4);
    nm    = rd16(c + 6);
    first = rd32(c + 8);

    from = want_method ? first + nf : first;
    to   = want_method ? first + nf + nm : first + nf;

    for (i = from; i < to && i < m->img->nmembers; i++) {
        const vm_u8 *e = m->img->members + i * MEMBER_SIZE;

        if (rd32(e) == name) {
            *value = rd32(e + 4);
            return 1;
        }
    }
    return 0;
}

/*
 * Отвести место в куче. Возвращает указатель или 0, если места нет.
 *
 * Ноль никому не выдаётся: он означает «ничего», и путать его с
 * настоящим указателем нельзя.
 */
static vm_u32 heap_alloc(struct vm *m, vm_u32 cells)
{
    vm_u32 at;

    if (m->hp == 0)
        m->hp = 1;
    if (m->hp + cells > VM_HEAP) {
        fail(m, "кончилась память под объекты");
        return 0;
    }
    at = m->hp;
    m->hp += cells;
    return at;
}

/* Список по указателю */
static int list_at(struct vm *m, vm_i64 h)
{
    if (h <= 0 || (vm_u32)h >= m->hp) {
        fail(m, "это не список");
        return 0;
    }
    if (m->heap[h] != SLT_LIST_TAG) {
        fail(m, "это объект, а не список");
        return 0;
    }
    return 1;
}

/* Объект по указателю: проверяем, что он вообще наш */
static int object_at(struct vm *m, vm_i64 h, vm_u32 *cls)
{
    if (h <= 0 || (vm_u32)h >= m->hp) {
        fail(m, "это не объект");
        return 0;
    }
    if (m->heap[h] == SLT_LIST_TAG) {
        fail(m, "это список, а не объект");
        return 0;
    }
    *cls = (vm_u32)m->heap[h];
    if (*cls >= m->img->nclasses) {
        fail(m, "объект испорчен");
        return 0;
    }
    return 1;
}

static void call_func(struct vm *m, vm_u32 idx)
{
    const vm_u8 *f;
    vm_u32 entry, nargs, nlocals;
    struct vm_frame *fr;
    vm_u32 i;

    if (idx >= m->img->nfuncs) {
        fail(m, "HITTIS: вызов несуществующей функции\n");
        return;
    }
    f = m->img->funcs + idx * FUNC_SIZE;
    entry   = rd32(f + FUNC_ENTRY);
    nargs   = rd16(f + FUNC_NARGS);
    nlocals = rd16(f + FUNC_NLOCALS);

    if (nlocals < nargs)
        nlocals = nargs;
    if (entry >= m->img->code_bytes) {
        fail(m, "HITTIS: вход функции за границей кода\n");
        return;
    }
    if (m->fp >= VM_FRAMES) {
        fail(m, "HITTIS: слишком глубокая вложенность вызовов\n");
        return;
    }
    if (m->lp + (int)nlocals > VM_LOCALS) {
        fail(m, "HITTIS: не хватило места под переменные\n");
        return;
    }

    fr = &m->frames[m->fp++];
    fr->ret_pc = m->pc;
    fr->base = (vm_u32)m->lp;
    fr->nlocals = nlocals;
    m->lp += (int)nlocals;

    /* Аргументы лежат на стеке в порядке вызова: снимаем с конца */
    for (i = 0; i < nlocals; i++)
        m->locals[fr->base + i] = 0;
    for (i = 0; i < nargs; i++)
        m->locals[fr->base + nargs - 1 - i] = pop(m);

    m->pc = entry;
}

static void do_return(struct vm *m)
{
    vm_i64 v = pop(m);
    struct vm_frame *fr;

    if (m->fp <= 0) {
        /* Возврат из верхнего уровня — конец программы */
        m->result = v;
        m->stopped = 1;
        return;
    }
    fr = &m->frames[--m->fp];
    m->lp = (int)fr->base;
    m->pc = fr->ret_pc;
    push(m, v);
}

int vm_run(const void *image, vm_u32 len, const struct vm_host *host)
{
    static struct vm m;         /* не на стеке: он у программы невелик */
    struct vm_image img;
    int i;

    if (parse_image(image, len, &img, host) != 0)
        return -1;

    m.img = &img;
    m.host = host;
    m.sp = 0;
    m.lp = 0;
    m.fp = 0;
    m.pc = 0;
    m.stopped = 0;
    m.result = 0;
    for (i = 0; i < VM_GLOBALS; i++)
        m.globals[i] = 0;

    /* Окно, если приложение его просило заголовком */
    if (img.win_w && img.win_h)
        host->window(img.win_w, img.win_h);

    while (!m.stopped) {
        vm_u8 op;

        if (m.pc >= img.code_bytes) {
            fail(&m, "HITTIS: выполнение ушло за конец кода\n");
            break;
        }
        op = img.code[m.pc++];

        switch (op) {
        case OP_HALT:
            m.stopped = 1;
            break;
        case OP_PUSH:
            /*
             * Число из исходника кладём БЕЗ знака.
             *
             * Операнд четырёхбайтовый, а отрицательных чисел в исходнике
             * не бывает вовсе: «-5» — это «5» и смена знака отдельной
             * командой. Значит старший бит операнда — часть числа, а не
             * знак. Пока он считался знаком, цвет 0xFF4C8DFF приезжал
             * отрицательным: рисовалось верно (там всё равно берут
             * младшие 32 бита), а сравнить или напечатать такой цвет
             * было нельзя.
             */
            push(&m, (vm_i64)operand(&m));
            break;
        case OP_PUSHS:
            push(&m, (int)operand(&m));
            break;
        case OP_LOADL:
        case OP_STOREL: {
            vm_u32 slot = operand(&m);
            struct vm_frame *fr = m.fp ? &m.frames[m.fp - 1] : 0;
            vm_u32 base = fr ? fr->base : 0;
            vm_u32 n = fr ? fr->nlocals : 0;

            if (!fr || slot >= n) {
                fail(&m, "HITTIS: обращение к ячейке за границей кадра\n");
                break;
            }
            if (op == OP_LOADL)
                push(&m, m.locals[base + slot]);
            else
                m.locals[base + slot] = pop(&m);
            break;
        }
        case OP_LOADG:
        case OP_STOREG: {
            vm_u32 slot = operand(&m);

            if (slot >= img.nglobals || slot >= VM_GLOBALS) {
                fail(&m, "HITTIS: обращение к глобальной за границей\n");
                break;
            }
            if (op == OP_LOADG)
                push(&m, m.globals[slot]);
            else
                m.globals[slot] = pop(&m);
            break;
        }

        case OP_ADD: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a + b); break; }
        case OP_SUB: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a - b); break; }
        case OP_MUL: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a * b); break; }
        case OP_DIV: {
            vm_i64 b = pop(&m), a = pop(&m);

            if (!b) {
                fail(&m, "HITTIS: деление на ноль\n");
                break;
            }
            push(&m, a / b);
            break;
        }
        case OP_MOD: {
            vm_i64 b = pop(&m), a = pop(&m);

            if (!b) {
                fail(&m, "HITTIS: остаток от деления на ноль\n");
                break;
            }
            push(&m, a % b);
            break;
        }
        case OP_NEG: push(&m, -pop(&m)); break;

        case OP_LT: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a <  b); break; }
        case OP_GT: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a >  b); break; }
        case OP_LE: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a <= b); break; }
        case OP_GE: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a >= b); break; }
        case OP_EQ: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a == b); break; }
        case OP_NE: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a != b); break; }
        case OP_AND: { vm_i64 b = pop(&m), a = pop(&m); push(&m, a && b); break; }
        case OP_OR:  { vm_i64 b = pop(&m), a = pop(&m); push(&m, a || b); break; }
        case OP_NOT: push(&m, !pop(&m)); break;

        case OP_JMP: {
            vm_u32 to = operand(&m);

            if (to >= img.code_bytes) {
                fail(&m, "HITTIS: переход за границу кода\n");
                break;
            }
            m.pc = to;
            break;
        }
        case OP_JZ: {
            vm_u32 to = operand(&m);

            if (to >= img.code_bytes) {
                fail(&m, "HITTIS: переход за границу кода\n");
                break;
            }
            if (!pop(&m))
                m.pc = to;
            break;
        }
        case OP_CALL:
            call_func(&m, operand(&m));
            break;
        case OP_RET:
            do_return(&m);
            break;
        case OP_NATIVE:
            do_native(&m, operand(&m));
            break;
        case OP_POP:
            pop(&m);
            break;

        /*
         * Завести объект.
         *
         * Отводим ячейку под номер класса и по одной на каждое поле,
         * поля обнуляем. Указателем служит индекс первой ячейки — он же
         * то самое число, которым объект и является для языка.
         */
        case OP_NEW: {
            vm_u32 cls = operand(&m);
            const vm_u8 *c;
            vm_u32 nf, i;

            if (cls >= img.nclasses) {
                fail(&m, "нет такого класса");
                break;
            }
            c = img.classes + cls * CLASS_SIZE;
            nf = rd16(c + 4);

            if (m.hp == 0)
                m.hp = 1;               /* ноль не выдаём: это «ничего» */
            if (m.hp + nf + 1 > VM_HEAP) {
                fail(&m, "кончилась память под объекты");
                break;
            }
            m.heap[m.hp] = cls;
            for (i = 0; i < nf; i++)
                m.heap[m.hp + 1 + i] = 0;
            push(&m, (vm_i64)m.hp);
            m.hp += nf + 1;
            break;
        }

        case OP_DUP: {
            vm_i64 v = pop(&m);

            push(&m, v);
            push(&m, v);
            break;
        }

        case OP_GETF: {
            vm_u32 name = operand(&m);
            vm_i64 h = pop(&m);
            vm_u32 cls, slot;

            if (!object_at(&m, h, &cls))
                break;
            if (!member_of(&m, cls, name, 0, &slot)) {
                fail(&m, "у объекта нет такого поля");
                break;
            }
            push(&m, m.heap[h + 1 + slot]);
            break;
        }

        case OP_SETF: {
            vm_u32 name = operand(&m);
            vm_i64 v = pop(&m);
            vm_i64 h = pop(&m);
            vm_u32 cls, slot;

            if (!object_at(&m, h, &cls))
                break;
            if (!member_of(&m, cls, name, 0, &slot)) {
                fail(&m, "у объекта нет такого поля");
                break;
            }
            m.heap[h + 1 + slot] = v;
            break;
        }

        /*
         * Вызов метода.
         *
         * Какого класса окажется объект, компилятор знать не может —
         * типов в языке нет, — поэтому метод ищется по имени во время
         * работы. Объект лежит под аргументами и становится первым из
         * них: внутри метода он и есть «сам».
         */
        /*
         * Собрать список из того, что лежит на стеке.
         *
         * Мест отводим не меньше четырёх: список почти всегда потом
         * дополняют, а переносить его с первого же добавления — значит
         * дважды пройти по памяти на пустом месте.
         */
        case OP_LIST: {
            vm_u32 n = operand(&m);
            vm_u32 cap = n < 4 ? 4 : n;
            vm_u32 at, data, i;

            if ((vm_u32)m.sp < n) {
                fail(&m, "на стеке меньше значений, чем в списке");
                break;
            }
            at = heap_alloc(&m, 4);
            if (!at)
                break;
            data = heap_alloc(&m, cap);
            if (!data)
                break;

            m.heap[at]     = SLT_LIST_TAG;
            m.heap[at + 1] = n;
            m.heap[at + 2] = cap;
            m.heap[at + 3] = data;
            for (i = 0; i < n; i++)
                m.heap[data + (n - 1 - i)] = pop(&m);
            push(&m, (vm_i64)at);
            break;
        }

        case OP_GETI: {
            vm_i64 i = pop(&m);
            vm_i64 h = pop(&m);

            if (!list_at(&m, h))
                break;
            if (i < 0 || i >= m.heap[h + 1]) {
                fail(&m, "номер за пределами списка");
                break;
            }
            push(&m, m.heap[m.heap[h + 3] + i]);
            break;
        }

        case OP_SETI: {
            vm_i64 v = pop(&m);
            vm_i64 i = pop(&m);
            vm_i64 h = pop(&m);

            if (!list_at(&m, h))
                break;
            if (i < 0 || i >= m.heap[h + 1]) {
                fail(&m, "номер за пределами списка");
                break;
            }
            m.heap[m.heap[h + 3] + i] = v;
            break;
        }

        case OP_LEN: {
            vm_i64 h = pop(&m);

            if (!list_at(&m, h))
                break;
            push(&m, m.heap[h + 1]);
            break;
        }

        /*
         * Добавить в конец.
         *
         * Места не хватило — отводим вдвое просторнее и переносим
         * значения, а в заголовке меняем ссылку. Указатель, который
         * держит программа, при этом остаётся верным: заголовок с места
         * не двигается. Ради этого он и заведён.
         */
        case OP_APPEND: {
            vm_i64 v = pop(&m);
            vm_i64 h = pop(&m);
            vm_i64 n, cap, data;

            if (!list_at(&m, h))
                break;
            n    = m.heap[h + 1];
            cap  = m.heap[h + 2];
            data = m.heap[h + 3];

            if (n >= cap) {
                vm_u32 grown = (vm_u32)(cap * 2);
                vm_u32 to = heap_alloc(&m, grown);
                vm_i64 i;

                if (!to)
                    break;
                for (i = 0; i < n; i++)
                    m.heap[to + i] = m.heap[data + i];
                m.heap[h + 2] = grown;
                m.heap[h + 3] = to;
                data = to;
            }

            m.heap[data + n] = v;
            m.heap[h + 1] = n + 1;
            /*
             * Возвращаем новую длину. В этом языке всякое выражение
             * даёт значение — иначе «add(список, 1)» отдельной строкой
             * оставляло бы стек пустым, а следующий за ним сброс
             * значения снимал бы чужое.
             */
            push(&m, n + 1);
            break;
        }

        case OP_CALLM: {
            vm_u32 name = operand(&m);
            vm_u32 argc = operand(&m);
            vm_i64 h;
            vm_u32 cls, fn;

            if ((vm_u32)m.sp < argc + 1) {
                fail(&m, "стек пуст, а метод ждёт объект");
                break;
            }
            h = m.stack[m.sp - argc - 1];
            if (!object_at(&m, h, &cls))
                break;
            if (!member_of(&m, cls, name, 1, &fn)) {
                fail(&m, "у объекта нет такого метода");
                break;
            }
            call_func(&m, fn);
            break;
        }

        default:
            fail(&m, "HITTIS: неизвестная команда\n");
            break;
        }
    }

    return (m.stopped < 0) ? -1 : 0;
}
