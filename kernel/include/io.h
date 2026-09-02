#ifndef IO_H
#define IO_H
#include "types.h"

/*
 * Доступ к регистрам периферии.
 * volatile обязателен: без него компилятор выбросит «бесполезные» чтения,
 * а барьеры не дают процессору переупорядочить обращения к MMIO.
 */

/*
 * Байтовый доступ. Нужен там, где регистр — массив байтов, а не слов:
 * например, приоритеты прерываний в GIC (по байту на прерывание).
 * Обратиться туда 32-битной записью нельзя: смещение обычно не кратно
 * четырём, а невыровненный доступ к Device-памяти на ARM — это исключение,
 * а не «медленнее».
 */
static inline void mmio_write8(uintptr_t addr, u8 val)
{
    *(volatile u8 *)addr = val;
}

static inline u8 mmio_read8(uintptr_t addr)
{
    return *(volatile u8 *)addr;
}

static inline void mmio_write32(uintptr_t addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline u32 mmio_read32(uintptr_t addr)
{
    return *(volatile u32 *)addr;
}

/* 64-битный доступ нужен GICv3: GICR_TYPER и GICD_IROUTER — одно 64-битное поле,
 * читать их двумя 32-битными обращениями спецификация не разрешает. */
static inline void mmio_write64(uintptr_t addr, u64 val)
{
    *(volatile u64 *)addr = val;
}

static inline u64 mmio_read64(uintptr_t addr)
{
    return *(volatile u64 *)addr;
}

/* Data Synchronization Barrier — дождаться реального завершения записи */
static inline void dsb(void)      { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void)      { __asm__ volatile("isb" ::: "memory"); }
static inline void wfi(void)      { __asm__ volatile("wfi"); }
static inline void wfe(void)      { __asm__ volatile("wfe"); }

static inline void irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

static inline u64 read_currentel(void)
{
    u64 v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return v >> 2;
}

static inline u64 read_mpidr(void)
{
    u64 v; __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v)); return v;
}

/* Системный счётчик: тикает всегда, частота в CNTFRQ_EL0 (обычно 13 МГц на MTK) */
static inline u64 read_cntpct(void)
{
    u64 v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v;
}

static inline u64 read_cntfrq(void)
{
    u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}

/* Виртуальный счётчик. В отличие от CNTPCT_EL0 доступен из EL1 всегда,
 * без разрешения со стороны EL2 — на телефоне это важно. */
static inline u64 read_cntvct(void)
{
    u64 v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v;
}

/*
 * Системные регистры GICv3 (ICC_*) и прочие, чьи имена старый ассемблер
 * может не знать. Пишем их «сырой» кодировкой S<op0>_<op1>_C<crn>_C<crm>_<op2> —
 * такая форма собирается всегда и заодно документирует саму кодировку.
 */
#define SYSREG_READ(reg) ({ u64 __v; __asm__ volatile("mrs %0, " reg : "=r"(__v)); __v; })
#define SYSREG_WRITE(reg, val)     __asm__ volatile("msr " reg ", %0" :: "r"((u64)(val)) : "memory")

#endif
