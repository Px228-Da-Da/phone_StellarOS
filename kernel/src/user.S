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
 * Запускает двоих. Первый честно отрабатывает и завершается сам —
 * ожидание должно вернуть его код. Второй нарушает границы, и его
 * снимает ядро — ожидание должно вернуть особый код, отличимый от
 * любого, который программа могла бы назначить себе сама. Разница между
 * этими двумя ответами и есть то, ради чего всё делалось.
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

    mov     x8, #SYS_EXIT
    mov     x0, #0
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
    .ascii  "EL0      : ЗАПУСКАЛА: БЕРУСЬ ЗА ДВОИХ\n"
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

boss_nostart:
    .ascii  "EL0      : ЗАПУСКАЛА: ЯДРО НЕ ДАЛО ЗАПУСТИТЬ ПРОГРАММУ\n"
boss_nostart_end:

.align 4
.global user_boss_end
user_boss_end:
