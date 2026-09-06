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
static int intern(const char *s, int len)
{
    int at = strlen_used;

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
            if (strchr("+-*/%<>=(),:", c)) {
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
        tpos++;
        emit(OP_PUSH);
        emit_i32((int)t->num);
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
    if (t->kind == T_NAME) {
        char name[64];
        int i;

        strncpy(name, t->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        tpos++;

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
    primary();
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
    if (cur()->kind == T_NUMBER) {
        app_w = (int)next()->num;
        if (cur()->kind != T_NUMBER)
            die(cur()->line, "после ширины ожидалась высота");
        app_h = (int)next()->num;
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
static void prescan_funcs(void)
{
    int i;

    /* Нулевая функция — сама программа: тело верхнего уровня */
    strcpy(func_name[0], "");
    nfuncs = 1;

    for (i = 0; i + 1 < ntok; i++) {
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
            strncpy(func_name[nfuncs], toks[i + 1].text, 63);
            func_name[nfuncs][63] = 0;
            funcs[nfuncs].nargs = (unsigned short)nargs;
            nfuncs++;
        }
    }
}

static void compile(void)
{
    int func_idx = 1;

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

        if (is_kw("def")) {
            /*
             * Тело функции выдаём тут же, посреди основного кода, а
             * вход в неё запоминаем. Обходить его не нужно: на верхнем
             * уровне мы до него не доходим — функции идут после
             * последней команды программы только в исходнике, а в
             * байткоде порядок неважен, потому что переход в функцию
             * всегда явный.
             */
            int skip;

            tpos++;
            if (cur()->kind != T_NAME)
                die(cur()->line, "после def ожидалось имя");
            {
                int f = find_func(cur()->text);

                if (f < 0)
                    die(cur()->line, "функция не найдена (внутренняя ошибка)");
                func_idx = f;
            }
            tpos++;

            skip = emit_jump(OP_JMP);   /* верхний уровень перепрыгнет тело */

            funcs[func_idx].entry = (unsigned int)code_len;
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

            funcs[func_idx].nlocals = (unsigned short)nlocals;
            in_func = 0;
            patch(skip, code_len);
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

    /*
     * Контрольная сумма считается по всему, что после заголовка. Она не
     * от злого умысла, а от испорченного файла: приложение читается с
     * флешки, и запустить полусчитанный байткод хуже, чем не запустить
     * ничего.
     */
    {
        unsigned char *all = malloc(nfuncs * sizeof(struct slt_func) +
                                    strlen_used + code_len);
        int at = 0;

        memcpy(all + at, funcs, nfuncs * sizeof(struct slt_func));
        at += nfuncs * sizeof(struct slt_func);
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
    return 0;
}
