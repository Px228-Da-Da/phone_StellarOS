# Сборка ядра VELO-OS.
#   make BOARD=qemu     — образ для эмулятора (быстрая итерация)
#   make BOARD=merlin   — образ для Redmi Note 9
#   make run            — запустить в QEMU
#   make debug          — QEMU с ожиданием gdb на :1234

BOARD      ?= qemu
CROSS      ?= aarch64-linux-gnu-
LOAD_ADDR  ?= 0x40080000

CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

BUILD := build/$(BOARD)

ifeq ($(BOARD),qemu)
  BOARD_DEF := -DBOARD_QEMU
else ifeq ($(BOARD),phone)
  # Тот же QEMU, но с телефонными параметрами: 4 ГБ памяти.
  # Остальное «как на телефоне» задаётся в строке запуска (см. run-phone).
  BOARD_DEF := -DBOARD_QEMU -DQEMU_RAM_SIZE_MB=4096
else ifeq ($(BOARD),merlin)
  BOARD_DEF := -DBOARD_MERLIN
else
  $(error BOARD должен быть qemu, phone или merlin)
endif

# -march=armv8-a (без -mcpu): merlin — big.LITTLE из Cortex-A75 и A55.
#   Ядро исполняется на любом из них, поэтому берём общий знаменатель ARMv8.
# -mstrict-align обязателен: MMU выключен, вся память трактуется как Device,
#   а невыровненный доступ к Device-памяти = alignment fault.
# -mgeneral-regs-only: не трогаем SIMD/FPU, пока не включили их в CPACR.
CFLAGS := -ffreestanding -nostdlib -nostartfiles -fno-builtin \
          -fno-stack-protector -fno-pic -fno-common \
          -mgeneral-regs-only -mstrict-align \
          -march=armv8-a \
          -Wall -Wextra -Werror -std=gnu11 -O2 -g \
          -Iinclude -Isrc $(BOARD_DEF)

ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -T linker.ld --defsym LOAD_ADDR=$(LOAD_ADDR)

C_SRCS := $(wildcard src/*.c)
S_SRCS := $(wildcard src/*.S)
OBJS   := $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRCS)) \
          $(patsubst src/%.S,$(BUILD)/%.o,$(S_SRCS))

all: $(BUILD)/Image

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@$(OBJDUMP) -d $@ > $(BUILD)/kernel.asm
	@echo "--- размер секций ---"
	@$(CROSS)size $@

# Raw-образ в формате arm64 Image: именно его ждёт и LK, и QEMU
$(BUILD)/Image: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Готово: $@ ($$(stat -c%s $@) байт)"

# gic-version=3 обязателен: в merlin стоит GIC-500 (v3), и мы хотим отлаживать
# в эмуляторе ровно тот код, который поедет на телефон.
# -smp 8 — столько же ядер, сколько у Helio G85: планировщик и PSCI
# проверяются в тех же условиях, что и на телефоне.
QEMU := qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -smp 8 -m 1G -nographic

run: BOARD := qemu
run: $(BUILD)/Image
	$(QEMU) \
	    -kernel $(BUILD)/Image

# Запуск в конфигурации, максимально близкой к Redmi Note 9:
#   cortex-a55   — малые ядра Helio G85 (большие A75 QEMU не эмулирует,
#                  но набор инструкций у них общий — ARMv8.2)
#   2 кластера   — как в G85; ядра второго кластера имеют Aff1=1, и вся
#                  работа с MPIDR проверяется по-настоящему только так
#   4 ГБ         — как в старшей версии телефона
run-phone: BOARD := phone
run-phone: $(BUILD)/Image
	qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a55 	    -smp 8,clusters=2,cores=4,threads=1 -m 4G -nographic 	    -kernel $(BUILD)/Image

debug: BOARD := qemu
debug: $(BUILD)/Image
	@echo "Ожидаю gdb на localhost:1234 ..."
	$(QEMU) \
	    -kernel $(BUILD)/Image -S -s

clean:
	rm -rf build

.PHONY: all run run-phone debug clean
