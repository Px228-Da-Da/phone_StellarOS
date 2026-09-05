/*
 * user.S — программы для пользовательского режима.
 *
 * Это НЕ часть ядра. Байты между метками start и end копируются в
 * отдельные страницы и исполняются в EL0, где ничего ядерного не видно:
 * ни функций, ни данных, ни даже этого файла. Отсюда два ограничения,
 * которые здесь важнее стиля:
 *
 *   1. Программа обязана быть позиционно-независимой: копия ляжет по
 *      совсем другому адресу, поэтому к своим данным обращаемся только
 *      через adr — он считает адрес относительно текущей команды.
 *   2. Всё, что программе нужно от мира, она получает через svc. Ни
 *      вызвать kprintf, ни прочитать переменную ядра нельзя — и это не
 *      соглашение, а то, что теперь обеспечивает процессор.
 *
 * Данные лежат внутри тех же границ, что и код: копируется один кусок.
 */
#include "syscall.h"

.section ".text.user", "ax"

/* ---------------------------------------------------------------------
 * Первая программа: здоровается и уходит спать. Пять раз, потом
 * завершается сама. Смысл её существования — показать, что из EL0
 * работают три вещи: вывод через ядро, сон через планировщик и
 * завершение по собственному желанию.
 */
.align 4
.global user_hello_start
user_hello_start:
    mov     x19, #5                     /* сколько раз здороваться */

hello_loop:
    mov     x8, #SYS_WRITE
    adr     x0, hello_msg
    mov     x1, #(hello_msg_end - hello_msg)
    svc     #0

    mov     x8, #SYS_SLEEP_MS
    mov     x0, #700
    svc     #0

    subs    x19, x19, #1
    b.ne    hello_loop

    mov     x8, #SYS_EXIT
    mov     x0, #0
    svc     #0
    b       .                           /* до сюда управление не доходит */

hello_msg:
    .ascii  "EL0      : ПРИВЕТ ИЗ ПОЛЬЗОВАТЕЛЬСКОГО РЕЖИМА\n"
hello_msg_end:

.align 4
.global user_hello_end
user_hello_end:

/* ---------------------------------------------------------------------
 * Вторая программа: нарушитель. Пытается прочитать память ядра по
 * адресу, с которого начинается его образ, — 0x40080000 и на телефоне,
 * и в эмуляторе.
 *
 * Эта страница отображена как принадлежащая EL1, и доступ из EL0 обязан
 * закончиться отказом. Программа при этом снимается, а система
 * продолжает работать — ровно ради этого пользовательский режим и
 * существует. Если же сообщение ниже когда-нибудь напечатается, значит
 * защиты нет и разбираться надо немедленно.
 */
.align 4
.global user_rogue_start
user_rogue_start:
    mov     x8, #SYS_WRITE
    adr     x0, rogue_msg
    mov     x1, #(rogue_msg_end - rogue_msg)
    svc     #0

    /*
     * Сначала более хитрая попытка: не читать ядро самим, а попросить
     * ядро вывести его же память. Проверка аргументов системного вызова —
     * самое ответственное место в ядре: адрес пришёл из EL0, и поверить
     * ему значит отдать наружу всё, что у нас есть. Отказ приходит
     * отрицательным числом.
     */
    mov     x8, #SYS_WRITE
    movz    x0, #0x4008, lsl #16        /* начало образа ядра */
    mov     x1, #32
    svc     #0
    tbz     x0, #63, rogue_leak         /* ответ не отрицательный — беда */

    /* Теперь напрямую. Дальше этой команды программа не живёт. */
    movz    x0, #0x4008, lsl #16
    ldr     x1, [x0]

    mov     x8, #SYS_WRITE
    adr     x0, rogue_bad
    mov     x1, #(rogue_bad_end - rogue_bad)
    svc     #0

    mov     x8, #SYS_EXIT
    mov     x0, #1
    svc     #0
    b       .

rogue_leak:
    mov     x8, #SYS_WRITE
    adr     x0, rogue_leak_msg
    mov     x1, #(rogue_leak_msg_end - rogue_leak_msg)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #4
    svc     #0
    b       .

rogue_msg:
    .ascii  "EL0      : ПРОБУЮ ПРОЧИТАТЬ ПАМЯТЬ ЯДРА\n"
rogue_msg_end:

rogue_leak_msg:
    .ascii  "EL0      : ЯДРО СОГЛАСИЛОСЬ ВЫВЕСТИ СВОЮ ПАМЯТЬ!\n"
rogue_leak_msg_end:

rogue_bad:
    .ascii  "EL0      : ЯДРО ПРОЧИТАЛОСЬ! ЗАЩИТЫ НЕТ!\n"
rogue_bad_end:

.align 4
.global user_rogue_end
user_rogue_end:

/* ---------------------------------------------------------------------
 * Третья программа: счёт без обращений к ядру.
 *
 * Нужна, чтобы проверить самый скользкий путь — вытеснение программы
 * прямо посреди её работы. Первые две почти всё время проводят внутри
 * системных вызовов, и прерывание застаёт их в EL1, а не в EL0.
 *
 * Заодно она сама себя проверяет. Перед долгим счётом кладёт на свой
 * стек приметное число и держит его копию в регистре, а после счёта
 * снимает и сверяет. Если SP_EL0 или регистры при переключении задач
 * достанутся ей от соседа, сверка не сойдётся — и мы узнаем об этом
 * из сообщения, а не по загадочному поведению через месяц.
 */
.align 4
.global user_spin_start
user_spin_start:
    mov     x8, #SYS_GETPID
    svc     #0
    mov     x23, x0                     /* свой номер, пригодится позже */

    mov     x8, #SYS_UPTIME_MS
    svc     #0
    cbz     x0, spin_no_time            /* ядро обязано знать своё время */

    mov     x19, #6                     /* столько кругов счёта */

spin_loop:
    movz    x20, #0xBEEF
    movk    x20, #0xDEAD, lsl #16
    str     x20, [sp, #-16]!            /* -16, чтобы стек остался ровным */

    /* Витков берём столько, чтобы круг заведомо не уложился в квант
     * планировщика: смысл этой программы в том, чтобы её вытесняли. */
    movz    x21, #0x100, lsl #16        /* около семнадцати миллионов */
spin_busy:
    subs    x21, x21, #1
    b.ne    spin_busy

    ldr     x22, [sp], #16
    cmp     x22, x20
    b.ne    spin_broken

    mov     x8, #SYS_WRITE
    adr     x0, spin_msg
    mov     x1, #(spin_msg_end - spin_msg)
    svc     #0

    subs    x19, x19, #1
    b.ne    spin_loop

    mov     x8, #SYS_EXIT
    mov     x0, #0
    svc     #0
    b       .

spin_broken:
    mov     x8, #SYS_WRITE
    adr     x0, spin_bad
    mov     x1, #(spin_bad_end - spin_bad)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #2
    svc     #0
    b       .

spin_no_time:
    mov     x8, #SYS_WRITE
    adr     x0, spin_notime
    mov     x1, #(spin_notime_end - spin_notime)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #3
    svc     #0
    b       .

spin_msg:
    .ascii  "EL0      : КРУГ СЧЁТА ПРОЙДЕН, СТЕК И РЕГИСТРЫ ЦЕЛЫ\n"
spin_msg_end:

spin_bad:
    .ascii  "EL0      : ПОСЛЕ ВЫТЕСНЕНИЯ ИСПОРЧЕН СТЕК ИЛИ РЕГИСТРЫ!\n"
spin_bad_end:

spin_notime:
    .ascii  "EL0      : ЯДРО НЕ ЗНАЕТ, СКОЛЬКО ОНО РАБОТАЕТ\n"
spin_notime_end:

.align 4
.global user_spin_end
user_spin_end:

/* ---------------------------------------------------------------------
 * Четвёртая программа: двойник.
 *
 * Запускается двумя одинаковыми копиями. Обе лежат по ОДНОМУ И ТОМУ ЖЕ
 * виртуальному адресу и пишут по одному и тому же адресу стека — и
 * именно поэтому проверяют раздельность пространств.
 *
 * Каждая кладёт на стек свой номер задачи, засыпает (за это время
 * соседка успевает сделать то же самое) и сверяет, что прочитала своё.
 * Если бы пространство было общим или пользовательские записи в таблицах
 * оказались глобальными, копии писали бы в одну физическую страницу, и
 * сверка не сошлась бы. Это дешёвая проверка того, что иначе проверяется
 * только чтением дескрипторов глазами.
 */
.align 4
.global user_twin_start
user_twin_start:
    mov     x8, #SYS_GETPID
    svc     #0
    mov     x19, x0                     /* свой номер задачи */
    mov     x21, #4                     /* столько раз сверяемся */

twin_loop:
    str     x19, [sp, #-16]!
    mov     x8, #SYS_SLEEP_MS
    mov     x0, #250
    svc     #0
    ldr     x20, [sp], #16
    cmp     x19, x20
    b.ne    twin_mixed

    mov     x8, #SYS_WRITE
    adr     x0, twin_ok
    mov     x1, #(twin_ok_end - twin_ok)
    svc     #0

    subs    x21, x21, #1
    b.ne    twin_loop

    mov     x8, #SYS_EXIT
    mov     x0, #0
    svc     #0
    b       .

twin_mixed:
    mov     x8, #SYS_WRITE
    adr     x0, twin_bad
    mov     x1, #(twin_bad_end - twin_bad)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #6
    svc     #0
    b       .

twin_ok:
    .ascii  "EL0      : НА МОЁМ СТЕКЕ МОЁ, СОСЕД НЕ ДОСТАЁТ\n"
twin_ok_end:

twin_bad:
    .ascii  "EL0      : НА МОЁМ СТЕКЕ ЧУЖОЕ! ПРОСТРАНСТВА ОБЩИЕ!\n"
twin_bad_end:

.align 4
.global user_twin_end
user_twin_end:

/* ---------------------------------------------------------------------
 * Пятая программа: разовая. Немного спит и завершается.
 *
 * Ничего не печатает: её запускают десятками подряд, чтобы посмотреть,
 * возвращается ли память после завершения. Сон нужен, чтобы несколько
 * копий жили одновременно — иначе проверялся бы не сборщик, а
 * последовательный запуск.
 */
.align 4
.global user_once_start
user_once_start:
    mov     x8, #SYS_SLEEP_MS
    mov     x0, #60
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #0
    svc     #0
    b       .

.align 4
.global user_once_end
user_once_end:

/* ---------------------------------------------------------------------
 * Шестая программа: запускала.
 *
 * Первая программа, которая распоряжается другими: запускает их и
 * дожидается конца. До неё всё запускалось ядром при старте, то есть
 * пользовательский режим был местом, куда система кого-то помещает.
 * Теперь это место, откуда можно управлять.
 *
 * Запускает троих. Первый честно отрабатывает и завершается сам —
 * ожидание должно вернуть его код. Второй нарушает границы, и его
 * снимает ядро — ожидание должно вернуть особый код, отличимый от
 * любого, который программа могла бы назначить себе сама. Разница между
 * этими двумя ответами и есть то, ради чего всё делалось. Третий считает
 * несколько секунд: ожидание должно всё это время ничего не стоить.
 */
.align 4
.global user_boss_start
user_boss_start:
    mov     x8, #SYS_WRITE
    adr     x0, boss_hello
    mov     x1, #(boss_hello_end - boss_hello)
    svc     #0

    /* --- первый: обычная программа --- */
    mov     x8, #SYS_SPAWN
    mov     x0, #IMG_ONCE
    svc     #0
    tbnz    x0, #63, boss_failed        /* отрицательный ответ — не вышло */
    mov     x19, x0

    mov     x8, #SYS_WAIT
    mov     x0, x19
    svc     #0
    cbnz    x0, boss_first_bad

    mov     x8, #SYS_WRITE
    adr     x0, boss_first_ok
    mov     x1, #(boss_first_ok_end - boss_first_ok)
    svc     #0
    b       boss_second

boss_first_bad:
    mov     x8, #SYS_WRITE
    adr     x0, boss_first_odd
    mov     x1, #(boss_first_odd_end - boss_first_odd)
    svc     #0

    /* --- второй: нарушитель, его должно снять ядро --- */
boss_second:
    mov     x8, #SYS_SPAWN
    mov     x0, #IMG_ROGUE
    svc     #0
    tbnz    x0, #63, boss_failed
    mov     x19, x0

    mov     x8, #SYS_WAIT
    mov     x0, x19
    svc     #0
    cmp     x0, #EXIT_KILLED
    b.ne    boss_second_odd

    mov     x8, #SYS_WRITE
    adr     x0, boss_second_ok
    mov     x1, #(boss_second_ok_end - boss_second_ok)
    svc     #0

    /*
     * Третий: долгий. Считает секунды без единого обращения к ядру.
     *
     * Он здесь ради ожидания, а не ради счёта: ждать его приходится
     * долго, и всё это время ждущая обязана не занимать процессор вовсе.
     * Раньше ожидание было переспросом раз в десять миллисекунд — сотни
     * пробуждений впустую; теперь одно, по событию.
     */
    mov     x8, #SYS_SPAWN
    mov     x0, #IMG_SPIN
    svc     #0
    tbnz    x0, #63, boss_failed
    mov     x19, x0

    mov     x8, #SYS_WAIT
    mov     x0, x19
    svc     #0
    cbnz    x0, boss_third_odd

    mov     x8, #SYS_WRITE
    adr     x0, boss_third_ok
    mov     x1, #(boss_third_ok_end - boss_third_ok)
    svc     #0

    mov     x8, #SYS_EXIT
    mov     x0, #0
    svc     #0
    b       .

boss_third_odd:
    mov     x8, #SYS_WRITE
    adr     x0, boss_third_bad
    mov     x1, #(boss_third_bad_end - boss_third_bad)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #9
    svc     #0
    b       .

boss_second_odd:
    mov     x8, #SYS_WRITE
    adr     x0, boss_second_bad
    mov     x1, #(boss_second_bad_end - boss_second_bad)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #7
    svc     #0
    b       .

boss_failed:
    mov     x8, #SYS_WRITE
    adr     x0, boss_nostart
    mov     x1, #(boss_nostart_end - boss_nostart)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #8
    svc     #0
    b       .

boss_hello:
    .ascii  "EL0      : ЗАПУСКАЛА: БЕРУСЬ ЗА ТРОИХ\n"
boss_hello_end:

boss_first_ok:
    .ascii  "EL0      : ЗАПУСКАЛА: ПЕРВЫЙ ОТРАБОТАЛ САМ, КОД 0\n"
boss_first_ok_end:

boss_first_odd:
    .ascii  "EL0      : ЗАПУСКАЛА: ПЕРВЫЙ ВЕРНУЛ ЧТО-ТО НЕ ТО\n"
boss_first_odd_end:

boss_second_ok:
    .ascii  "EL0      : ЗАПУСКАЛА: ВТОРОГО СНЯЛО ЯДРО, ЖДАЛА ИМЕННО ЭТОГО\n"
boss_second_ok_end:

boss_second_bad:
    .ascii  "EL0      : ЗАПУСКАЛА: ВТОРОЙ ВЕРНУЛСЯ САМ — ЗАЩИТА НЕ СРАБОТАЛА\n"
boss_second_bad_end:

boss_third_ok:
    .ascii  "EL0      : ЗАПУСКАЛА: ДОЖДАЛАСЬ ДОЛГОГО, КОД 0\n"
boss_third_ok_end:

boss_third_bad:
    .ascii  "EL0      : ЗАПУСКАЛА: ДОЛГИЙ ВЕРНУЛ ЧТО-ТО НЕ ТО\n"
boss_third_bad_end:

boss_nostart:
    .ascii  "EL0      : ЗАПУСКАЛА: ЯДРО НЕ ДАЛО ЗАПУСТИТЬ ПРОГРАММУ\n"
boss_nostart_end:

.align 4
.global user_boss_end
user_boss_end:

/* ---------------------------------------------------------------------
 * Седьмая программа: художник.
 *
 * Первая, которая что-то показывает на экране сама. Просит у ядра окно,
 * рисует в нём шахматку с рамкой — обычными записями в память, без
 * единого обращения к ядру, — и просит показать окно в разных местах.
 *
 * Ни фреймбуфера, ни регистров контроллера дисплея ей при этом не видно.
 * Всё, что у неё есть, — страницы буфера в её собственном пространстве.
 */
#define WIN_W   320
#define WIN_H   240

.align 4
.global user_paint_start
user_paint_start:
    mov     x8, #SYS_WINDOW
    mov     x0, #WIN_W
    mov     x1, #WIN_H
    svc     #0
    cbz     x0, paint_nowin
    mov     x19, x0                     /* адрес буфера окна */

    /* Три цвета: две клетки шахматки и белая рамка. Формат — тот же
     * 0xAARRGGBB, которым пользуется весь остальной вывод. */
    movz    w25, #0x50C0
    movk    w25, #0xFF20, lsl #16
    movz    w26, #0xD0FF
    movk    w26, #0xFF80, lsl #16
    movz    w27, #0xFFFF
    movk    w27, #0xFFFF, lsl #16

    mov     x20, #0                     /* y */
paint_row:
    mov     x21, #0                     /* x */
paint_col:
    mov     x22, #WIN_W
    mul     x22, x20, x22
    add     x22, x22, x21
    lsl     x22, x22, #2
    add     x22, x19, x22               /* адрес пикселя */

    cmp     x21, #3
    b.le    paint_white
    cmp     x20, #3
    b.le    paint_white
    mov     x23, #(WIN_W - 4)
    cmp     x21, x23
    b.ge    paint_white
    mov     x23, #(WIN_H - 4)
    cmp     x20, x23
    b.ge    paint_white

    lsr     x23, x21, #5                /* клетка 32 на 32 */
    lsr     x24, x20, #5
    add     x23, x23, x24
    tbz     x23, #0, paint_light
    str     w26, [x22]
    b       paint_next
paint_light:
    str     w25, [x22]
    b       paint_next
paint_white:
    str     w27, [x22]

paint_next:
    add     x21, x21, #1
    cmp     x21, #WIN_W
    b.lt    paint_col
    add     x20, x20, #1
    cmp     x20, #WIN_H
    b.lt    paint_row

    /* Нарисовано. Теперь показать — пять раз в разных местах экрана. */
    mov     x21, #5
    mov     x22, #60                    /* x */
    mov     x23, #300                   /* y */

paint_show:
    mov     x8, #SYS_PRESENT
    mov     x0, x22
    mov     x1, x23
    svc     #0
    tbnz    x0, #63, paint_noshow

    mov     x8, #SYS_SLEEP_MS
    mov     x0, #500
    svc     #0

    add     x22, x22, #100
    add     x23, x23, #250
    subs    x21, x21, #1
    b.ne    paint_show

    /*
     * Окно больше не нужно — закрываем его, не завершаясь, и тут же
     * просим другое. Так проверяется, что слот и слой действительно
     * вернулись: раньше отдать их можно было только вместе с самой
     * программой.
     */
    mov     x8, #SYS_CLOSE
    svc     #0
    tbnz    x0, #63, paint_noclose

    mov     x8, #SYS_SLEEP_MS
    mov     x0, #400
    svc     #0

    mov     x8, #SYS_WINDOW
    mov     x0, #160
    mov     x1, #160
    svc     #0
    cbz     x0, paint_nowin
    mov     x19, x0

    /* Второе окно — сплошной заливкой, чтобы его нельзя было спутать
     * с первым. */
    movz    w25, #0x40A0
    movk    w25, #0xFF20, lsl #16
    mov     x20, #(160 * 160)
    mov     x21, x19
paint_fill2:
    str     w25, [x21], #4
    subs    x20, x20, #1
    b.ne    paint_fill2

    mov     x8, #SYS_PRESENT
    mov     x0, #400
    mov     x1, #300
    svc     #0

    mov     x8, #SYS_SLEEP_MS
    mov     x0, #1200
    svc     #0

    mov     x8, #SYS_WRITE
    adr     x0, paint_done
    mov     x1, #(paint_done_end - paint_done)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #0
    svc     #0
    b       .

paint_noclose:
    mov     x8, #SYS_WRITE
    adr     x0, paint_no_close
    mov     x1, #(paint_no_close_end - paint_no_close)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #16
    svc     #0
    b       .

paint_nowin:
    mov     x8, #SYS_WRITE
    adr     x0, paint_no_window
    mov     x1, #(paint_no_window_end - paint_no_window)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #10
    svc     #0
    b       .

paint_noshow:
    mov     x8, #SYS_WRITE
    adr     x0, paint_no_show
    mov     x1, #(paint_no_show_end - paint_no_show)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #11
    svc     #0
    b       .

paint_done:
    .ascii  "EL0      : ХУДОЖНИК: ПОКАЗАЛ, ЗАКРЫЛ ОКНО И ОТКРЫЛ ДРУГОЕ\n"
paint_done_end:

paint_no_window:
    .ascii  "EL0      : ХУДОЖНИК: ОКНА НЕ ДАЛИ\n"
paint_no_window_end:

paint_no_show:
    .ascii  "EL0      : ХУДОЖНИК: ПОКАЗАТЬ ОКНО НЕ ВЫШЛО\n"
paint_no_show_end:

paint_no_close:
    .ascii  "EL0      : ХУДОЖНИК: ОКНО НЕ ЗАКРЫЛОСЬ\n"
paint_no_close_end:

.align 4
.global user_paint_end
user_paint_end:

/* ---------------------------------------------------------------------
 * Восьмая программа: следопыт.
 *
 * Окно, которое ходит за пальцем. Первая программа, которая получает
 * ввод: до неё касания разбирала только оболочка внутри ядра.
 *
 * Устройство простое до предела — ждать событие, переставить окно,
 * повторить. Всё остальное делают ядро и контроллер дисплея: программа
 * не рисует ни пикселя после первой заливки и не опрашивает ничего.
 * Ожидание касания ей ничего не стоит: пока пальца нет, задача не
 * получает процессор вовсе.
 */
#define TRK_SIZE    96
#define TRK_HALF    48

.align 4
.global user_track_start
user_track_start:
    mov     x8, #SYS_WINDOW
    mov     x0, #TRK_SIZE
    mov     x1, #TRK_SIZE
    svc     #0
    cbz     x0, track_nowin
    mov     x19, x0

    /* Заливка одним цветом: следопыта видно и без узора */
    movz    w25, #0x30A0
    movk    w25, #0xFFF0, lsl #16
    mov     x20, #(TRK_SIZE * TRK_SIZE)
    mov     x21, x19
track_fill:
    str     w25, [x21], #4
    subs    x20, x20, #1
    b.ne    track_fill

    mov     x22, #40                    /* столько касаний обслужим */

track_loop:
    sub     sp, sp, #16
    mov     x8, #SYS_INPUT
    mov     x0, sp
    svc     #0
    tbnz    x0, #63, track_noinput

    ldrh    w20, [sp, #0]               /* x касания */
    ldrh    w21, [sp, #2]               /* y касания */
    add     sp, sp, #16

    /* Окно ставим серединой под палец, не давая уйти за левый и
     * верхний край: там координаты стали бы отрицательными, а ядро
     * ждёт беззнаковые. */
    subs    x20, x20, #TRK_HALF
    b.ge    track_x_ok
    mov     x20, #0
track_x_ok:
    subs    x21, x21, #TRK_HALF
    b.ge    track_y_ok
    mov     x21, #0
track_y_ok:

    mov     x8, #SYS_PRESENT
    mov     x0, x20
    mov     x1, x21
    svc     #0

    subs    x22, x22, #1
    b.ne    track_loop

    /*
     * Сорок касаний обслужено — говорим об этом и продолжаем.
     *
     * Раньше здесь программа завершалась, и это было ошибкой замысла:
     * сорок событий при ведении пальцем набираются за один взмах, и
     * окно исчезало через секунду после первого касания. Приложение,
     * которое ждёт человека, живёт столько же, сколько система.
     */
    mov     x8, #SYS_WRITE
    adr     x0, track_done
    mov     x1, #(track_done_end - track_done)
    svc     #0
    mov     x22, #40
    b       track_loop

track_nowin:
    mov     x8, #SYS_WRITE
    adr     x0, track_no_window
    mov     x1, #(track_no_window_end - track_no_window)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #12
    svc     #0
    b       .

track_noinput:
    add     sp, sp, #16
    mov     x8, #SYS_WRITE
    adr     x0, track_no_input
    mov     x1, #(track_no_input_end - track_no_input)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #13
    svc     #0
    b       .

track_done:
    .ascii  "EL0      : СЛЕДОПЫТ: ЕЩЁ СОРОК КАСАНИЙ ОТСЛЕЖЕНО\n"
track_done_end:

track_no_window:
    .ascii  "EL0      : СЛЕДОПЫТ: ОКНА НЕ ДАЛИ\n"
track_no_window_end:

track_no_input:
    .ascii  "EL0      : СЛЕДОПЫТ: ЯДРО НЕ ОТДАЁТ КАСАНИЯ\n"
track_no_input_end:

.align 4
.global user_track_end
user_track_end:

/* ---------------------------------------------------------------------
 * Девятая программа: панель.
 *
 * Первое настоящее приложение: своё окно, свой текст, живые числа,
 * отклик на касания. Всё, что ей дало ядро, — буфер окна, шрифт и
 * очередь касаний; остального она не видит.
 *
 * Числа она собирает сама: строки складываются на стеке, потому что
 * страницы кода отображены только на чтение — писать в свои же данные
 * программа не может, и это правильно.
 */
#define PNL_W       480
#define PNL_H       200
#define PNL_TOUCHES 40

.align 4
.global user_panel_start
user_panel_start:
    mov     x8, #SYS_WINDOW
    mov     x0, #PNL_W
    mov     x1, #PNL_H
    svc     #0
    cbz     x0, panel_nowin
    mov     x19, x0                     /* буфер окна */

    mov     x24, #0                     /* счётчик касаний */
    mov     x27, #PNL_TOUCHES           /* до следующего доклада */
    mov     x25, #0                     /* x последнего касания */
    mov     x26, #0                     /* y последнего касания */

panel_draw:
    /* Заливка окна: тёмный фон, чтобы текст читался */
    movz    w0, #0x2030
    movk    w0, #0xFF10, lsl #16
    /* Число пикселей считаем умножением: 96000 одной командой в
     * регистр не положить, а сомножители кладутся. */
    mov     x1, #PNL_W
    mov     x3, #PNL_H
    mul     x1, x1, x3
    mov     x2, x19
panel_fill:
    str     w0, [x2], #4
    subs    x1, x1, #1
    b.ne    panel_fill

    /* Заголовок */
    mov     x8, #SYS_TEXT
    mov     x0, #16
    mov     x1, #16
    mov     x2, #3
    movz    w3, #0xFFFF
    movk    w3, #0xFF80, lsl #16
    adr     x4, panel_title
    mov     x5, #(panel_title_end - panel_title)
    svc     #0

    /* Строка «КАСАНИЙ: N» собирается на стеке */
    sub     sp, sp, #64
    mov     x20, sp
    adr     x0, panel_touch_label
    mov     x1, x20
    mov     x2, #(panel_touch_label_end - panel_touch_label)
    bl      panel_copy
    mov     x0, x24
    bl      panel_number
    sub     x2, x1, x20                 /* длина получившейся строки */

    mov     x8, #SYS_TEXT
    mov     x5, x2
    mov     x4, x20
    mov     x0, #16
    mov     x1, #70
    mov     x2, #2
    movz    w3, #0xFFFF
    movk    w3, #0xFFFF, lsl #16
    svc     #0
    add     sp, sp, #64

    /* Строка «ГДЕ: x,y» */
    sub     sp, sp, #64
    mov     x20, sp
    adr     x0, panel_where_label
    mov     x1, x20
    mov     x2, #(panel_where_label_end - panel_where_label)
    bl      panel_copy
    mov     x0, x25
    bl      panel_number
    mov     x0, #44                     /* запятая */
    strb    w0, [x1], #1
    mov     x0, x26
    bl      panel_number
    sub     x2, x1, x20

    mov     x8, #SYS_TEXT
    mov     x5, x2
    mov     x4, x20
    mov     x0, #16
    mov     x1, #120
    mov     x2, #2
    movz    w3, #0xC0FF
    movk    w3, #0xFF60, lsl #16
    svc     #0
    add     sp, sp, #64

    /* Показать окно там, где оно и было */
    mov     x8, #SYS_PRESENT
    mov     x0, #40
    mov     x1, #900
    svc     #0

    /* Каждые сорок касаний — строка в консоль, и живём дальше:
     * приложение, которое ждёт человека, не должно уходить само. */
    subs    x27, x27, #1
    b.ne    panel_keep

    mov     x8, #SYS_WRITE
    adr     x0, panel_bye
    mov     x1, #(panel_bye_end - panel_bye)
    svc     #0
    mov     x27, #PNL_TOUCHES

panel_keep:

    /* Ждём следующее касание */
    sub     sp, sp, #16
    mov     x8, #SYS_INPUT
    mov     x0, sp
    svc     #0
    tbnz    x0, #63, panel_noinput
    ldrh    w25, [sp, #0]
    ldrh    w26, [sp, #2]
    add     sp, sp, #16
    add     x24, x24, #1
    b       panel_draw

panel_nowin:
    mov     x8, #SYS_WRITE
    adr     x0, panel_no_window
    mov     x1, #(panel_no_window_end - panel_no_window)
    svc     #0
    mov     x8, #SYS_EXIT
    mov     x0, #14
    svc     #0
    b       .

panel_noinput:
    add     sp, sp, #16
    mov     x8, #SYS_EXIT
    mov     x0, #15
    svc     #0
    b       .

/*
 * Скопировать x2 байт из x0 в x1. Возвращает конец в x1.
 * Нужна потому, что писать строки можно только на стек: страницы кода
 * отображены на чтение, и свои же данные программа не изменит.
 */
panel_copy:
    cbz     x2, panel_copy_end
1:  ldrb    w3, [x0], #1
    strb    w3, [x1], #1
    subs    x2, x2, #1
    b.ne    1b
panel_copy_end:
    ret

/*
 * Число x0 десятичными цифрами по адресу x1. Возвращает конец в x1.
 *
 * Цифры получаются с конца — от младшей, — поэтому сначала складываем их
 * в обратном порядке на стек, а потом переписываем. Делить приходится
 * самим: умножения на обратное число у нас нет, а udiv есть.
 */
panel_number:
    sub     sp, sp, #32
    mov     x3, #10
    add     x4, sp, #24                 /* пишем назад от этого места */
    mov     x5, #0                      /* сколько цифр вышло */
1:  udiv    x6, x0, x3
    msub    x7, x6, x3, x0              /* остаток = x0 - (x0/10)*10 */
    add     x7, x7, #48
    sub     x4, x4, #1
    strb    w7, [x4]
    add     x5, x5, #1
    mov     x0, x6
    cbnz    x0, 1b
2:  ldrb    w7, [x4], #1
    strb    w7, [x1], #1
    subs    x5, x5, #1
    b.ne    2b
    add     sp, sp, #32
    ret

panel_title:
    .ascii  "ПАНЕЛЬ EL0\n"
panel_title_end:

panel_touch_label:
    .ascii  "КАСАНИЙ: "
panel_touch_label_end:

panel_where_label:
    .ascii  "ГДЕ: "
panel_where_label_end:

panel_bye:
    .ascii  "EL0      : ПАНЕЛЬ: ЕЩЁ СОРОК КАСАНИЙ ПОКАЗАНО\n"
panel_bye_end:

panel_no_window:
    .ascii  "EL0      : ПАНЕЛЬ: ОКНА НЕ ДАЛИ\n"
panel_no_window_end:

.align 4
.global user_panel_end
user_panel_end:

/* ---------------------------------------------------------------------
 * Программы на Си.
 *
 * Собираются отдельно (см. Makefile и user/), а сюда попадают готовым
 * образом. Ассемблер здесь остаётся только ради границ: ядру нужно
 * знать, откуда и сколько копировать.
 *
 * Писать программы на ассемблере дальше смысла нет: те, что выше,
 * проверяют по одной вещи каждая, а настоящее приложение — это работа с
 * текстом, разбором касаний и раскладкой на экране.
 */
.align 4
.global user_hello_c_start
user_hello_c_start:
    .incbin "user/hello.bin"
.align 4
.global user_hello_c_end
user_hello_c_end:

.align 4
.global user_shell_start
user_shell_start:
    .incbin "user/shell.bin"
.align 4
.global user_shell_end
user_shell_end:
