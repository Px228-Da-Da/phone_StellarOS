# Сборка инструментов Hittis для компьютера.
#
# Компилятор и запускалка собираются обычным gcc: они работают на
# компьютере, а не в телефоне. Виртуальная машина (vm.c) при этом одна и
# та же — тот же файл собирается и в программу для StellarOS.

CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu99

all: hittis hostrun

hittis: hittis.c slt.h
	$(CC) $(CFLAGS) -o $@ hittis.c

hostrun: hostrun.c vm.c vm.h slt.h
	$(CC) $(CFLAGS) -o $@ hostrun.c vm.c

# Собрать и тут же прогнать все приложения из apps/
check: hittis hostrun
	@for f in ../apps/*.ht; do \
		echo "--- $$f"; \
		./hittis $$f /tmp/`basename $$f .ht`.slt >/dev/null || exit 1; \
		./hostrun /tmp/`basename $$f .ht`.slt || exit 1; \
	done

clean:
	rm -f hittis hostrun
