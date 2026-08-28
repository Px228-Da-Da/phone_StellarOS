# Паспорт устройства

| Параметр | Значение | Источник |
|---|---|---|
| Кодовое имя | **merlinnfc** | `ro.product.device` |
| Модель | M2003J15SC / M2003J15SG | `ro.product.model` |
| Маркетинг | Redmi Note 9 NFC (Global) | `ro.boot.hwc` = Global |
| Платформа | **mt6768** | `ro.board.platform` |
| SoC | MediaTek Helio G85 | |
| CPU | 2x Cortex-A75 @2.0 + 6x Cortex-A55 @1.8 | ARMv8-A |
| RAM | 4 ГБ LPDDR4X | |
| Storage | 128 ГБ **eMMC** (`mmcblk0`, 53 раздела) | `/dev/block/by-name` |
| Экран | 1080x2340, IPS, MIPI DSI | |
| Прошивка | MIUI V13.0.2.0.SJOMIXM, Android 12 | `ro.build.version.incremental` |
| Ядро Linux | 4.14.186 | |
| Загрузчик | **ЗАБЛОКИРОВАН** | `ro.boot.flash.locked` = 1 |
| OEM unlock | разрешён в настройках | `sys.oem_unlock_allowed` = 1 |
| Verified boot | green, verity enforcing | `ro.boot.verifiedbootstate` |

## Карта регистров — снята с живого устройства

Имена узлов в `/proc/device-tree` содержат базовые адреса, поэтому
угадывать ничего не пришлось. Полный список: `device-info/dt-nodes.txt`.

| Блок | Адрес | Узел |
|---|---|---|
| UART0 | `0x11002000` | `serial@11002000` |
| UART1 | `0x11003000` | `serial@11003000` |
| GIC distributor | `0x0C000000` | `gic500@0c000000` — **GIC-500, то есть GICv3** |
| GIC CPU | `0x0C400000` | `gic_cpu@0c400000` |
| DISP_OVL0 | `0x1400B000` | `disp_ovl0@1400b000` |
| DISP_RDMA0 | `0x1400D000` | `disp_rdma0@1400d000` |
| DISP_PWM | `0x1100E000` | `disp_pwm@1100e000` |
| I2C0..I2C8 | `0x11007000`+ | `i2c0@11007000` ... |
| USB | `0x11200000` | `usb0@11200000` |

Две поправки к первоначальным предположениям:
- DISP_OVL0 оказался по `0x1400B000`, а не `0x14008000`.
- Контроллер прерываний — **GICv3**, значит драйвер пишем через системные
  регистры `ICC_*`, а не через MMIO-интерфейс GICv2.

## Где лежит адрес фреймбуфера

LK передаёт его прямо в DTB, в узле `/chosen`:
`atag,videolfb-fb_base_h` и `atag,videolfb-fb_base_l` (плюс `vramSize`,
`lcmname`, `islcm_inited`). Прочитать их из-под Android нельзя — SELinux
не пускает пользователя `shell`. Но наше ядро работает в EL1 без SELinux
и прочитает их свободно.

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
