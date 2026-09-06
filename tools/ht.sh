#!/bin/sh
#
# ht — работа с приложениями Hittis.
#
#   ht run   apps/demo.ht    окно на компьютере, мышь вместо пальца
#   ht build apps/demo.ht    собрать .slt рядом с исходником
#   ht shot  apps/demo.ht    снять первый кадр картинкой
#   ht check                 прогнать все приложения из apps/
#
# Из Windows то же самое зовётся ht.bat из корня репозитория.
set -e
cd "$(dirname "$0")/.."

cmd=$1
app=$2

usage() {
    echo ""
    echo "  ht run   apps/demo.ht   окно на компьютере, мышь вместо пальца"
    echo "  ht build apps/demo.ht   собрать .slt рядом с исходником"
    echo "  ht shot  apps/demo.ht   снять первый кадр в kernel/build/ht-shot.png"
    echo "  ht check                прогнать все приложения из apps/"
    echo ""
    exit 1
}

case "$cmd" in
run)
    [ -n "$app" ] || usage
    make -s -C hittis
    exec hittis/preview "$app"
    ;;
build)
    [ -n "$app" ] || usage
    make -s -C hittis hittis
    out="${app%.ht}.slt"
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
