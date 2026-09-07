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

# Просмотр приложения в браузере. Никаких библиотек: сокеты и картинка.
#
# Сначала было окно X11 — и оказалось, что WSLg не доносит окна до
# рабочего стола Windows. Браузер есть везде, работает без графики в WSL
# и заодно по сети.
preview: preview.c vm.c vm.h slt.h manrope.stf ide_html.h
	$(CC) $(CFLAGS) -o $@ preview.c vm.c

# Страница среды разработки: держим её обычным html-файлом, а в программу
# она попадает массивом байт. Иначе разметку пришлось бы писать строковым
# литералом в C — без подсветки, без отступов и без возможности открыть
# её в браузере отдельно.
ide_html.h: ide.html bin2c.c ../tools/jscheck.py
	python3 ../tools/jscheck.py ide.html
	$(CC) $(CFLAGS) -o /tmp/hittis-bin2c bin2c.c
	/tmp/hittis-bin2c ide.html ide_html > $@

# Шрифт для просмотра — тот же самый, что уезжает в телефон.
# Делается здесь, чтобы просмотр работал и без собранного ядра.
manrope.stf: ../tools/fontgen.c ../fonts/Manrope-SemiBold.ttf
	$(CC) $(CFLAGS) -o /tmp/hittis-fontgen ../tools/fontgen.c -lm
	/tmp/hittis-fontgen ../fonts/Manrope-SemiBold.ttf 16,24,32,48 $@

# Собрать и тут же прогнать все исходники
check: hittis hostrun
	@for d in ../sources/*/; do \
		n=`basename $$d`; \
		echo "--- $$n"; \
		./hittis $$d/main.ht /tmp/$$n.slt >/dev/null || exit 1; \
		./hostrun /tmp/$$n.slt || exit 1; \
	done

clean:
	rm -f hittis hostrun preview manrope.stf ide_html.h
