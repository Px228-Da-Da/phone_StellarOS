# Стенд для парсера device tree: тот же fdt.c, но собранный обычным gcc.
#
# Смысл в том, чтобы гонять разбор настоящего дерева телефона на ПК —
# до всякой прошивки. Копии кода здесь нет: берётся файл ядра как есть.

KERNEL := ../../kernel

CFLAGS := -Wall -Wextra -Werror -std=gnu11 -O2 -g -I$(KERNEL)/include

all: fdt-test

fdt-test: fdt-test.c $(KERNEL)/src/fdt.c
	$(CC) $(CFLAGS) $^ -o $@

# Быстрая проверка на дереве, которое выдаёт QEMU
check: fdt-test
	@qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=/tmp/fdt-check.dtb \
	    -cpu cortex-a53 -smp 8 -m 4G -nographic >/dev/null 2>&1
	./fdt-test /tmp/fdt-check.dtb

clean:
	rm -f fdt-test

.PHONY: all check clean
