/*
 * psci.c — вызовы прошивки EL3 по протоколу PSCI.
 *
 * Все аргументы и результат идут через x0..x3 по соглашению SMCCC.
 * Инструкция smc переключает процессор в EL3, там прошивка делает работу
 * и возвращает управление следующей за smc инструкцией.
 */
#include "psci.h"

/* Идентификаторы функций. Префикс 0x84 — 32-битный вызов,
 * 0xC4 — 64-битный (аргументы шире 32 бит, как MPIDR и адрес входа). */
#define PSCI_FN_VERSION         0x84000000UL
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
