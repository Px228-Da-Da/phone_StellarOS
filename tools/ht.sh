#!/bin/sh
#
# ht — работа с приложениями Hittis.
#
#   ht ide   [исходники/demo.ht]  среда разработки в браузере
#   ht run   исходники/demo.ht    окно на компьютере, мышь вместо пальца
#   ht build исходники/demo.ht    собрать готовое приложение в apps/
#   ht shot  исходники/demo.ht    снять первый кадр картинкой
#   ht check                      прогнать все приложения
#
# Две папки, и разница между ними важная. В исходники/ лежит то, что
# пишет человек, — текст на Hittis. В apps/ лежит то, что понимает
# система, — готовые .slt. Ядро читает только apps/ и ничего не
# компилирует: сборка это отдельное, осознанное действие, а не то, что
# случается само посреди сборки системы.
#
# Из Windows то же самое зовётся ht.bat из корня репозитория.
set -e
cd "$(dirname "$0")/.."

cmd=$1
app=$2

usage() {
    echo ""
    echo "  ht ide   [исходники/demo.ht] среда разработки в браузере"
    echo "  ht run   исходники/demo.ht   окно на компьютере, мышь вместо пальца"
    echo "  ht build исходники/demo.ht   собрать готовое приложение в apps/"
    echo "  ht shot  исходники/demo.ht   снять первый кадр картинкой"
    echo "  ht check                     прогнать все приложения"
    echo ""
    exit 1
}

case "$cmd" in
ide)
    # Без имени открываем первое приложение из apps: среда всё равно
    # покажет список слева, а начинать с пустого экрана незачем.
    [ -n "$app" ] || app=$(ls исходники/*.ht 2>/dev/null | head -1)
    [ -n "$app" ] || { echo "в исходники/ нет ни одного .ht"; exit 1; }
    make -s -C hittis
    exec hittis/preview "$app" 8080 --ide
    ;;
run)
    [ -n "$app" ] || usage
    make -s -C hittis
    exec hittis/preview "$app"
    ;;
build)
    # Готовое кладём в apps/, а не рядом с исходником: в apps/ живут
    # приложения, которые понимает система, и ничего кроме них.
    [ -n "$app" ] || usage
    make -s -C hittis hittis
    mkdir -p apps
    out="apps/$(basename "${app%.ht}").slt"
    hittis/hittis "$app" "$out"
    echo "готово: $out — этот файл и едет в телефон"
    ;;
shot)
    [ -n "$app" ] || usage
    make -s -C hittis
    hittis/preview "$app" --кадр /tmp/ht-shot.ppm
    mkdir -p kernel/build
    python3 tools/ppm2png.py /tmp/ht-shot.ppm kernel/build/ht-shot.png
    ;;
check)
    make -s -C hittis check
    ;;
*)
    usage
    ;;
esac
