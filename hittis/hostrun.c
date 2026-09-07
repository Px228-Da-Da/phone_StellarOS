/*
 * Запуск приложения .slt на компьютере.
 *
 * Нужен ровно для одного: проверять язык и приложения, не трогая
 * телефон. Прошивка стоит минуты и пары кнопок, а ошибка в компиляторе
 * ловится за секунду — и ловить её лучше здесь.
 *
 * Рисование здесь не рисует: прямоугольники и текст печатаются словами.
 * Это не заглушка ради галочки — по такому выводу видно, что именно
 * приложение собиралось показать, и в каком порядке.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm.h"

static int win_w, win_h;

/* print переводит строку сам — как в питоне, откуда язык и взял вид */
static void h_print(const char *s)      { fputs(s, stdout); fputc('\n', stdout); }
static void h_printn(vm_i64 v)          { printf("%lld\n", v); }

static int h_window(int w, int h)
{
    win_w = w;
    win_h = h;
    printf("[окно %dx%d]\n", w, h);
    return 1;
}

static void h_rect(int x, int y, int w, int h, vm_u32 c)
{
    printf("[прямоугольник %d,%d %dx%d цвет %08x]\n", x, y, w, h, c);
}

static void h_text(int x, int y, int scale, vm_u32 c, const char *s)
{
    printf("[текст %d,%d размер %d цвет %08x: %s]\n", x, y, scale, c, s);
}

static void h_textn(int x, int y, int scale, vm_u32 c, vm_i64 v)
{
    printf("[число %d,%d размер %d цвет %08x: %lld]\n", x, y, scale, c, v);
}

static void h_show(void)                { printf("[показать]\n"); }
/*
 * Касаний на компьютере нет, поэтому выдаём несколько придуманных и
 * заканчиваем. Не заглушка «всегда ноль»: приложение обязано уметь
 * дожить до конца касаний, и проверять это лучше здесь, чем на телефоне.
 */
static vm_i64 h_touch(void)
{
    /*
     * Касание выдаём парой: сначала «нажали», следом «отпустили» в той
     * же точке.
     *
     * Раньше приходили одни нажатия, и это было не мелочью, а дырой в
     * проверке: приличный интерфейс срабатывает на отпускании — иначе
     * нельзя отменить случайное нажатие, уведя палец в сторону. Значит
     * приложение, у которого сломана вся обработка отпускания, проходило
     * проверку молча.
     */
    static int n;
    static const struct { int x, y; } spots[] = {
        { 120, 300 },       /* первая кнопка   */
        { 120, 300 },
        { 360, 300 },       /* вторая          */
        { 120, 480 },       /* третья          */
        { 900, 1800 },      /* мимо всего      */
    };
    int step = n / 2;
    int down = (n % 2) == 0;
    vm_i64 x, y;

    if (step >= (int)(sizeof(spots) / sizeof(spots[0]))) {
        printf("[придуманные касания кончились]\n");
        return -1;
    }

    x = spots[step].x;
    y = spots[step].y;
    n++;

    printf("[%s %lld,%lld]\n", down ? "нажали" : "отпустили", x, y);
    return ((vm_i64)(down ? 0 : 2) << 48) | (x << 32) | y;
}

static void h_sleep(vm_i64 ms)          { printf("[сон %lld мс]\n", ms); }

static vm_i64 h_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (vm_i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int h_width(void)                { return win_w; }
static int h_height(void)               { return win_h; }

static const struct vm_host host = {
    h_print, h_printn, h_window, h_rect, h_text, h_textn,
    h_show, h_touch, h_sleep, h_time, h_width, h_height
};

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    void *buf;
    int rc;

    if (argc != 2) {
        fprintf(stderr, "как пользоваться: hostrun приложение.slt\n");
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "не открыть %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size);
    if (fread(buf, 1, size, f) != (size_t)size) {
        fprintf(stderr, "не прочитать %s\n", argv[1]);
        return 1;
    }
    fclose(f);

    {
        const char *name = vm_app_name(buf, (vm_u32)size);

        printf("=== %s ===\n", name ? name : "не приложение");
    }

    rc = vm_run(buf, (vm_u32)size, &host);
    printf("=== %s ===\n", rc == 0 ? "приложение завершилось" :
                                     "приложение остановлено машиной");
    return rc == 0 ? 0 : 1;
}
