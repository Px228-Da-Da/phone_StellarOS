#!/usr/bin/env python3
"""
Заслон против битого скрипта на странице среды разработки.

Появился по следам ошибки, которая стоила часа: в строковый литерал
JavaScript попал настоящий перевод строки. Для браузера это неустранимая
синтаксическая ошибка — он не исполняет ВЕСЬ скрипт целиком и не говорит
об этом никак. Наружу это выглядит как страница, где ничего не нажимается
и нигде нет текста, а причина не видна ни в одном логе.

Полноценного разбора JavaScript здесь нет и не нужно: ловим ровно тот
класс поломок, который делает страницу мёртвой и при этом незаметен
глазом — незакрытые строки и несошедшиеся скобки.

    python3 tools/jscheck.py hittis/ide.html
"""
import re
import sys


def scripts_of(html):
    """Все куски <script>...</script> с номером строки, где каждый начался."""
    out = []
    for m in re.finditer(r"<script[^>]*>(.*?)</script>", html, re.S):
        out.append((html[:m.start(1)].count("\n") + 1, m.group(1)))
    return out


def check(code, base):
    """Вернуть список жалоб: (строка, что не так)."""
    bad = []
    depth = {"(": 0, "[": 0, "{": 0}
    pair = {")": "(", "]": "[", "}": "{"}
    line = base
    i = 0
    n = len(code)

    while i < n:
        c = code[i]

        if c == "\n":
            line += 1
            i += 1
        elif c == "/" and i + 1 < n and code[i + 1] == "/":
            while i < n and code[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and code[i + 1] == "*":
            i += 2
            while i + 1 < n and not (code[i] == "*" and code[i + 1] == "/"):
                if code[i] == "\n":
                    line += 1
                i += 1
            i += 2
        elif c in "'\"":
            # Обычная строка: перевод строки внутри неё — та самая беда.
            start = line
            i += 1
            while i < n and code[i] != c:
                if code[i] == "\\":
                    i += 1
                elif code[i] == "\n":
                    bad.append((start, "строка не закрыта до конца строки"))
                    break
                i += 1
            i += 1
        elif c == "`":
            # Шаблонная строка: перевод строки в ней законен.
            i += 1
            while i < n and code[i] != "`":
                if code[i] == "\\":
                    i += 1
                elif code[i] == "\n":
                    line += 1
                i += 1
            i += 1
        else:
            if c in depth:
                depth[c] += 1
            elif c in pair:
                depth[pair[c]] -= 1
                if depth[pair[c]] < 0:
                    bad.append((line, "лишняя закрывающая " + c))
                    depth[pair[c]] = 0
            i += 1

    for k, v in depth.items():
        if v:
            bad.append((base, "не закрыто скобок «%s»: %d" % (k, v)))
    return bad


def main():
    if len(sys.argv) < 2:
        sys.exit("как пользоваться: jscheck.py страница.html")

    with open(sys.argv[1], encoding="utf-8") as f:
        html = f.read()

    parts = scripts_of(html)
    if not parts:
        sys.exit("в %s нет ни одного <script>" % sys.argv[1])

    bad = []
    for base, code in parts:
        bad += check(code, base)

    if bad:
        for line, why in bad:
            print("%s:%d: %s" % (sys.argv[1], line, why), file=sys.stderr)
        sys.exit(1)


main()
