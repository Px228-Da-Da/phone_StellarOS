#!/usr/bin/env bash
# Упаковка нашего ядра в Android boot.img, который примет LK.
#
# Идея: не выдумывать параметры, а взять их из ОРИГИНАЛЬНОГО boot.img
# телефона (base, offsets, page size, cmdline) и подменить только ядро.
# Так исключаем целый класс ошибок «образ собран, но LK его не грузит».
#
# Запуск: tools/mkboot.sh prebuilt/backup/boot.img kernel/build/merlin/Image
set -e

ORIG_BOOT="${1:-prebuilt/backup/boot.img}"
KERNEL="${2:-kernel/build/merlin/Image}"
OUT="${3:-out/velo-boot.img}"

# Пакета mkbootimg на PyPI нет — инструмент берётся из AOSP,
# это делает tools/setup-wsl.sh.
command -v mkbootimg >/dev/null || {
    echo "Нет mkbootimg. Поставить:  bash tools/setup-wsl.sh" >&2
    exit 1
}
command -v unpack_bootimg >/dev/null || {
    echo "Нет unpack_bootimg. Поставить:  bash tools/setup-wsl.sh" >&2
    exit 1
}
[ -f "$ORIG_BOOT" ] || { echo "Нет оригинального boot.img: $ORIG_BOOT" >&2; exit 1; }
[ -f "$KERNEL" ]    || { echo "Нет ядра: $KERNEL (сделай make BOARD=merlin)" >&2; exit 1; }

mkdir -p out unpacked
echo "==> Разбираю оригинальный boot.img, чтобы взять его параметры"
unpack_bootimg --boot_img "$ORIG_BOOT" --out unpacked > unpacked/params.txt
cat unpacked/params.txt

# Достаём параметры из отчёта unpack_bootimg
PAGESIZE=$(grep -oP 'page size:\s*\K[0-9]+'          unpacked/params.txt | head -1)
OSVER=$(grep -oP 'os version:\s*\K\S+'               unpacked/params.txt | head -1)
OSPATCH=$(grep -oP 'os patch level:\s*\K\S+'         unpacked/params.txt | head -1)
HDRVER=$(grep -oP 'boot image header version:\s*\K[0-9]+' unpacked/params.txt | head -1)
CMDLINE=$(grep -oP 'command line args:\s*\K.*'       unpacked/params.txt | head -1)

echo
echo "==> Собираю velo-boot.img (header v${HDRVER}, page ${PAGESIZE})"
echo "    ВНИМАНИЕ: ramdisk намеренно НЕ кладём — нашему ядру он не нужен."

mkbootimg \
    --kernel "$KERNEL" \
    --header_version "${HDRVER:-2}" \
    --pagesize "${PAGESIZE:-2048}" \
    --os_version "${OSVER:-12.0.0}" \
    --os_patch_level "${OSPATCH:-2022-09}" \
    --cmdline "$CMDLINE" \
    --output "$OUT"

echo
echo "Готово: $OUT"
echo "Прошивка:  tools/flash.sh $OUT"
