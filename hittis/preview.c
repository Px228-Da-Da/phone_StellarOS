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

static void restart(void)
{
    printf("\n=== исходник изменился, перезапускаю ===\n\n");
    fflush(stdout);
    execl(self_path, self_path, src_path, (char *)NULL);
    die("перезапуститься не вышло");
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
"cursor:crosshair;user-select:none}"
"b{color:#cfe0ff}"
"</style>"
"<div><b id=n>приложение</b> — мышь работает пальцем, "
"правка исходника перезапускает</div>"
"<img id=e draggable=false>"
"<script>"
"let v=-1,down=false;"
"const e=document.getElementById('e');"
"function pos(ev){const r=e.getBoundingClientRect();"
"return[Math.round(ev.clientX-r.left),Math.round(ev.clientY-r.top)];}"
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

/* --- Сервер --------------------------------------------------------- */

static int  listen_fd = -1;
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

static void serve_one(int fd)
{
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);

    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = 0;

    if (strncmp(req, "GET /v", 6) == 0) {
        char buf[128];
        int k = snprintf(buf, sizeof(buf), "%u %s", frame_no, app_name);

        reply(fd, "text/plain; charset=utf-8", buf, (size_t)k);
    } else if (strncmp(req, "GET /f", 6) == 0) {
        if (!pix) {
            reply(fd, "text/plain; charset=utf-8", "нет кадра", 17);
        } else {
            u32 len = 0;
            u8 *bmp = bmp_make(&len);

            reply(fd, "image/bmp", bmp, len);
            free(bmp);
        }
    } else if (strncmp(req, "GET /t", 6) == 0) {
        int a = arg_int(req, "a=");
        int x = arg_int(req, "x=");
        int y = arg_int(req, "y=");

        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        touch_put(((vm_i64)a << 48) | ((vm_i64)x << 32) | (vm_i64)y);
        reply(fd, "text/plain", "ok", 2);
    } else {
        reply(fd, "text/html; charset=utf-8", page, sizeof(page) - 1);
    }

    close(fd);
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

static int h_window(int w, int h)
{
    if (pix)
        return 1;               /* окно уже есть, второго не бывает */

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
    char cmd[1200];
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

    /* Рядом с собой лежит компилятор — им и собираем */
    snprintf(dir, sizeof(dir), "%s", argv[0]);
    {
        char *d = strrchr(dir, '/');

        if (d)
            *d = 0;
        else
            snprintf(dir, sizeof(dir), ".");
    }

    if (strstr(path, ".ht")) {
        src_path = path;
        src_mtime = mtime_of(path);
        snprintf(slt, sizeof(slt), "/tmp/hittis-preview.slt");
        snprintf(cmd, sizeof(cmd), "%s/hittis %s %s", dir, path, slt);
        if (system(cmd) != 0) {
            /*
             * Не выходим: приложение дописывают прямо во время просмотра,
             * и половина правок компилятору не нравится. Ждём следующей
             * правки, а не заставляем запускать всё заново.
             */
            fprintf(stderr, "\n=== жду исправления исходника ===\n");
            for (;;) {
                struct timespec ts = { 0, 200 * 1000 * 1000 };

                nanosleep(&ts, NULL);
                if (mtime_of(path) != src_mtime)
                    restart();
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
        char open_cmd[128];

        http_start();

        /*
         * Открываем браузер сами. Из WSL это делается через cmd.exe:
         * страница должна показаться в Windows, а не внутри Linux, где
         * её всё равно некому показать. Не вышло — ничего страшного,
         * адрес напечатан выше.
         */
        snprintf(open_cmd, sizeof(open_cmd),
                 "cmd.exe /c start http://localhost:%d >/dev/null 2>&1 || "
                 "xdg-open http://localhost:%d >/dev/null 2>&1", port, port);
        if (system(open_cmd) != 0)
            printf("(браузер не открылся сам — открой адрес выше руками)\n");

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
