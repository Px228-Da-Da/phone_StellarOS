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
# -MMD -MP: компилятор сам записывает, от каких заголовков зависит объектник.
# Без этого правка любого .h не вызывала пересборку вообще — файл менялся,
# а в образ ехал старый объектник, и проверка «сломаю константу и посмотрю»
# молча проверяла предыдущую сборку.
CFLAGS := -MMD -MP -ffreestanding -nostdlib -nostartfiles -fno-builtin \
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

# Запуск в эмуляторе.
#
# Сборку вызываем отдельным make с явным BOARD. Причина не в стиле:
# BUILD вычисляется один раз при разборе Makefile, и «run-phone: BOARD := phone»
# на него уже не влияет — из-за этого run-phone однажды честно собрал и запустил
# сборку для qemu, молча подсунув 1 ГБ вместо телефонных 4.
#
# gic-version=3 обязателен везде: в merlin стоит GIC-500 (v3), и отлаживать
# надо ровно тот код, который поедет на телефон.
QEMU_BASE := qemu-system-aarch64 -M virt,gic-version=3 -nographic

run:
	@$(MAKE) --no-print-directory BOARD=qemu build/qemu/Image
	$(QEMU_BASE) -cpu cortex-a53 -smp 8 -m 1G -kernel build/qemu/Image

# То же самое, но с окном: ядро рисует свой экран через ramfb.
#
# -device ramfb   даёт ядру дисплей (см. src/ramfb.c)
# -serial stdio   лог по-прежнему в терминал, чтобы видеть оба канала сразу
#
# В WSL окно открывается через WSLg. Если его нет, есть два пути:
#   make screenshot            — снять кадр в PNG, без всякого окна
#   запустить Windows-сборку QEMU (см. README, «Как посмотреть экран»)
run-screen:
	@$(MAKE) --no-print-directory BOARD=qemu build/qemu/Image
	qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -smp 8 -m 1G 	    -device ramfb -serial stdio -kernel build/qemu/Image

# Снимок экрана в PNG — работает где угодно, окно не нужно
screenshot:
	@$(MAKE) --no-print-directory BOARD=qemu build/qemu/Image
	@cd .. && python3 tools/screenshot.py 3.5 kernel/build/velo-screen.png

# Конфигурация, максимально близкая к Redmi Note 9:
#   cortex-a55 — малые ядра Helio G85; большие A75 QEMU не эмулирует,
#                но набор инструкций у них общий
#   8 ядер     — как у G85
#   4 ГБ       — как у старшей версии телефона, и сборка BOARD=phone
#                знает про этот объём: иначе ядро распорядится лишь гигабайтом
#
# Кластеры здесь не задаём: при GICv3 QEMU кладёт в кластер 16 ядер, и на
# восьми ядрах «clusters=2» меняет только описание в DTB, а MPIDR оставляет
# плоскими. Второй кластер проверяется отдельно, запуском с -smp 20.
run-phone:
	@$(MAKE) --no-print-directory BOARD=phone build/phone/Image
	$(QEMU_BASE) -cpu cortex-a55 -smp 8 -m 4G -kernel build/phone/Image

debug:
	@$(MAKE) --no-print-directory BOARD=qemu build/qemu/Image
	@echo "Ожидаю gdb на localhost:1234 ..."
	$(QEMU_BASE) -cpu cortex-a53 -smp 8 -m 1G -kernel build/qemu/Image -S -s

clean:
	rm -rf build

# Зависимости от заголовков, записанные компилятором
-include $(OBJS:.o=.d)

.PHONY: all run run-screen run-phone screenshot debug clean
