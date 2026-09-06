#!/usr/bin/env bash
# Прошивка нашего образа в раздел boot + быстрый откат.
#
#   tools/flash.sh out/stellar-boot.img   — прошить наше ядро
#   tools/flash.sh --restore           — вернуть заводской Android
set -e

BACKUP="prebuilt/backup/boot.img"

if [ "$1" = "--restore" ]; then
    [ -f "$BACKUP" ] || { echo "НЕТ БЭКАПА $BACKUP — откат невозможен!" >&2; exit 1; }
    echo "==> Возвращаю заводской boot"
    fastboot flash boot "$BACKUP"
    fastboot reboot
    exit 0
fi

IMG="${1:-out/stellar-boot.img}"
[ -f "$IMG" ] || { echo "Нет образа: $IMG" >&2; exit 1; }

if [ ! -f "$BACKUP" ]; then
    echo "СТОП. Нет бэкапа $BACKUP." >&2
    echo "Сначала сними его — иначе откатиться будет нечем." >&2
    echo "См. docs/01-safety.md" >&2
    exit 1
fi

echo "==> Жду телефон в fastboot (Vol- + Power)"
fastboot devices
fastboot getvar unlocked

echo
echo "==> Прошиваю $IMG в раздел boot"
echo "    Откат в любой момент:  tools/flash.sh --restore"
read -p "Продолжить? [y/N] " ans
[ "$ans" = "y" ] || exit 1

fastboot flash boot "$IMG"
echo
echo "Прошито. Перезагружаю."
echo "Если экран чёрный дольше 15 секунд — держи Power 20 сек,"
echo "потом Vol- + Power для fastboot и запусти:  tools/flash.sh --restore"
fastboot reboot
