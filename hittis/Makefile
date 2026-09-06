# Сборка инструментов Hittis для компьютера.
#
# Компилятор и запускалка собираются обычным gcc: они работают на
# компьютере, а не в телефоне. Виртуальная машина (vm.c) при этом одна и
# та же — тот же файл собирается и в программу для StellarOS.

CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu99

all: hittis hostrun preview

hittis: hittis.c slt.h
	$(CC) $(CFLAGS) -o $@ hittis.c

hostrun: hostrun.c vm.c vm.h slt.h
	$(CC) $(CFLAGS) -o $@ hostrun.c vm.c

# Просмотр в настоящем окне. Нужен X11 — в WSL его даёт WSLg.
# Нет X11 — остаётся hostrun, который печатает рисование словами.
preview: preview.c vm.c vm.h slt.h manrope.stf
	$(CC) $(CFLAGS) -o $@ preview.c vm.c -lX11

# Шрифт для просмотра — тот же самый, что уезжает в телефон.
# Делается здесь, чтобы просмотр работал и без собранного ядра.
manrope.stf: ../tools/fontgen.c ../fonts/Manrope-SemiBold.ttf
	$(CC) $(CFLAGS) -o /tmp/hittis-fontgen ../tools/fontgen.c -lm
	/tmp/hittis-fontgen ../fonts/Manrope-SemiBold.ttf 16,24,32,48 $@

# Собрать и тут же прогнать все приложения из apps/
check: hittis hostrun
	@for f in ../apps/*.ht; do \
		echo "--- $$f"; \
		./hittis $$f /tmp/`basename $$f .ht`.slt >/dev/null || exit 1; \
		./hostrun /tmp/`basename $$f .ht`.slt || exit 1; \
	done

clean:
	rm -f hittis hostrun preview manrope.stf
