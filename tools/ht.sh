#!/bin/sh
#
# ht — работа с приложениями Hittis.
#
#   ht ide   [calc]     среда разработки в браузере
#   ht run   calc       окно на компьютере, мышь вместо пальца
#   ht build calc       собрать готовое приложение в apps/calc.slt
#   ht shot  calc       снять первый кадр картинкой
#   ht check            прогнать все приложения
#
# Две папки, и разница между ними важная. В sources/ лежит то, что
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

# Корень исходников: там лежат папки приложений
ROOT=sources

#
# Приложение — это папка с манифестом, а не отдельный файл.
#
#   sources/calc/config.json   кто это, как называется, чем открывать
#   sources/calc/main.ht       сам текст
#   sources/calc/logo.png      значок
#   sources/calc/icons/        остальные картинки
#
# Так у приложения появляется имя, отличное от имени файла, и место, куда
# класть всё остальное. Раньше приложение было одним файлом, и всё, что к
# нему прилагалось, класть было некуда.
#
# Манифест читаем питоном: разбирать json оболочкой — верный способ
# однажды поперхнуться запятой.
#

# Значение поля из манифеста: ключ, путь к папке
manifest() {
    python3 - "$2" "$1" <<'EOF'
import json, sys
try:
    with open(sys.argv[1] + "/config.json", encoding="utf-8") as f:
        cfg = json.load(f)
    print(cfg.get("app", {}).get(sys.argv[2], ""))
except Exception:
    print("")
EOF
}

# Развернуть то, что назвал человек, в путь до точки входа.
# Понимает и «calc», и «sources/calc», и путь к самому .ht.
entry_of() {
    a=$1
    case "$a" in
        *.ht) echo "$a"; return ;;
    esac
    [ -d "$a" ] || a="$ROOT/$a"
    if [ ! -d "$a" ]; then
        echo ""
        return
    fi
    e=$(manifest entry "$a")
    [ -n "$e" ] || e=main.ht
    echo "$a/$e"
}

# Имя приложения для готового файла: имя папки, а не имя точки входа —
# main.slt у всех был бы один и тот же.
name_of() {
    a=$1
    case "$a" in
        *.ht) basename "${a%.ht}"; return ;;
    esac
    [ -d "$a" ] || a="$ROOT/$a"
    basename "$a"
}

usage() {
    echo ""
    echo "  ht ide   [calc]   среда разработки в браузере"
    echo "  ht run   calc     окно на компьютере, мышь вместо пальца"
    echo "  ht build calc     собрать готовое в apps/calc.slt"
    echo "  ht shot  calc     снять первый кадр картинкой"
    echo "  ht check          прогнать все приложения"
    echo ""
    exit 1
}

case "$cmd" in
ide)
    # Без имени открываем первое приложение: среда всё равно покажет
    # список слева, а начинать с пустого экрана незачем.
    [ -n "$app" ] || app=$(ls -d $ROOT/*/ 2>/dev/null | head -1)
    [ -n "$app" ] || { echo "в $ROOT/ нет ни одного приложения"; exit 1; }
    e=$(entry_of "$app")
    [ -n "$e" ] || { echo "не нашёл приложение: $app"; exit 1; }
    make -s -C hittis
    exec hittis/preview "$e" 8080 --ide --корень "$ROOT"
    ;;
run)
    [ -n "$app" ] || usage
    e=$(entry_of "$app")
    [ -n "$e" ] || { echo "не нашёл приложение: $app"; exit 1; }
    make -s -C hittis
    exec hittis/preview "$e"
    ;;
build)
    # Готовое кладём в apps/, а не рядом с исходником: в apps/ живут
    # приложения, которые понимает система, и ничего кроме них.
    [ -n "$app" ] || usage
    e=$(entry_of "$app")
    [ -n "$e" ] || { echo "не нашёл приложение: $app"; exit 1; }
    make -s -C hittis hittis
    mkdir -p apps
    out="apps/$(name_of "$app").slt"
    hittis/hittis "$e" "$out"
    echo "готово: $out — этот файл и едет в телефон"
    ;;
shot)
    [ -n "$app" ] || usage
    e=$(entry_of "$app")
    [ -n "$e" ] || { echo "не нашёл приложение: $app"; exit 1; }
    make -s -C hittis
    hittis/preview "$e" --кадр /tmp/ht-shot.ppm
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
