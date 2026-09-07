/*
 * hittis — компилятор языка Hittis в приложение .slt
 *
 * Язык читается как питон и на этом сходство кончается: это отдельный
 * маленький язык, у которого нет ни объектов, ни списков, ни словарей.
 * Внутри всё — целые числа; строки существуют только как готовые
 * постоянные, которые можно напечатать или вывести на экран.
 *
 * Так сделано намеренно. Приложение исполняет виртуальная машина
 * размером в пару сотен строк, работающая в телефоне без библиотек и
 * без выделения памяти по требованию. Всё, что нельзя туда уместить,
 * языку не нужно.
 *
 * Пример целиком:
 *
 *      app "Часы" 400 200
 *
 *      def показать(t):
 *          text(20, 20, 4, 0xFF80D0FF, "СЕКУНД")
 *          printn(t)
 *
 *      window(400, 200)
 *      while 1:
 *          rect(0, 0, 400, 200, 0xFF101828)
 *          показать(time() / 1000)
 *          show()
 *          sleep(200)
 *
 * Компилятор однопроходный: разбирает и сразу выдаёт байткод, а места
 * переходов проставляет задним числом. Для языка без объявлений типов и
 * без оптимизаций этого достаточно, а читать такой компилятор можно
 * подряд, сверху вниз.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slt.h"

/* --- Общие ограничения ------------------------------------------------
 *
 * Все размеры заданы числом и невелики. Это не лень: приложение должно
 * помещаться в телефон, у которого виртуальная машина не умеет просить
 * память. Упереться в предел лучше при сборке, с внятным сообщением,
 * чем на устройстве.
 */
#define MAX_CODE     65536
#define MAX_STR      8192
#define MAX_FUNCS    128
#define MAX_NAMES    256
#define MAX_TOKENS   65536
#define MAX_DEPTH    32

/* --- Разбор на слова -------------------------------------------------- */

enum tok_kind {
    T_EOF, T_NEWLINE, T_INDENT, T_DEDENT,
    T_NAME, T_NUMBER, T_STRING, T_OP,
};

struct token {
    int  kind;
    long long num;          /* для чисел                        */
    int  str;               /* номер строки в области строк     */
    char text[64];          /* для имён и знаков                */
    int  line;              /* для сообщений об ошибках         */
};

static struct token toks[MAX_TOKENS];
static int ntok, tpos;

static char strbuf[MAX_STR];
static int  strlen_used;

static const char *src;
static int srclen, spos, sline = 1;

static void die(int line, const char *what)
{
    fprintf(stderr, "hittis: строка %d: %s\n", line, what);
    exit(1);
}

/* Положить строку в общую область, вернуть её смещение */
/*
 * Положить строку в общую область и вернуть её смещение.
 *
 * Одинаковые строки кладём один раз, и это не экономия, а свойство, на
 * которое опирается работа с объектами: имя поля машина сравнивает по
 * смещению, а не посимвольно. Пока одинаковый текст давал разные
 * смещения, ни одно поле не находилось — объект был, метод был, а
 * встретиться они не могли.
 */
static int intern(const char *s, int len)
{
    int at = 0;

    while (at < strlen_used) {
        int have = (int)strlen(strbuf + at);

        if (have == len && !memcmp(strbuf + at, s, (size_t)len))
            return at;
        at += have + 1;
    }

    at = strlen_used;
    if (strlen_used + len + 1 > MAX_STR)
        die(sline, "слишком много текста в программе");
    memcpy(strbuf + at, s, len);
    strbuf[at + len] = 0;
    strlen_used += len + 1;
    return at;
}

static void push_tok(int kind, const char *text, long long num, int str)
{
    struct token *t;

    if (ntok >= MAX_TOKENS)
        die(sline, "программа слишком велика");
    t = &toks[ntok++];
    t->kind = kind;
    t->num = num;
    t->str = str;
    t->line = sline;
    t->text[0] = 0;
    if (text) {
        strncpy(t->text, text, sizeof(t->text) - 1);
        t->text[sizeof(t->text) - 1] = 0;
    }
}

static int is_name_start(int c)
{
    /* Имена могут быть и русскими: буквы вне латиницы приходят
     * многобайтовыми, и мы просто считаем такой байт частью имени. */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || (unsigned char)c >= 0x80;
}

static int is_name_char(int c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/*
 * Разбор на слова с учётом отступов.
 *
 * Отступ — это и есть блок: там, где в других языках скобки, здесь
 * сдвиг вправо. Поэтому лексер обязан помнить лестницу отступов и
 * выдавать «шаг вправо» и «шаг влево» отдельными словами — иначе
 * разборщику неоткуда узнать, где кончается тело цикла.
 *
 * Пустые строки и строки из одного комментария на лестницу не влияют:
 * иначе комментарий, начатый не с той позиции, закрывал бы блок.
 */
static void lex(void)
{
    int stack[MAX_DEPTH];
    int depth = 0;

    stack[0] = 0;

    while (spos < srclen) {
        int indent = 0, i;

        /* Отступ в начале строки */
        while (spos < srclen && (src[spos] == ' ' || src[spos] == '\t')) {
            indent += (src[spos] == '\t') ? 4 : 1;
            spos++;
        }

        /* Пустая строка или комментарий — пропускаем целиком */
        if (spos >= srclen || src[spos] == '\n' || src[spos] == '#') {
            while (spos < srclen && src[spos] != '\n')
                spos++;
            if (spos < srclen) {
                spos++;
                sline++;
            }
            continue;
        }

        if (indent > stack[depth]) {
            if (depth + 1 >= MAX_DEPTH)
                die(sline, "слишком глубокая вложенность");
            stack[++depth] = indent;
            push_tok(T_INDENT, NULL, 0, 0);
        }
        while (indent < stack[depth]) {
            depth--;
            push_tok(T_DEDENT, NULL, 0, 0);
        }
        if (indent != stack[depth])
            die(sline, "отступ не совпадает ни с одним из открытых");

        /* Сама строка */
        while (spos < srclen && src[spos] != '\n') {
            char c = src[spos];

            if (c == ' ' || c == '\t' || c == '\r') {
                spos++;
                continue;
            }
            if (c == '#') {
                while (spos < srclen && src[spos] != '\n')
                    spos++;
                break;
            }

            if (c >= '0' && c <= '9') {
                long long v = 0;

                if (c == '0' && spos + 1 < srclen &&
                    (src[spos + 1] == 'x' || src[spos + 1] == 'X')) {
                    spos += 2;
                    while (spos < srclen) {
                        char h = src[spos];
                        int d;

                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                        else break;
                        v = v * 16 + d;
                        spos++;
                    }
                } else {
                    while (spos < srclen && src[spos] >= '0' && src[spos] <= '9')
                        v = v * 10 + (src[spos++] - '0');
                }
                push_tok(T_NUMBER, NULL, v, 0);
                continue;
            }

            if (is_name_start(c)) {
                int start = spos;

                while (spos < srclen && is_name_char(src[spos]))
                    spos++;
                {
                    char name[64];
                    int len = spos - start;

                    if (len > (int)sizeof(name) - 1)
                        die(sline, "слишком длинное имя");
                    memcpy(name, src + start, len);
                    name[len] = 0;
                    push_tok(T_NAME, name, 0, 0);
                }
                continue;
            }

            if (c == '"') {
                int start = ++spos;

                while (spos < srclen && src[spos] != '"' && src[spos] != '\n')
                    spos++;
                if (spos >= srclen || src[spos] != '"')
                    die(sline, "строка не закрыта кавычкой");
                push_tok(T_STRING, NULL, 0, intern(src + start, spos - start));
                spos++;
                continue;
            }

            /* Знаки: сначала двухсимвольные */
            {
                static const char *two[] = { "==", "!=", "<=", ">=", NULL };
                char pair[3];

                pair[0] = c;
                pair[1] = (spos + 1 < srclen) ? src[spos + 1] : 0;
                pair[2] = 0;
                for (i = 0; two[i]; i++) {
                    if (!strcmp(pair, two[i])) {
                        push_tok(T_OP, pair, 0, 0);
                        spos += 2;
                        goto next_char;
                    }
                }
            }
            /* Точка появилась вместе с классами: «объект.поле» */
            if (strchr("+-*/%<>=(),:.[]", c)) {
                char one[2] = { c, 0 };

                push_tok(T_OP, one, 0, 0);
                spos++;
                continue;
            }
            die(sline, "непонятный знак");
next_char:
            ;
        }

        push_tok(T_NEWLINE, NULL, 0, 0);
        if (spos < srclen) {
            spos++;
            sline++;
        }
    }

    while (depth > 0) {
        depth--;
        push_tok(T_DEDENT, NULL, 0, 0);
    }
    push_tok(T_EOF, NULL, 0, 0);
}

/* --- Выдача байткода -------------------------------------------------- */

static unsigned char code[MAX_CODE];
static int code_len;

static struct slt_func funcs[MAX_FUNCS];
static char func_name[MAX_FUNCS][64];
static int nfuncs;

static char global_name[MAX_NAMES][64];
static int nglobals;

static char local_name[MAX_NAMES][64];
static int nlocals;
static int in_func;             /* 0 — верхний уровень */

/*
 * Классы.
 *
 * Объект здесь — число, указатель на область в куче машины: значений,
 * кроме целых, в языке нет. Поэтому какого класса окажется выражение,
 * компилятор знать не может, и обращение «что-то.поле» разбирается уже
 * на ходу, по имени. Имя кладём в область строк и передаём машине его
 * смещением — сравнение имён у неё выходит в одно действие.
 *
 * Метод — обычная функция, у которой первый аргумент «сам». Имя ей
 * даётся составное, «Класс.метод»: иначе два класса с методом
 * «нарисовать» столкнулись бы в общей таблице функций.
 */
#define MAX_CLASSES 32
#define MAX_MEMBERS 256

static struct slt_class classes[MAX_CLASSES];
static char class_name[MAX_CLASSES][64];
static int  nclasses;

static struct slt_member members[MAX_MEMBERS];
static char member_name[MAX_MEMBERS][64];
static int  nmembers;

static int cur_class = -1;      /* внутри какого класса компилируем */

/* «Сам» можно писать и по-русски, и по-английски: слово служебное, а
 * привычка у всех своя. Внутри всё равно один и тот же нулевой аргумент. */
static int is_self_name(const char *n)
{
    return !strcmp(n, "self") || !strcmp(n, "сам");
}

static int find_class(const char *n)
{
    int i;

    for (i = 0; i < nclasses; i++)
        if (!strcmp(class_name[i], n))
            return i;
    return -1;
}

/* Номер поля в классе; -1 — нет такого */
static int find_field(int cls, const char *n)
{
    int i;

    if (cls < 0)
        return -1;
    for (i = 0; i < classes[cls].nfields; i++)
        if (!strcmp(member_name[classes[cls].members + i], n))
            return i;
    return -1;
}

static void emit(unsigned char op)
{
    if (code_len >= MAX_CODE)
        die(0, "программа слишком велика");
    code[code_len++] = op;
}

static void emit_i32(int v)
{
    if (code_len + 4 > MAX_CODE)
        die(0, "программа слишком велика");
    code[code_len++] = (unsigned char)(v & 0xFF);
    code[code_len++] = (unsigned char)((v >> 8) & 0xFF);
    code[code_len++] = (unsigned char)((v >> 16) & 0xFF);
    code[code_len++] = (unsigned char)((v >> 24) & 0xFF);
}

static int emit_jump(unsigned char op)
{
    int at;

    emit(op);
    at = code_len;
    emit_i32(0);            /* заполним, когда узнаем куда */
    return at;
}

static void patch(int at, int target)
{
    code[at + 0] = (unsigned char)(target & 0xFF);
    code[at + 1] = (unsigned char)((target >> 8) & 0xFF);
    code[at + 2] = (unsigned char)((target >> 16) & 0xFF);
    code[at + 3] = (unsigned char)((target >> 24) & 0xFF);
}

/* --- Имена ------------------------------------------------------------ */

static int find_local(const char *n)
{
    int i;

    for (i = 0; i < nlocals; i++)
        if (!strcmp(local_name[i], n))
            return i;
    return -1;
}

static int find_global(const char *n)
{
    int i;

    for (i = 0; i < nglobals; i++)
        if (!strcmp(global_name[i], n))
            return i;
    return -1;
}

static int add_local(const char *n)
{
    if (nlocals >= MAX_NAMES)
        die(0, "слишком много переменных в функции");
    strncpy(local_name[nlocals], n, 63);
    local_name[nlocals][63] = 0;
    return nlocals++;
}

static int add_global(const char *n)
{
    if (nglobals >= MAX_NAMES)
        die(0, "слишком много глобальных переменных");
    strncpy(global_name[nglobals], n, 63);
    global_name[nglobals][63] = 0;
    return nglobals++;
}

static int find_func(const char *n)
{
    int i;

    for (i = 0; i < nfuncs; i++)
        if (!strcmp(func_name[i], n))
            return i;
    return -1;
}

/* Встроенные вызовы: имя — номер. Договор с виртуальной машиной. */
static const struct {
    const char *name;
    int id;
    int argc;
} natives[] = {
    { "print",  NAT_PRINT,  1 },
    { "printn", NAT_PRINTN, 1 },
    { "window", NAT_WINDOW, 2 },
    { "rect",   NAT_RECT,   5 },
    { "text",   NAT_TEXT,   5 },
    { "textn",  NAT_TEXTN,  5 },
    { "show",   NAT_SHOW,   0 },
    { "touch",  NAT_TOUCH,  0 },
    { "sleep",  NAT_SLEEP,  1 },
    { "time",   NAT_TIME,   0 },
    { "width",  NAT_WIDTH,  0 },
    { "height", NAT_HEIGHT, 0 },
    { "exit",   NAT_EXIT,   1 },
    { NULL, 0, 0 }
};

/* --- Разбор ----------------------------------------------------------- */

static struct token *cur(void)      { return &toks[tpos]; }
static struct token *next(void)     { return &toks[tpos++]; }

static int is_op(const char *s)
{
    return cur()->kind == T_OP && !strcmp(cur()->text, s);
}

static int is_kw(const char *s)
{
    return cur()->kind == T_NAME && !strcmp(cur()->text, s);
}

static void expect_op(const char *s)
{
    if (!is_op(s))
        die(cur()->line, "ожидался другой знак");
    tpos++;
}

static void expect_newline(void)
{
    if (cur()->kind != T_NEWLINE)
        die(cur()->line, "лишнее в конце строки");
    tpos++;
}

static void expr(void);
static void postfix(void);

static void call_args(int expected, const char *what)
{
    int n = 0;

    expect_op("(");
    if (!is_op(")")) {
        for (;;) {
            expr();
            n++;
            if (is_op(",")) {
                tpos++;
                continue;
            }
            break;
        }
    }
    expect_op(")");
    if (expected >= 0 && n != expected)
        die(cur()->line, what);
}

static void primary(void)
{
    struct token *t = cur();

    if (t->kind == T_NUMBER) {
        /*
         * Больше четырёх байт число в команду не влезет, и молча
         * обрезать его нельзя: обрезанная константа — ошибка, которая
         * потом ищется часами, потому что программа при этом честно
         * работает и даёт неверный ответ. Считать шире можно, писать —
         * нет.
         */
        if (t->num > 4294967295LL)
            die(t->line, "число не помещается в четыре байта");
        tpos++;
        emit(OP_PUSH);
        emit_i32((int)(unsigned int)t->num);
        return;
    }
    if (t->kind == T_STRING) {
        tpos++;
        emit(OP_PUSHS);
        emit_i32(t->str);
        return;
    }
    if (is_op("(")) {
        tpos++;
        expr();
        expect_op(")");
        return;
    }

    /* Список прямо в тексте: [1, 2, 3] или просто [] */
    if (is_op("[")) {
        int n = 0;

        tpos++;
        if (!is_op("]")) {
            for (;;) {
                expr();
                n++;
                if (is_op(",")) {
                    tpos++;
                    continue;
                }
                break;
            }
        }
        expect_op("]");
        emit(OP_LIST);
        emit_i32(n);
        return;
    }
    if (t->kind == T_NAME) {
        char name[64];
        int i;

        strncpy(name, t->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        tpos++;

        /*
         * len и add — команды самой машины, а не обращения к системе.
         *
         * Обращение наружу нужно там, где без мира не обойтись:
         * нарисовать, напечатать, спросить палец. Длина списка миру не
         * интересна, и гонять её через ту же дверь значило бы делать
         * вид, что список живёт где-то снаружи. Он живёт в машине.
         */
        if (is_op("(") && (!strcmp(name, "len") || !strcmp(name, "add"))) {
            int want = !strcmp(name, "len") ? 1 : 2;
            int argc = 0;

            tpos++;
            if (!is_op(")")) {
                for (;;) {
                    expr();
                    argc++;
                    if (is_op(",")) {
                        tpos++;
                        continue;
                    }
                    break;
                }
            }
            expect_op(")");
            if (argc != want)
                die(t->line, "не столько аргументов");
            emit(want == 1 ? OP_LEN : OP_APPEND);
            return;
        }

        if (is_op("(") && find_class(name) >= 0) {
            /*
             * Создание объекта.
             *
             * Заводим его, удваиваем указатель и зовём create — тот самый
             * метод, который в других языках называют конструктором. Удвоение нужно потому, что метод указатель
             * съест, а вернуть нам надо именно его: значение метода
             * выбрасываем, объект остаётся.
             *
             * Нет метода create — значит объект просто заводится с
             * нулевыми полями, и это законно.
             */
            int cls = find_class(name);
            int has_new = 0;
            int j;

            for (j = 0; j < classes[cls].nmethods; j++) {
                int at = classes[cls].members + classes[cls].nfields + j;

                if (!strcmp(member_name[at], "create"))
                    has_new = 1;
            }

            emit(OP_NEW);
            emit_i32(cls);

            if (has_new) {
                int argc = 0;

                emit(OP_DUP);
                tpos++;                 /* съели «(» */
                if (!is_op(")")) {
                    for (;;) {
                        expr();
                        argc++;
                        if (is_op(",")) {
                            tpos++;
                            continue;
                        }
                        break;
                    }
                }
                expect_op(")");
                emit(OP_CALLM);
                emit_i32(intern("create", 6));
                emit_i32(argc);
                emit(OP_POP);           /* что вернул create, не нужно */
            } else {
                expect_op("(");
                expect_op(")");
            }
            return;
        }

        if (is_op("(")) {
            /* Вызов: сначала встроенные, потом свои */
            for (i = 0; natives[i].name; i++) {
                if (!strcmp(natives[i].name, name)) {
                    call_args(natives[i].argc, "не столько аргументов");
                    emit(OP_NATIVE);
                    emit_i32(natives[i].id);
                    return;
                }
            }
            {
                int f = find_func(name);

                if (f < 0)
                    die(t->line, "неизвестная функция");
                call_args(funcs[f].nargs, "не столько аргументов");
                emit(OP_CALL);
                emit_i32(f);
                return;
            }
        }

        /* Просто имя: сначала местная переменная, потом глобальная */
        if (in_func) {
            int l = find_local(name);

            if (l >= 0) {
                emit(OP_LOADL);
                emit_i32(l);
                return;
            }
        }
        {
            int g = find_global(name);

            if (g < 0)
                die(t->line, "переменная не определена");
            emit(OP_LOADG);
            emit_i32(g);
            return;
        }
    }

    die(t->line, "ожидалось значение");
}

/*
 * Точка после значения: поле или метод.
 *
 * Разбираем цепочкой, чтобы «а.б.в» работало само собой. Имя кладём в
 * область строк и передаём машине смещением — сравнивать смещения ей
 * дешевле, чем строки, а совпадают они ровно тогда, когда совпадают
 * имена: одинаковый текст компилятор интернирует один раз.
 */
/*
 * Чем кончилась последняя цепочка.
 *
 * Нужно для присваивания: «а.б = 1» и «а[и] = 1» разбираются так же,
 * как чтение, а потом последнее взятие превращается в запись. Так
 * работает любая цепочка любой длины, и не приходится заранее угадывать
 * её вид по двум-трём токенам вперёд.
 */
static int last_get_op;         /* OP_GETF или OP_GETI, 0 — не было   */
static int last_get_at;         /* где эта команда началась в коде    */
static int last_get_name;       /* имя поля, если это было поле       */

static void postfix(void)
{
    primary();
    last_get_op = 0;

    for (;;) {
        struct token *t;
        char name[64];

        /* Номер в списке: значение[номер] */
        if (is_op("[")) {
            tpos++;
            expr();
            expect_op("]");
            last_get_op = OP_GETI;
            last_get_at = code_len;
            emit(OP_GETI);
            continue;
        }

        if (!is_op("."))
            break;

        tpos++;
        if (cur()->kind != T_NAME)
            die(cur()->line, "после точки ожидалось имя");
        t = cur();
        strncpy(name, t->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        tpos++;

        if (is_op("(")) {
            int argc = 0;

            tpos++;
            if (!is_op(")")) {
                for (;;) {
                    expr();
                    argc++;
                    if (is_op(",")) {
                        tpos++;
                        continue;
                    }
                    break;
                }
            }
            expect_op(")");
            emit(OP_CALLM);
            emit_i32(intern(name, (int)strlen(name)));
            emit_i32(argc);
        } else {
            last_get_op = OP_GETF;
            last_get_at = code_len;
            last_get_name = intern(name, (int)strlen(name));
            emit(OP_GETF);
            emit_i32(last_get_name);
        }
    }
}

static void unary(void)
{
    if (is_op("-")) {
        tpos++;
        unary();
        emit(OP_NEG);
        return;
    }
    if (is_kw("not")) {
        tpos++;
        unary();
        emit(OP_NOT);
        return;
    }
    postfix();
}

static void mul_expr(void)
{
    unary();
    for (;;) {
        if (is_op("*"))      { tpos++; unary(); emit(OP_MUL); }
        else if (is_op("/")) { tpos++; unary(); emit(OP_DIV); }
        else if (is_op("%")) { tpos++; unary(); emit(OP_MOD); }
        else return;
    }
}

static void add_expr(void)
{
    mul_expr();
    for (;;) {
        if (is_op("+"))      { tpos++; mul_expr(); emit(OP_ADD); }
        else if (is_op("-")) { tpos++; mul_expr(); emit(OP_SUB); }
        else return;
    }
}

static void cmp_expr(void)
{
    add_expr();
    for (;;) {
        if (is_op("<"))       { tpos++; add_expr(); emit(OP_LT); }
        else if (is_op(">"))  { tpos++; add_expr(); emit(OP_GT); }
        else if (is_op("<=")) { tpos++; add_expr(); emit(OP_LE); }
        else if (is_op(">=")) { tpos++; add_expr(); emit(OP_GE); }
        else if (is_op("==")) { tpos++; add_expr(); emit(OP_EQ); }
        else if (is_op("!=")) { tpos++; add_expr(); emit(OP_NE); }
        else return;
    }
}

static void and_expr(void)
{
    cmp_expr();
    while (is_kw("and")) {
        tpos++;
        cmp_expr();
        emit(OP_AND);
    }
}

static void expr(void)
{
    and_expr();
    while (is_kw("or")) {
        tpos++;
        and_expr();
        emit(OP_OR);
    }
}

static void block(void);

static void statement(void)
{
    struct token *t = cur();

    if (is_kw("if")) {
        int jz, jend = -1;

        tpos++;
        expr();
        expect_op(":");
        jz = emit_jump(OP_JZ);
        block();
        if (is_kw("else")) {
            tpos++;
            expect_op(":");
            jend = emit_jump(OP_JMP);
            patch(jz, code_len);
            block();
            patch(jend, code_len);
        } else {
            patch(jz, code_len);
        }
        return;
    }

    if (is_kw("while")) {
        int top = code_len;
        int jz;

        tpos++;
        expr();
        expect_op(":");
        jz = emit_jump(OP_JZ);
        block();
        {
            int back = emit_jump(OP_JMP);

            patch(back, top);
        }
        patch(jz, code_len);
        return;
    }

    if (is_kw("return")) {
        tpos++;
        if (cur()->kind == T_NEWLINE) {
            emit(OP_PUSH);
            emit_i32(0);
        } else {
            expr();
        }
        emit(OP_RET);
        expect_newline();
        return;
    }

    /*
     * Присваивание в поле или в элемент списка.
     *
     * Левую часть разбираем как обычное чтение, а потом, если следом
     * стоит знак равенства, отматываем последнее взятие и ставим на его
     * место запись. Стек при этом уже подготовлен правильно: под запись
     * поля лежит объект, под запись элемента — список и номер.
     *
     * Так работает цепочка любой длины и любого вида. Заглядывание
     * вперёд на два-три токена, с которого я начал, разбирало только
     * заранее придуманные случаи и молча пропускало остальные.
     */
    if (t->kind == T_NAME && toks[tpos + 1].kind == T_OP &&
        (!strcmp(toks[tpos + 1].text, ".") ||
         !strcmp(toks[tpos + 1].text, "["))) {
        int save_code = code_len;
        int save_tok = tpos;

        postfix();

        if (last_get_op && is_op("=")) {
            int op = last_get_op;
            int name = last_get_name;

            code_len = last_get_at;     /* отматываем взятие */
            tpos++;                     /* съели «=»         */
            expr();
            if (op == OP_GETF) {
                emit(OP_SETF);
                emit_i32(name);
            } else {
                emit(OP_SETI);
            }
            expect_newline();
            return;
        }

        /* Не присваивание — начинаем заново и идём общим путём */
        code_len = save_code;
        tpos = save_tok;
    }

    /* Присваивание или просто выражение */
    if (t->kind == T_NAME && toks[tpos + 1].kind == T_OP &&
        !strcmp(toks[tpos + 1].text, "=")) {
        char name[64];
        int slot;

        strncpy(name, t->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        tpos += 2;
        expr();

        if (in_func) {
            slot = find_local(name);
            if (slot < 0)
                slot = add_local(name);
            emit(OP_STOREL);
            emit_i32(slot);
        } else {
            slot = find_global(name);
            if (slot < 0)
                slot = add_global(name);
            emit(OP_STOREG);
            emit_i32(slot);
        }
        expect_newline();
        return;
    }

    expr();
    emit(OP_POP);           /* значение никому не нужно */
    expect_newline();
}

static void block(void)
{
    expect_newline();
    if (cur()->kind != T_INDENT)
        die(cur()->line, "после двоеточия ожидался сдвинутый блок");
    tpos++;
    while (cur()->kind != T_DEDENT && cur()->kind != T_EOF)
        statement();
    if (cur()->kind == T_DEDENT)
        tpos++;
}

/* --- Заголовок приложения --------------------------------------------- */

static char app_name[SLT_NAME_MAX] = "БЕЗ ИМЕНИ";
static int app_w, app_h;

static void parse_app(void)
{
    tpos++;                 /* app */
    if (cur()->kind != T_STRING)
        die(cur()->line, "после app ожидалось имя в кавычках");
    strncpy(app_name, strbuf + cur()->str, SLT_NAME_MAX - 1);
    app_name[SLT_NAME_MAX - 1] = 0;
    tpos++;

    /*
     * Размер необязателен, и без него приложение занимает весь экран.
     *
     * Так правильнее по умолчанию: приложение на телефоне — это экран
     * целиком, а не окошко посередине. Раньше размер приходилось писать
     * числами, и числа эти были про конкретный телефон — приложение
     * переставало быть переносимым от одной строки заголовка.
     *
     * 0xFFFF означает «сколько дадут»: система всё равно обрезает
     * просьбу до рабочей области, и это единственное место, где она
     * знает, сколько на самом деле места.
     */
    if (cur()->kind == T_NUMBER) {
        app_w = (int)next()->num;
        if (cur()->kind != T_NUMBER)
            die(cur()->line, "после ширины ожидалась высота");
        app_h = (int)next()->num;
    } else {
        app_w = 0xFFFF;
        app_h = 0xFFFF;
    }
    expect_newline();
}

/* --- Сборка целиком --------------------------------------------------- */

/*
 * Функции ищем заранее, отдельным проходом по словам.
 *
 * Иначе вызвать функцию, объявленную ниже по тексту, было бы нельзя, а
 * это ровно то, чего никто не ожидает от языка: порядок объявлений не
 * должен решать, что можно вызвать.
 */
/*
 * Обойти классы заранее.
 *
 * Как и с функциями: тело метода может обращаться к другому классу,
 * который в исходнике ниже. Разбираем сначала объявления, потом код —
 * иначе порядок в файле начал бы решать, что можно назвать.
 *
 * Поля класса нигде не объявляются: ими становится всё, чему метод
 * присваивает через «сам». Так меньше слов и не бывает расхождения
 * между списком полей и тем, что на самом деле используется.
 */
static void prescan_classes(void)
{
    int i, cls = -1, depth = 0;

    for (i = 0; i + 1 < ntok; i++) {
        if (toks[i].kind == T_INDENT && cls >= 0) {
            depth++;
            continue;
        }
        if (toks[i].kind == T_DEDENT && cls >= 0) {
            if (--depth <= 0)
                cls = -1;               /* тело класса кончилось */
            continue;
        }

        if (toks[i].kind == T_NAME && !strcmp(toks[i].text, "class") &&
            toks[i + 1].kind == T_NAME) {
            if (nclasses >= MAX_CLASSES)
                die(toks[i].line, "слишком много классов");
            strncpy(class_name[nclasses], toks[i + 1].text, 63);
            class_name[nclasses][63] = 0;
            classes[nclasses].members = (unsigned int)nmembers;
            classes[nclasses].nfields = 0;
            classes[nclasses].nmethods = 0;
            cls = nclasses++;
            depth = 0;
            continue;
        }

        /* Поле: «сам.имя =» внутри класса */
        if (cls >= 0 && toks[i].kind == T_NAME &&
            is_self_name(toks[i].text) &&
            i + 3 < ntok &&
            toks[i + 1].kind == T_OP && !strcmp(toks[i + 1].text, ".") &&
            toks[i + 2].kind == T_NAME &&
            toks[i + 3].kind == T_OP && !strcmp(toks[i + 3].text, "=")) {
            if (find_field(cls, toks[i + 2].text) < 0) {
                if (nmembers >= MAX_MEMBERS)
                    die(toks[i].line, "слишком много полей");
                /* Поля идут подряд, поэтому новое можно добавлять только
                 * пока у класса нет ни одного метода. */
                if (classes[cls].nmethods)
                    die(toks[i].line,
                        "поле появилось после метода: заведи его раньше");
                strncpy(member_name[nmembers], toks[i + 2].text, 63);
                member_name[nmembers][63] = 0;
                members[nmembers].value = classes[cls].nfields;
                nmembers++;
                classes[cls].nfields++;
            }
        }
    }
}

static void prescan_funcs(void)
{
    int i, cls = -1, depth = 0;

    /* Нулевая функция — сама программа: тело верхнего уровня */
    strcpy(func_name[0], "");
    nfuncs = 1;

    for (i = 0; i + 1 < ntok; i++) {
        if (toks[i].kind == T_INDENT && cls >= 0) {
            depth++;
            continue;
        }
        if (toks[i].kind == T_DEDENT && cls >= 0) {
            if (--depth <= 0)
                cls = -1;
            continue;
        }
        if (toks[i].kind == T_NAME && !strcmp(toks[i].text, "class") &&
            toks[i + 1].kind == T_NAME) {
            cls = find_class(toks[i + 1].text);
            depth = 0;
            continue;
        }

        if (toks[i].kind == T_NAME && !strcmp(toks[i].text, "def") &&
            toks[i + 1].kind == T_NAME) {
            int nargs = 0, j = i + 2;

            if (nfuncs >= MAX_FUNCS)
                die(toks[i].line, "слишком много функций");
            if (toks[j].kind == T_OP && !strcmp(toks[j].text, "(")) {
                j++;
                while (toks[j].kind == T_NAME) {
                    nargs++;
                    j++;
                    if (toks[j].kind == T_OP && !strcmp(toks[j].text, ","))
                        j++;
                }
            }
            if (cls >= 0) {
                /*
                 * Метод. Имя составное — «Класс.метод», — иначе два
                 * класса с методом «нарисовать» столкнулись бы в общей
                 * таблице функций.
                 */
                if (nmembers >= MAX_MEMBERS)
                    die(toks[i].line, "слишком много членов класса");
                snprintf(func_name[nfuncs], 64, "%s.%s",
                         class_name[cls], toks[i + 1].text);
                strncpy(member_name[nmembers], toks[i + 1].text, 63);
                member_name[nmembers][63] = 0;
                members[nmembers].value = (unsigned int)nfuncs;
                nmembers++;
                classes[cls].nmethods++;
            } else {
                strncpy(func_name[nfuncs], toks[i + 1].text, 63);
                func_name[nfuncs][63] = 0;
            }
            funcs[nfuncs].nargs = (unsigned short)nargs;
            nfuncs++;
        }
    }
}

/*
 * Тело функции или метода.
 *
 * Метод отличается от функции ровно одним: он числится за классом, и
 * его первый аргумент — «сам». Отдельного кода для этого не нужно, имя
 * ему уже дано составное при предпросмотре, а «сам» становится обычной
 * нулевой ячейкой кадра, когда мы разбираем список аргументов.
 */
static void compile_def(int *func_idx, int cls)
{
    int skip;

    tpos++;
    if (cur()->kind != T_NAME)
        die(cur()->line, "после def ожидалось имя");
    {
        int f;

        if (cls >= 0) {
            char full[64];

            snprintf(full, sizeof(full), "%s.%s", class_name[cls],
                     cur()->text);
            f = find_func(full);
        } else {
            f = find_func(cur()->text);
        }
        if (f < 0)
            die(cur()->line, "функция не найдена (внутренняя ошибка)");
        *func_idx = f;
    }
    tpos++;

    skip = emit_jump(OP_JMP);   /* верхний уровень перепрыгнет тело */

    funcs[*func_idx].entry = (unsigned int)code_len;
    nlocals = 0;
    in_func = 1;

    expect_op("(");
    while (cur()->kind == T_NAME) {
        add_local(next()->text);
        if (is_op(","))
            tpos++;
    }
    expect_op(")");
    expect_op(":");
    block();

    /* Функция без явного возврата всё равно обязана вернуть */
    emit(OP_PUSH);
    emit_i32(0);
    emit(OP_RET);

    funcs[*func_idx].nlocals = (unsigned short)nlocals;
    in_func = 0;
    patch(skip, code_len);
}

static void compile(void)
{
    int func_idx = 1;

    prescan_classes();
    prescan_funcs();

    /* Тело верхнего уровня — нулевая функция */
    funcs[0].entry = 0;
    funcs[0].nargs = 0;

    while (cur()->kind != T_EOF) {
        if (cur()->kind == T_NEWLINE) {
            tpos++;
            continue;
        }

        if (is_kw("app")) {
            parse_app();
            continue;
        }

        if (is_kw("class")) {
            /*
             * Класс — это только объявление: своего кода у него нет,
             * есть код его методов. Поля уже собраны предпросмотром,
             * поэтому здесь мы просто идём по телу и компилируем каждый
             * метод как обычную функцию, запомнив, чьим он был.
             */
            tpos++;
            if (cur()->kind != T_NAME)
                die(cur()->line, "после class ожидалось имя");
            cur_class = find_class(cur()->text);
            if (cur_class < 0)
                die(cur()->line, "класс не найден (внутренняя ошибка)");
            tpos++;
            expect_op(":");
            expect_newline();
            if (cur()->kind != T_INDENT)
                die(cur()->line, "после класса ожидался сдвинутый блок");
            tpos++;

            while (cur()->kind != T_DEDENT && cur()->kind != T_EOF) {
                if (cur()->kind == T_NEWLINE) {
                    tpos++;
                    continue;
                }
                if (!is_kw("def"))
                    die(cur()->line, "в классе бывают только методы");
                compile_def(&func_idx, cur_class);
            }
            if (cur()->kind == T_DEDENT)
                tpos++;
            cur_class = -1;
            continue;
        }

        if (is_kw("def")) {
            compile_def(&func_idx, -1);
            continue;
        }



        statement();
    }

    emit(OP_HALT);
}

/* --- Запись файла ----------------------------------------------------- */

static unsigned int checksum(const unsigned char *p, int n)
{
    unsigned int s = 0x5A17;
    int i;

    for (i = 0; i < n; i++)
        s = (s * 31u) + p[i];
    return s;
}

int main(int argc, char **argv)
{
    FILE *f;
    struct slt_header h;
    long size;
    char *buf;
    const char *in, *out;
    unsigned int sum;

    if (argc != 3) {
        fprintf(stderr, "как пользоваться: hittis программа.ht приложение.slt\n");
        return 1;
    }
    in = argv[1];
    out = argv[2];

    f = fopen(in, "rb");
    if (!f) {
        fprintf(stderr, "hittis: не открыть %s\n", in);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size + 1);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        fprintf(stderr, "hittis: не прочитать %s\n", in);
        return 1;
    }
    buf[size] = 0;
    fclose(f);

    src = buf;
    srclen = (int)size;
    lex();
    compile();

    memset(&h, 0, sizeof(h));
    h.magic[0] = SLT_MAGIC0;
    h.magic[1] = SLT_MAGIC1;
    h.magic[2] = SLT_MAGIC2;
    h.magic[3] = SLT_MAGIC3;
    h.version = SLT_VERSION;
    strncpy(h.name, app_name, SLT_NAME_MAX - 1);
    h.win_w = (unsigned short)app_w;
    h.win_h = (unsigned short)app_h;
    h.func_count = (unsigned int)nfuncs;
    h.global_count = (unsigned int)nglobals;
    h.str_bytes = (unsigned int)strlen_used;
    h.code_bytes = (unsigned int)code_len;
    h.class_count = (unsigned int)nclasses;
    h.member_count = (unsigned int)nmembers;

    /*
     * Имена членов уезжают в область строк, и туда же смотрит машина.
     * Делаем это здесь, а не при разборе: пока классы не дочитаны,
     * список членов ещё меняется.
     */
    {
        int i;

        for (i = 0; i < nclasses; i++)
            classes[i].name = (unsigned int)intern(class_name[i],
                                                   (int)strlen(class_name[i]));
        for (i = 0; i < nmembers; i++)
            members[i].name = (unsigned int)intern(member_name[i],
                                                   (int)strlen(member_name[i]));
        h.str_bytes = (unsigned int)strlen_used;
    }

    /*
     * Контрольная сумма считается по всему, что после заголовка. Она не
     * от злого умысла, а от испорченного файла: приложение читается с
     * флешки, и запустить полусчитанный байткод хуже, чем не запустить
     * ничего.
     */
    {
        unsigned char *all = malloc(nfuncs * sizeof(struct slt_func) +
                                    nclasses * sizeof(struct slt_class) +
                                    nmembers * sizeof(struct slt_member) +
                                    strlen_used + code_len);
        int at = 0;

        memcpy(all + at, funcs, nfuncs * sizeof(struct slt_func));
        at += nfuncs * sizeof(struct slt_func);
        memcpy(all + at, classes, nclasses * sizeof(struct slt_class));
        at += nclasses * sizeof(struct slt_class);
        memcpy(all + at, members, nmembers * sizeof(struct slt_member));
        at += nmembers * sizeof(struct slt_member);
        memcpy(all + at, strbuf, strlen_used);
        at += strlen_used;
        memcpy(all + at, code, code_len);
        at += code_len;
        sum = checksum(all, at);
        h.checksum = sum;

        f = fopen(out, "wb");
        if (!f) {
            fprintf(stderr, "hittis: не создать %s\n", out);
            return 1;
        }
        fwrite(&h, 1, sizeof(h), f);
        fwrite(all, 1, at, f);
        fclose(f);
        free(all);
    }

    printf("hittis: %s -> %s\n", in, out);
    printf("        имя «%s», окно %dx%d\n", h.name, h.win_w, h.win_h);
    printf("        функций %u, глобальных %u, строк %u байт, кода %u байт\n",
           h.func_count, h.global_count, h.str_bytes, h.code_bytes);
    if (h.class_count)
        printf("        классов %u, полей и методов %u\n",
               h.class_count, h.member_count);
    return 0;
}
