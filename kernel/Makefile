# Сборка ядра StellarOS.
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

# HALT_AT=N — собрать ядро, которое доходит до этапа N и вечно висит в wfi.
# Единственный способ отладки на телефоне, пока нет канала вывода: телефон
# либо зависает (дошли), либо перезагружается (упали раньше). Этапы
# перечислены в src/main.c рядом с HALT_STAGE.
#   make BOARD=merlin HALT_AT=1   — сразу после выключения сторожа
#   make BOARD=merlin HALT_AT=4   — после инициализации экрана
ifneq ($(HALT_AT),)
  BOARD_DEF += -DHALT_AT=$(HALT_AT)
endif

# RESET_AT_HALT=1 — в точке залипания намеренно перезагрузить телефон.
# Чёрный экран не отличает «дошли, но экран не работает» от «не дошли»,
# а перезагрузка видна всегда: цикл ресетов = дошли, тишина = упали раньше.
# BOOTLOG=1 — лог загрузки на экране вместо заставки, вместе с цветными
# полосами. Нужен, когда телефон не отдаёт консоль по USB: тогда экран
# снова единственный канал, и красота уступает связи.
ifneq ($(BOOTLOG),)
  BOARD_DEF += -DBOOTLOG=1
endif

ifneq ($(RESET_AT_HALT),)
  BOARD_DEF += -DRESET_AT_HALT=1
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

# Программы для EL0 собираются отдельно: свой скрипт компоновки, свой
# адрес, никакой связи с ядром, кроме номеров системных вызовов. Готовый
# образ вшивается в ядро (см. src/user.S), поэтому каталог сборки
# добавлен в пути поиска ассемблера — .incbin ищет по ним же.
ASFLAGS := $(CFLAGS) -I$(BUILD)
LDFLAGS := -nostdlib -T linker.ld --defsym LOAD_ADDR=$(LOAD_ADDR)

# Приложения на Hittis.
#
# Компилятор и виртуальная машина живут в ../hittis и собираются дважды:
# для компьютера — здесь, чтобы превратить .ht в .slt, и для телефона —
# вместе с исполнителем в EL0. Исходник у машины при этом один, и разойтись
# двум сборкам негде.
#
# HOSTCC — именно компилятор этой машины, не кроссовый: инструменты
# должны запускаться здесь и сейчас.
HITTIS  := ../hittis
HOSTCC  ?= cc
APP_SRCS := $(wildcard ../apps/*.ht)

USER_SRCS := $(wildcard user/*.c)
USER_BINS := $(patsubst user/%.c,$(BUILD)/user/%.bin,$(USER_SRCS))

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

# --- Программы для EL0 ---
#
# Линкуются по фиксированному адресу 0x1000000000 — ядро кладёт их ровно
# туда. Заголовков в образе нет: только код и постоянные данные, всё
# остальное программа получает от ядра.
$(BUILD)/user:
	@mkdir -p $(BUILD)/user

# Заголовки программ перечислены явно: у пользовательских образов нет
# автоматического учёта зависимостей, и правка ulib.h или общего
# заголовка вызовов иначе не пересобрала бы ни одной программы. Молча —
# а значит с расхождением между тем, что зовёт программа, и тем, что
# ждёт ядро.
USER_DEPS := user/ulib.h include/syscall.h user/user.ld

# --- Шрифт ---
#
# Manrope растрируется при сборке: ядру нужны пиксели, а не кривые —
# разбирать TrueType на телефоне не на чем и незачем. Кегли те же, что
# просит интерфейс (scale 2,3,4,6 в вызовах вывода текста).
$(BUILD)/host/fontgen: ../tools/fontgen.c
	@mkdir -p $(BUILD)/host
	$(HOSTCC) -O2 -o $@ $< -lm

$(BUILD)/font/manrope.stf: ../fonts/Manrope-SemiBold.ttf $(BUILD)/host/fontgen
	@mkdir -p $(BUILD)/font
	$(BUILD)/host/fontgen $< 16,24,32,48,102:StellarOS,150:StellarOS $@

# font_data.S вшивает шрифт, поэтому пересобирается вместе с ним
$(BUILD)/font_data.o: $(BUILD)/font/manrope.stf

# --- Приложения на Hittis ---

$(BUILD)/host/hittis: $(HITTIS)/hittis.c $(HITTIS)/slt.h
	@mkdir -p $(BUILD)/host
	$(HOSTCC) -O2 -o $@ $<

$(BUILD)/host/bin2c: $(HITTIS)/bin2c.c
	@mkdir -p $(BUILD)/host
	$(HOSTCC) -O2 -o $@ $<

# Приложение становится массивом байт: вшить его в образ — единственный
# способ доставить .slt в телефон, пока разделы не читаются.
#
# Берём готовое из apps/ и НЕ компилируем ничего по дороге. Раньше это
# правило само превращало ../apps/demo.ht в .slt посреди сборки системы,
# и получалось, что сборка ядра тайком собирает ещё и приложение —
# два разных дела в одном действии. Теперь они разделены: исходники
# лежат в исходники/ и превращаются в приложения командой ht build,
# а система читает только готовое.
$(BUILD)/user/app_slt.h: ../apps/calc.slt $(BUILD)/host/bin2c | $(BUILD)/user
	$(BUILD)/host/bin2c $< app_slt > $@

# Нет готового приложения — говорим об этом прямо, а не молчим об
# отсутствующем файле языком make.
../apps/calc.slt:
	@echo "нет apps/calc.slt — собери его: ht build calc"; false

# Исполнитель — единственная программа из двух исходников: свой и машина
# из ../hittis. Поэтому у него отдельное правило, а не общее по шаблону.
$(BUILD)/user/vmapp.elf: user/vmapp.c $(HITTIS)/vm.c $(HITTIS)/vm.h                          $(HITTIS)/slt.h $(BUILD)/user/app_slt.h                          $(USER_DEPS) | $(BUILD)/user
	$(CC) $(CFLAGS) -Iuser -I$(HITTIS) -I$(BUILD)/user -static 	      -T user/user.ld -o $@ user/vmapp.c $(HITTIS)/vm.c

$(BUILD)/user/%.elf: user/%.c $(USER_DEPS) | $(BUILD)/user
	$(CC) $(CFLAGS) -Iuser -static -T user/user.ld -o $@ $<

$(BUILD)/user/%.bin: $(BUILD)/user/%.elf
	$(OBJCOPY) -O binary $< $@

# user.S вшивает эти образы, поэтому пересобирается вместе с ними
$(BUILD)/user.o: $(USER_BINS)

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
	@cd .. && python3 tools/screenshot.py 3.5 kernel/build/stellar-screen.png

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
