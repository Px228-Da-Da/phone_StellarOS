/*
 * psci.c — вызовы прошивки EL3 по протоколу PSCI.
 *
 * Все аргументы и результат идут через x0..x3 по соглашению SMCCC.
 * Инструкция smc переключает процессор в EL3, там прошивка делает работу
 * и возвращает управление следующей за smc инструкцией.
 */
#include "psci.h"
#include "print.h"
#include "usb.h"
#include "io.h"
#if defined(BOARD_MERLIN)
#include "soc/mt6768.h"
#include "pmic.h"
#include "battery.h"

/*
 * Выключение телефона идёт через контроллер питания MT6358.
 *
 * Прошивка в EL3 этого не умеет — проверено на устройстве: вызов PSCI
 * SYSTEM_OFF не возвращается и навсегда уносит с собой вызвавшее ядро.
 * Настоящий выключатель живёт в блоке RTC внутри PMIC: снятие
 * удерживающего бита BBPU и есть то, что размыкает питание.
 *
 * Числа взяты из исходников ядра Linux, а не из головы:
 *   include/linux/mfd/mt6397/rtc.h   — RTC_BBPU, RTC_BBPU_KEY, CBUSY,
 *                                      RTC_WRTGR_MT6358
 *   drivers/mfd/mt6397-core.c        — MT6358_RTC_BASE
 *   drivers/power/reset/mt6323-poweroff.c — сама последовательность
 *
 * Запись в PMIC — самое дорогое место в системе, поэтому перед ней мы
 * доказываем, что попали куда собирались: читаем секунды часов реального
 * времени и убеждаемся, что они лежат в 0..59 и идут. Если по этому
 * адресу окажется что-то другое, проверка не сойдётся и записи не будет
 * вовсе. Лучше не выключиться, чем выключить не то.
 */
#define MT6358_RTC_BASE     0x0588
#define MT6358_RTC_BBPU     (MT6358_RTC_BASE + 0x0000)
#define MT6358_RTC_TC_SEC   (MT6358_RTC_BASE + 0x000A)
#define MT6358_RTC_WRTGR    (MT6358_RTC_BASE + 0x003A)

#define RTC_BBPU_KEY        0x4300      /* ключ: без него запись не примут */
#define RTC_BBPU_CBUSY      (1U << 6)   /* блок ещё переваривает запись    */

/*
 * Настоящий выключатель MT6358.
 *
 * Одного RTC оказалось мало: на устройстве запись проходила (бит BBPU
 * честно менялся с 1 на 0), а телефон оставался жив даже без кабеля.
 * Значит питание держит что-то ещё — и это «PWRHOLD», отдельный бит,
 * которым процессор говорит контроллеру «не отпускай питание». Пока он
 * стоит, никакой RTC телефон не погасит.
 *
 * Адрес взят из исходников coreboot для MT8183 — там стоит ровно этот
 * же MT6358, и файл открыт:
 *   src/soc/mediatek/mt8183/include/soc/mt6358.h   PMIC_PWRHOLD = 0x0a08
 *   src/soc/mediatek/mt8183/mt6358.c               pmic_set_power_hold():
 *       pwrap_write_field(PMIC_PWRHOLD, enable ? 1 : 0, 0x1, 0);
 *
 * То есть бит нулевой, и снимать его надо чтением-правкой-записью:
 * соседние биты этого регистра нам не принадлежат.
 */
#define MT6358_PWRHOLD      0x0A08
#define PWRHOLD_BIT         (1U << 0)

static void spin_ms(u32 ms)
{
    u64 end = read_cntpct() + (read_cntfrq() / 1000) * ms;

    while (read_cntpct() < end)
        ;
}

/*
 * Те ли это часы.
 *
 * Секунды — единственное, что в PMIC меняется само и предсказуемо.
 * Значение вне 0..59 или застывшее на месте означает, что по этому
 * адресу лежит не RTC, и трогать его нельзя.
 */
static int rtc_looks_real(void)
{
    u16 a, b;

    if (pmic_read(MT6358_RTC_TC_SEC, &a) != 0)
        return 0;
    spin_ms(1200);
    if (pmic_read(MT6358_RTC_TC_SEC, &b) != 0)
        return 0;

    kprintf("ПИТАНИЕ  : СЕКУНДЫ RTC %u -> %u\n", a & 0x3F, b & 0x3F);

    if ((a & ~0x3Fu) || (b & ~0x3Fu))
        return 0;               /* в регистре не только секунды — не RTC */
    if ((a & 0x3F) > 59 || (b & 0x3F) > 59)
        return 0;
    return a != b;              /* стоят на месте — часы не идут         */
}
#endif

/* Идентификаторы функций. Префикс 0x84 — 32-битный вызов,
 * 0xC4 — 64-битный (аргументы шире 32 бит, как MPIDR и адрес входа). */
#define PSCI_FN_VERSION         0x84000000UL
#define PSCI_FN_SYSTEM_OFF      0x84000008UL
#define PSCI_FN_SYSTEM_RESET    0x84000009UL
#define PSCI_FN_CPU_ON_64       0xC4000003UL
#define PSCI_FN_AFFINITY_INFO64 0xC4000004UL

/*
 * Чем звать прошивку — smc или hvc.
 *
 * Инструкция зависит от того, на каком уровне живёт тот, кто отвечает
 * за PSCI. На телефоне это ATF в EL3, туда ведёт smc. В QEMU без
 * secure-мира EL3 попросту нет, и PSCI эмулируется на уровне EL2 —
 * там нужен hvc, а smc окажется неопределённой инструкцией и уронит ядро.
 *
 * Спрашиваем процессор, реализован ли у него EL3 (ID_AA64PFR0_EL1):
 * есть EL3 — там сидит прошивка, зовём smc; нет — значит PSCI обслуживает
 * тот, кто ниже, и это hvc.
 *
 * Проверять заодно и наличие EL2 бессмысленно: QEMU без secure-мира
 * принимает hvc из EL1, даже когда EL2 не реализован вовсе — именно так
 * он и отдаёт PSCI. Проверка «есть ли EL2» отсекла бы рабочий случай.
 */
enum { CONDUIT_UNKNOWN, CONDUIT_SMC, CONDUIT_HVC };

static int conduit;

static void psci_detect_conduit(void)
{
    u64 pfr0;

    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));

    conduit = (((pfr0 >> 12) & 0xF) != 0) ? CONDUIT_SMC : CONDUIT_HVC;
}

static s64 psci_call(u64 fn, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 __asm__("x0") = fn;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;

    if (conduit == CONDUIT_UNKNOWN)
        psci_detect_conduit();

    if (conduit == CONDUIT_SMC)
        __asm__ volatile("smc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :: "memory");
    else
        __asm__ volatile("hvc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         :: "memory");

    /* Коды PSCI 32-битные: 0xFFFFFFFF означает -1, а не четыре миллиарда.
     * Без приведения через s32 любая ошибка выглядела бы успехом. */
    return (s64)(s32)x0;
}

s64 psci_version(void)
{
    return psci_call(PSCI_FN_VERSION, 0, 0, 0);
}

s64 psci_cpu_on(u64 mpidr, u64 entry, u64 context_id)
{
    return psci_call(PSCI_FN_CPU_ON_64, mpidr, entry, context_id);
}

s64 psci_affinity_info(u64 mpidr)
{
    /* Последний аргумент — уровень аффинити; 0 означает «конкретное ядро» */
    return psci_call(PSCI_FN_AFFINITY_INFO64, mpidr, 0, 0);
}

/*
 * Перезагрузка.
 *
 * Порядок попыток — от общего к частному: PSCI работает и на телефоне, и
 * в эмуляторе, а сторожевой таймер только на merlin, зато не спрашивая
 * ни у кого разрешения.
 */
/*
 * Выключить телефон.
 *
 * Просим прошивку в EL3 — это единственный честный путь. Своими руками
 * снять питание нельзя: этим распоряжается контроллер питания, а его
 * регистры выключения мы не знаем и выдумывать не будем. Правило здесь
 * то же, что и везде: сведения о железе берутся у производителя, а не
 * из догадок, — цена ошибки в регистре PMIC заметно выше, чем в любом
 * другом месте системы.
 *
 * Поэтому и возвращаемое значение: если прошивка выключать не умеет,
 * мы возвращаемся и говорим об этом вслух, а не делаем вид, что телефон
 * выключен, оставив его с погашенным экраном и живым процессором.
 */
s64 machine_power_off(void)
{
#if defined(BOARD_MERLIN)
    struct battery_state bat;
    u16 bbpu;
    u64 deadline;

    kprintf("ПИТАНИЕ  : ВЫКЛЮЧАЮ ЧЕРЕЗ КОНТРОЛЛЕР ПИТАНИЯ\n");

    /*
     * С воткнутым кабелем телефон не выключается — и это не наша
     * недоработка, а устройство железа: пока контроллер питания видит
     * зарядку, он держит линии под напряжением. Ровно поэтому Android на
     * шнуре показывает картинку зарядки вместо того, чтобы погаснуть.
     *
     * Проверено на устройстве: удерживающий бит BBPU честно менялся с 1
     * на 0, а телефон оставался живым. И это хуже, чем просто «не
     * сработало»: бит так и оставался снятым, то есть телефон мог
     * погаснуть потом, в случайный момент, когда шнур вынут. Поэтому со
     * шнуром мы не пишем вовсе.
     */
    battery_last(&bat);
    if (bat.charging || bat.current_ma > 0) {
        kprintf("ПИТАНИЕ  : ПОДКЛЮЧЕН КАБЕЛЬ — С НИМ НЕ ВЫКЛЮЧИТЬ\n");
        usb_flush();
        return PSCI_DENIED;
    }

    if (!rtc_looks_real()) {
        kprintf("ПИТАНИЕ  : ЭТО НЕ ЧАСЫ PMIC — НЕ ПИШУ, ОТМЕНЯЮ\n");
        usb_flush();
        return PSCI_NOT_SUPPORTED;
    }

    if (pmic_read(MT6358_RTC_BBPU, &bbpu) == 0)
        kprintf("ПИТАНИЕ  : BBPU БЫЛО %04x\n", bbpu);
    usb_flush();

    /*
     * Две записи, и обе обязательны: первая кладёт новое значение с
     * ключом, вторая велит блоку принять его. Без второй запись просто
     * лежит и ничего не делает — так устроен RTC, а не наша выдумка.
     */
    if (pmic_write(MT6358_RTC_BBPU, RTC_BBPU_KEY) != 0 ||
        pmic_write(MT6358_RTC_WRTGR, 1) != 0) {
        kprintf("ПИТАНИЕ  : PMIC НЕ ПРИНЯЛ ЗАПИСЬ\n");
        usb_flush();
        return PSCI_INTERNAL_FAILURE;
    }

    /* Ждём, пока блок примет запись */
    deadline = read_cntpct() + read_cntfrq() / 10;      /* 100 мс */
    while (read_cntpct() < deadline)
        if (pmic_read(MT6358_RTC_BBPU, &bbpu) == 0 &&
            !(bbpu & RTC_BBPU_CBUSY))
            break;

    /*
     * И отпускаем удержание питания. Это и есть выключатель: пока бит
     * стоит, контроллер держит линии под напряжением, что бы ни делал
     * RTC. Правим один бит, остальные сохраняем — регистр не наш.
     */
    {
        u16 hold;

        if (pmic_read(MT6358_PWRHOLD, &hold) != 0) {
            kprintf("ПИТАНИЕ  : PWRHOLD НЕ ЧИТАЕТСЯ\n");
            usb_flush();
            return PSCI_INTERNAL_FAILURE;
        }
        kprintf("ПИТАНИЕ  : PWRHOLD БЫЛО %04x, ОТПУСКАЮ\n", hold);
        usb_flush();
        pmic_write(MT6358_PWRHOLD, hold & ~PWRHOLD_BIT);
    }

    spin_ms(300);

    /* Досюда доходить не должны: телефон обязан был погаснуть */
    kprintf("ПИТАНИЕ  : НЕ ВЫКЛЮЧИЛСЯ, BBPU %04x\n", bbpu);
    usb_flush();
    return PSCI_INTERNAL_FAILURE;
#else
    s64 rc;

    kprintf("ПИТАНИЕ  : ВЫКЛЮЧАЮ ТЕЛЕФОН\n");
    usb_flush();

    rc = psci_call(PSCI_FN_SYSTEM_OFF, 0, 0, 0);

    /* Сюда попадаем только если не вышло: удачный вызов не возвращается */
    kprintf("ПИТАНИЕ  : ПРОШИВКА ОТКАЗАЛА, КОД %ld\n", rc);
    return rc ? rc : PSCI_NOT_SUPPORTED;
#endif
}

void machine_reset(void)
{
    kprintf("СБРОС    : ПЕРЕЗАГРУЖАЮ ТЕЛЕФОН\n");
    usb_flush();

    /*
     * Печатаем отказ вслух.
     *
     * Удачный вызов не возвращается, значит сюда мы попадаем только
     * когда прошивка перезагружать не умеет. До сих пор это молчаливо
     * лечилось сторожевым таймером, и со стороны выглядело одинаково —
     * телефон перезагружался. А знать разницу нужно: если прошивка не
     * делает даже сброс, то и выключения от неё ждать неоткуда, и
     * искать его надо совсем в другом месте.
     */
    kprintf("СБРОС    : ПРОШИВКА ОТКАЗАЛА, КОД %ld\n",
            psci_call(PSCI_FN_SYSTEM_RESET, 0, 0, 0));
    usb_flush();

#if defined(BOARD_MERLIN)
    /* Прошивка отказала — бьём по сторожевому таймеру */
    mmio_write32(MT_TOPRGU_BASE + WDT_SWRST, WDT_SWRST_KEY);
    dsb();
#endif

    for (;;)
        wfi();
}
