/*
 * preview — показать приложение Hittis на компьютере, до телефона.
 *
 * Замысел простой: писать приложение и видеть его сразу. Показываем в
 * браузере: открываешь localhost и видишь приложение того размера, что
 * оно попросило, тем же шрифтом, что на телефоне. Мышь работает пальцем,
 * а правка исходника перезапускает приложение сама.
 *
 * Почему браузер, а не окно. Первым был X11: библиотека нашлась, окно
 * создавалось, X-сервер честно докладывал «показано» — а на экране у
 * человека не появлялось ничего, потому что WSLg окна до рабочего стола
 * не доносил. Спорить с этим бессмысленно. Браузер есть на всякой
 * машине, работает из WSL без графики вовсе и заодно по сети — можно
 * смотреть с телефона на приложение, собранное на компьютере.
 *
 * Это честная проверка, а не похожая: виртуальная машина здесь ТА ЖЕ
 * САМАЯ, файл hittis/vm.c берётся как есть. Разница ровно в таблице
 * vm_host — что делать, когда приложение просит нарисовать или подождать
 * палец: на телефоне системные вызовы, здесь картинка и щелчки мышью.
 * Значит если приложение ведёт себя тут и там по-разному, расходятся не
 * две реализации, а эти две таблицы, и смотреть надо в них.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#include "vm.h"
#include "slt.h"

typedef unsigned char  u8;
typedef unsigned int   u32;

/* --- Растровый шрифт .stf, приготовленный tools/fontgen ------------- */

struct face {
    u32 px, line, base, count;
    const u32 *codes;
    const u8  *glyphs;
    const u8  *bits;
};

static u8         *font_blob;
static struct face faces[8];
static u32         nfaces;

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int font_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    u32 n;

    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    font_blob = malloc((size_t)len);
    if (fread(font_blob, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return -1;
    }
    fclose(f);

    if (memcmp(font_blob, "STF1", 4) != 0)
        return -1;

    n = rd32(font_blob + 4);
    if (n > 8)
        return -1;

    for (u32 i = 0; i < n; i++) {
        const u8 *d = font_blob + 8 + i * 32;

        faces[i].px = rd32(d);
        faces[i].line = rd32(d + 4);
        faces[i].base = rd32(d + 8);
        faces[i].count = rd32(d + 12);
        faces[i].codes = (const u32 *)(font_blob + rd32(d + 16));
        faces[i].glyphs = font_blob + rd32(d + 20);
        faces[i].bits = font_blob + rd32(d + 24);
    }
    nfaces = n;
    return 0;
}

static const struct face *face_for(u32 px)
{
    const struct face *best = NULL;
    u32 bestd = 0;

    for (u32 i = 0; i < nfaces; i++) {
        u32 d = faces[i].px > px ? faces[i].px - px : px - faces[i].px;

        if (!best || d < bestd) {
            best = &faces[i];
            bestd = d;
        }
    }

    return best;
}

/* --- Кадр ----------------------------------------------------------- */

static u32 *pix;                /* 0xAARRGGBB, как на телефоне */
static int  win_w, win_h;
static u32  frame_no;           /* сколько раз приложение показало кадр */

/* --- Касания, пришедшие из браузера --------------------------------- */

#define TOUCH_RING 64

static vm_i64 ring[TOUCH_RING];
static int    ring_head, ring_tail;

static void touch_put(vm_i64 t)
{
    int next = (ring_head + 1) % TOUCH_RING;

    if (next == ring_tail)
        return;                 /* переполнение: теряем новое, не старое */
    ring[ring_head] = t;
    ring_head = next;
}

static int touch_get(vm_i64 *t)
{
    if (ring_tail == ring_head)
        return 0;
    *t = ring[ring_tail];
    ring_tail = (ring_tail + 1) % TOUCH_RING;
    return 1;
}

/* --- Исходник, за которым следим ------------------------------------ */

static const char *src_path;
static time_t      src_mtime;
static const char *self_path;
static const char *shot_path;
static int         port = 8080;

/*
 * Среда разработки.
 *
 * Тот же самый двоичный файл, только страница другая: слева список
 * приложений, посередине редактор, справа — телефон, который уже умел
 * показывать просмотр. Держать это одной программой правильнее, чем
 * двумя: машина, шрифт и отрисовка кадра нужны и там, и там, а
 * копировать их значило бы завести вторую правду о том, как выглядит
 * приложение.
 */
static int  listen_fd = -1;
static int  ide_mode;                   /* показывать редактор, а не один кадр */
static int  reopened;                   /* это перезапуск, а не первый пуск    */
static char apps_dir[512] = "apps";     /* где искать .ht                      */
static char build_dir[512] = ".";       /* где лежит компилятор                */
static char compile_err[4096];          /* что сказал компилятор в прошлый раз */
static int  compile_ok;                 /* и чем это кончилось                 */

static time_t mtime_of(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void die(const char *why)
{
    fprintf(stderr, "preview: %s\n", why);
    exit(1);
}

/*
 * Перезапуск с тем же исходником или с другим.
 *
 * Слушающий сокет закрываем ПЕРЕД сменой образа. Открытые файлы
 * переживают exec, и прежний сокет остался бы висеть на порту: новый
 * образ не смог бы его занять и уехал бы на соседний. Для просмотра это
 * мелочь, а для среды разработки — беда: адрес в браузере обязан
 * оставаться одним и тем же, иначе каждое сохранение уводило бы человека
 * на новую вкладку.
 *
 * Порт и режим передаём себе же аргументами: без них перезапуск вернул
 * бы простой просмотр вместо редактора.
 */
static void restart_with(const char *path)
{
    char port_arg[16];

    snprintf(port_arg, sizeof(port_arg), "%d", port);
    printf("\n=== перезапускаю на %s ===\n\n", path);
    fflush(stdout);

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }

    /*
     * Помечаем себя «снова»: браузер открывать больше не нужно, вкладка
     * уже открыта и никуда не делась. Порт мы держим тот же нарочно —
     * ровно чтобы она пережила перезапуск. Открывать вторую значит
     * каждым сохранением подсовывать человеку новое окно.
     */
    if (ide_mode)
        execl(self_path, self_path, path, port_arg, "--ide", "--снова",
              (char *)NULL);
    else
        execl(self_path, self_path, path, port_arg, "--снова", (char *)NULL);
    die("перезапуститься не вышло");
}

static void restart(void)
{
    restart_with(src_path);
}

/* --- Картинка кадра: BMP -------------------------------------------- */

/*
 * BMP, а не PNG.
 *
 * PNG требует сжатия, то есть zlib, которого в системе нет: менять одну
 * зависимость на другую ради просмотра не стоит. BMP показывают все
 * браузеры, а кадр уходит по localhost, где два мегабайта ничего не
 * стоят. И отдаём мы его только когда приложение нарисовало новый:
 * браузер сначала спрашивает номер кадра, а это несколько байт.
 */
static u8 *bmp_make(u32 *out_len)
{
    int row = win_w * 3;
    int pad = (4 - (row % 4)) % 4;
    u32 data = (u32)(row + pad) * (u32)win_h;
    u32 len = 54 + data;
    u8 *b = calloc(len, 1);
    u32 o;

    b[0] = 'B'; b[1] = 'M';
    b[2] = (u8)len; b[3] = (u8)(len >> 8);
    b[4] = (u8)(len >> 16); b[5] = (u8)(len >> 24);
    b[10] = 54;
    b[14] = 40;
    b[18] = (u8)win_w; b[19] = (u8)(win_w >> 8);
    b[20] = (u8)(win_w >> 16); b[21] = (u8)(win_w >> 24);
    b[22] = (u8)win_h; b[23] = (u8)(win_h >> 8);
    b[24] = (u8)(win_h >> 16); b[25] = (u8)(win_h >> 24);
    b[26] = 1;
    b[28] = 24;
    b[34] = (u8)data; b[35] = (u8)(data >> 8);
    b[36] = (u8)(data >> 16); b[37] = (u8)(data >> 24);

    o = 54;
    for (int y = win_h - 1; y >= 0; y--) {       /* BMP идёт снизу вверх */
        for (int x = 0; x < win_w; x++) {
            u32 c = pix[y * win_w + x];

            b[o++] = (u8)c;                      /* синий   */
            b[o++] = (u8)(c >> 8);               /* зелёный */
            b[o++] = (u8)(c >> 16);              /* красный */
        }
        o += (u32)pad;
    }

    *out_len = len;
    return b;
}

/* --- Страница ------------------------------------------------------- */

static const char page[] =
"<!doctype html><meta charset=utf-8><title>Hittis</title>"
"<style>"
"body{background:#0b0f1a;color:#8fa3c8;font:14px system-ui;margin:0;"
"display:flex;flex-direction:column;align-items:center;gap:12px;padding:16px}"
"img{image-rendering:pixelated;border-radius:8px;box-shadow:0 8px 40px #0008;"
"cursor:crosshair;user-select:none;max-height:88vh;width:auto}"
"b{color:#cfe0ff}"
"</style>"
"<div><b id=n>приложение</b> — мышь работает пальцем, "
"правка исходника перезапускает</div>"
"<img id=e draggable=false>"
"<script>"
"let v=-1,down=false;"
"const e=document.getElementById('e');"
/* Кадр телефона выше окна браузера, поэтому картинку ужимаем — и
 * координаты щелчка возвращаем обратно в пиксели приложения. Без
 * этого палец попадал бы не туда, куда человек смотрит. */
"function pos(ev){const r=e.getBoundingClientRect();"
"const kx=e.naturalWidth/r.width,ky=e.naturalHeight/r.height;"
"return[Math.round((ev.clientX-r.left)*kx),Math.round((ev.clientY-r.top)*ky)];}"
"function send(a,ev){const p=pos(ev);"
"fetch('/t?a='+a+'&x='+p[0]+'&y='+p[1]);}"
"e.addEventListener('mousedown',ev=>{down=true;send(0,ev);ev.preventDefault();});"
"e.addEventListener('mousemove',ev=>{if(down)send(1,ev);});"
"window.addEventListener('mouseup',ev=>{if(down){down=false;send(2,ev);}});"
"async function tick(){try{"
"const r=await fetch('/v');const t=await r.text();const n=+t.split(' ')[0];"
"if(n!==v){v=n;e.src='/f?'+n;"
"document.getElementById('n').textContent=t.split(' ').slice(1).join(' ');}"
"}catch(err){}setTimeout(tick,60);}"
"tick();"
"</script>";

/*
 * Страница среды разработки лежит отдельным файлом ide.html и попадает
 * сюда при сборке. Держать полторы сотни строк разметки строковым
 * литералом в C можно, но читать и править их потом нельзя: пропадает
 * подсветка, отступы и всякая возможность увидеть страницу целиком.
 */
#include "ide_html.h"

/* --- Сервер --------------------------------------------------------- */

static char app_name[64] = "приложение";

static void http_start(void)
{
    struct sockaddr_in a;
    int on = 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        die("нет сокета");
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    /* Порт может быть занят прошлым просмотром — берём следующий */
    for (int i = 0; i < 20; i++) {
        a.sin_port = htons((unsigned short)(port + i));
        if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            port += i;
            listen(listen_fd, 8);
            printf("смотреть здесь:  http://localhost:%d\n", port);
            fflush(stdout);
            return;
        }
    }
    die("не занять порт");
}

static void send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;

    while (n) {
        ssize_t k = write(fd, p, n);

        if (k <= 0)
            return;
        p += k;
        n -= (size_t)k;
    }
}

static void reply(int fd, const char *type, const void *body, size_t len)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", type, len);

    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static int arg_int(const char *req, const char *key)
{
    const char *p = strstr(req, key);

    return p ? atoi(p + strlen(key)) : 0;
}

/* --- Файлы приложений ------------------------------------------------
 *
 * Среда разработки пишет в файлы, а значит обязана быть подозрительной.
 * Имя приходит из браузера, и хотя браузер этот — свой, на localhost,
 * принимать из него путь целиком нельзя: одна точка с косой чертой, и
 * запись уходит куда угодно. Поэтому имя проверяется на вид, а папка
 * приложений подставляется здесь и только здесь.
 */
static int name_ok(const char *f)
{
    if (!f || !*f)
        return 0;
    for (const char *p = f; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            return 0;
    if (strstr(f, ".."))
        return 0;
    return strstr(f, ".ht") != NULL;
}

static void app_path(char *out, size_t max, const char *name)
{
    snprintf(out, max, "%s/%s", apps_dir, name);
}

/* Список приложений одной строкой: имена через перевод строки */
static void files_list(char *out, size_t max)
{
    DIR *d = opendir(apps_dir);
    struct dirent *e;
    size_t n = 0;

    out[0] = 0;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        size_t k;

        if (!name_ok(e->d_name))
            continue;
        k = strlen(e->d_name);
        if (n + k + 2 >= max)
            break;
        memcpy(out + n, e->d_name, k);
        n += k;
        out[n++] = '\n';
    }
    out[n] = 0;
    closedir(d);
}

static char *text_read(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
        n = 0;
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    if (len)
        *len = (size_t)n;
    return buf;
}

static int text_write(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (!f)
        return -1;
    ok = (len == 0) || fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}

/*
 * Собрать и запомнить, что сказал компилятор.
 *
 * Вывод забираем целиком, вместе с руганью: в среде разработки он и есть
 * главное, что человеку нужно увидеть после сохранения. Раньше он уходил
 * в терминал, где его никто не читал.
 */
static int compile_to(const char *src, const char *out_slt)
{
    char cmd[2600];
    FILE *p;
    size_t n = 0;
    int rc;

    snprintf(cmd, sizeof(cmd), "%s/hittis '%s' '%s' 2>&1",
             build_dir, src, out_slt);

    compile_err[0] = 0;
    p = popen(cmd, "r");
    if (!p) {
        snprintf(compile_err, sizeof(compile_err),
                 "не запустить компилятор");
        return -1;
    }
    while (n + 1 < sizeof(compile_err)) {
        size_t k = fread(compile_err + n, 1, sizeof(compile_err) - 1 - n, p);

        if (!k)
            break;
        n += k;
    }
    compile_err[n] = 0;
    rc = pclose(p);
    compile_ok = (rc == 0);
    return compile_ok ? 0 : -1;
}

/*
 * Прочитать запрос целиком, вместе с телом.
 *
 * Раньше хватало одного read: все запросы были короткими GET. Сохранение
 * текста — это POST с телом в несколько килобайт, и оно приходит не
 * обязательно одним куском. Читаем, пока не увидим конец заголовков, а
 * потом добираем ровно столько, сколько обещано в Content-Length.
 *
 * Возвращает длину прочитанного или 0.
 */
static size_t read_request(int fd, char *buf, size_t max)
{
    size_t n = 0;
    char *head_end = NULL;
    long need = 0;

    while (n + 1 < max) {
        ssize_t k = read(fd, buf + n, max - 1 - n);

        if (k <= 0)
            break;
        n += (size_t)k;
        buf[n] = 0;

        if (!head_end) {
            head_end = strstr(buf, "\r\n\r\n");
            if (head_end) {
                const char *cl = strcasestr(buf, "Content-Length:");

                need = cl ? strtol(cl + 15, NULL, 10) : 0;
                head_end += 4;
            }
        }
        if (head_end && (size_t)(head_end - buf) + (size_t)need <= n)
            break;
    }
    buf[n] = 0;
    return n;
}

/* Значение строкового параметра из запроса: ?f=имя */
static void arg_str(const char *req, const char *key, char *out, size_t max)
{
    const char *p = strstr(req, key);
    size_t n = 0;

    out[0] = 0;
    if (!p)
        return;
    p += strlen(key);
    while (*p && *p != '&' && *p != ' ' && *p != '\r' && n + 1 < max) {
        /* Имена приходят в процентной записи: кириллица иначе не проедет */
        if (*p == '%' && p[1] && p[2]) {
            char h[3] = { p[1], p[2], 0 };

            out[n++] = (char)strtol(h, NULL, 16);
            p += 3;
        } else {
            out[n++] = (*p == '+') ? ' ' : *p;
            p++;
        }
    }
    out[n] = 0;
}

/*
 * Совпадает ли запрос с этим путём.
 *
 * Сравнение по префиксу тут не годится: «GET /f» совпадает и с «/files»,
 * и список приложений уезжал в обработчик картинки. Поэтому после пути
 * обязателен разделитель — пробел или вопрос.
 */
static int route(const char *req, const char *what)
{
    size_t n = strlen(what);

    if (strncmp(req, what, n) != 0)
        return 0;
    return req[n] == ' ' || req[n] == '?';
}

static void serve_one(int fd)
{
    static char req[262144];        /* сюда влезает исходник целиком */
    size_t n = read_request(fd, req, sizeof(req));
    char name[256];
    char path[768];

    if (!n) {
        close(fd);
        return;
    }

    if (route(req, "GET /v")) {
        char buf[128];
        int k = snprintf(buf, sizeof(buf), "%u %s", frame_no, app_name);

        reply(fd, "text/plain; charset=utf-8", buf, (size_t)k);
    } else if (route(req, "GET /f")) {
        if (!pix) {
            reply(fd, "text/plain; charset=utf-8", "нет кадра", 17);
        } else {
            u32 len = 0;
            u8 *bmp = bmp_make(&len);

            reply(fd, "image/bmp", bmp, len);
            free(bmp);
        }
    } else if (route(req, "GET /t")) {
        int a = arg_int(req, "a=");
        int x = arg_int(req, "x=");
        int y = arg_int(req, "y=");

        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        touch_put(((vm_i64)a << 48) | ((vm_i64)x << 32) | (vm_i64)y);
        reply(fd, "text/plain", "ok", 2);

    /* --- дальше только для среды разработки --- */

    } else if (route(req, "GET /files")) {
        char list[4096];

        files_list(list, sizeof(list));
        reply(fd, "text/plain; charset=utf-8", list, strlen(list));

    } else if (route(req, "GET /src")) {
        arg_str(req, "f=", name, sizeof(name));
        if (!name_ok(name)) {
            reply(fd, "text/plain; charset=utf-8", "", 0);
        } else {
            size_t len = 0;
            char *txt;

            app_path(path, sizeof(path), name);
            txt = text_read(path, &len);
            reply(fd, "text/plain; charset=utf-8", txt ? txt : "", txt ? len : 0);
            free(txt);
        }

    } else if (route(req, "GET /err")) {
        /*
         * Первой строкой — чем кончилась сборка, дальше сам вывод.
         * Компилятор говорит и при удаче: имя приложения, размер окна,
         * сколько вышло кода. Это полезно видеть, но красить его в цвет
         * ошибки нельзя, а по одному тексту успех от неудачи не отличить.
         */
        char buf[4200];
        int k = snprintf(buf, sizeof(buf), "%s\n%s",
                         compile_ok ? "ок" : "ошибка", compile_err);

        reply(fd, "text/plain; charset=utf-8", buf, (size_t)k);

    } else if (route(req, "GET /now")) {
        /* Какой файл сейчас открыт машиной: браузер должен знать, что
         * показывает телефон справа, а не гадать по имени приложения. */
        const char *p = src_path ? src_path : "";
        const char *slash = strrchr(p, '/');

        if (slash)
            p = slash + 1;
        reply(fd, "text/plain; charset=utf-8", p, strlen(p));

    } else if (route(req, "POST /save")) {
        const char *body = strstr(req, "\r\n\r\n");

        arg_str(req, "f=", name, sizeof(name));
        if (!name_ok(name) || !body) {
            reply(fd, "text/plain; charset=utf-8", "имя не годится", 27);
        } else {
            body += 4;
            app_path(path, sizeof(path), name);
            if (text_write(path, body, strlen(body)) != 0) {
                reply(fd, "text/plain; charset=utf-8", "не записать", 21);
            } else {
                /*
                 * Сохранили — и всё. Перезапуск случится сам: за временем
                 * правки следит тот же цикл, что и раньше. Второго
                 * механизма заводить незачем, а один общий заодно значит,
                 * что правка из любого редактора работает так же.
                 */
                reply(fd, "text/plain; charset=utf-8", "ок", 4);
            }
        }

    } else if (route(req, "GET /open")) {
        arg_str(req, "f=", name, sizeof(name));
        if (!name_ok(name)) {
            reply(fd, "text/plain; charset=utf-8", "имя не годится", 27);
        } else {
            static char keep[768];

            app_path(keep, sizeof(keep), name);
            reply(fd, "text/plain; charset=utf-8", "ок", 4);
            close(fd);
            restart_with(keep);         /* не возвращается */
            return;
        }

    } else if (route(req, "GET /build")) {
        char out[768];
        char msg[5200];
        int rc;

        arg_str(req, "f=", name, sizeof(name));
        if (!name_ok(name)) {
            reply(fd, "text/plain; charset=utf-8", "имя не годится", 27);
        } else {
            char *dot;

            app_path(path, sizeof(path), name);
            snprintf(out, sizeof(out), "%s", path);
            dot = strrchr(out, '.');
            if (dot)
                strcpy(dot, ".slt");

            rc = compile_to(path, out);
            snprintf(msg, sizeof(msg), "%s%s\n%s",
                     rc == 0 ? "собрано: " : "не собралось: ",
                     out, compile_err);
            reply(fd, "text/plain; charset=utf-8", msg, strlen(msg));
        }

    } else if (ide_mode) {
        reply(fd, "text/html; charset=utf-8", ide_html, sizeof(ide_html));
    } else {
        reply(fd, "text/html; charset=utf-8", page, sizeof(page) - 1);
    }

    close(fd);
}

/*
 * Поднять сервер и показать страницу. Оба действия однократные: при
 * ошибке в исходнике сюда заходят раньше сборки, а потом ещё раз —
 * обычным путём, и второй заход не должен ни занимать порт заново, ни
 * открывать вторую вкладку.
 */
static void serve_begin(void)
{
    char open_cmd[160];
    static int shown;

    if (listen_fd < 0)
        http_start();
    if (shown || reopened)
        return;
    shown = 1;

    /*
     * Открываем браузер сами. Из WSL это делается через cmd.exe:
     * страница должна показаться в Windows, а не внутри Linux, где её
     * всё равно некому показать. Не вышло — ничего страшного, адрес
     * напечатан выше.
     */
    printf("открываю браузер\n");
    fflush(stdout);

    snprintf(open_cmd, sizeof(open_cmd),
             "cmd.exe /c start http://localhost:%d >/dev/null 2>&1 || "
             "xdg-open http://localhost:%d >/dev/null 2>&1", port, port);
    if (system(open_cmd) != 0)
        printf("(браузер не открылся сам — открой адрес выше руками)\n");
}

/*
 * Обслужить запросы. wait_ms — сколько ждать, если ничего не пришло.
 *
 * Заодно следим за исходником: правка перезапускает всё целиком.
 */
static void http_pump(int wait_ms)
{
    fd_set r;
    struct timeval tv;
    int fd;

    if (src_mtime && mtime_of(src_path) != src_mtime)
        restart();

    FD_ZERO(&r);
    FD_SET(listen_fd, &r);
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;

    if (select(listen_fd + 1, &r, NULL, NULL, &tv) <= 0)
        return;

    fd = accept(listen_fd, NULL, NULL);
    if (fd >= 0)
        serve_one(fd);
}

/* --- Что машина просит у мира --------------------------------------- */

static void h_print(const char *s)  { printf("%s", s); fflush(stdout); }
static void h_printn(vm_i64 v)      { printf("%lld\n", v); fflush(stdout); }

/*
 * Сколько места «даёт» просмотр.
 *
 * Ровно столько же, сколько даст телефон: рабочая область merlin — экран
 * 1080x2340 без полосы состояния и полоски «домой». Приложение, которое
 * просит «сколько дадут», должно увидеть здесь то же самое, что увидит
 * там, иначе проверка на компьютере перестаёт что-либо значить.
 */
#define AREA_W  1080
#define AREA_H  2124

static int h_window(int w, int h)
{
    if (pix)
        return 1;               /* окно уже есть, второго не бывает */

    if (w <= 0 || w > AREA_W)
        w = AREA_W;
    if (h <= 0 || h > AREA_H)
        h = AREA_H;

    win_w = w;
    win_h = h;
    pix = calloc((size_t)w * h, 4);
    return 1;
}

static void h_rect(int x, int y, int w, int h, vm_u32 color)
{
    int x1 = x + w, y1 = y + h;

    if (!pix)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > win_w)
        x1 = win_w;
    if (y1 > win_h)
        y1 = win_h;

    for (int py = y; py < y1; py++)
        for (int px = x; px < x1; px++)
            pix[py * win_w + px] = color;
}

/* Смешать цвет буквы с тем, что под ней — сглаживание как на телефоне */
static u32 blend(u32 dst, u32 fg, u32 a)
{
    u32 r, g, b;

    if (a >= 255)
        return fg;
    r = (((fg >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * (255 - a)) / 255;
    g = (((fg >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * (255 - a)) / 255;
    b = ((fg & 0xFF) * a + (dst & 0xFF) * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static int glyph_find(const struct face *f, u32 cp, const u8 **out)
{
    u32 lo = 0, hi;

    if (!f->count)
        return -1;
    hi = f->count - 1;
    while (lo <= hi) {
        u32 mid = (lo + hi) / 2;
        u32 c = f->codes[mid];

        if (c == cp) {
            *out = f->glyphs + mid * 8;
            return 0;
        }
        if (c < cp)
            lo = mid + 1;
        else if (!mid)
            break;
        else
            hi = mid - 1;
    }
    return -1;
}

static u32 utf8_next(const char **s)
{
    const u8 *p = (const u8 *)*s;
    u32 b = *p++;

    if (b >= 0xC0 && b <= 0xDF && (*p & 0xC0) == 0x80) {
        u32 cp = ((b & 0x1F) << 6) | (*p & 0x3F);

        p++;
        *s = (const char *)p;
        return cp;
    }
    if (b >= 0xE0 && b <= 0xEF && (p[0] & 0xC0) == 0x80 &&
        (p[1] & 0xC0) == 0x80) {
        u32 cp = ((b & 0x0F) << 12) | ((u32)(p[0] & 0x3F) << 6) | (p[1] & 0x3F);

        p += 2;
        *s = (const char *)p;
        return cp;
    }
    *s = (const char *)p;
    return b;
}

static void draw_text(int x, int y, int scale, vm_u32 color, const char *s)
{
    const struct face *f = face_for((u32)scale * 8);

    if (!f || !pix)
        return;

    while (*s) {
        u32 cp = utf8_next(&s);
        const u8 *g;
        u32 off, w, h, adv;
        int left, top;

        if (glyph_find(f, cp, &g) != 0) {
            x += (int)f->px / 2;
            continue;
        }

        off = (u32)g[0] | ((u32)g[1] << 8) | ((u32)g[2] << 16);
        w = g[3];
        h = g[4];
        adv = g[5];
        left = (signed char)g[6];
        top = (signed char)g[7];

        for (u32 row = 0; row < h; row++) {
            int py = y + (int)f->base + top + (int)row;

            if (py < 0 || py >= win_h)
                continue;
            for (u32 col = 0; col < w; col++) {
                int px = x + left + (int)col;
                u32 a = f->bits[off + row * w + col];

                if (!a || px < 0 || px >= win_w)
                    continue;
                pix[py * win_w + px] = blend(pix[py * win_w + px], color, a);
            }
        }
        x += (int)adv;
    }
}

static void h_text(int x, int y, int scale, vm_u32 color, const char *s)
{
    draw_text(x, y, scale, color, s);
}

static void h_textn(int x, int y, int scale, vm_u32 color, vm_i64 v)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%lld", v);
    draw_text(x, y, scale, color, buf);
}

/* Снять кадр в файл: нужно там, где браузера нет вовсе */
static void save_shot(void)
{
    FILE *f = fopen(shot_path, "wb");

    if (!f) {
        fprintf(stderr, "preview: не создать %s\n", shot_path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
    for (int i = 0; i < win_w * win_h; i++) {
        u32 c = pix[i];
        u8 rgb[3] = { (u8)(c >> 16), (u8)(c >> 8), (u8)c };

        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("кадр снят: %s (%dx%d)\n", shot_path, win_w, win_h);
    exit(0);
}

static void h_show(void)
{
    frame_no++;
    if (shot_path)
        save_shot();
    http_pump(0);               /* отдать кадр тем, кто уже ждёт */
}

static vm_i64 h_touch(void)
{
    vm_i64 t;

    for (;;) {
        if (touch_get(&t))
            return t;
        http_pump(20);
    }
}

static void h_sleep(vm_i64 ms)
{
    while (ms > 0) {
        vm_i64 step = ms > 20 ? 20 : ms;

        http_pump((int)step);
        ms -= step;
    }
}

static vm_i64 h_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (vm_i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int h_width(void)  { return win_w; }
static int h_height(void) { return win_h; }

static const struct vm_host host = {
    h_print, h_printn, h_window, h_rect, h_text, h_textn,
    h_show, h_touch, h_sleep, h_time, h_width, h_height
};

/* --- Запуск --------------------------------------------------------- */

static const char *font_paths[] = {
    "hittis/manrope.stf",       /* делается вместе с просмотром */
    "manrope.stf",
    "kernel/build/merlin/font/manrope.stf",
    "kernel/build/qemu/font/manrope.stf",
    "../kernel/build/merlin/font/manrope.stf",
    NULL
};

static void *load_file(const char *path, vm_u32 *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (vm_u32)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *path;
    char slt[512];
    char dir[512];
    void *image;
    vm_u32 len;
    const char *name;
    int rc;

    if (argc < 2) {
        fprintf(stderr,
                "как пользоваться: preview приложение.ht [порт]\n"
                "  показывает приложение в браузере на localhost\n"
                "  preview приложение.ht --кадр вид.ppm — снять первый кадр\n");
        return 1;
    }

    self_path = argv[0];
    path = argv[1];
    if (argc >= 4 && strcmp(argv[2], "--кадр") == 0)
        shot_path = argv[3];
    else if (argc >= 3 && atoi(argv[2]))
        port = atoi(argv[2]);

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ide") == 0)
            ide_mode = 1;
        if (strcmp(argv[i], "--снова") == 0)
            reopened = 1;
    }

    /* Рядом с собой лежит компилятор — им и собираем */
    snprintf(dir, sizeof(dir), "%s", argv[0]);
    {
        char *d = strrchr(dir, '/');

        if (d)
            *d = 0;
        else
            snprintf(dir, sizeof(dir), ".");
    }

    snprintf(build_dir, sizeof(build_dir), "%s", dir);
    {
        /* Папка приложений — та, где лежит открытый файл. Список слева
         * должен показывать соседей по папке, а не гадать по имени. */
        const char *slash = strrchr(path, '/');

        if (slash && (size_t)(slash - path) < sizeof(apps_dir))
            snprintf(apps_dir, sizeof(apps_dir), "%.*s",
                     (int)(slash - path), path);
        else
            snprintf(apps_dir, sizeof(apps_dir), ".");
    }

    if (strstr(path, ".ht")) {
        src_path = path;
        src_mtime = mtime_of(path);
        snprintf(slt, sizeof(slt), "/tmp/hittis-preview.slt");

        /*
         * В среде разработки сервер поднимаем ДО сборки.
         *
         * Иначе при ошибке в исходнике страницы просто нет: человек
         * сохранил файл с опечаткой и остался с мёртвой вкладкой вместо
         * текста ошибки. А ошибка — это ровно то, ради чего среда и
         * нужна; показать её важнее, чем показать кадр.
         */
        if (ide_mode && !shot_path)
            serve_begin();

        if (compile_to(path, slt) != 0) {
            fprintf(stderr, "%s", compile_err);
            /*
             * Не выходим: приложение дописывают прямо во время просмотра,
             * и половина правок компилятору не нравится. Ждём следующей
             * правки, а не заставляем запускать всё заново.
             */
            fprintf(stderr, "\n=== жду исправления исходника ===\n");
            for (;;) {
                if (mtime_of(path) != src_mtime)
                    restart();

                /* Страница жива и показывает ошибку: править можно прямо
                 * в ней, а сохранение перезапустит нас само. */
                if (listen_fd >= 0) {
                    http_pump(200);
                } else {
                    struct timespec ts = { 0, 200 * 1000 * 1000 };

                    nanosleep(&ts, NULL);
                }
            }
        }
    } else {
        snprintf(slt, sizeof(slt), "%s", path);
    }

    for (int i = 0; font_paths[i]; i++)
        if (font_load(font_paths[i]) == 0)
            break;
    if (!nfaces)
        fprintf(stderr, "preview: шрифт не найден, текста не будет\n");

    image = load_file(slt, &len);
    if (!image) {
        fprintf(stderr, "preview: не открыть %s\n", slt);
        return 1;
    }

    name = vm_app_name(image, len);
    if (name)
        snprintf(app_name, sizeof(app_name), "%s", name);

    printf("=== %s ===\n", app_name);
    if (!shot_path) {
        serve_begin();

        printf("мышь работает пальцем; правка %s перезапускает сама\n\n",
               src_path ? src_path : "исходника");
        fflush(stdout);
    }

    rc = vm_run(image, len, &host);

    printf(rc == 0 ? "\n=== приложение завершилось ===\n"
                   : "\n=== приложение остановлено машиной ===\n");

    /*
     * Не выходим сразу: последний кадр надо успеть разглядеть, а правка
     * исходника всё равно перезапустит нас заново.
     */
    if (!shot_path) {
        printf("страница осталась открытой; Ctrl+C — выход\n");
        fflush(stdout);
        for (;;)
            http_pump(100);
    }

    return rc == 0 ? 0 : 1;
}
