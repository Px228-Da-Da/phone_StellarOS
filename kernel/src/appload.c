/*
 * Приём приложений по проводу.
 *
 * Замысел и устройство посылки описаны в appload.h. Здесь только разбор
 * и хранение.
 *
 * Разбор устроен как конечный автомат по одному байту, а не «накопить
 * всё и разобрать»: сколько приедет, заранее неизвестно, а держать
 * второй буфер такого же размера ради удобства разбора незачем.
 */
#include "appload.h"
#include "usb.h"
#include "sched.h"
#include "print.h"
#include "syscall.h"
#include "uspace.h"

/*
 * Сколько места отвести под приложение.
 *
 * Столько же, сколько отведено на той стороне, в самой программе: там
 * предел жёсткий — системе отдано под изменяемые данные программы 64 КБ
 * на всё про всё. Принять больше, чем можно отдать, значит получить
 * посылку и не суметь ею воспользоваться; отказ на приёме честнее.
 *
 * Память в bss и лежит там всегда: выделять её по приходу посылки
 * значило бы уметь не выделить в самый неподходящий момент.
 */
#define APP_MAX     (16 * 1024)

static u8  app_buf[APP_MAX];
static u32 app_len;                 /* сколько принято и проверено */

static u32 want_len;                /* сколько обещано в заголовке */
static u32 want_sum;                /* и с какой суммой            */
static u32 have_len;
static u32 have_sum;
static u32 nibble;                  /* половина байта в ожидании второй */
static int nibble_full;

enum { WAIT, HEAD, BODY, TAIL };
static int state;

static char line[64];
static u32  line_n;

static u32 fresh;                   /* приехало новое — пора показать */

const u8 *appload_image(u32 *len)
{
    if (!app_len)
        return 0;
    if (len)
        *len = app_len;
    return app_buf;
}

static int hex_of(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Своё разложение строки на число: библиотеки нет, а читать надо */
static u32 num_of(const char **p)
{
    u32 v = 0;

    while (**p == ' ')
        (*p)++;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (u32)(**p - '0');
        (*p)++;
    }
    return v;
}

static void begin(const char *rest)
{
    want_len = num_of(&rest);
    want_sum = num_of(&rest);

    if (!want_len || want_len > APP_MAX) {
        kprintf("ПРИЁМ    : ОТКАЗ, ДЛИНА %u НЕ ГОДИТСЯ\n", want_len);
        state = WAIT;
        return;
    }

    have_len = 0;
    have_sum = 0;
    nibble_full = 0;
    state = BODY;
    kprintf("ПРИЁМ    : ЖДУ ПРИЛОЖЕНИЕ, %u БАЙТ\n", want_len);
}

static void finish(void)
{
    if (have_len != want_len) {
        kprintf("ПРИЁМ    : ОБРЫВ, ПРИНЯТО %u ИЗ %u\n", have_len, want_len);
        state = WAIT;
        return;
    }
    if (have_sum != want_sum) {
        /*
         * Сумма не сошлась — значит по дороге что-то потерялось. Взять
         * такое приложение нельзя: машина исполнит испорченный код и
         * упадёт где угодно, а виноват окажется язык.
         */
        kprintf("ПРИЁМ    : СУММА НЕ СОШЛАСЬ, %u ВМЕСТО %u\n",
                have_sum, want_sum);
        state = WAIT;
        return;
    }

    app_len = have_len;
    fresh = 1;
    state = WAIT;
    kprintf("ПРИЁМ    : ПРИНЯТО %u БАЙТ, СУММА СОШЛАСЬ\n", app_len);
}

static void feed(u8 c)
{
    switch (state) {
    case WAIT:
    case TAIL:
        /* Ждём строку разметки. Всё остальное на проводе — не наше. */
        if (c == '\n' || line_n + 1 >= sizeof(line)) {
            line[line_n] = 0;
            if (line[0] == '!') {
                if (line[1] == 'S' && line[2] == 'L' && line[3] == 'T')
                    begin(line + 4);
            }
            line_n = 0;
        } else if (c != '\r') {
            line[line_n++] = (char)c;
        }
        break;

    case BODY: {
        int v;

        if (c == '!') {                 /* началась строка «!END» */
            state = TAIL;
            line_n = 0;
            line[line_n++] = '!';
            finish();
            break;
        }

        v = hex_of((char)c);
        if (v < 0)
            break;                      /* переводы строк и пробелы мимо */

        if (!nibble_full) {
            nibble = (u32)v;
            nibble_full = 1;
        } else {
            u8 b = (u8)((nibble << 4) | (u32)v);

            nibble_full = 0;
            if (have_len < APP_MAX) {
                app_buf[have_len++] = b;
                have_sum += b;
            }
        }
        break;
    }
    }
}

/*
 * Задача приёма.
 *
 * Разбирать байты в обработчике прерывания нельзя, а запускать оттуда
 * задачи — тем более. Поэтому здесь: провод читает прерывание и кладёт
 * в кольцо, а всё остальное происходит в обычной задаче, где можно и
 * печатать, и создавать программы.
 */
static void appload_task(void *arg)
{
    u32 told_lost = 0;

    (void)arg;

    for (;;) {
        u8 c;
        int worked = 0;

        while (usb_recv(&c)) {
            feed(c);
            worked = 1;
        }

        if (fresh) {
            fresh = 0;
            /*
             * Показываем сразу, не дожидаясь просьбы.
             *
             * Смысл всей затеи в том, чтобы между «сохранил» и «вижу на
             * телефоне» не было ни одного действия руками. Старое
             * приложение при этом снимать не нужно: оно закроет своё
             * окно само, когда его вытеснит новое, а два окна одной
             * программы система и так не даёт.
             */
            kprintf("ПРИЁМ    : ЗАПУСКАЮ ПРИНЯТОЕ\n");
            uspace_spawn_image(IMG_HITTIS);
        }

        if (usb_rx_lost() != told_lost) {
            told_lost = usb_rx_lost();
            kprintf("ПРИЁМ    : ПОТЕРЯНО БАЙТ %u\n", told_lost);
        }

        /* Пока идёт посылка — вычитываем без пауз, иначе кольцо
         * переполнится; в тишине спим и не занимаем ядро. */
        task_sleep_ms(worked ? 1 : 50);
    }
}

void appload_start(void)
{
    task_create("приём", appload_task, NULL);
}
