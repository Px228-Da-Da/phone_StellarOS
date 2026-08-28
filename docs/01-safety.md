# Как не превратить телефон в кирпич

Прочитать целиком **до** первой прошивки. Это самый важный файл в проекте.

## Почему MediaTek — удачный выбор для такого эксперимента

У всех MTK есть **BROM** (Boot ROM) — неубиваемая программа в ПЗУ чипа. Она
запускается раньше всего и её невозможно перезаписать. Если испортить
preloader, LK, boot — телефон всё равно определится на ПК в режиме BROM,
и его можно полностью перепрошить. Это наша страховка.

Инструмент: **mtkclient** (https://github.com/bkerler/mtkclient) — открытый,
поддерживает MT6768/MT6769, умеет обходить проверку подписи через
эксплойт `kamakiri`. Работает даже когда телефон «мёртв».

## Правило №1 — сначала полный бэкап

До любых экспериментов снять образы всех критичных разделов:

```bash
# в WSL (Ubuntu), телефон выключен, зажать Vol- + Vol+ и вставить USB
python mtk r preloader,lk,boot,dtbo,vbmeta,recovery,nvram,nvdata,persist,proinfo \
    prebuilt/backup/preloader.bin prebuilt/backup/lk.bin prebuilt/backup/boot.img \
    prebuilt/backup/dtbo.img prebuilt/backup/vbmeta.img prebuilt/backup/recovery.img \
    prebuilt/backup/nvram.bin prebuilt/backup/nvdata.bin prebuilt/backup/persist.bin \
    prebuilt/backup/proinfo.bin
```

`nvram` / `nvdata` / `proinfo` содержат **IMEI и калибровку модема**. Если их
потерять — телефон навсегда останется без связи. Скопировать бэкап в облако.

## Правило №2 — трогаем только раздел `boot`

Наше ядро прошивается **только** в `boot`. Не трогаем: `preloader`, `lk`,
`nvram`, `nvdata`, `persist`, `proinfo`, `modem`.

Пока preloader и LK целы — телефон всегда войдёт в fastboot
(Vol- + Power) и раздел `boot` можно вернуть одной командой.

## Правило №3 — путь отката всегда наготове

| Симптом | Что делать |
|---|---|
| Чёрный экран, но fastboot работает | `fastboot flash boot prebuilt/backup/boot.img` |
| Fastboot не открывается | mtkclient: `python mtk w boot prebuilt/backup/boot.img` |
| Вообще ничего, ПК не видит | Отсоединить, зажать Power 20 сек, повторить BROM (Vol-+Vol++USB) |
| Не помогает | Разобрать, замкнуть test point на плате → BROM гарантированно |

## Правило №4 — сначала QEMU

Каждая новая фича сначала работает в `make run-qemu`. На телефон льём
только то, что уже не падает в эмуляторе. Это экономит десятки циклов
«прошил → чёрный экран → откатил».

## Что мы НЕ ломаем

- Разблокировка загрузчика **стирает все данные** — заранее вынуть фото/контакты.
- Прошивка своего `boot` не мешает вернуть Android: залил бэкап — всё как было.
- Гарантия на телефон 2020 года всё равно кончилась.
