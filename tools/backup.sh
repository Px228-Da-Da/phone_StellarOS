#!/usr/bin/env bash
# Полный бэкап разделов через mtkclient (правило №1 из docs/01-safety.md).
#
# Читает и ничего не пишет. Разблокировка для этого НЕ нужна: режим BROM
# живёт до загрузчика и о его состоянии ничего не знает, поэтому и таймер
# ожидания Xiaomi этим не тратится.
#
# Подготовка телефона:
#   1. Выключить полностью (не перезагрузка — именно выключение)
#   2. Зажать ОБЕ клавиши громкости и держать
#   3. Воткнуть USB, клавиши не отпускать секунды три
#   Экран останется чёрным — так и должно быть, в BROM телефон не рисует ничего.
#
# На Windows перед этим пробросить устройство в WSL:
#   usbipd list                       (найти MediaTek USB Port)
#   usbipd bind --busid <BUSID>       (один раз, нужны права админа)
#   usbipd attach --wsl --busid <BUSID>
set -e

MTK_DIR="${MTK_DIR:-/opt/mtkclient}"
MTK_PY="${MTK_PY:-/opt/mtk-env/bin/python}"
OUT="${1:-prebuilt/backup}"

# Порядок важен: сначала то, без чего телефон не восстановить.
#   nvram/nvdata/proinfo — IMEI и калибровка модема, теряются НАВСЕГДА
#   preloader/lk         — загрузочная цепочка, наша страховка
#   boot                 — из него mkboot.sh возьмёт параметры образа
#   dtbo                 — оверлеи device tree
PARTS="preloader,lk,lk2,boot,dtbo,vbmeta,vbmeta_system,vbmeta_vendor,recovery,nvram,nvdata,persist,proinfo,seccfg,devinfo,logo"

command -v "$MTK_PY" >/dev/null 2>&1 || {
    echo "Нет python окружения mtkclient: $MTK_PY" >&2
    exit 1
}
[ -f "$MTK_DIR/mtk.py" ] || { echo "Нет mtkclient в $MTK_DIR" >&2; exit 1; }

mkdir -p "$OUT"

# Путь делаем абсолютным СРАЗУ: mtkclient запускается из своего каталога,
# и любой относительный путь после этого укажет не туда, куда ждёшь.
OUT_ABS=$(cd "$OUT" && pwd)

# mtkclient ждёт по одному имени файла на каждый раздел, в том же порядке
FILES=""
for part in $(echo "$PARTS" | tr ',' ' '); do
    FILES="$FILES $OUT_ABS/$part.img"
done

echo "==> Жду телефон в режиме BROM (обе громкости + USB, экран чёрный)"
echo "    Читаю: $PARTS"
echo "    Кладу в: $OUT_ABS"
echo

(cd "$MTK_DIR" && "$MTK_PY" mtk.py r "$PARTS" $FILES)

OUT="$OUT_ABS"
echo
echo "==> Готово. Что получилось:"
ls -la "$OUT"
echo
echo "ВАЖНО: скопировать $OUT в облако или на другой диск."
echo "nvram/nvdata/proinfo содержат IMEI — восстановить их больше неоткуда."
