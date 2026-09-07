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

/*
 * Размер, который прогон выдаёт за экран.
 *
 * Приложение, не просившее окна определённого размера, получает в
 * заголовке 65535 — это «во весь экран», и превратить его в число может
 * только тот, у кого экран есть. У прогона его нет, поэтому он называет
 * рабочую область телефона: 1080 на 2196.
 *
 * Число здесь не для красоты. Пока прогон отвечал теми же 65535,
 * раскладка любого приложения уезжала в бессмыслицу, придуманные
 * касания не попадали ни в одну кнопку — и проверка молча проходила,
 * ничего не проверив.
 */
#define FAKE_W  1080
#define FAKE_H  2196

static int h_window(int w, int h)
{
    win_w = (w > 0 && w < 0xFFFF) ? w : FAKE_W;
    win_h = (h > 0 && h < 0xFFFF) ? h : FAKE_H;
    printf("[окно %dx%d]\n", win_w, win_h);
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
    /*
     * Точки подобраны по сетке 4x4 во весь экран — такой, какую строят
     * приложения на qt2. Бьём и по левому столбцу, и по правому: в
     * калькуляторе слева цифры, справа действия, и прогон, тыкавший
     * только слева, проверял набор числа, но никогда — счёт.
     *
     * Для калькулятора выходит «4 + 1 =», и в выводе видно ответ.
     */
    static const struct { int x, y; } spots[] = {
        { 140, 900 },       /* левый столбец, второй ряд  */
        { 900, 1750 },      /* правый столбец, низ        */
        { 140, 1300 },      /* левый столбец, третий ряд  */
        { 650, 1750 },      /* третий столбец, низ        */
        { 1000, 2100 },     /* мимо всего                 */
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

/*
 * Один знак. Экрана у прогона нет, поэтому просто говорим, что нарисован
 * бы, и отвечаем правдоподобной шириной — приложение по ней выстраивает
 * буквы, и ноль сбил бы его с толку.
 */
static int h_textc(int x, int y, int scale, vm_u32 color, vm_i64 code)
{
    printf("[знак %d,%d размер %d цвет %x: %lld]\n",
           x, y, scale, color, code);
    return scale * 4;
}

/*
 * Ширина строки без рисования.
 *
 * Шрифта здесь нет вовсе — hostrun это проверка поведения, а не вида, —
 * поэтому отвечаем той же правдоподобной шириной, что и textc: размер на
 * четыре за знак. Ноль сбил бы разметку приложения с толку и превратил
 * бы проверку в ложную тревогу.
 *
 * Настоящую ширину показывает предпросмотр: там шрифт тот же, что на
 * телефоне, и меряет он по начертаниям.
 */
static int h_textw(int scale, const char *s)
{
    int n = 0;

    if (!s || scale <= 0)
        return 0;
    while (*s) {
        if ((*s++ & 0xC0) != 0x80)      /* считаем знаки, а не байты */
            n++;
    }
    return n * scale * 4;
}

static const struct vm_host host = {
    .print = h_print,   .printn = h_printn, .window = h_window,
    .rect  = h_rect,    .text   = h_text,   .textn  = h_textn,
    .textc = h_textc,   .textw  = h_textw,  .show   = h_show,
    .touch = h_touch,   .sleep_ms = h_sleep, .time_ms = h_time,
    .width = h_width,   .height = h_height,
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
