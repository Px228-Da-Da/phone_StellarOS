# Паспорт устройства

| Параметр | Значение |
|---|---|
| Модель | M2003J15SG |
| Маркетинг | Redmi Note 9 (Global) |
| Кодовое имя | **merlin** |
| SoC | MediaTek Helio G85, **MT6769Z** |
| Архитектура | ARMv8-A, AArch64 |
| CPU | 2x Cortex-A75 @ 2.0 ГГц + 6x Cortex-A55 @ 1.8 ГГц |
| GPU | Mali-G52 MC2 |
| RAM | 4 ГБ (+2 ГБ swap MIUI) |
| Storage | 128 ГБ, **eMMC 5.1** (не UFS) |
| Экран | 6.53", 1080x2340, IPS, MIPI DSI |
| Прошивка | MIUI Global 13.0.2, Android 12 (SP1A.210812.016) |
| Ядро Linux | 4.14.186 |
| Модем | MOLY.LR12A.R3.MP.V98.1.P22 |

## Откуда брать техническую информацию

MediaTek не публикует datasheet на MT6769. **Единственный достоверный источник адресов
регистров и параметров периферии — официальные исходники ядра от Xiaomi:**

- Ядро merlin: https://github.com/MiCode/Xiaomi_Kernel_OpenSource — ветка `merlin-*-opensource`
- Что искать: `arch/arm64/boot/dts/mediatek/mt6768.dtsi`, `mt6769.dtsi`, `merlin.dts`
- Драйверы: `drivers/misc/mediatek/` (дисплей, тач, i2c, pmic)

Родственные SoC с той же периферией: MT6765 / MT6768 / MT6769 — одно семейство,
адреса регистров почти полностью совпадают. Полезен код mainline-порта:
https://github.com/torvalds/linux/tree/master/arch/arm64/boot/dts/mediatek (mt6765.dtsi)

## Статус проверки адресов

Всё, что помечено `TODO-VERIFY` в `kernel/src/soc/mt6769.h`, взято по аналогии
с MT6765/MT6768 и **должно быть сверено с DTS от merlin** перед прошивкой.
