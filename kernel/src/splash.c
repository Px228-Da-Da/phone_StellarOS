/*
 * Заставка загрузки StellarOS.
 *
 * Название посреди тёмного экрана, проявляющееся из темноты, и тонкая
 * полоса под ним. Полоса показывает НАСТОЯЩИЕ шаги загрузки: экран,
 * консоль, память, прерывания, ядра, касания, задачи, оболочка. Рисовать
 * движение, не связанное с делом, — обманывать того, кто на него смотрит;
 * а по честной полосе сразу видно, где система встала, если она встанет.
 *
 * Два режима рисования, и это не усложнение ради красоты:
 *
 *   до планировщика — прямо в показываемый буфер. Кадры редкие, по шагу
 *   загрузки, шва на статичной картинке не будет;
 *
 *   после — задача с двойной буферизацией, шестьдесят кадров в секунду.
 *   Рисовать плавное проявление прямо в показываемый буфер нельзя:
 *   контроллер дисплея читает его сам и покажет половину кадра.
 *
 * Перерисовываем только две полосы — с логотипом и с ползунком. Кадр
 * 1080x2340 в некэшируемой памяти стоит около пятнадцати миллисекунд, и
 * заливать его целиком шестьдесят раз в секунду значило бы съесть на
 * заставку больше, чем на всю остальную загрузку.
 */
#include "splash.h"
#include "fb.h"
#include "font.h"
#include "sched.h"
#include "timer.h"
#include "print.h"

/* Шаги загрузки. Порядок — порядок появления в kmain */
#define SPLASH_STAGES   8

#define BG_TOP      0xFF080B16      /* верх фона                    */
#define BG_BOTTOM   0xFF161C31      /* низ фона                     */
#define LOGO_FG     0xFFE8EDFB      /* цвет названия                */
#define BAR_BACK    0xFF1E2740      /* дорожка ползунка             */
#define BAR_FILL    0xFF4C8DFF      /* сам ползунок                 */

#define LOGO_PX     150             /* кегль названия               */
#define BAR_W_PCT   34              /* ширина дорожки, % от экрана  */
#define BAR_H       6
#define BAR_GAP     120             /* от низа названия до дорожки  */

static const char logo[] = "StellarOS";

static u32 sw, sh;                  /* экран                        */
static int ready;                   /* заставка живёт               */
static int running;                 /* задача плавности работает    */
static int done;                    /* пора уходить                 */
static u32 stage;                   /* сколько шагов позади         */
static u32 shown_fill;              /* ползунок сейчас, в пикселях  */
static u32 fade;                    /* яркость названия, 0..255     */

/* Где что лежит на экране — считается один раз, от размеров панели */
static u32 logo_x, logo_y, logo_w, logo_h, logo_scale;
static u32 bar_x, bar_y, bar_w;

/*
 * Цвет фона в строке y: сверху темнее, снизу светлее.
 *
 * Градиент, а не заливка: на большом чёрном поле любая неравномерность
 * панели видна как пятно, а плавный переход её прячет. Считаем на целых
 * числах — дробных в ядре нет.
 */
static u32 bg_at(u32 y)
{
    u32 t = sh ? (y * 255) / sh : 0;
    u32 r = (((BG_TOP >> 16) & 0xFF) * (255 - t) +
             ((BG_BOTTOM >> 16) & 0xFF) * t) / 255;
    u32 g = (((BG_TOP >> 8) & 0xFF) * (255 - t) +
             ((BG_BOTTOM >> 8) & 0xFF) * t) / 255;
    u32 b = ((BG_TOP & 0xFF) * (255 - t) + (BG_BOTTOM & 0xFF) * t) / 255;

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void paint_band(u32 y0, u32 h)
{
    for (u32 y = y0; y < y0 + h && y < sh; y++)
        fb_fill_rect(0, y, sw, 1, bg_at(y));
}

static void paint_all(void)
{
    paint_band(0, sh);
}

/*
 * Приглушить цвет к фону.
 *
 * Проявление делаем смешиванием с фоном, а не прозрачностью: фон под
 * названием известен и однороден по строке, и результат тот же, а
 * лишнего слоя и лишнего прохода по памяти нет.
 */
static u32 dim(u32 color, u32 bg, u32 k)
{
    u32 r = (((color >> 16) & 0xFF) * k + ((bg >> 16) & 0xFF) * (255 - k)) / 255;
    u32 g = (((color >> 8) & 0xFF) * k + ((bg >> 8) & 0xFF) * (255 - k)) / 255;
    u32 b = ((color & 0xFF) * k + (bg & 0xFF) * (255 - k)) / 255;

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void draw_logo(void)
{
    u32 mid = logo_y + logo_h / 2;

    paint_band(logo_y, logo_h);
    fb_text(logo_x, logo_y, logo_scale, dim(LOGO_FG, bg_at(mid), fade),
            0, logo);
}

static void draw_bar(void)
{
    u32 want = (stage * bar_w) / SPLASH_STAGES;

    /*
     * Ползунок подтягивается к цели, а не прыгает. Шаг — восьмая часть
     * оставшегося: у самой цели движение замедляется само, без всякой
     * таблицы плавности.
     */
    if (shown_fill < want)
        shown_fill += (want - shown_fill + 7) / 8;
    else
        shown_fill = want;

    paint_band(bar_y, BAR_H);
    fb_fill_rect(bar_x, bar_y, bar_w, BAR_H, BAR_BACK);
    if (shown_fill)
        fb_fill_rect(bar_x, bar_y, shown_fill, BAR_H, BAR_FILL);
}

void splash_show(void)
{
    u32 stride;
    u64 base;
    const struct font_face *f;

    if (!fb_available())
        return;

    fb_info(&base, &sw, &sh, &stride);
    if (!sw || !sh)
        return;

    /*
     * Кегль подбираем под экран: название должно занимать около двух
     * третей ширины и никогда не упираться в края. Экран у телефона один,
     * но эмулятор уже, а заставка обязана выглядеть одинаково нарочно
     * везде — иначе проверять её в эмуляторе бессмысленно.
     */
    logo_scale = LOGO_PX / 8;
    while (logo_scale > 4) {
        logo_w = fb_text_width(logo_scale, logo);
        if (logo_w <= sw * 78 / 100)
            break;
        logo_scale = (logo_scale * 2) / 3;
    }

    f = font_pick(logo_scale * 8);
    logo_w = fb_text_width(logo_scale, logo);
    logo_h = f ? f->line : logo_scale * 8;
    logo_x = (sw > logo_w) ? (sw - logo_w) / 2 : 0;
    logo_y = (sh > logo_h) ? (sh - logo_h) / 2 : 0;

    bar_w = sw * BAR_W_PCT / 100;
    bar_x = (sw - bar_w) / 2;
    bar_y = logo_y + logo_h + BAR_GAP;

    ready = 1;
    fade = 90;                  /* сразу видно, но ещё не в полную силу */

    paint_all();
    draw_logo();
    draw_bar();
}

void splash_stage(u32 n)
{
    if (n > SPLASH_STAGES)
        n = SPLASH_STAGES;
    if (n > stage)
        stage = n;

    /* Пока задачи нет, кадр обновляем сами — иначе полоса стояла бы всю
     * загрузку и ожила только к её концу, когда уже незачем. Название
     * при этом светлеет с каждым шагом: до планировщика кадры редкие, и
     * плавность взять неоткуда, а движение видно и по ступенькам. */
    if (ready && !running && !done) {
        u32 want = 90 + stage * 20;

        fade = want > 255 ? 255 : want;
        draw_logo();
        draw_bar();
    }
}

void splash_finish(void)
{
    done = 1;
}

/*
 * Задача плавности.
 *
 * Живёт ровно столько, сколько идёт загрузка, и ни секундой дольше:
 * оболочка показывается своим слоем поверх кадра, и заставка под ним
 * никому не нужна — а кадр за собой мы оставляем чистым, потому что в
 * него ещё будут писать окна, показанные копией.
 */
void splash_task(void *arg)
{
    int buffered;

    (void)arg;

    if (!ready) {
        return;
    }

    buffered = (fb_double_buffer(1) == 0);
    running = 1;

    /*
     * Оба буфера должны нести одинаковый фон: дальше мы перерисовываем
     * только две полосы, а всё остальное берётся из того, что уже лежит
     * в буфере, куда вернулись.
     */
    if (buffered) {
        paint_all();
        fb_flip();
        paint_all();
        fb_flip();
    }

    while (!done) {
        if (fade < 255)
            fade += 12;
        if (fade > 255)
            fade = 255;

        draw_logo();
        draw_bar();
        if (buffered)
            fb_flip();

        task_sleep_ms(16);
    }

    /* Дотягиваем полосу до конца — иначе она замрёт на середине именно
     * тогда, когда всё получилось. */
    stage = SPLASH_STAGES;
    for (int i = 0; i < 10; i++) {
        draw_bar();
        if (buffered)
            fb_flip();
        task_sleep_ms(16);
    }

    running = 0;
    if (buffered)
        fb_double_buffer(0);

    /* Кадр остаётся тёмным: поверх него ляжет оболочка своим слоем, а
     * то, что показывается копией, рисует в него само. */
    paint_all();
    kprintf("ЗАСТАВКА : УБРАНА, ЗАГРУЗКА ПОКАЗАНА ЦЕЛИКОМ\n");
}
