#ifndef PSCI_H
#define PSCI_H
#include "types.h"

/*
 * PSCI — Power State Coordination Interface.
 *
 * Разбудить второе ядро самостоятельно мы не можем: включение питания
 * кластера, сброс, тактирование — всё это делает прошивка, живущая
 * в EL3 (на телефоне это ARM Trusted Firmware, в QEMU — эмуляция).
 * Разговариваем с ней инструкцией smc по стандартному протоколу PSCI:
 * «включи ядро с таким-то MPIDR и начни исполнение по такому адресу».
 *
 * Это ровно то же, что делает Linux при загрузке. Ничего своего тут
 * изобрести нельзя — интерфейс диктует прошивка.
 */

#define PSCI_SUCCESS            0
#define PSCI_NOT_SUPPORTED      (-1)
#define PSCI_INVALID_PARAMS     (-2)
#define PSCI_DENIED             (-3)
#define PSCI_ALREADY_ON         (-4)
#define PSCI_ON_PENDING         (-5)
#define PSCI_INTERNAL_FAILURE   (-6)

/* Версия протокола: старшая половина — major, младшая — minor.
 * Отрицательный результат означает, что PSCI недоступен вовсе. */
s64 psci_version(void);

/* Запустить ядро. mpidr — как в MPIDR_EL1, entry — физический адрес входа,
 * context_id придёт в x0 запущенному ядру. */
s64 psci_cpu_on(u64 mpidr, u64 entry, u64 context_id);

/* Состояние ядра: 0 — включено, 1 — выключено, 2 — включается */
s64 psci_affinity_info(u64 mpidr);

#endif
