#!/usr/bin/env bash
# Упаковка нашего ядра в Android boot.img, который примет LK.
#
# Идея: не выдумывать параметры, а взять их из ОРИГИНАЛЬНОГО boot.img
# телефона и подменить ровно одно — ядро. unpack_bootimg умеет отдать
# готовую командную строку mkbootimg (--format mkbootimg), поэтому копируем
# у оригинала всё сразу: версию заголовка, страницу, смещения, cmdline и DTB.
#
# Почему это важно: у merlin заголовок версии 2 и внутри лежит DTB на
# 110 КБ. Собранный «на глазок» образ v0 без DTB загрузчик не берёт.
#
# Ядро кладём сжатым: в стоковом образе тоже gzip, а не голый Image.
#
# Запуск: tools/mkboot.sh prebuilt/backup/boot.img kernel/build/merlin/Image
set -e

ORIG_BOOT="${1:-prebuilt/backup/boot.img}"
KERNEL="${2:-kernel/build/merlin/Image}"
OUT="${3:-out/stellar-boot.img}"
WORK=unpacked

# Пакета mkbootimg на PyPI нет — инструмент берётся из AOSP,
# это делает tools/setup-wsl.sh.
for t in mkbootimg unpack_bootimg; do
    command -v "$t" >/dev/null || {
        echo "Нет $t. Поставить:  bash tools/setup-wsl.sh" >&2
        exit 1
    }
done
[ -f "$ORIG_BOOT" ] || { echo "Нет оригинального boot.img: $ORIG_BOOT" >&2; exit 1; }
[ -f "$KERNEL" ]    || { echo "Нет ядра: $KERNEL (сделай make BOARD=merlin)" >&2; exit 1; }

mkdir -p out "$WORK"

echo "==> Разбираю оригинальный boot.img"
rm -rf "$WORK"; mkdir -p "$WORK"
unpack_bootimg --boot_img "$ORIG_BOOT" --out "$WORK" > "$WORK/info.txt"
unpack_bootimg --boot_img "$ORIG_BOOT" --format info | sed -n '1,12p'

echo
echo "==> Сжимаю ядро (в оригинале тоже gzip)"
gzip -9 -c "$KERNEL" > out/Image.gz
echo "    $(stat -c%s "$KERNEL") -> $(stat -c%s out/Image.gz) байт"

# Готовая строка аргументов от оригинала. Пути в ней даны с префиксом out/,
# переставляем их на наш каталог распаковки и подменяем ядро на своё.
ARGS=$(unpack_bootimg --boot_img "$ORIG_BOOT" --format mkbootimg \
       | sed -e "s#out/#$WORK/#g" -e "s#--kernel $WORK/kernel#--kernel out/Image.gz#")

echo
echo "==> Собираю $OUT с параметрами оригинала"
echo "    $ARGS"
rm -f "$OUT"
eval mkbootimg $ARGS -o "$OUT"

echo
echo "==> Проверка собранного"
unpack_bootimg --boot_img "$OUT" --format info \
    | grep -iE "header version|page size|kernel load|kernel_size|tags|dtb size|command line"
echo
echo "Готово: $OUT ($(stat -c%s "$OUT") байт)"
echo "Прошивка:  tools/flash.sh $OUT"
