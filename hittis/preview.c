/*
 * preview — запустить приложение Hittis на компьютере, в настоящем окне.
 *
 * Замысел простой: писать приложение и видеть его сразу, не трогая
 * телефон. Окно того же размера, что попросит приложение, тот же шрифт,
 * мышь вместо пальца. Правка исходника перезапускает приложение сама —
 * сохранил файл, и через мгновение оно уже другое.
 *
 * Это работает не потому, что мы старательно повторили поведение
 * телефона, а потому, что повторять нечего: виртуальная машина здесь та
 * же самая, файл hittis/vm.c берётся как есть. Разница ровно в таблице
 * vm_host — что делать, когда приложение просит нарисовать или подождать
 * палец. Там системные вызовы, здесь X11.
 *
 * Отсюда правило, которым стоит пользоваться: если приложение ведёт себя
 * здесь и на телефоне по-разному, то расходятся не «две реализации», а
 * ровно эти две таблицы, и смотреть надо в них.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

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

/* --- Окно и кадр ---------------------------------------------------- */

static Display *dpy;
static Window   win;
static GC       gc;
static XImage  *img;
static u32     *pix;            /* кадр в 0xAARRGGBB, как на телефоне */
static int      win_w, win_h;
static Atom     wm_delete;

/* Что происходит с мышью — она же палец */
static int mouse_down;
static int last_mx, last_my;

/* Куда сохранить первый кадр и выйти. Нужно, когда экрана нет вовсе —
 * по ssh или в сборочной машине: посмотреть глазами всё равно надо. */
static const char *shot_path;

/* Исходник, за которым следим */
static const char *src_path;
static time_t      src_mtime;
static const char *self_path;

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

/* Перезапуститься целиком: и компилятор, и машина начнут с чистого листа */
static void restart(void)
{
    printf("\n=== исходник изменился, перезапускаю ===\n\n");
    fflush(stdout);
    if (dpy)
        XCloseDisplay(dpy);
    execl(self_path, self_path, src_path, (char *)NULL);
    die("перезапуститься не вышло");
}

static void window_open(int w, int h, const char *name)
{
    XSizeHints hints;
    char title[128];

    dpy = XOpenDisplay(NULL);
    if (!dpy)
        die("нет дисплея (в WSL нужен WSLg или свой X-сервер)");

    win_w = w;
    win_h = h;
    pix = calloc((size_t)w * h, 4);

    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0,
                              (unsigned)w, (unsigned)h, 0, 0, 0x00101828);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | KeyPressMask | StructureNotifyMask);

    /* Размер окна постоянный: приложение просило именно такой, и тянуть
     * его мышью значило бы показывать не то, что будет на телефоне. */
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = w;
    hints.min_height = hints.max_height = h;
    XSetWMNormalHints(dpy, win, &hints);

    snprintf(title, sizeof(title), "Hittis: %s", name);
    XStoreName(dpy, win, title);

    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XMapWindow(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);

    img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
                       ZPixmap, 0, (char *)pix, (unsigned)w, (unsigned)h, 32, 0);
    if (!img)
        die("не вышло завести кадр");
}

static void window_show(void)
{
    if (!dpy)
        return;
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned)win_w, (unsigned)win_h);
    XFlush(dpy);
}

/*
 * Разобрать накопившиеся события окна.
 *
 * Возвращает упакованное касание (как на телефоне) или -1, если касаний
 * не было. Заодно следит за исходником: правка файла перезапускает всё.
 */
static vm_i64 pump(int block)
{
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            int action = -1;

            XNextEvent(dpy, &e);
            switch (e.type) {
            case Expose:
                window_show();
                break;
            case ButtonPress:
                if (e.xbutton.button != Button1)
                    break;
                mouse_down = 1;
                last_mx = e.xbutton.x;
                last_my = e.xbutton.y;
                action = 0;
                break;
            case ButtonRelease:
                if (e.xbutton.button != Button1)
                    break;
                mouse_down = 0;
                last_mx = e.xbutton.x;
                last_my = e.xbutton.y;
                action = 2;
                break;
            case MotionNotify:
                if (!mouse_down)
                    break;
                last_mx = e.xmotion.x;
                last_my = e.xmotion.y;
                action = 1;
                break;
            case KeyPress: {
                KeySym k = XLookupKeysym(&e.xkey, 0);

                if (k == 'q' || k == XK_Escape) {
                    printf("=== закрыто ===\n");
                    exit(0);
                }
                if (k == 'r')
                    restart();
                break;
            }
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == wm_delete) {
                    printf("=== окно закрыли ===\n");
                    exit(0);
                }
                break;
            }

            if (action >= 0) {
                int x = last_mx < 0 ? 0 : last_mx;
                int y = last_my < 0 ? 0 : last_my;

                return ((vm_i64)action << 48) | ((vm_i64)x << 32) | (vm_i64)y;
            }
        }

        /* Правка исходника — повод начать всё заново */
        if (src_mtime && mtime_of(src_path) != src_mtime)
            restart();

        if (!block)
            return -1;

        /* Спим коротко: и события не задерживаем, и ядро не жжём */
        {
            struct timespec ts = { 0, 8 * 1000 * 1000 };

            nanosleep(&ts, NULL);
        }
    }
}

/* --- Что машина просит у мира --------------------------------------- */

static void h_print(const char *s)  { printf("%s", s); fflush(stdout); }
static void h_printn(vm_i64 v)      { printf("%lld\n", v); fflush(stdout); }

static int h_window(int w, int h)
{
    if (dpy)
        return 1;               /* окно уже есть, второго не бывает */
    window_open(w, h, "приложение");
    return 1;
}

static void h_rect(int x, int y, int w, int h, vm_u32 color)
{
    int x1 = x + w, y1 = y + h;

    if (!pix)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > win_w) x1 = win_w;
    if (y1 > win_h) y1 = win_h;

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
    if (b >= 0xE0 && b <= 0xEF && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
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
        unsigned char rgb[3] = { (unsigned char)(c >> 16),
                                 (unsigned char)(c >> 8),
                                 (unsigned char)c };

        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("кадр снят: %s (%dx%d)\n", shot_path, win_w, win_h);
    exit(0);
}

static void h_show(void)
{
    window_show();
    if (shot_path)
        save_shot();
    pump(0);                    /* не копим события, пока рисуем */
}

static vm_i64 h_touch(void)
{
    if (!dpy)
        return -1;              /* окна нет — касаться нечего */
    return pump(1);
}

static void h_sleep(vm_i64 ms)
{
    struct timespec ts;

    if (ms <= 0)
        return;

    /* Спим короткими кусками, разбирая события: иначе окно на время сна
     * перестаёт отвечать, и всякая длинная пауза выглядит зависанием. */
    while (ms > 0) {
        vm_i64 step = ms > 10 ? 10 : ms;

        ts.tv_sec = 0;
        ts.tv_nsec = step * 1000 * 1000;
        nanosleep(&ts, NULL);
        if (dpy)
            pump(0);
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

/* Шрифт ищем там, где его оставляет сборка ядра */
static const char *font_paths[] = {
    "hittis/manrope.stf",       /* делается вместе с просмотром */
    "manrope.stf",
    "kernel/build/merlin/font/manrope.stf",
    "kernel/build/qemu/font/manrope.stf",
    "../kernel/build/merlin/font/manrope.stf",
    "../kernel/build/qemu/font/manrope.stf",
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
                "как пользоваться: preview приложение.ht\n"
                "  мышь — палец, q — выход, r — перезапуск,\n"
                "  правка файла перезапускает сама\n");
        return 1;
    }

    self_path = argv[0];
    path = argv[1];
    if (argc >= 4 && strcmp(argv[2], "--кадр") == 0)
        shot_path = argv[3];

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
             * Не выходим: приложение можно дописывать прямо во время
             * просмотра, и половина правок компилятору не нравится.
             * Ждём следующей правки, а не заставляем запускать заново.
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
    printf("=== %s ===\n", name ? name : "не наше приложение");
    printf("мышь — палец, q — выход, r — перезапуск; правка %s перезапускает сама\n\n",
           src_path ? src_path : "файла");
    fflush(stdout);

    rc = vm_run(image, len, &host);

    printf(rc == 0 ? "\n=== приложение завершилось ===\n"
                   : "\n=== приложение остановлено машиной ===\n");

    /* Окно не закрываем сразу: иначе последний кадр не разглядеть.
     * Ждём закрытия или правки исходника. */
    if (dpy) {
        printf("окно осталось открытым: q — закрыть\n");
        fflush(stdout);
        for (;;)
            pump(1);
    }

    return rc == 0 ? 0 : 1;
}
