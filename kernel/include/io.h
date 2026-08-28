#ifndef IO_H
#define IO_H
#include "types.h"

/*
 * Доступ к регистрам периферии.
 * volatile обязателен: без него компилятор выбросит «бесполезные» чтения,
 * а барьеры не дают процессору переупорядочить обращения к MMIO.
 */

static inline void mmio_write32(uintptr_t addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline u32 mmio_read32(uintptr_t addr)
{
    return *(volatile u32 *)addr;
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

#endif
